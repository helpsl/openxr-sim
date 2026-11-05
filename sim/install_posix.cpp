// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
//
// sim/install.hpp ON LINUX: the active runtime for this user, as a link in their configuration.
//
// THE LOADER'S ORDER (the OpenXR SDK's manifest_file.cpp): $XDG_CONFIG_HOME (~/.config), then each of
// $XDG_CONFIG_DIRS (/etc/xdg), then /etc; in each, openxr/1/active_runtime.<arch>.json before
// openxr/1/active_runtime.json; the first file found is the runtime. So a link in the user's own directory
// beats whatever the system chose, and moving it away gives the system's choice back. Whatever the user had
// there already is moved aside (.before-openxr-sim) and restored afterwards.
//
// Only an INSTALLED runtime is offered: OPENXR_SIM_MANIFEST, its manifest's installed path. A build tree
// has none, and a link to a manifest that is not there would leave every OpenXR application without a
// runtime.
#include "install.hpp"

#include <unistd.h>

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <system_error>
#include <vector>

#ifndef OPENXR_SIM_MANIFEST
#define OPENXR_SIM_MANIFEST "/usr/share/openxr/1/openxr_sim.json"
#endif

namespace fs = std::filesystem;

namespace oxs {

namespace {

#if defined(__x86_64__)
constexpr const char* kArch = "x86_64";
#elif defined(__aarch64__)
constexpr const char* kArch = "aarch64";
#elif defined(__i386__)
constexpr const char* kArch = "i686";
#else
constexpr const char* kArch = "";
#endif

constexpr const char* kAside = ".before-openxr-sim";

fs::path user_dir() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg != nullptr && xdg[0] == '/') return fs::path(xdg) / "openxr" / "1";
    const char* home = std::getenv("HOME");
    if (home != nullptr && home[0] != '\0') return fs::path(home) / ".config" / "openxr" / "1";
    return {};
}

std::vector<fs::path> system_dirs() {
    std::vector<fs::path> out;
    const char* dirs = std::getenv("XDG_CONFIG_DIRS");
    std::string list = dirs != nullptr && dirs[0] != '\0' ? dirs : "/etc/xdg";
    for (size_t at = 0; at <= list.size();) {
        const size_t colon = std::min(list.find(':', at), list.size());
        const std::string d = list.substr(at, colon - at);
        if (!d.empty() && d[0] == '/') out.push_back(fs::path(d) / "openxr" / "1");
        at = colon + 1;
    }
    out.push_back("/etc/openxr/1");
    return out;
}

std::vector<std::string> names() {
    std::vector<std::string> n;
    if (kArch[0] != '\0') n.push_back(std::string("active_runtime.") + kArch + ".json");
    n.push_back("active_runtime.json");
    return n;
}

bool present(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);   // follows links: a link to nothing is no runtime
}

// The file the loader would choose, looking in `dirs`; empty when it would find none.
fs::path chosen(const std::vector<fs::path>& dirs) {
    for (const auto& d : dirs)
        for (const auto& n : names())
            if (present(d / n)) return d / n;
    return {};
}

bool is_ours(const fs::path& p) {
    std::error_code ec;
    return !p.empty() && fs::equivalent(p, OPENXR_SIM_MANIFEST, ec);
}

