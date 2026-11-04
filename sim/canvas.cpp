// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
#include "canvas.hpp"

#include "stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace oxs {

void Canvas::resize(int w, int h) {
    w_ = std::max(0, w);
    h_ = std::max(0, h);
    px_.assign(static_cast<size_t>(w_) * h_ * 4, 0);
    reset_clip();
}

void Canvas::clear(Rgba c) {
    const float a = c.a / 255.0f;
    const uint8_t p[4] = { static_cast<uint8_t>(c.b * a + 0.5f), static_cast<uint8_t>(c.g * a + 0.5f),
                           static_cast<uint8_t>(c.r * a + 0.5f), static_cast<uint8_t>(c.a + 0.5f) };
    for (size_t i = 0; i < px_.size(); i += 4) std::memcpy(&px_[i], p, 4);
}

void Canvas::set_clip(const RectF& r) {
    cx0_ = std::clamp(static_cast<int>(std::floor(r.x)), 0, w_);
    cy0_ = std::clamp(static_cast<int>(std::floor(r.y)), 0, h_);
    cx1_ = std::clamp(static_cast<int>(std::ceil(r.x + r.w)), 0, w_);
    cy1_ = std::clamp(static_cast<int>(std::ceil(r.y + r.h)), 0, h_);
}

void Canvas::reset_clip() {
    cx0_ = cy0_ = 0;
    cx1_ = w_;
    cy1_ = h_;
}

// Source-over, premultiplied: the colour at `cov` coverage over what is there.
void Canvas::blend(int x, int y, float cov, Rgba c) {
    const float a = cov * c.a / 255.0f;
    if (a <= 0) return;
    uint8_t* p = &px_[(static_cast<size_t>(y) * w_ + x) * 4];
    const float k = 1 - a;
    p[0] = static_cast<uint8_t>(std::min(255.0f, c.b * a + p[0] * k + 0.5f));
    p[1] = static_cast<uint8_t>(std::min(255.0f, c.g * a + p[1] * k + 0.5f));
    p[2] = static_cast<uint8_t>(std::min(255.0f, c.r * a + p[2] * k + 0.5f));
    p[3] = static_cast<uint8_t>(std::min(255.0f, 255 * a + p[3] * k + 0.5f));
}

namespace {

// HOW MUCH OF THE PIXEL CENTRED AT px, py IS INSIDE the rounded rectangle: a signed distance to its edge,
// one pixel of it as the antialiasing.
float coverage(const RectF& r, const Radii& rad, float px, float py) {
    const float x0 = r.x, y0 = r.y, x1 = r.x + r.w, y1 = r.y + r.h;
    float d;
    // The corner whose quarter the point is in, if any.
    float rr = 0, cx = 0, cy = 0;
    if (px < x0 + rad.tl && py < y0 + rad.tl) { rr = rad.tl; cx = x0 + rr; cy = y0 + rr; }
    else if (px > x1 - rad.tr && py < y0 + rad.tr) { rr = rad.tr; cx = x1 - rr; cy = y0 + rr; }
    else if (px < x0 + rad.bl && py > y1 - rad.bl) { rr = rad.bl; cx = x0 + rr; cy = y1 - rr; }
    else if (px > x1 - rad.br && py > y1 - rad.br) { rr = rad.br; cx = x1 - rr; cy = y1 - rr; }
    if (rr > 0) d = std::hypot(px - cx, py - cy) - rr;
    else d = std::max({ x0 - px, px - x1, y0 - py, py - y1 });
    return std::clamp(0.5f - d, 0.0f, 1.0f);
}

Radii clamp_radii(const RectF& r, Radii rad) {
    const float m = std::max(0.0f, std::min(r.w, r.h) / 2);
    rad.tl = std::clamp(rad.tl, 0.0f, m);
    rad.tr = std::clamp(rad.tr, 0.0f, m);
    rad.bl = std::clamp(rad.bl, 0.0f, m);
    rad.br = std::clamp(rad.br, 0.0f, m);
    return rad;
}

}  // namespace

void Canvas::fill(const RectF& r0, const Radii& radii, Rgba c) {
    if (r0.w <= 0 || r0.h <= 0 || c.a <= 0) return;
    const Radii rad = clamp_radii(r0, radii);
    const int x0 = std::max(cx0_, static_cast<int>(std::floor(r0.x))), x1 = std::min(cx1_, static_cast<int>(std::ceil(r0.x + r0.w)));
    const int y0 = std::max(cy0_, static_cast<int>(std::floor(r0.y))), y1 = std::min(cy1_, static_cast<int>(std::ceil(r0.y + r0.h)));
    const bool round = rad.tl > 0 || rad.tr > 0 || rad.bl > 0 || rad.br > 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            const float cov = round || x == x0 || x == x1 - 1 || y == y0 || y == y1 - 1 ? coverage(r0, rad, x + 0.5f, y + 0.5f) : 1.0f;
            if (cov > 0) blend(x, y, cov, c);
        }
}

