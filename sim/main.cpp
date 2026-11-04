// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
//
// openxr-sim -- the simulated headset's window (DESIGN.md). The LOS OpenXR runtime starts it with
// --headset when an application creates a session; from the menu it shows the one setting there is.

#include "app.hpp"

#include "los/parse.hpp"

#include <cstdlib>
#include <print>
#include <string>

int main(int argc, char** argv) {
    std::string app_name, screenshot;
    bool headset = false, check = false;
    int w = 1280, h = 760;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--help" || a == "-h") {
            std::println(
                "openxr-sim -- the simulated OpenXR headset\n\n"
                "usage: openxr-sim [--use | --unuse]\n\n"
                "  --use             OpenXR applications get the simulated headset (device = sim in\n"
                "                    ~/.config/los/openxr); OPENXR_SIM_DEVICE overrides it per process\n"
                "  --unuse           they find no headset connected again (device = null)\n"
                "  --screenshot OUT  render one frame to a PNG and exit; no compositor needed\n"
                "  --size WxH        the size --screenshot and --layout-check use (default 1280x760)\n"
                "  --layout-check    lay one frame out headless and report its layout faults\n\n"
                "The runtime starts it with --headset --app NAME and the shared memory and socket on\n"
                "descriptors 3 and 4. OPENXR_SIM_HEADLESS=DIR makes that a test: no window, a scripted\n"
                "move, and the last frame and this window written to DIR.");
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
        if (a == "--headset") { headset = true; continue; }
        if (a == "--app" && i + 1 < argc) { app_name = argv[++i]; continue; }
        if (a == "--screenshot" && i + 1 < argc) { screenshot = argv[++i]; continue; }
        if (a == "--layout-check") { check = true; continue; }
        if (a == "--size" && i + 1 < argc) {
            const auto v = los::parse_tuple<int, 2>(argv[++i], 'x');
            if (!v) { std::println(stderr, "openxr-sim: --size wants WxH"); return 2; }
            w = (*v)[0];
            h = (*v)[1];
            continue;
        }
        std::println(stderr, "openxr-sim: unknown option '{}'", a);
        return 2;
    }

    oxs::App app(app_name);
    if (headset) {
        std::string error;
        if (!app.attach(OXS_SHM_FD, OXS_SOCKET_FD, &error)) {
            std::println(stderr, "openxr-sim: {}", error);
            return 1;
        }
        if (const char* dir = std::getenv("OPENXR_SIM_HEADLESS"); dir != nullptr && dir[0] != '\0') return app.headless(dir);
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
