// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
#pragma once

// TEXT: TrueType fonts rasterised by stb_truetype, glyph by glyph, cached by size. The fonts are the
// system's -- found by fontconfig on Linux, in %WINDIR%\Fonts on Windows (sim/os.hpp, find_font) -- so the
// window reads like the desktop it is on, and nothing is bundled.

#include "canvas.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace oxs {

enum class Face : uint16_t { Regular = 0, Bold = 1, Mono = 2 };
constexpr int kFaces = 3;

class Fonts {
public:
    Fonts();
    ~Fonts();
    Fonts(const Fonts&) = delete;
    Fonts& operator=(const Fonts&) = delete;

    // Loads the three faces. False, with why, when not even a regular face could be found.
    bool load(std::string* error);

    // Width of `text` at `size` pixels (fractional: the same at every scale), and the line height.
    float measure(std::string_view text, Face face, float size);
    float line_height(Face face, float size);
    // `text` cut to fit `max_w`, with an ellipsis, if it does not already.
    std::string elide(std::string_view text, Face face, float size, float max_w);

    // Draws `text` with its top-left at x, y (pixels) on `canvas`.
    void draw(Canvas& canvas, std::string_view text, Face face, float size, float x, float y, Rgba colour);

private:
    struct Glyph { int x0, y0, w, h; std::vector<uint8_t> cov; };
    struct Face_;
    Face_* face(Face f);
    const Glyph& glyph(Face_& f, float size, uint32_t cp, float subpixel);
    static void embolden(Glyph& g, float size);
    std::unique_ptr<Face_> faces_[kFaces];
};

// UTF-8 to code points, a malformed byte as U+FFFD.
uint32_t next_codepoint(std::string_view s, size_t& i);

}  // namespace oxs
