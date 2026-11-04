// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
#include "os.hpp"

#include <fontconfig/fontconfig.h>
#include <sys/stat.h>

#include <cerrno>
#include <cstdlib>

namespace oxs {

FontFile find_font(Face face) {
    static FcConfig* config = FcInitLoadConfigAndFonts();
    if (config == nullptr) return {};
    const char* pattern = face == Face::Bold ? "sans-serif:weight=bold" : face == Face::Mono ? "monospace" : "sans-serif";
    FcPattern* pat = FcNameParse(reinterpret_cast<const FcChar8*>(pattern));
    if (pat == nullptr) return {};
    // TrueType outlines only: stb_truetype reads glyf and CFF, not bitmap fonts.
    FcPatternAddBool(pat, FC_SCALABLE, FcTrue);
    FcConfigSubstitute(config, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult result = FcResultNoMatch;
    FcPattern* match = FcFontMatch(config, pat, &result);
    FontFile out;
    FcChar8* file = nullptr;
    int index = 0;
    if (match != nullptr && FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch) out.path = reinterpret_cast<const char*>(file);
    if (match != nullptr) FcPatternGetInteger(match, FC_INDEX, 0, &index);
    // A named instance has its number in the index's high 16 bits (fontconfig's FC_INDEX).
    out.embolden = face == Face::Bold && (index >> 16) != 0;
    if (match != nullptr) FcPatternDestroy(match);
    FcPatternDestroy(pat);
    return out;
}

std::string font_search_description() { return "fontconfig matched no scalable sans-serif font"; }

std::string config_path(int which) {
    const char* rel = which == 0 ? "openxr-sim/config" : "los/openxr";
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg != nullptr && xdg[0] == '/') return std::string(xdg) + "/" + rel;
    const char* home = std::getenv("HOME");
    if (home != nullptr && home[0] != '\0') return std::string(home) + "/.config/" + rel;
    return {};
}

bool make_parent_dirs(const std::string& path) {
    for (size_t at = path.find('/', 1); at != std::string::npos; at = path.find('/', at + 1)) {
        const std::string dir = path.substr(0, at);
        if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) return false;
    }
    return true;
}

}  // namespace oxs
