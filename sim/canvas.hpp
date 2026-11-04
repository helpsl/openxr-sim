// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
#pragma once

// THE WINDOW'S PIXELS, drawn in software: what Clay's render commands become (sim/ui.cpp), and then either a
// texture on the GLFW window (sim/window.cpp) or a PNG (headless, and the control socket's `shot`). In
// software because the window is a few rounded rectangles, some text and two images, and so the same
// pixels come out with a GPU, without one, on every platform.
//
// PREMULTIPLIED, in memory B G R A -- the order the eyes already come in (sim/link.hpp, EyePixels) -- so an
// eye is copied, not converted.

#include <cstdint>
#include <string>
#include <vector>

namespace oxs {

struct Rgba { float r = 0, g = 0, b = 0, a = 255; };   // straight alpha, 0..255 each (Clay's Clay_Color)

struct RectF { float x = 0, y = 0, w = 0, h = 0; };
struct Radii { float tl = 0, tr = 0, bl = 0, br = 0; };

// One eye, or any image: premultiplied B G R A, rows top to bottom.
struct ImageView {
    const uint8_t* bgra = nullptr;
    int width = 0, height = 0, stride = 0;
};

class Canvas {
public:
    void resize(int w, int h);
    int width() const { return w_; }
    int height() const { return h_; }
    const uint8_t* data() const { return px_.data(); }
    void clear(Rgba c);

    // Drawing is limited to the clip rectangle (the whole canvas when none is set). Coordinates are pixels.
    void set_clip(const RectF& r);
    void reset_clip();

    void fill(const RectF& r, const Radii& radii, Rgba c);
    // A border of `width` pixels on each side (left, right, top, bottom) inside `r`, following its corners.
    void border(const RectF& r, const Radii& radii, const float width[4], Rgba c);
    // `img` scaled into `r`, bilinear.
    void image(const RectF& r, const ImageView& img);
    // An 8-bit coverage mask (a glyph) at x, y in colour `c`.
    void mask(int x, int y, const uint8_t* cov, int w, int h, int stride, Rgba c);

    bool write_png(const std::string& path, std::string* error) const;
    // PNG bytes of the whole canvas, or of `img` alone.
    static std::string encode_png(const ImageView& img);

private:
    void blend(int x, int y, float cov, Rgba c);
    int w_ = 0, h_ = 0;
    std::vector<uint8_t> px_;
    int cx0_ = 0, cy0_ = 0, cx1_ = 0, cy1_ = 0;   // the clip, [x0, x1) x [y0, y1)
};

}  // namespace oxs
