// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
//
// openxr-sim-mcp -- the simulated headset as a Model Context Protocol server (DESIGN.md, "The MCP
// server"). It speaks MCP over stdio -- one JSON-RPC message a line -- and drives every simulator this
// user has running through its control socket (src/sim/control.hpp): see both eyes, move the headset and
// the hands, hold and press the buttons, and start an OpenXR application on the simulated headset.
//
// STDOUT IS THE PROTOCOL. Nothing else may be written there -- a stray line is a client that drops the
// server with no reason given -- so every diagnostic goes to stderr, and an application `launch` starts
// has its output sent to a log file, never to ours.

#include <json/json.h>

#include "notices.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr const char* kVersion = OPENXR_SIM_VERSION;   // the project's (mcp/CMakeLists.txt)

std::string prefix() { return std::format("openxr-sim/{}/", ::getuid()); }

// ---- the simulators -------------------------------------------------------------------------------

// EVERY SIMULATOR LISTENING, by its id: /proc/net/unix lists abstract sockets with a leading '@', and a
// listening one is in state 01 with no peer. Only this user's names are looked for; the simulator refuses
// anyone else's connection anyway.
std::vector<std::string> simulators() {
    std::vector<std::string> out;
    std::ifstream f("/proc/net/unix");
    const std::string want = "@" + prefix();
    for (std::string line; std::getline(f, line);) {
        const size_t at = line.find(want);
        if (at == std::string::npos) continue;
        std::istringstream in(line);
        std::string num, refs, proto, flags, type, st;
        in >> num >> refs >> proto >> flags >> type >> st;
        if (st != "01") continue;
        std::string id = line.substr(at + want.size());
        while (!id.empty() && std::isspace(static_cast<unsigned char>(id.back()))) id.pop_back();
        if (!id.empty() && std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
    }
    return out;
}

// One command to one simulator, its one-line answer parsed. Nullopt, with why, if it could not be asked.
std::optional<Json::Value> ask(const std::string& id, const std::string& command, std::string* error) {
    const int s = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0) { *error = std::format("socket: {}", std::strerror(errno)); return std::nullopt; }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const std::string name = prefix() + id;
    if (name.size() + 1 > sizeof addr.sun_path) { ::close(s); *error = "a simulator id that is too long"; return std::nullopt; }
    std::memcpy(addr.sun_path + 1, name.data(), name.size());
    const socklen_t len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name.size());
    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), len) != 0) {
        *error = std::format("simulator {} did not answer ({}): its application may have ended", id, std::strerror(errno));
        ::close(s);
        return std::nullopt;
    }
    const std::string line = command + "\n";
    if (::send(s, line.data(), line.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(line.size())) {
        *error = "the simulator closed the connection";
        ::close(s);
        return std::nullopt;
    }
    std::string in;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (in.find('\n') == std::string::npos) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        pollfd p{ s, POLLIN, 0 };
        if (left <= 0 || ::poll(&p, 1, static_cast<int>(left)) <= 0) { *error = "the simulator did not answer in 5 seconds"; break; }
        char buf[65536];
        const ssize_t n = ::recv(s, buf, sizeof buf, 0);
        if (n <= 0) { *error = "the simulator closed the connection"; break; }
        in.append(buf, static_cast<size_t>(n));
    }
    ::close(s);
    const size_t nl = in.find('\n');
    if (nl == std::string::npos) return std::nullopt;
    Json::Value v;
    Json::CharReaderBuilder b;
    std::string errs;
    std::istringstream js(in.substr(0, nl));
    if (!Json::parseFromStream(b, js, &v, &errs)) { *error = "the simulator's answer was not JSON: " + errs; return std::nullopt; }
    return v;
}

// WHICH SIMULATOR A CALL MEANS: the one named (its id, or the start of it), else the only one there is.
std::optional<std::string> choose(const Json::Value& args, std::string* error) {
    const std::vector<std::string> all = simulators();
    const std::string want = args.get("simulator", "").asString();
    std::vector<std::string> match;
    for (const auto& id : all)
        if (want.empty() || id.starts_with(want)) match.push_back(id);
    if (match.size() == 1) return match[0];
    if (all.empty()) {
        *error = "no simulator is running: an OpenXR application has to have a session open on the simulated headset "
                 "(start one with `launch`, or with OPENXR_SIM_DEVICE=sim, or set `device = sim` with openxr-sim --use)";
    } else if (match.empty()) {
        *error = "no simulator's id starts with '" + want + "'; list_simulators shows them";
    } else {
        *error = "more than one simulator is running; name one with `simulator` (list_simulators shows them)";
    }
    return std::nullopt;
}

