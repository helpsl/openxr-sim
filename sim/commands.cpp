// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
//
// WHAT THE CONTROL SOCKET ANSWERS (src/sim/control.hpp; DESIGN.md, "The control socket"). One command a
// line, words separated by spaces; one JSON object back, always with "ok". The devices move exactly as the
// mouse and keys move them, and a button held from here is held as the panel's buttons are -- alongside
// whatever the person holds, not instead of it.

#include "app.hpp"

#include "canvas.hpp"
#include "xrmath.h"

#include <charconv>
#include <cstdlib>

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>
#include <sstream>
#include <vector>

namespace oxs {

namespace {

std::string json_str(std::string_view s) {
    std::string out = "\"";
    for (const char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) out += std::format("\\u{:04x}", static_cast<unsigned char>(c));
                else out += c;
        }
    }
    return out + "\"";
}

std::string fail(std::string_view why) { return std::format(R"({{"ok":false,"error":{}}})", json_str(why)); }

std::optional<Part> part_of(std::string_view w) {
    if (w == "headset" || w == "head") return Part::Headset;
    if (w == "right") return Part::Right;
    if (w == "left") return Part::Left;
    return std::nullopt;
}

const char* part_name(Part p) { return p == Part::Headset ? "headset" : p == Part::Right ? "right" : "left"; }

// The names a control goes by, the buttons' own (A, X) as well as what they do.
struct Named { const char* name; uint32_t control; };
constexpr Named kControls[] = {
    { "trigger", kTrigger }, { "grip", kGrip }, { "squeeze", kGrip },
    { "primary", kPrimary }, { "a", kPrimary }, { "x", kPrimary },
    { "secondary", kSecondary }, { "b", kSecondary }, { "y", kSecondary },
    { "menu", kMenu }, { "stick", kStickClick },
    { "stick-up", kStickUp }, { "stick-down", kStickDown }, { "stick-left", kStickLeft }, { "stick-right", kStickRight },
};

std::string controls_json(uint32_t held) {
    static constexpr Named kOut[] = {
        { "trigger", kTrigger }, { "grip", kGrip }, { "primary", kPrimary }, { "secondary", kSecondary },
        { "menu", kMenu }, { "stick", kStickClick }, { "stick-up", kStickUp }, { "stick-down", kStickDown },
        { "stick-left", kStickLeft }, { "stick-right", kStickRight },
    };
    std::string out = "[";
    for (const Named& n : kOut)
        if ((held & n.control) != 0) out += std::string(out.size() > 1 ? "," : "") + json_str(n.name);
    return out + "]";
}

double number(const std::string& w, bool* ok) {
    char* end = nullptr;
    const double v = std::strtod(w.c_str(), &end);
    if (w.empty() || end != w.c_str() + w.size() || !std::isfinite(v)) { *ok = false; return 0; }
    return v;
}

std::optional<int> integer(const std::string& w) {
    int v = 0;
    const auto [p, ec] = std::from_chars(w.data(), w.data() + w.size(), v);
    if (ec != std::errc() || p != w.data() + w.size()) return std::nullopt;
    return v;
}

// The eyes side by side, each at most `max` pixels on its longer side, as a PNG.
std::string eyes_png(const EyePixels eyes[2], int max, int* out_w, int* out_h) {
    double scale[2] = { 1, 1 };
    int w[2] = {}, h[2] = {};
    for (int e = 0; e < 2; e++) {
        if (eyes[e].width <= 0 || eyes[e].height <= 0) continue;
        scale[e] = std::min(1.0, static_cast<double>(max) / std::max(eyes[e].width, eyes[e].height));
        w[e] = std::max(1, static_cast<int>(std::lround(eyes[e].width * scale[e])));
        h[e] = std::max(1, static_cast<int>(std::lround(eyes[e].height * scale[e])));
    }
    *out_w = w[0] + w[1];
    *out_h = std::max(h[0], h[1]);
    if (*out_w <= 0 || *out_h <= 0) return {};
    Canvas out;
    out.resize(*out_w, *out_h);
    out.clear({ 0, 0, 0, 255 });
    float x = 0;
    for (int e = 0; e < 2; e++) {
        if (w[e] == 0) continue;
        out.image({ x, 0, static_cast<float>(w[e]), static_cast<float>(h[e]) },
                  { eyes[e].argb.data(), eyes[e].width, eyes[e].height, eyes[e].width * 4 });
        x += static_cast<float>(w[e]);
    }
    return Canvas::encode_png({ out.data(), out.width(), out.height(), out.width() * 4 });
}