// runtime.name from a manifest, else its library's file name (install_win32.cpp, manifest_name).
std::string manifest_name(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::smatch m;
    if (std::regex_search(text, m, std::regex(R"re("name"\s*:\s*"([^"]*)")re")) && !m[1].str().empty()) return m[1].str();
    if (std::regex_search(text, m, std::regex(R"re("library_path"\s*:\s*"([^"]*)")re"))) return fs::path(m[1].str()).filename().string();
    return path.filename().string();
}

int do_register(std::string* error) {
    const fs::path dir = user_dir();
    if (dir.empty()) { *error = "there is no home directory to configure"; return kRegisterFailed; }
    if (!present(OPENXR_SIM_MANIFEST)) { *error = std::string("the runtime is not installed (") + OPENXR_SIM_MANIFEST + ")"; return kRegisterFailed; }
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) { *error = "could not make " + dir.string() + ": " + ec.message(); return kRegisterFailed; }
    // Whatever the user had, kept -- unless it is already ours, or a backup is already there (ours was made
    // active before, and the backup is the person's own choice, not this).
    for (const auto& n : names()) {
        const fs::path p = dir / n;
        if (!fs::is_symlink(p, ec) && !present(p)) continue;
        if (is_ours(p)) { fs::remove(p, ec); continue; }
        const fs::path aside = dir / (n + kAside);
        if (!fs::is_symlink(aside, ec) && !present(aside)) fs::rename(p, aside, ec);
        else fs::remove(p, ec);
        if (ec) { *error = "could not move " + p.string() + " aside: " + ec.message(); return kRegisterFailed; }
    }
    fs::create_symlink(OPENXR_SIM_MANIFEST, dir / "active_runtime.json", ec);
    if (ec) { *error = "could not link " + (dir / "active_runtime.json").string() + ": " + ec.message(); return kRegisterFailed; }
    return kRegisterOk;
}

int do_unregister(std::string* error) {
    const fs::path dir = user_dir();
    if (dir.empty()) return kRegisterOk;
    std::error_code ec;
    for (const auto& n : names()) {
        const fs::path p = dir / n, aside = dir / (n + kAside);
        if (is_ours(p) || (fs::is_symlink(p, ec) && fs::read_symlink(p, ec) == OPENXR_SIM_MANIFEST)) fs::remove(p, ec);
        if ((fs::is_symlink(aside, ec) || present(aside)) && !fs::is_symlink(p, ec) && !present(p)) fs::rename(aside, p, ec);
        if (ec) { *error = "could not restore " + p.string() + ": " + ec.message(); return kRegisterFailed; }
    }
    return kRegisterOk;
}

}  // namespace

RuntimeState runtime_state() {
    RuntimeState st;
    st.can_register = present(OPENXR_SIM_MANIFEST) && !user_dir().empty();
    if (!st.can_register) st.why_not = std::string("not installed: ") + OPENXR_SIM_MANIFEST + " is not there";
    std::vector<fs::path> all{ user_dir() };
    for (const auto& d : system_dirs()) all.push_back(d);
    const fs::path active = chosen(all);
    st.active_name = active.empty() ? std::string() : manifest_name(active);
    st.active_is_ours = is_ours(active);
    st.ours_is_this_build = st.active_is_ours;   // one installed copy, the package's
    if (st.active_is_ours) {
        // What restoring gives: the user's own file moved aside, else the system's choice.
        fs::path back;
        for (const auto& n : names())
            if (back.empty() && present(user_dir() / (n + kAside))) back = user_dir() / (n + kAside);
        if (back.empty()) back = chosen(system_dirs());
        if (!back.empty() && !is_ours(back)) st.previous_name = manifest_name(back);
    }
    return st;
}

RegisterResult make_runtime_active(std::string* error) {
    return do_register(error) == kRegisterOk ? RegisterResult::Done : RegisterResult::Failed;
}

RegisterResult restore_previous_runtime(std::string* error) {
    return do_unregister(error) == kRegisterOk ? RegisterResult::Done : RegisterResult::Failed;
}

int register_runtime_command(bool) {
    std::string error;
    const int r = do_register(&error);
    if (r != kRegisterOk) std::fprintf(stderr, "openxr-sim: %s\n", error.c_str());
    return r;
}

int unregister_runtime_command(bool, bool) {
    std::string error;
    const int r = do_unregister(&error);
    if (r != kRegisterOk) std::fprintf(stderr, "openxr-sim: %s\n", error.c_str());
    return r;
}

}  // namespace oxs
