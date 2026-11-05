// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
//
// sim/os.hpp ON WINDOWS. Narrow strings are UTF-8 throughout: the executable's manifest sets the process's
// code page to UTF-8 (sim/openxr-sim.manifest), so a std::string path opens the same file the wide one would.
#include "os.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdlib>
#include <cstring>

namespace oxs {

namespace {

std::string narrow(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring widen(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

// THE BYTES OF THE FONT GDI CHOSE for `name` at `weight`: the whole file, as GetFontData gives it with
// table 0. On a collection (.ttc) that is one face's tables at offsets from the collection's start, so the
// collection is taken whole ('ttcf') and stb_truetype reads its first face. Empty when GDI had nothing --
// and what it has for a name it does not know is its own substitute, which is still a font.
std::vector<unsigned char> gdi_font(const wchar_t* name, int weight, std::wstring* chosen) {
    std::vector<unsigned char> out;
    HDC dc = CreateCompatibleDC(nullptr);
    if (dc == nullptr) return out;
    HFONT font = CreateFontW(-32, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_ONLY_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, name);
    if (font != nullptr) {
        HGDIOBJ old = SelectObject(dc, font);
        wchar_t face[LF_FACESIZE] = {};
        GetTextFaceW(dc, LF_FACESIZE, face);
        *chosen = face;
        constexpr DWORD ttcf = 0x66637474;   // 'ttcf', little-endian
        DWORD table = ttcf;
        DWORD size = GetFontData(dc, table, 0, nullptr, 0);
        if (size == GDI_ERROR) { table = 0; size = GetFontData(dc, table, 0, nullptr, 0); }
        if (size != GDI_ERROR && size > 0) {
            out.resize(size);
            if (GetFontData(dc, table, 0, out.data(), size) != size) out.clear();
        }
        SelectObject(dc, old);
        DeleteObject(font);
    }
    DeleteDC(dc);
    return out;
}

bool registry_dword(HKEY root, const wchar_t* key, const wchar_t* value, DWORD* out) {
    DWORD size = sizeof *out;
    return RegGetValueW(root, key, value, RRF_RT_REG_DWORD, nullptr, out, &size) == ERROR_SUCCESS;
}

}  // namespace

FontFile find_font(Face face) {
    const wchar_t* name = face == Face::Mono ? L"Consolas" : face == Face::Bold ? L"Segoe UI Semibold" : L"Segoe UI";
    const int weight = face == Face::Bold ? FW_SEMIBOLD : FW_NORMAL;
    FontFile out;
    std::wstring chosen;
    out.data = gdi_font(name, weight, &chosen);
    if (out.data.empty() && face == Face::Mono) out.data = gdi_font(L"Courier New", FW_NORMAL, &chosen);
    if (out.data.empty()) return {};
    out.path = narrow(chosen.c_str());
    // A bold GDI only pretends to (it has no bold face for the name, and would thicken the regular one as
    // it draws): the same bytes as the regular face, so the glyphs are made heavier here instead.
    if (face == Face::Bold) {
        std::wstring regular_name;
        out.embolden = gdi_font(chosen.c_str(), FW_NORMAL, &regular_name) == out.data;
    }
    return out;
}

std::string font_search_description() { return "GDI had no TrueType font for Segoe UI, nor any substitute"; }

// %APPDATA%\openxr-sim\config, as the runtime reads it (runtime/plat_win32.c, plat_config_path). There is
// no older file on Windows: LOS's runtime never ran there.
std::string config_path(int which) {
    if (which != 0) return {};
    const wchar_t* appdata = _wgetenv(L"APPDATA");
    if (appdata == nullptr || appdata[0] == L'\0') return {};
    return narrow(appdata) + "\\openxr-sim\\config";
}

bool make_parent_dirs(const std::string& path) {
    const std::wstring w = widen(path);
    // After the drive ("C:\") or the share ("\\server\share\"), every separator ends a directory.
    size_t start = 0;
    if (w.size() >= 3 && w[1] == L':') start = 3;
    else if (w.rfind(L"\\\\", 0) == 0) {
        start = w.find_first_of(L"\\/", 2);
        if (start != std::wstring::npos) start = w.find_first_of(L"\\/", start + 1);
        if (start == std::wstring::npos) return false;
        start++;
    }
    for (size_t at = w.find_first_of(L"\\/", start); at != std::wstring::npos; at = w.find_first_of(L"\\/", at + 1)) {
        const std::wstring dir = w.substr(0, at);
        if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    }
    return true;
}

// AppsUseLightTheme (0 dark, 1 light: Settings > Personalisation > Colours, "app mode") and the accent
// Windows shows in Start and on title bars, AccentColor -- 0xAABBGGRR, where the DWM's ColorizationColor
// beside it is 0xAARRGGBB and blended with the glass, so it is only the fallback.
Appearance desktop_appearance(bool ask_desktop) {
    Appearance a;
    if (const char* forced = std::getenv("OPENXR_SIM_THEME"); forced != nullptr && forced[0] != '\0') {
        if (std::strcmp(forced, "dark") == 0) { a.scheme = Appearance::Scheme::Dark; return a; }
        if (std::strcmp(forced, "light") == 0) { a.scheme = Appearance::Scheme::Light; return a; }
    }
    if (!ask_desktop) return a;
    DWORD v = 0;
    if (registry_dword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                       L"AppsUseLightTheme", &v))
        a.scheme = v != 0 ? Appearance::Scheme::Light : Appearance::Scheme::Dark;
    if (registry_dword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", &v)) {
        a.has_accent = true;
        a.accent_r = static_cast<float>(v & 0xFF) / 255;
        a.accent_g = static_cast<float>((v >> 8) & 0xFF) / 255;
        a.accent_b = static_cast<float>((v >> 16) & 0xFF) / 255;
    } else if (registry_dword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"ColorizationColor", &v)) {
        a.has_accent = true;
        a.accent_r = static_cast<float>((v >> 16) & 0xFF) / 255;
        a.accent_g = static_cast<float>((v >> 8) & 0xFF) / 255;
        a.accent_b = static_cast<float>(v & 0xFF) / 255;
    }
    return a;
}

bool compositor_draws_frames() { return false; }

}  // namespace oxs