std::string base64(const std::string& in) {
    static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const uint32_t v = static_cast<uint8_t>(in[i]) << 16 | static_cast<uint8_t>(in[i + 1]) << 8 | static_cast<uint8_t>(in[i + 2]);
        out += k[v >> 18]; out += k[(v >> 12) & 63]; out += k[(v >> 6) & 63]; out += k[v & 63];
    }
    if (i < in.size()) {
        const uint32_t v = static_cast<uint8_t>(in[i]) << 16 | (i + 1 < in.size() ? static_cast<uint8_t>(in[i + 1]) << 8 : 0);
        out += k[v >> 18]; out += k[(v >> 12) & 63];
        out += i + 1 < in.size() ? k[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

// THE MARKERS' KINDS by name: qr, micro-qr, aruco-4x4-50 .. aruco-7x7-1000, apriltag-16h5 .. apriltag-36h11.
// The dictionaries are numbered as OpenXR numbers them (common/device.h, struct oxs_marker).
struct Kind { std::string name; int kind, dict; };

const std::vector<Kind>& kinds() {
    static const std::vector<Kind> all = [] {
        std::vector<Kind> k = { { "qr", OXS_MARKER_QR, 0 }, { "micro-qr", OXS_MARKER_MICRO_QR, 0 } };
        int dict = 1;
        for (int side = 4; side <= 7; side++)
            for (const int n : { 50, 100, 250, 1000 }) k.push_back({ std::format("aruco-{}x{}-{}", side, side, n), OXS_MARKER_ARUCO, dict++ });
        dict = 1;
        for (const char* d : { "16h5", "25h9", "36h10", "36h11" }) k.push_back({ std::format("apriltag-{}", d), OXS_MARKER_APRIL_TAG, dict++ });
        return k;
    }();
    return all;
}

std::string kind_name(const oxs_marker& m) {
    for (const Kind& k : kinds())
        if (k.kind == m.kind && k.dict == m.dict) return k.name;
    return "unknown";
}

}  // namespace

std::string marker_name(const oxs_marker& m) {
    const std::string k = kind_name(m);
    switch (m.kind) {
        case OXS_MARKER_QR: return std::format("QR code \"{}\"", m.text);
        case OXS_MARKER_MICRO_QR: return std::format("Micro QR \"{}\"", m.text);
        case OXS_MARKER_ARUCO: return std::format("ArUco {} #{}", k.substr(6), m.id);
        case OXS_MARKER_APRIL_TAG: return std::format("AprilTag {} #{}", k.substr(9), m.id);
        default: return k;
    }
}

std::string App::markers_json() const {
    const oxs_pose head = rig_.tracking().head;
    std::string out = R"({"ok":true,"markers":[)";
    for (uint32_t i = 0; i < room_.count; i++) {
        const oxs_marker& m = room_.marker[i];
        // Which way its printed face looks, as add-marker takes it.
        const float z[3] = { 0, 0, 1 };
        float face[3];
        oxs_quat_rotate(m.pose.q, z, face);
        const double deg = 180 / 3.14159265358979;
        out += std::format(R"({}{{"serial":{},"kind":{},"position":[{:.4f},{:.4f},{:.4f}],"yaw":{:.2f},"pitch":{:.2f},"size":{:.4f})",
                           i ? "," : "", m.serial, json_str(kind_name(m)), m.pose.p[0], m.pose.p[1], m.pose.p[2],
                           std::atan2(face[0], face[2]) * deg, std::asin(std::clamp(face[1], -1.0f, 1.0f)) * deg, m.size);
        if (m.kind == OXS_MARKER_QR || m.kind == OXS_MARKER_MICRO_QR) out += std::format(R"(,"text":{})", json_str(m.text));
        else out += std::format(R"(,"id":{})", m.id);
        out += std::format(R"(,"seen":{}}})", oxs_marker_seen(&head, &m) ? "true" : "false");
    }
    return out + "]}";
}

// add-marker KIND X Y Z YAW PITCH SIZE (ID | TEXT...), remove-marker SERIAL|all, reset-markers.
std::string App::marker_command(const std::vector<std::string>& w) {
    const std::string& verb = w[0];
    if (verb == "reset-markers") {
        // The markers as they start; one that was taken away comes back as a new marker, with a serial of its
        // own -- an entity once stopped stays stopped.
        oxs_room was = room_;
        oxs_room_home(&room_);
        for (uint32_t i = 0; i < room_.count; i++) {
            bool there = false;
            for (uint32_t k = 0; k < was.count; k++) there |= was.marker[k].serial == room_.marker[i].serial;
            if (!there) room_.marker[i].serial = next_serial_++;
        }
        publish();
        return markers_json();
    }
    if (verb == "remove-marker") {
        if (w.size() != 2) return fail("remove-marker SERIAL (from markers), or all");
        if (w[1] == "all") room_.count = 0;
        else {
            const auto serial = integer(w[1]);
            uint32_t i = 0;
            while (serial && i < room_.count && room_.marker[i].serial != static_cast<uint32_t>(*serial)) i++;
            if (!serial || i == room_.count) return fail("no marker " + w[1] + " in the room");
            std::copy(room_.marker + i + 1, room_.marker + room_.count, room_.marker + i);
            room_.count--;
        }
        publish();
        return markers_json();
    }
    // add-marker
    const char* usage = "add-marker KIND X Y Z YAW PITCH SIZE (ID | TEXT...): KIND qr, micro-qr, aruco-4x4-50 .. aruco-7x7-1000, "
                        "apriltag-16h5 .. apriltag-36h11; STAGE metres; YAW and PITCH the way its face looks, in degrees "
                        "(0 0 faces +Z, back towards the start; pitch 90 lies face up); SIZE its side in metres";
    if (w.size() < 9) return fail(usage);
    const auto kind = std::find_if(kinds().begin(), kinds().end(), [&](const Kind& k) { return k.name == w[1]; });
    if (kind == kinds().end()) return fail(std::string("no kind of marker called ") + w[1] + "; " + usage);
    bool ok = true;
    double v[6];
    for (size_t i = 0; i < 6; i++) v[i] = number(w[2 + i], &ok);
    if (!ok) return fail(usage);
    if (!(v[5] > 0 && v[5] <= 10)) return fail("a marker's SIZE is its side, more than 0 and at most 10 metres");
    if (room_.count == OXS_MARKERS_MAX) return fail(std::format("the room holds at most {} markers", OXS_MARKERS_MAX));
    oxs_marker m{};
    m.kind = kind->kind;
    m.dict = kind->dict;
    if (m.kind == OXS_MARKER_QR || m.kind == OXS_MARKER_MICRO_QR) {
        std::string t = w[8];
        for (size_t i = 9; i < w.size(); i++) t += " " + w[i];
        if (t.size() >= sizeof m.text) return fail(std::format("a code's text is at most {} bytes", sizeof m.text - 1));
        std::copy(t.begin(), t.end(), m.text);
    } else {
        const auto id = w.size() == 9 ? integer(w[8]) : std::nullopt;
        const uint32_t n = oxs_marker_dict_size(m.kind, m.dict);
        if (!id || *id < 0 || static_cast<uint32_t>(*id) >= n) return fail(std::format("{} has codes 0 to {}", kind->name, n - 1));
        m.id = static_cast<uint32_t>(*id);
    }
    const double rad = 3.14159265358979 / 180;
    oxs_marker_facing(static_cast<float>(v[3] * rad), static_cast<float>(v[4] * rad), m.pose.q);
    for (int i = 0; i < 3; i++) m.pose.p[i] = static_cast<float>(v[i]);
    m.size = static_cast<float>(v[5]);
    m.serial = next_serial_++;
    room_.marker[room_.count++] = m;
    publish();
    return markers_json();
}

std::string App::status_json() const {
    return std::format(R"({{"ok":true,"app":{},"profile":{},"state":{},"state_name":{},"fps":{:.1f},"frames":{},)"
                       R"("eyes":[[{},{}],[{},{}]],"haptic":[{:.2f},{:.2f}],"selected":"{}","control":{}}})",
                       json_str(status_.app[0] ? status_.app : app_name_.c_str()), json_str(status_.profile), status_.state,
                       json_str(session_state_name(status_.state)), status_.fps, link_.frames(),
                       eyes_[0].source_w, eyes_[0].source_h, eyes_[1].source_w, eyes_[1].source_h,
                       status_.haptic[0], status_.haptic[1], part_name(rig_.selected()), json_str(control_.name()));
}

std::string App::poses_json() const {
    std::string out = R"({"ok":true)";
    for (const Part p : { Part::Headset, Part::Left, Part::Right }) {
        double pos[3], yaw, pitch;
        rig_.readout(p, pos, yaw, pitch);
        out += std::format(R"(,"{}":{{"position":[{:.4f},{:.4f},{:.4f}],"yaw":{:.2f},"pitch":{:.2f})", part_name(p),
                           pos[0], pos[1], pos[2], yaw, pitch);
        if (p != Part::Headset) out += std::format(R"(,"held":{})", controls_json(rig_.held(p == Part::Left ? 0 : 1)));
        out += "}";
    }
    return out + "}";
}

std::string App::command(const std::string& line) {
    std::istringstream in(line);
    std::vector<std::string> w;
    for (std::string s; in >> s;) w.push_back(s);
    if (w.empty()) return fail("an empty command");
    const std::string& verb = w[0];

    if (verb == "status") return status_json();
    if (verb == "poses") return poses_json();
    if (verb == "reset") {
        rig_.reset();
        publish();
        return poses_json();
    }
    if (verb == "set" || verb == "move" || verb == "turn") {
        const size_t want = verb == "set" ? 7 : verb == "move" ? 5 : 4;
        const auto part = w.size() > 1 ? part_of(w[1]) : std::nullopt;
        if (!part || w.size() != want)
            return fail(verb == "set" ? "set PART X Y Z YAW PITCH (PART: headset, left, right; metres, degrees)"
                      : verb == "move" ? "move PART FORWARD RIGHT UP (metres)" : "turn PART YAW PITCH (degrees, yaw left)");
        bool ok = true;
        double v[5] = {};
        for (size_t i = 2; i < w.size(); i++) v[i - 2] = number(w[i], &ok);
        if (!ok) return fail("not a number: " + line);
        if (verb == "set") rig_.set_pose(*part, v, v[3], v[4]);
        else if (verb == "move") rig_.move_by(*part, v[0], v[1], v[2]);
        else rig_.turn_by(*part, v[0], v[1]);
        publish();
        return poses_json();
    }
    if (verb == "hold" || verb == "release") {
        const auto part = w.size() > 1 ? part_of(w[1]) : std::nullopt;
        if (!part || *part == Part::Headset || (verb == "hold" && w.size() < 3))
            return fail(verb + " left|right CONTROL... (trigger grip a b x y menu stick stick-up stick-down stick-left stick-right)" +
                        (verb == "release" ? std::string(", or none for all") : std::string()));
        const int hand = *part == Part::Left ? 0 : 1;
        uint32_t controls = 0;
        for (size_t i = 2; i < w.size(); i++) {
            std::string name = w[i];
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            const auto it = std::find_if(std::begin(kControls), std::end(kControls), [&](const Named& n) { return name == n.name; });
            if (it == std::end(kControls)) return fail("no control called " + w[i]);
            controls |= it->control;
        }
        if (verb == "hold") remote_held_[hand] |= controls;
        else remote_held_[hand] &= w.size() > 2 ? ~controls : 0u;
        publish();
        return poses_json();
    }
    if (verb == "markers") return markers_json();
    if (verb == "add-marker" || verb == "remove-marker" || verb == "reset-markers") return marker_command(w);
    if (verb == "shot") {
        int max = 512;
        if (w.size() > 1) {
            const auto v = integer(w[1]);
            if (!v || *v < 16 || *v > 2048) return fail("shot [MAX]: each eye at most MAX pixels, 16 to 2048");
            max = *v;
        }
        if (eyes_[0].width <= 0 && eyes_[1].width <= 0) return fail("no frame yet: the application has drawn nothing");
        int pw = 0, ph = 0;
        const std::string png = eyes_png(eyes_, max, &pw, &ph);
        if (png.empty()) return fail("the picture could not be encoded");
        return std::format(R"({{"ok":true,"frame":{},"width":{},"height":{},"png":"{}"}})", link_.frames(), pw, ph, base64(png));
    }
    return fail("unknown command '" + verb + "': status poses set move turn hold release reset shot markers add-marker "
                "remove-marker reset-markers");
}

}  // namespace oxs
