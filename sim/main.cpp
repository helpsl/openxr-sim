// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
//
// openxr-sim -- the simulated headset's window (docs/DESIGN.md). The runtime starts it with --headset when
// an application creates a session; started by itself it shows the one setting there is.

#include "app.hpp"
#include "install.hpp"
#include "notices.hpp"
#include "os.hpp"
#include "sim_protocol.h"
#include "ui.hpp"

#include <cstdio>
#include <cstdlib>
#include <print>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

bool parse_size(const std::string& s, int& w, int& h) {
    char x = 0;
    return std::sscanf(s.c_str(), "%d%c%d", &w, &x, &h) == 3 && x == 'x' && w >= 64 && h >= 64 && w <= 16384 && h <= 16384;
}

}  // namespace

int main(int argc, char** argv) {
    std::string app_name, screenshot, link;
    bool headset = false, check = false;
    int w = 1280, h = 760;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--help" || a == "-h") {
            std::println(
                "openxr-sim -- the simulated OpenXR headset\n\n"
                "usage: openxr-sim [--use | --unuse | --register-runtime | --unregister-runtime]\n\n"
                "  --use             OpenXR applications get the simulated headset (device = sim in the\n"
                "                    configuration file); OPENXR_SIM_DEVICE overrides it per process\n"
                "  --unuse           they find no headset connected again (device = null)\n"
                "  --register-runtime\n"
                "                    make this the active OpenXR runtime: on Windows install it (Program\n"
                "                    Files, asking for an administrator) and register it for every user; on\n"
                "                    Linux link it from ~/.config/openxr/1 for this user\n"
                "  --unregister-runtime [--remove]\n"
                "                    give back the runtime that was active before; --remove (Windows) also\n"
                "                    removes the installed copy -- what Settings > Apps' Uninstall runs\n"
                "  --screenshot OUT  render one frame to a PNG and exit; no display needed\n"
                "  --size WxH        the size --screenshot and --layout-check use (default 1280x760)\n"
                "  --layout-check    lay one frame out headless and report its layout faults\n"
                "  --licenses        the licences of the code it is built with (THIRD_PARTY_NOTICES.md)\n\n"
                "The runtime starts it with --headset --app NAME and the shared memory and its wake channel\n"
                "(descriptors 3 and 4; on Windows --link SHM,PIPE). OPENXR_SIM_HEADLESS=DIR makes that a test:\n"
                "no window, a scripted move, and the last frame and this window written to DIR.\n\n"
                "The window follows the desktop's light or dark scheme and accent colour (the XDG portal's\n"
                "org.freedesktop.appearance; on Windows the personalisation settings). OPENXR_SIM_THEME=dark\n"
                "or light sets the scheme instead; --screenshot and --layout-check use only that (default dark).");
            return 0;
        }
        if (a == "--licenses") {
#ifdef _WIN32
            SetConsoleOutputCP(CP_UTF8);
#endif
            std::fputs(oxs::kThirdPartyNotices, stdout);
            return 0;
        }
        if (a == "--use" || a == "--unuse") {
            std::string error;
            if (!oxs::set_configured_device(a == "--use" ? "sim" : "null", &error)) {
                std::println(stderr, "openxr-sim: {}", error);
                return 1;
            }
            std::println("OpenXR applications {}", a == "--use" ? "will use the simulated headset" : "will find no headset");
            return 0;
        }
        if (a == "--register-runtime" || a == "--unregister-runtime") {
            bool remove = false, elevated = false;
            for (int k = i + 1; k < argc; k++) {
                remove |= std::string(argv[k]) == "--remove";
                elevated |= std::string(argv[k]) == "--elevated";
            }
            return a == "--register-runtime" ? oxs::register_runtime_command(elevated)
                                             : oxs::unregister_runtime_command(remove, elevated);
        }
        if (a == "--headset") { headset = true; continue; }
        if (a == "--app" && i + 1 < argc) { app_name = argv[++i]; continue; }
        if (a == "--link" && i + 1 < argc) { link = argv[++i]; continue; }
        if (a == "--screenshot" && i + 1 < argc) { screenshot = argv[++i]; continue; }
        if (a == "--layout-check") { check = true; continue; }
        if (a == "--size" && i + 1 < argc) {
            if (!parse_size(argv[++i], w, h)) { std::println(stderr, "openxr-sim: --size wants WxH"); return 2; }
            continue;
        }
        std::println(stderr, "openxr-sim: unknown option '{}'", a);
        return 2;
    }

    // The desktop's look for a window someone sees; a picture or a test is the same everywhere.
    const char* headless = headset ? std::getenv("OPENXR_SIM_HEADLESS") : nullptr;
    const bool shown = !check && screenshot.empty() && (headless == nullptr || headless[0] == '\0');
    const oxs::Appearance look = oxs::desktop_appearance(shown);
    oxs::ui::set_theme(look);
    if (const char* log = std::getenv("OPENXR_SIM_LOG"); log != nullptr && log[0] != '\0' && log[0] != '0') {
        const char* scheme = look.scheme == oxs::Appearance::Scheme::Dark ? "dark" : look.scheme == oxs::Appearance::Scheme::Light ? "light" : "no preference";
        const oxs::Rgba a = oxs::ui::theme().accent;
        std::println(stderr, "openxr-sim: the desktop's scheme: {}; accent #{:02x}{:02x}{:02x}{}", scheme, static_cast<int>(a.r),
                     static_cast<int>(a.g), static_cast<int>(a.b), look.has_accent ? "" : " (none given: our own)");
    }

    oxs::App app(app_name);
    if (headset) {
        std::string error;
        if (!app.attach(link, &error)) {
            std::println(stderr, "openxr-sim: {}", error);
            return 1;
        }
        if (headless != nullptr && headless[0] != '\0') return app.headless(headless);
    }
    if (check) return app.layout_check(w, h);
    if (!screenshot.empty()) {
        std::string error;
        if (!app.render_png(screenshot, w, h, &error)) { std::println(stderr, "openxr-sim: {}", error); return 1; }
        return 0;
    }
    std::string error;
    const int rc = app.run(&error);
    if (rc != 0) std::println(stderr, "openxr-sim: {}", error);
    return rc;
}