// ---- tools ----------------------------------------------------------------------------------------

struct Result {
    Json::Value content = Json::Value(Json::arrayValue);
    bool error = false;
    void text(const std::string& s) {
        Json::Value t;
        t["type"] = "text";
        t["text"] = s;
        content.append(t);
    }
    void image(const std::string& base64, const char* mime) {
        Json::Value t;
        t["type"] = "image";
        t["data"] = base64;
        t["mimeType"] = mime;
        content.append(t);
    }
};

Result failed(const std::string& why) {
    Result r;
    r.error = true;
    r.text(why);
    return r;
}

std::string compact(const Json::Value& v) {
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    w["emitUTF8"] = true;
    return Json::writeString(w, v);
}

std::string pretty(const Json::Value& v) {
    Json::StreamWriterBuilder w;
    w["indentation"] = "  ";
    w["emitUTF8"] = true;
    w["precision"] = 6;   // the simulator's own figures, not 1.6000000000000001
    return Json::writeString(w, v);
}

// A simulator's answer as the tool's: its error as the tool's error; "ok" dropped from the text.
Result relay(const std::string& id, const std::string& command) {
    std::string error;
    const auto v = ask(id, command, &error);
    if (!v) return failed(error);
    if (!(*v)["ok"].asBool()) return failed((*v)["error"].asString());
    Json::Value shown = *v;
    shown.removeMember("ok");
    Result r;
    r.text(pretty(shown));
    return r;
}

std::string num(const Json::Value& v) { return std::format("{}", v.asDouble()); }

std::string part_arg(const Json::Value& args, const char* key, bool headset_too, std::string* error) {
    const std::string p = args.get(key, "").asString();
    if (p == "right" || p == "left" || (headset_too && p == "headset")) return p;
    *error = std::format("`{}` must be {}", key, headset_too ? "headset, left or right" : "left or right");
    return {};
}

std::string controls_arg(const Json::Value& args, std::string* error) {
    std::string out;
    const Json::Value& c = args["controls"];
    if (c.isString()) return " " + c.asString();
    if (!c.isArray() || c.empty()) { *error = "`controls` must be a list such as [\"trigger\", \"a\"]"; return {}; }
    for (const auto& x : c) out += " " + x.asString();
    return out;
}

std::string log_dir() {
    const char* state = std::getenv("XDG_STATE_HOME");
    const char* home = std::getenv("HOME");
    const std::string base = state != nullptr && state[0] == '/' ? state : std::string(home != nullptr ? home : "/tmp") + "/.local/state";
    return base + "/openxr-sim-mcp";
}

