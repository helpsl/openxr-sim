// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
#pragma once

// WHAT THE SIMULATOR ASKS OF THE OPERATING SYSTEM besides its window (GLFW's), its link (sim/link.cpp) and
// its control socket (sim/control.cpp): where the fonts are and where the configuration lives.
// os_posix.cpp and os_win32.cpp.

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

}  // namespace oxs
