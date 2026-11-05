// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
#include "font.hpp"

#include "os.hpp"
#include "stb_truetype.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <unordered_map>

namespace oxs {

struct Fonts::Face_ {
    std::vector<unsigned char> file;
    stbtt_fontinfo info{};
    int ascent = 0, descent = 0, gap = 0;
    bool embolden = false;   // sim/os.hpp, FontFile
    // Advances in font units, by code point; glyph bitmaps by (size in 1/64 px, code point, quarter pixel).
    std::unordered_map<uint32_t, int> advance;
    std::unordered_map<uint64_t, Glyph> glyphs;
};

uint32_t next_codepoint(std::string_view s, size_t& i) {
    const auto b = [&](size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned char c = b(i);
    int n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
    if (n < 0 || i + n >= s.size() + (n == 0 ? 1 : 0)) { i++; return 0xFFFD; }
    uint32_t cp = n == 0 ? c : n == 1 ? (c & 0x1F) : n == 2 ? (c & 0x0F) : (c & 0x07);
    for (int k = 1; k <= n; k++) {
        if (i + k >= s.size() || (b(i + k) & 0xC0) != 0x80) { i++; return 0xFFFD; }
        cp = cp << 6 | (b(i + k) & 0x3F);
    }
    i += static_cast<size_t>(n) + 1;
    return cp;
}

Fonts::Fonts() = default;
Fonts::~Fonts() = default;

bool Fonts::load(std::string* error) {
    for (int f = 0; f < kFaces; f++) {
        const FontFile ff = find_font(static_cast<Face>(f));
        const std::string& path = ff.path;
        if (path.empty() && ff.data.empty()) continue;
        auto face = std::make_unique<Face_>();
        if (!ff.data.empty()) {
            face->file.assign(ff.data.begin(), ff.data.end());
        } else {
            std::ifstream in(path, std::ios::binary);
            face->file.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        const int offset = face->file.empty() ? -1 : stbtt_GetFontOffsetForIndex(face->file.data(), 0);
        if (offset < 0 || !stbtt_InitFont(&face->info, face->file.data(), offset)) {
            std::fprintf(stderr, "openxr-sim: %s is not a font stb_truetype reads\n", path.c_str());
            continue;
        }
        stbtt_GetFontVMetrics(&face->info, &face->ascent, &face->descent, &face->gap);
        face->embolden = ff.embolden;
        faces_[f] = std::move(face);
    }
    if (faces_[0] == nullptr) {
        *error = "no font: " + font_search_description();
        return false;
    }
    return true;
}

// A face that is missing is the regular one.
Fonts::Face_* Fonts::face(Face f) {
    Face_* p = faces_[static_cast<int>(f)].get();
    return p != nullptr ? p : faces_[0].get();
}

float Fonts::line_height(Face f, float size) {
    Face_* p = face(f);
    if (p == nullptr) return size;
    const float scale = stbtt_ScaleForMappingEmToPixels(&p->info, size);
    return std::ceil((p->ascent - p->descent + p->gap) * scale);
}

float Fonts::measure(std::string_view text, Face f, float size) {
    Face_* p = face(f);
    if (p == nullptr) return 0;
    const float scale = stbtt_ScaleForMappingEmToPixels(&p->info, size);
    int units = 0;
    for (size_t i = 0; i < text.size();) {
        const uint32_t cp = next_codepoint(text, i);
        auto it = p->advance.find(cp);
        if (it == p->advance.end()) {
            int adv = 0, lsb = 0;
            stbtt_GetCodepointHMetrics(&p->info, static_cast<int>(cp), &adv, &lsb);
            it = p->advance.emplace(cp, adv).first;
        }
        units += it->second;
    }
    return units * scale;
}

std::string Fonts::elide(std::string_view text, Face f, float size, float max_w) {
    if (measure(text, f, size) <= max_w) return std::string(text);
    const std::string_view dots = "\xE2\x80\xA6";   // …
    const float room = max_w - measure(dots, f, size);
    size_t keep = 0;
    for (size_t i = 0; i < text.size();) {
        size_t j = i;
        next_codepoint(text, j);
        if (measure(text.substr(0, j), f, size) > room) break;
        keep = i = j;
    }
    return std::string(text.substr(0, keep)) + std::string(dots);
}

// HEAVIER BY SMEARING TO THE RIGHT: each pixel the most of itself and the d pixels to its left (the last in
// part), d a twentieth of the size -- about what a semibold adds to a regular's stems.
void Fonts::embolden(Glyph& g, float size) {
    const float d = std::max(0.6f, size / 20);
    const int n = static_cast<int>(d);
    const float frac = d - n;
    const int w = g.w + n + 1;
    std::vector<uint8_t> out(static_cast<size_t>(w) * g.h, 0);
    for (int y = 0; y < g.h; y++)
        for (int x = 0; x < w; x++) {
            float v = 0;
            for (int k = 0; k <= n + 1; k++) {
                const int sx = x - k;
                if (sx < 0 || sx >= g.w) continue;
                const float c = g.cov[static_cast<size_t>(y) * g.w + sx] * (k == n + 1 ? frac : 1.0f);
                v = std::max(v, c);
            }
            out[static_cast<size_t>(y) * w + x] = static_cast<uint8_t>(v);
        }
    g.cov.swap(out);
    g.w = w;
}

const Fonts::Glyph& Fonts::glyph(Face_& p, float size, uint32_t cp, float subpixel) {
    const uint64_t key = static_cast<uint64_t>(std::lround(size * 64)) << 40 | static_cast<uint64_t>(std::lround(subpixel * 4)) << 32 | cp;
    auto it = p.glyphs.find(key);
    if (it != p.glyphs.end()) return it->second;
    const float scale = stbtt_ScaleForMappingEmToPixels(&p.info, size);
    Glyph g{};
    int x1 = 0, y1 = 0;
    stbtt_GetCodepointBitmapBoxSubpixel(&p.info, static_cast<int>(cp), scale, scale, subpixel, 0, &g.x0, &g.y0, &x1, &y1);
    g.w = x1 - g.x0;
    g.h = y1 - g.y0;
    if (g.w > 0 && g.h > 0) {
        g.cov.resize(static_cast<size_t>(g.w) * g.h);
        stbtt_MakeCodepointBitmapSubpixel(&p.info, g.cov.data(), g.w, g.h, g.w, scale, scale, subpixel, 0, static_cast<int>(cp));
        if (p.embolden) embolden(g, size);
    }
    return p.glyphs.emplace(key, std::move(g)).first->second;
}

void Fonts::draw(Canvas& canvas, std::string_view text, Face f, float size, float x, float y, Rgba colour) {
    Face_* p = face(f);
    if (p == nullptr) return;
    const float scale = stbtt_ScaleForMappingEmToPixels(&p->info, size);
    // The baseline on a whole pixel, so every line's text is equally sharp.
    const float baseline = std::round(y + p->ascent * scale);
    float pen = x;
    for (size_t i = 0; i < text.size();) {
        const uint32_t cp = next_codepoint(text, i);
        const float base = std::floor(pen);
        const float sub = std::round((pen - base) * 4) / 4;
        const Glyph& g = glyph(*p, size, cp, sub);
        if (!g.cov.empty()) canvas.mask(static_cast<int>(base) + g.x0, static_cast<int>(baseline) + g.y0, g.cov.data(), g.w, g.h, g.w, colour);
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&p->info, static_cast<int>(cp), &adv, &lsb);
        pen += adv * scale;
    }
}

}  // namespace oxs