void Canvas::border(const RectF& r, const Radii& radii, const float width[4], Rgba c) {
    if (r.w <= 0 || r.h <= 0 || c.a <= 0) return;
    const Radii out = clamp_radii(r, radii);
    const RectF in{ r.x + width[0], r.y + width[2], r.w - width[0] - width[1], r.h - width[2] - width[3] };
    const Radii inner = clamp_radii(in, { std::max(0.0f, out.tl - std::max(width[0], width[2])), std::max(0.0f, out.tr - std::max(width[1], width[2])),
                                          std::max(0.0f, out.bl - std::max(width[0], width[3])), std::max(0.0f, out.br - std::max(width[1], width[3])) });
    const int x0 = std::max(cx0_, static_cast<int>(std::floor(r.x))), x1 = std::min(cx1_, static_cast<int>(std::ceil(r.x + r.w)));
    const int y0 = std::max(cy0_, static_cast<int>(std::floor(r.y))), y1 = std::min(cy1_, static_cast<int>(std::ceil(r.y + r.h)));
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            const float o = coverage(r, out, x + 0.5f, y + 0.5f);
            const float i = in.w > 0 && in.h > 0 ? coverage(in, inner, x + 0.5f, y + 0.5f) : 0.0f;
            if (o - i > 0) blend(x, y, o - i, c);
        }
}

void Canvas::image(const RectF& r, const ImageView& img) {
    if (img.bgra == nullptr || img.width <= 0 || img.height <= 0 || r.w <= 0 || r.h <= 0) return;
    const int x0 = std::max(cx0_, static_cast<int>(std::floor(r.x))), x1 = std::min(cx1_, static_cast<int>(std::ceil(r.x + r.w)));
    const int y0 = std::max(cy0_, static_cast<int>(std::floor(r.y))), y1 = std::min(cy1_, static_cast<int>(std::ceil(r.y + r.h)));
    const float sx = img.width / r.w, sy = img.height / r.h;
    for (int y = y0; y < y1; y++) {
        const float fy = std::clamp((y + 0.5f - r.y) * sy - 0.5f, 0.0f, static_cast<float>(img.height - 1));
        const int iy = static_cast<int>(fy);
        const int iy1 = std::min(iy + 1, img.height - 1);
        const float ty = fy - iy;
        const uint8_t* row0 = img.bgra + static_cast<size_t>(iy) * img.stride;
        const uint8_t* row1 = img.bgra + static_cast<size_t>(iy1) * img.stride;
        uint8_t* out = &px_[(static_cast<size_t>(y) * w_ + x0) * 4];
        for (int x = x0; x < x1; x++, out += 4) {
            const float fx = std::clamp((x + 0.5f - r.x) * sx - 0.5f, 0.0f, static_cast<float>(img.width - 1));
            const int ix = static_cast<int>(fx);
            const int ix1 = std::min(ix + 1, img.width - 1);
            const float tx = fx - ix;
            for (int k = 0; k < 4; k++) {
                const float top = row0[ix * 4 + k] * (1 - tx) + row0[ix1 * 4 + k] * tx;
                const float bot = row1[ix * 4 + k] * (1 - tx) + row1[ix1 * 4 + k] * tx;
                out[k] = static_cast<uint8_t>(top * (1 - ty) + bot * ty + 0.5f);
            }
        }
    }
}

void Canvas::mask(int x, int y, const uint8_t* cov, int w, int h, int stride, Rgba c) {
    for (int j = std::max(0, cy0_ - y); j < h && y + j < cy1_; j++)
        for (int i = std::max(0, cx0_ - x); i < w && x + i < cx1_; i++) {
            const uint8_t v = cov[j * stride + i];
            if (v != 0) blend(x + i, y + j, v / 255.0f, c);
        }
}

namespace {

// PNG wants straight RGBA; the canvas is premultiplied BGRA.
std::vector<uint8_t> to_rgba(const ImageView& img) {
    std::vector<uint8_t> out(static_cast<size_t>(img.width) * img.height * 4);
    for (int y = 0; y < img.height; y++) {
        const uint8_t* s = img.bgra + static_cast<size_t>(y) * img.stride;
        uint8_t* d = &out[static_cast<size_t>(y) * img.width * 4];
        for (int x = 0; x < img.width; x++, s += 4, d += 4) {
            const int a = s[3];
            auto un = [a](int v) { return static_cast<uint8_t>(a == 0 ? 0 : std::min(255, (v * 255 + a / 2) / a)); };
            d[0] = un(s[2]);
            d[1] = un(s[1]);
            d[2] = un(s[0]);
            d[3] = static_cast<uint8_t>(a);
        }
    }
    return out;
}

}  // namespace

std::string Canvas::encode_png(const ImageView& img) {
    if (img.bgra == nullptr || img.width <= 0 || img.height <= 0) return {};
    const std::vector<uint8_t> rgba = to_rgba(img);
    std::string png;
    stbi_write_png_to_func([](void* ctx, void* data, int size) { static_cast<std::string*>(ctx)->append(static_cast<const char*>(data), static_cast<size_t>(size)); },
                           &png, img.width, img.height, 4, rgba.data(), img.width * 4);
    return png;
}

bool Canvas::write_png(const std::string& path, std::string* error) const {
    const std::string png = encode_png({ px_.data(), w_, h_, w_ * 4 });
    if (png.empty()) { *error = "nothing to write"; return false; }
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) { *error = "could not write " + path; return false; }
    const bool ok = std::fwrite(png.data(), 1, png.size(), f) == png.size();
    if (std::fclose(f) != 0 || !ok) { *error = "could not write " + path; return false; }
    return true;
}

}  // namespace oxs