// AN APPLICATION ON THE SIMULATED HEADSET, started apart from this server: its own session (setsid), and
// a double fork so it is not our child -- it outlives the server, which its client may restart at will.
// Its output goes to a log; OPENXR_SIM_DEVICE=sim makes the runtime start a simulator for its session.
Result launch(const Json::Value& args) {
    const Json::Value& cmd = args["command"];
    if (!cmd.isArray() || cmd.empty()) return failed("`command` must be the program and its arguments, as a list");
    std::vector<std::string> argv;
    for (const auto& a : cmd) argv.push_back(a.asString());
    std::vector<std::string> env;
    for (char** e = environ; *e != nullptr; e++)
        if (std::strncmp(*e, "OPENXR_SIM_DEVICE=", 18) != 0) env.emplace_back(*e);
    env.emplace_back("OPENXR_SIM_DEVICE=sim");
    const Json::Value& extra = args["env"];
    if (extra.isObject())
        for (const auto& k : extra.getMemberNames()) {
            std::erase_if(env, [&](const std::string& e) { return e.starts_with(k + "="); });
            env.push_back(k + "=" + extra[k].asString());
        }
    const std::string cwd = args.get("cwd", "").asString();
    std::error_code ec;
    fs::create_directories(log_dir(), ec);
    const std::time_t now = std::time(nullptr);
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", std::localtime(&now));
    const std::string log = std::format("{}/{}-{}.log", log_dir(), stamp, fs::path(argv[0]).filename().string());

    const std::vector<std::string> before = simulators();
    int pfd[2];
    if (::pipe2(pfd, O_CLOEXEC) != 0) return failed(std::format("pipe: {}", std::strerror(errno)));
    const pid_t child = ::fork();
    if (child < 0) return failed(std::format("fork: {}", std::strerror(errno)));
    if (child == 0) {
        ::close(pfd[0]);
        ::setsid();
        const pid_t grand = ::fork();
        if (grand != 0) {
            // The grandchild's pid, or the errno it failed with, through the pipe; then this one is done.
            const int v = grand;
            (void)!::write(pfd[1], &v, sizeof v);
            ::_exit(0);
        }
        const int in = ::open("/dev/null", O_RDONLY);
        const int out = ::open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (in >= 0) ::dup2(in, 0);
        if (out >= 0) { ::dup2(out, 1); ::dup2(out, 2); }
        if (!cwd.empty() && ::chdir(cwd.c_str()) != 0) { std::fprintf(stderr, "openxr-sim-mcp: cd %s: %s\n", cwd.c_str(), std::strerror(errno)); ::_exit(127); }
        ::signal(SIGPIPE, SIG_DFL);
        std::vector<char*> a, e;
        for (auto& s : argv) a.push_back(s.data());
        a.push_back(nullptr);
        for (auto& s : env) e.push_back(s.data());
        e.push_back(nullptr);
        ::execvpe(a[0], a.data(), e.data());
        std::fprintf(stderr, "openxr-sim-mcp: %s: %s\n", a[0], std::strerror(errno));
        ::_exit(127);
    }
    ::close(pfd[1]);
    int pid = -1;
    (void)!::read(pfd[0], &pid, sizeof pid);
    ::close(pfd[0]);
    ::waitpid(child, nullptr, 0);

    // UNTIL ITS SIMULATOR APPEARS: a session's simulator listens once the application has created a session.
    const int wait_s = std::clamp(args.get("wait_seconds", 20).asInt(), 0, 120);
    std::string found;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(wait_s);
    while (found.empty() && std::chrono::steady_clock::now() < deadline) {
        for (const auto& id : simulators())
            if (std::find(before.begin(), before.end(), id) == before.end()) { found = id; break; }
        if (found.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    const bool alive = pid > 0 && ::kill(pid, 0) == 0;
    Json::Value v;
    v["pid"] = pid;
    v["log"] = log;
    v["running"] = alive;
    if (!found.empty()) v["simulator"] = found;
    Result r;
    if (found.empty()) {
        r.error = !alive;
        r.text(pretty(v) + (alive ? "\nNo simulator appeared yet: the application has not created an OpenXR session. "
                                    "list_simulators will show it once it does; the log says what it is doing."
                                  : "\nThe application has already exited; its log says why."));
    } else {
        r.text(pretty(v));
    }
    return r;
}

Result call(const std::string& name, const Json::Value& args) {
    if (name == "list_simulators") {
        Json::Value list(Json::arrayValue);
        for (const auto& id : simulators()) {
            std::string error;
            Json::Value entry;
            entry["simulator"] = id;
            if (const auto v = ask(id, "status", &error)) {
                for (const char* k : { "app", "state_name", "profile", "fps", "frames" }) entry[k] = (*v)[k];
            } else {
                entry["error"] = error;
            }
            list.append(entry);
        }
        Result r;
        r.text(list.empty() ? "No simulator is running. One starts when an OpenXR application creates a session on the "
                              "simulated headset: `launch` starts an application that way."
                            : pretty(list));
        return r;
    }
    if (name == "launch") return launch(args);

    std::string error;
    const auto id = choose(args, &error);
    if (!id) return failed(error);

    if (name == "status") return relay(*id, "status");
    if (name == "get_poses") return relay(*id, "poses");
    if (name == "reset") return relay(*id, "reset");
    if (name == "set_pose") {
        const std::string part = part_arg(args, "part", true, &error);
        const Json::Value& p = args["position"];
        if (part.empty()) return failed(error);
        if (!p.isArray() || p.size() != 3) return failed("`position` must be [x, y, z] in metres");
        return relay(*id, std::format("set {} {} {} {} {} {}", part, num(p[0]), num(p[1]), num(p[2]),
                                      num(args.get("yaw", 0)), num(args.get("pitch", 0))));
    }
    if (name == "move") {
        const std::string part = part_arg(args, "part", true, &error);
        if (part.empty()) return failed(error);
        return relay(*id, std::format("move {} {} {} {}", part, num(args.get("forward", 0)), num(args.get("right", 0)),
                                      num(args.get("up", 0))));
    }
    if (name == "turn") {
        const std::string part = part_arg(args, "part", true, &error);
        if (part.empty()) return failed(error);
        return relay(*id, std::format("turn {} {} {}", part, num(args.get("yaw", 0)), num(args.get("pitch", 0))));
    }
    if (name == "hold" || name == "release" || name == "press") {
        const std::string hand = part_arg(args, "hand", false, &error);
        if (hand.empty()) return failed(error);
        std::string controls;
        if (name != "release" || args.isMember("controls")) {
            controls = controls_arg(args, &error);
            if (controls.empty()) return failed(error);
        }
        if (name != "press") return relay(*id, name + " " + hand + controls);
        // HELD LONG ENOUGH FOR A FRAME TO SEE IT: an application samples its actions once a frame.
        const int ms = std::clamp(args.get("milliseconds", 150).asInt(), 20, 10000);
        Result held = relay(*id, "hold " + hand + controls);
        if (held.error) return held;
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        return relay(*id, "release " + hand + controls);
    }
    if (name == "list_markers") return relay(*id, "markers");
    if (name == "reset_markers") return relay(*id, "reset-markers");
    if (name == "remove_marker") {
        const Json::Value& s = args["serial"];
        if (s.isString() && s.asString() == "all") return relay(*id, "remove-marker all");
        if (!s.isIntegral()) return failed("`serial` is a marker's serial, from list_markers, or \"all\"");
        return relay(*id, std::format("remove-marker {}", s.asInt64()));
    }
    if (name == "move_marker" || name == "set_marker_code") {
        if (!args["serial"].isIntegral()) return failed("`serial` is a marker's serial, from list_markers");
        const int64_t serial = args["serial"].asInt64();
        if (name == "set_marker_code") {
            std::string code;
            if (args["text"].isString()) {
                code = args["text"].asString();
                if (code.empty() || code.find_first_of("\r\n") != std::string::npos) return failed("`text` is a QR code's text, on one line");
            } else if (args["id"].isIntegral()) {
                code = std::to_string(args["id"].asInt64());
            } else {
                return failed("give a QR code's `text`, or an ArUco marker's or AprilTag's `id`");
            }
            return relay(*id, std::format("set-marker-code {} {}", serial, code));
        }
        // What is not given stays as it is: the marker's present pose, from the simulator.
        const auto v = ask(*id, "markers", &error);
        if (!v) return failed(error);
        const Json::Value* now = nullptr;
        for (const Json::Value& m : (*v)["markers"])
            if (m["serial"].asInt64() == serial) now = &m;
        if (now == nullptr) return failed(std::format("no marker {} in the room", serial));
        const Json::Value& p = args.isMember("position") ? args["position"] : (*now)["position"];
        if (!p.isArray() || p.size() != 3) return failed("`position` must be [x, y, z] in metres");
        return relay(*id, std::format("move-marker {} {} {} {} {} {} {} {}", serial, num(p[0]), num(p[1]), num(p[2]),
                                      num(args.get("yaw", (*now)["yaw"])), num(args.get("pitch", (*now)["pitch"])),
                                      num(args.get("roll", (*now)["roll"])), num(args.get("size", (*now)["size"]))));
    }
    if (name == "add_marker") {
        const std::string kind = args.get("kind", "").asString();
        const Json::Value& p = args["position"];
        if (kind.empty() || kind.find_first_of(" \t\r\n") != std::string::npos) return failed("`kind` names the kind of marker");
        if (!p.isArray() || p.size() != 3) return failed("`position` must be [x, y, z] in metres");
        std::string code;
        if (kind == "qr" || kind == "micro-qr") {
            code = args.get("text", "").asString();
            if (code.empty() || code.find_first_of("\r\n") != std::string::npos) return failed("a QR code needs `text`, on one line");
        } else {
            if (!args["id"].isIntegral()) return failed("an ArUco marker or AprilTag needs its `id` in its dictionary");
            code = std::to_string(args["id"].asInt64());
        }
        return relay(*id, std::format("add-marker {} {} {} {} {} {} {} {}", kind, num(p[0]), num(p[1]), num(p[2]),
                                      num(args.get("yaw", 0)), num(args.get("pitch", 0)), num(args.get("size", 0.15)), code));
    }
    if (name == "screenshot") {
        const int max = std::clamp(args.get("max_size", 512).asInt(), 16, 2048);
        const auto v = ask(*id, std::format("shot {}", max), &error);
        if (!v) return failed(error);
        if (!(*v)["ok"].asBool()) return failed((*v)["error"].asString());
        Result r;
        r.image((*v)["png"].asString(), "image/png");
        r.text(std::format("Both eyes side by side, left then right: {}x{} pixels, frame {}.", (*v)["width"].asInt(),
                           (*v)["height"].asInt(), (*v)["frame"].asUInt64()));
        return r;
    }
    return failed("no tool called " + name);
}

// ---- the tool list ------------------------------------------------------------------------------------

Json::Value prop(const char* type, const char* description) {
    Json::Value p;
    p["type"] = type;
    p["description"] = description;
    return p;
}

Json::Value tool(const char* name, const char* description, Json::Value props = Json::Value(Json::objectValue),
                 std::vector<const char*> required = {}, bool per_simulator = true) {
    if (per_simulator)
        props["simulator"] = prop("string", "Which simulator (its id from list_simulators, or the start of it). "
                                            "Not needed when only one is running.");
    Json::Value schema;
    schema["type"] = "object";
    schema["properties"] = props;
    schema["required"] = Json::Value(Json::arrayValue);
    for (const char* r : required) schema["required"].append(r);
    Json::Value t;
    t["name"] = name;
    t["description"] = description;
    t["inputSchema"] = schema;
    return t;
}

Json::Value part_prop(bool headset_too) {
    Json::Value p = prop("string", headset_too ? "headset, left or right" : "left or right");
    p["enum"].append(headset_too ? "headset" : "left");
    p["enum"].append(headset_too ? "left" : "right");
    if (headset_too) p["enum"].append("right");
    return p;
}

Json::Value controls_prop() {
    Json::Value p = prop("array", "Controls: trigger, grip, a, b (right hand) or x, y (left hand), menu, stick (pressed in), "
                                  "stick-up, stick-down, stick-left, stick-right. primary and secondary also name A/X and B/Y.");
    p["items"]["type"] = "string";
    return p;
}

Json::Value tools() {
    Json::Value list(Json::arrayValue);
    list.append(tool("list_simulators",
                     "The simulated headsets running now -- one per OpenXR session -- with each one's application, "
                     "session state and interaction profile.", Json::Value(Json::objectValue), {}, false));
    {
        Json::Value p;
        Json::Value c = prop("array", "The program and its arguments, e.g. [\"/usr/bin/hello_xr\", \"-g\", \"Vulkan2\"].");
        c["items"]["type"] = "string";
        p["command"] = c;
        p["env"] = prop("object", "Extra environment variables for it (strings).");
        p["cwd"] = prop("string", "The directory to start it in.");
        p["wait_seconds"] = prop("integer", "How long to wait for its session's simulator to appear (default 20).");
        list.append(tool("launch",
                         "Start an OpenXR application on the simulated headset (OPENXR_SIM_DEVICE=sim), detached from this "
                         "server, its output in a log file. Waits until its simulator appears, and returns its id.",
                         p, { "command" }, false));
    }
    list.append(tool("status", "What the application is doing: its name, session state (Focused is running and receiving "
                               "input), interaction profile, frame rate, frames shown, eye sizes, haptics."));
    list.append(tool("screenshot", "What the application drew: both eyes side by side, left then right, as an image.",
                     [] { Json::Value p; p["max_size"] = prop("integer", "Each eye at most this many pixels on its longer side (default 512)."); return p; }()));
    list.append(tool("get_poses", "Where the headset and both hands are (STAGE metres: +X right, +Y up, -Z forward; the "
                                  "floor at y = 0) and which way each faces (yaw degrees, positive to the left; pitch, up), "
                                  "and what each hand holds."));
    {
        Json::Value p;
        p["part"] = part_prop(true);
        Json::Value pos = prop("array", "[x, y, z] in metres, in STAGE.");
        pos["items"]["type"] = "number";
        p["position"] = pos;
        p["yaw"] = prop("number", "Degrees, positive to the left (0 faces -Z).");
        p["pitch"] = prop("number", "Degrees, positive up, within +-83.");
        list.append(tool("set_pose", "Put the headset or a hand at a pose, as get_poses reports one. Hands move with the "
                                     "head: place the headset first.", p, { "part", "position" }));
    }
    {
        Json::Value p;
        p["part"] = part_prop(true);
        p["forward"] = prop("number", "Metres ahead (negative: back).");
        p["right"] = prop("number", "Metres to the right (negative: left).");
        p["up"] = prop("number", "Metres up (negative: down).");
        list.append(tool("move", "Move the headset (along the way it faces, level) or a hand (in the body's frame), as "
                                 "W A S D Q E do in the window.", p, { "part" }));
    }
    {
        Json::Value p;
        p["part"] = part_prop(true);
        p["yaw"] = prop("number", "Degrees to turn left (negative: right).");
        p["pitch"] = prop("number", "Degrees to tilt up (negative: down).");
        list.append(tool("turn", "Turn the headset or a hand, as dragging in the window does.", p, { "part" }));
    }
    {
        Json::Value p;
        p["hand"] = part_prop(false);
        p["controls"] = controls_prop();
        p["milliseconds"] = prop("integer", "How long to hold them (default 150).");
        list.append(tool("press", "Press buttons on a hand and let go: held long enough for the application to see.",
                         p, { "hand", "controls" }));
    }
    {
        Json::Value p;
        p["hand"] = part_prop(false);
        p["controls"] = controls_prop();
        list.append(tool("hold", "Hold buttons on a hand down until release.", p, { "hand", "controls" }));
    }
    {
        Json::Value p;
        p["hand"] = part_prop(false);
        p["controls"] = controls_prop();
        list.append(tool("release", "Let go of buttons held with hold; all of that hand's if `controls` is left out.", p, { "hand" }));
    }
    list.append(tool("reset", "Put the headset and hands back where an application first sees them: standing at the "
                              "origin, eyes 1.6 m up, facing -Z."));
    list.append(tool("list_markers", "The printed markers in the simulated room -- QR codes, Micro QR codes, ArUco markers and "
                                     "AprilTags -- with each one's serial, pose (STAGE metres; yaw and pitch the way its face "
                                     "looks), size, code, and whether the headset sees it now. An application using "
                                     "XR_EXT_spatial_marker_tracking finds those it sees."));
    {
        Json::Value p;
        Json::Value kind = prop("string", "qr, micro-qr, aruco-4x4-50 .. aruco-7x7-1000 (side 4 to 7; 50, 100, 250 or 1000 "
                                          "codes), or apriltag-16h5, apriltag-25h9, apriltag-36h10, apriltag-36h11.");
        p["kind"] = kind;
        Json::Value pos = prop("array", "[x, y, z] of its centre, metres, in STAGE.");
        pos["items"]["type"] = "number";
        p["position"] = pos;
        p["yaw"] = prop("number", "Degrees the way its printed face looks: 0 faces +Z (back towards the start), 90 faces +X.");
        p["pitch"] = prop("number", "Degrees its face tilts up: 90 lies face up, -90 on the ceiling.");
        p["size"] = prop("number", "Its side in metres (default 0.15). The headset reads one up to 20 sides away.");
        p["text"] = prop("string", "A QR or Micro QR code's text.");
        p["id"] = prop("integer", "An ArUco marker's or AprilTag's number in its dictionary.");
        list.append(tool("add_marker", "Put up a printed marker in the room. It is seen while it is within the headset's "
                                       "90-degree view, faces it, and is near enough.", p, { "kind", "position" }));
    }
    {
        Json::Value p;
        p["serial"] = prop("integer", "The marker's serial, from list_markers; or the string \"all\".");
        p["serial"]["type"] = Json::Value(Json::arrayValue);
        p["serial"]["type"].append("integer");
        p["serial"]["type"].append("string");
        list.append(tool("remove_marker", "Take a marker out of the room: an application tracking it sees it stop.", p, { "serial" }));
    }
    {
        Json::Value p;
        p["serial"] = prop("integer", "The marker's serial, from list_markers.");
        Json::Value pos = prop("array", "[x, y, z] of its centre, metres, in STAGE (default: where it is).");
        pos["items"]["type"] = "number";
        p["position"] = pos;
        p["yaw"] = prop("number", "Degrees the way its printed face looks: 0 faces +Z, 90 faces +X (default: as it is).");
        p["pitch"] = prop("number", "Degrees its face tilts up, after the yaw: 90 lies face up (default: as it is).");
        p["roll"] = prop("number", "Degrees it turns about its own face, after the pitch; anticlockwise seen from the front "
                                   "(default: as it is).");
        p["size"] = prop("number", "Its side in metres (default: as it is).");
        list.append(tool("move_marker", "Move or turn a marker in the room: turned by yaw, then pitch, then roll. It stays the "
                                        "same marker: an application tracking it sees it move, by the same entity id.", p, { "serial" }));
    }
    {
        Json::Value p;
        p["serial"] = prop("integer", "The marker's serial, from list_markers.");
        p["text"] = prop("string", "A QR or Micro QR code's new text.");
        p["id"] = prop("integer", "An ArUco marker's or AprilTag's new number in its dictionary.");
        list.append(tool("set_marker_code", "Print a different code on a marker, where it is. To an application that is a "
                                            "different marker: the old one stops, and a new one (a new serial) is there to "
                                            "discover.", p, { "serial" }));
    }
    list.append(tool("reset_markers", "The room's markers as they start: a QR code ahead, a Micro QR code beside it, an ArUco "
                                      "marker on a table and an AprilTag on the wall to the left."));
    return list;
}

// ---- JSON-RPC ---------------------------------------------------------------------------------------

void send(const Json::Value& msg) {
    std::cout << compact(msg) << "\n";
    std::cout.flush();
}

void reply(const Json::Value& id, const Json::Value& result) {
    Json::Value m;
    m["jsonrpc"] = "2.0";
    m["id"] = id;
    m["result"] = result;
    send(m);
}

void reply_error(const Json::Value& id, int code, const std::string& message) {
    Json::Value m;
    m["jsonrpc"] = "2.0";
    m["id"] = id;
    m["error"]["code"] = code;
    m["error"]["message"] = message;
    send(m);
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--help" || a == "-h") {
            std::fprintf(stderr,
                "openxr-sim-mcp -- the simulated OpenXR headset as an MCP server, over stdio\n\n"
                "Register it with an MCP client, e.g. Claude Code:\n"
                "  claude mcp add openxr-sim -- openxr-sim-mcp\n\n"
                "It drives the simulators (openxr-sim) running for this user's OpenXR sessions.\n"
                "--licenses prints the licences of the code it is built with.\n");
            return 0;
        }
        if (a == "--licenses") {
            std::fputs(oxs::kThirdPartyNotices, stdout);
            return 0;
        }
        std::fprintf(stderr, "openxr-sim-mcp: unknown option '%s'\n", argv[i]);
        return 2;
    }
    ::signal(SIGPIPE, SIG_IGN);
    std::ios::sync_with_stdio(false);
    Json::CharReaderBuilder rb;
    for (std::string line; std::getline(std::cin, line);) {
        if (line.empty() || line == "\r") continue;
        Json::Value msg;
        std::string errs;
        std::istringstream in(line);
        if (!Json::parseFromStream(rb, in, &msg, &errs) || !msg.isObject()) {
            reply_error(Json::Value(), -32700, "parse error: " + errs);
            continue;
        }
        const std::string method = msg.get("method", "").asString();
        const bool has_id = msg.isMember("id");
        const Json::Value id = msg["id"];
        if (!has_id) continue;   // a notification (notifications/initialized, cancelled): nothing to answer
        if (method == "initialize") {
            Json::Value r;
            r["protocolVersion"] = msg["params"].get("protocolVersion", "2025-06-18");
            r["capabilities"]["tools"] = Json::Value(Json::objectValue);
            r["serverInfo"]["name"] = "openxr-sim";
            r["serverInfo"]["version"] = kVersion;
            r["instructions"] =
                "Drives openxr-sim's simulated OpenXR headset. A simulator exists while an OpenXR application has a session on it: "
                "start one with launch (or find one with list_simulators), then screenshot to see the eyes, move/turn/set_pose "
                "to look and reach, press/hold/release for the controllers' buttons. Poses are in STAGE metres, yaw positive "
                "to the left.";
            reply(id, r);
        } else if (method == "ping") {
            reply(id, Json::Value(Json::objectValue));
        } else if (method == "tools/list") {
            Json::Value r;
            r["tools"] = tools();
            reply(id, r);
        } else if (method == "tools/call") {
            const Json::Value& p = msg["params"];
            const Json::Value args = p["arguments"].isObject() ? p["arguments"] : Json::Value(Json::objectValue);
            const Result res = call(p.get("name", "").asString(), args);
            Json::Value r;
            r["content"] = res.content;
            r["isError"] = res.error;
            reply(id, r);
        } else {
            reply_error(id, -32601, "no method " + method);
        }
    }
    return 0;
}
