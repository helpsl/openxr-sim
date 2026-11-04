// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
#pragma once

// WHAT THE SIMULATOR ASKS OF THE OPERATING SYSTEM besides its window (GLFW's), its link (sim/link.cpp) and
// its control socket (sim/control.cpp): where the fonts are, where the configuration lives, what the desktop
// looks like, and who draws the window's frame. os_posix.cpp and os_win32.cpp.

#include "font.hpp"

#include <string>

namespace oxs {

// The file of a face: fontconfig's match for sans-serif, sans-serif:bold and monospace on Linux; Segoe UI,
// Segoe UI Semibold and Consolas on Windows. Empty when there is none. `embolden`: the file has no bold of
// its own that stb_truetype can draw -- the match is a variable font's named instance, whose default
// instance is all stb_truetype reads -- so its regular glyphs are to be made heavier.
struct FontFile { std::string path; bool embolden = false; };
FontFile find_font(Face face);
// Where find_font looked, for the error when it found nothing.
std::string font_search_description();

// THE CONFIGURATION FILE (the runtime reads the same: runtime/plat.h, plat_config_path): which = 0 for this
// project's, 1 for the one LOS's runtime read before the split (until 2027-05-01). Empty when there is no
// home to put it in.
std::string config_path(int which);
// Makes the directories `path` is in. False when they could not be.
bool make_parent_dirs(const std::string& path);

// THE DESKTOP'S APPEARANCE: light or dark, and the person's accent colour, so the window looks like the
// desktop it is on (sim/ui.cpp, set_theme). Asked of the desktop, never of any one desktop's own files:
//   Linux    the XDG desktop portal's Settings, org.freedesktop.appearance color-scheme and accent-color,
//            which GNOME, KDE and LOS answer (LOS since its los-portal 0.1.0) --
//            os_posix.cpp, ask_portal. A desktop with no portal
//            answers nothing, and the window keeps its own dark theme.
//   Windows  AppsUseLightTheme and the DWM colourisation colour.
// OPENXR_SIM_THEME=dark or light overrides the scheme. `ask_desktop` false: only that -- a --screenshot or a
// headless test looks the same on every machine, and in a build with no session bus.
struct Appearance {
    enum class Scheme { Unknown, Dark, Light } scheme = Scheme::Unknown;
    bool has_accent = false;
    float accent_r = 0, accent_g = 0, accent_b = 0;   // sRGB, 0..1
};
Appearance desktop_appearance(bool ask_desktop);

// WHO DRAWS THE FRAME (sim/window.cpp): true when the compositor offers to draw the window's title bar and
// borders itself (Wayland's xdg-decoration), so GLFW should ask it to instead of drawing one with libdecor.
// False on Windows, on X11 and on a compositor that offers nothing.
bool compositor_draws_frames();

}  // namespace oxs
