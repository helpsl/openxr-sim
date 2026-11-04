// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
//
// COMPOSITION LAYERS, composited into each eye by the simulator (DESIGN.md, "Composition layers"). A
// frame of one plain projection layer never comes here -- its eyes are shown as they are. Anything else --
// a quad, a cylinder, an equirect, a second projection, blending, a colour scale -- is drawn here, in the
// order submitted, at the size the window shows the eye, seen through the first projection layer's views,
// from the images the runtime put in the frame's slot. In the simulator's process, at its size: a slow
// frame here slows the window, never the application (measured, NOTES.md).
//
// THE RULES are the specification's (10.6.2, "Composition Layer Blending"), in linear light: the
// composite starts at zero; a layer's alpha is one unless BLEND_TEXTURE_SOURCE_ALPHA is set; an
// UNPREMULTIPLIED layer's colour is multiplied by its alpha; then CompositeColor = LayerColor +
// CompositeColor * (1 - LayerColor.A). A colour scale and bias (XR_KHR_composition_layer_color_scale_bias)
// applies to the unpremultiplied colour, which is then premultiplied again.
//
// EACH PIXEL IS A RAY from the eye, through the pixel's centre, into the layer's own frame: a plane for
// a quad (front face only), the inside of a cylinder, a sphere for an equirect (or, with no radius, only
// the ray's direction). Only the pixels a layer can cover are visited -- the rectangle its outline
// projects to -- unless some of the outline is behind the eye, when every pixel is.
#include "composite.h"
#include "xrmath.h"

#include <math.h>
#include "threads.h"
#include <stdlib.h>
#include <string.h>

// ---- light ---------------------------------------------------------------------------------------

static float linear_of_srgb8[256];
static uint8_t srgb8_of_linear12[4096];   // indexed by linear light in 1/4095 steps

static void tables_init(void)
{
    for (int i = 0; i < 256; i++) {
        const double s = i / 255.0;
        linear_of_srgb8[i] = (float)(s <= 0.04045 ? s / 12.92 : pow((s + 0.055) / 1.055, 2.4));
    }
    for (int i = 0; i < 4096; i++) {
        const double l = i / 4095.0;
        const double s = l <= 0.0031308 ? l * 12.92 : 1.055 * pow(l, 1 / 2.4) - 0.055;
        srgb8_of_linear12[i] = (uint8_t)lround(s * 255);
    }
}

static uint8_t encode(float l)
{
    if (!(l > 0)) return 0;
    if (l >= 1) return 255;
    return srgb8_of_linear12[(int)(l * 4095 + 0.5f)];
}

// One texel as linear light and alpha.
static void texel(const struct oxs_eye *im, uint32_t x, uint32_t y, float out[4])
{
    if (im->flipped) y = im->height - 1 - y;
    const uint8_t *p = im->pixels + (size_t)y * im->stride + (size_t)x * 4;
    const uint8_t r = im->bgra ? p[2] : p[0], g = p[1], b = im->bgra ? p[0] : p[2];
    if (im->linear) { out[0] = r / 255.0f; out[1] = g / 255.0f; out[2] = b / 255.0f; }
    else { out[0] = linear_of_srgb8[r]; out[1] = linear_of_srgb8[g]; out[2] = linear_of_srgb8[b]; }
    out[3] = p[3] / 255.0f;
}

// Bilinear, at (u, v) across the sub-image with (0, 0) its top left; edges clamped.
static void sample(const struct oxs_eye *im, float u, float v, float out[4])
{
    float x = u * (float)im->width - 0.5f, y = v * (float)im->height - 0.5f;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    const float maxx = (float)im->width - 1, maxy = (float)im->height - 1;
    if (x > maxx) x = maxx;
    if (y > maxy) y = maxy;
    const uint32_t x0 = (uint32_t)x, y0 = (uint32_t)y;
    const uint32_t x1 = x0 + 1 < im->width ? x0 + 1 : x0, y1 = y0 + 1 < im->height ? y0 + 1 : y0;
    const float fx = x - (float)x0, fy = y - (float)y0;
    float a[4], b[4], c[4], d[4];
    texel(im, x0, y0, a);
    texel(im, x1, y0, b);
    texel(im, x0, y1, c);
    texel(im, x1, y1, d);
    for (int i = 0; i < 4; i++) {
        const float top = a[i] + (b[i] - a[i]) * fx, bottom = c[i] + (d[i] - c[i]) * fx;
        out[i] = top + (bottom - top) * fy;
    }
}

static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

// The layer's colour as the blend wants it: premultiplied, every rule above applied.
static void layer_colour(const struct oxs_layer *l, float c[4])
{
    if (!l->source_alpha) c[3] = 1;
    if (l->unpremultiplied)
        for (int i = 0; i < 3; i++) c[i] *= c[3];
    for (int i = 0; i < 4; i++) c[i] = clamp01(c[i]);
    if (l->scale_bias) {
        const float a = c[3];
        for (int i = 0; i < 3; i++) c[i] = a > 0 ? c[i] / a : 0;
        for (int i = 0; i < 4; i++) c[i] = c[i] * (l->scale[i] > 0 ? l->scale[i] : 0) + l->bias[i];
        for (int i = 0; i < 4; i++) c[i] = clamp01(c[i]);
        for (int i = 0; i < 3; i++) c[i] *= c[3];
    }
}

// Into a B G R A pixel.
static void blend(uint8_t *dst, const float c[4])
{
    const float keep = 1 - c[3];
    for (int i = 0; i < 3; i++) dst[2 - i] = encode(c[i] + linear_of_srgb8[dst[2 - i]] * keep);
    dst[3] = 255;
}

// ---- geometry ------------------------------------------------------------------------------------

// Where a point in STAGE lands in the camera's picture, in pixels; false when it is not ahead of the eye.
static bool project(const struct oxs_camera *cam, const float p[3], float *x, float *y)
{
    const struct oxs_pose inv = oxs_pose_invert(&cam->pose);
    float q[3];
    oxs_quat_rotate(inv.q, p, q);
    for (int i = 0; i < 3; i++) q[i] += inv.p[i];
    if (q[2] > -1e-4f) return false;
    const float tx = q[0] / -q[2], ty = q[1] / -q[2];
    *x = (tx - cam->tan_left) / (cam->tan_right - cam->tan_left) * (float)cam->width;
    *y = (ty - cam->tan_up) / (cam->tan_down - cam->tan_up) * (float)cam->height;
    return true;
}

// The pixels a layer can cover: the rectangle round its outline's points (in the layer's frame), a pixel
// of margin for the curve between them. False -- visit everything -- when a point is behind the eye.
struct rect { int x0, y0, x1, y1; };

static bool outline_rect(const struct oxs_camera *cam, const struct oxs_pose *layer, const float (*pts)[3], int n,
                         struct rect *r)
{
    float minx = 1e30f, miny = 1e30f, maxx = -1e30f, maxy = -1e30f;
    for (int i = 0; i < n; i++) {
        float w[3], x, y;
        oxs_quat_rotate(layer->q, pts[i], w);
        for (int k = 0; k < 3; k++) w[k] += layer->p[k];
        if (!project(cam, w, &x, &y)) return false;
        if (x < minx) minx = x;
        if (x > maxx) maxx = x;
        if (y < miny) miny = y;
        if (y > maxy) maxy = y;
    }
    r->x0 = (int)floorf(minx) - 2;
    r->y0 = (int)floorf(miny) - 2;
    r->x1 = (int)ceilf(maxx) + 2;
    r->y1 = (int)ceilf(maxy) + 2;
    return true;
}

// A radius of zero or infinity is infinitely far: only the ray's direction counts.
static bool far_away(float radius) { return !(radius > 0) || isinf(radius); }

// The outline of each kind of layer, as points in its own frame.
#define ARC 16
static int quad_outline(const struct oxs_layer *l, float (*pts)[3])
{
    const float w = l->size[0] / 2, h = l->size[1] / 2;
    const float c[4][3] = { { -w, h, 0 }, { w, h, 0 }, { -w, -h, 0 }, { w, -h, 0 } };
    memcpy(pts, c, sizeof c);
    return 4;
}

static int cylinder_outline(const struct oxs_layer *l, float (*pts)[3])
{
    const float r = far_away(l->radius) ? 1000 : l->radius, h = r * l->angle / l->aspect;
    int n = 0;
    for (int i = 0; i <= ARC; i++) {
        const float a = l->angle * ((float)i / ARC - 0.5f);
        for (int s = -1; s <= 1; s += 2) {
            pts[n][0] = sinf(a) * r;
            pts[n][1] = s * h / 2;
            pts[n][2] = -cosf(a) * r;
            n++;
        }
    }
    return n;
}

static int equirect_outline(const struct oxs_layer *l, float (*pts)[3])
{
    const float r = far_away(l->radius) ? 1000 : l->radius;
    int n = 0;
    for (int i = 0; i <= ARC; i++)
        for (int j = 0; j <= 4; j++) {
            const float yaw = l->angle * ((float)i / ARC - 0.5f);
            const float pitch = l->lower + (l->upper - l->lower) * (float)j / 4;
            pts[n][0] = sinf(yaw) * cosf(pitch) * r;
            pts[n][1] = sinf(pitch) * r;
            pts[n][2] = -cosf(yaw) * cosf(pitch) * r;
            n++;
        }
    return n;
}

// Where a ray (origin o, direction d, in the layer's frame) meets the layer, as (u, v) on its image.
static bool hit_quad(const struct oxs_layer *l, const float o[3], const float d[3], float *u, float *v)
{
    if (d[2] >= 0) return false;   // the eye is in front (o[2] > 0, checked once); the ray must go back
    const float t = -o[2] / d[2];
    const float x = o[0] + t * d[0], y = o[1] + t * d[1];
    *u = x / l->size[0] + 0.5f;
    *v = 0.5f - y / l->size[1];
    return *u >= 0 && *u <= 1 && *v >= 0 && *v <= 1;
}

// The far side of the cylinder: its inside is what is drawn.
static bool hit_cylinder(const struct oxs_layer *l, const float o[3], const float d[3], float *u, float *v)
{
    const bool inf = far_away(l->radius);
    const float r = inf ? 1 : l->radius;
    const float ox = inf ? 0 : o[0], oy = inf ? 0 : o[1], oz = inf ? 0 : o[2];
    const float a = d[0] * d[0] + d[2] * d[2];
    if (a < 1e-12f) return false;
    const float b = 2 * (ox * d[0] + oz * d[2]), c = ox * ox + oz * oz - r * r;
    const float disc = b * b - 4 * a * c;
    if (disc < 0) return false;
    const float t = (-b + sqrtf(disc)) / (2 * a);
    if (t <= 0) return false;
    const float px = ox + t * d[0], py = oy + t * d[1], pz = oz + t * d[2];
    const float theta = atan2f(px, -pz), h = r * l->angle / l->aspect;
    *u = theta / l->angle + 0.5f;
    *v = 0.5f - py / h;
    return *u >= 0 && *u <= 1 && *v >= 0 && *v <= 1;
}

static bool hit_equirect(const struct oxs_layer *l, const float o[3], const float d[3], float *u, float *v)
{
    float p[3] = { d[0], d[1], d[2] };
    if (!far_away(l->radius)) {
        const float dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
        const float b = o[0] * d[0] + o[1] * d[1] + o[2] * d[2];
        const float c = o[0] * o[0] + o[1] * o[1] + o[2] * o[2] - l->radius * l->radius;
        const float disc = b * b - dd * c;
        if (disc < 0) return false;
        const float t = (-b + sqrtf(disc)) / dd;
        if (t <= 0) return false;
        for (int i = 0; i < 3; i++) p[i] = o[i] + t * d[i];
    }
    const float n = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
    if (n <= 0) return false;
    const float sy = p[1] / n, yaw = atan2f(p[0], -p[2]), pitch = asinf(sy > 1 ? 1 : sy < -1 ? -1 : sy);
    if (pitch > l->upper || pitch < l->lower || l->upper <= l->lower) return false;
    *u = l->angle >= 2 * (float)M_PI ? yaw / (2 * (float)M_PI) + 0.5f : yaw / l->angle + 0.5f;
    *v = (l->upper - pitch) / (l->upper - l->lower);
    return *u >= 0 && *u <= 1;
}

// Another projection layer's view: the ray's direction projected into it (its own eye position is not
// reprojected -- the views of one frame differ by the head's motion within it, which is none here).
static bool hit_view(const struct oxs_layer *l, const float o[3], const float d[3], float *u, float *v)
{
    (void)o;
    if (d[2] >= -1e-6f) return false;
    const float tx = d[0] / -d[2], ty = d[1] / -d[2];
    *u = (tx - l->tan_left) / (l->tan_right - l->tan_left);
    *v = (ty - l->tan_up) / (l->tan_down - l->tan_up);
    return *u >= 0 && *u <= 1 && *v >= 0 && *v <= 1;
}

// ---- the eye -------------------------------------------------------------------------------------

void composite_clear(uint8_t *canvas, const struct oxs_camera *cam)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, tables_init);
    for (size_t i = 0; i < (size_t)cam->width * cam->height; i++) {
        canvas[i * 4] = canvas[i * 4 + 1] = canvas[i * 4 + 2] = 0;
        canvas[i * 4 + 3] = 255;
    }
}

void composite_layer(uint8_t *canvas, const struct oxs_camera *cam, const struct oxs_layer *l)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, tables_init);
    if (l->image.pixels == NULL || l->image.width == 0 || l->image.height == 0) return;
    if (cam->width == 0 || cam->height == 0 || cam->width > 4096 || cam->height > 4096) return;
    // The eye in the layer's frame, and the rotation that takes a camera ray there.
    const struct oxs_pose eye = oxs_pose_relative(&l->pose, &cam->pose);
    if (l->kind == OXS_QUAD && eye.p[2] <= 0) return;   // behind the quad: its back face is not drawn

    struct rect r = { 0, 0, (int)cam->width, (int)cam->height };
    float pts[(ARC + 1) * 5][3];
    int n = 0;
    switch (l->kind) {
        case OXS_QUAD: n = quad_outline(l, pts); break;
        case OXS_CYLINDER: n = cylinder_outline(l, pts); break;
        case OXS_EQUIRECT: n = equirect_outline(l, pts); break;
        default: break;
    }
    // A layer that surrounds the eye -- a cylinder or sphere it is inside, wide enough -- has outline
    // points behind it, and so covers anything: every pixel then.
    if (n > 0 && !outline_rect(cam, &l->pose, (const float (*)[3])pts, n, &r)) r = (struct rect){ 0, 0, (int)cam->width, (int)cam->height };
    if (r.x0 < 0) r.x0 = 0;
    if (r.y0 < 0) r.y0 = 0;
    if (r.x1 > (int)cam->width) r.x1 = (int)cam->width;
    if (r.y1 > (int)cam->height) r.y1 = (int)cam->height;
    if (r.x0 >= r.x1 || r.y0 >= r.y1) return;

    // A camera ray through a pixel's centre is (tx, ty, -1): tx by column and ty by row, worked out once.
    // Taken to the layer's frame it is az + tx * ax + ty * ay, the camera's axes rotated there.
    float tx[4096], ty[4096];
    for (int x = r.x0; x < r.x1; x++) tx[x] = cam->tan_left + (cam->tan_right - cam->tan_left) * ((float)x + 0.5f) / (float)cam->width;
    for (int y = r.y0; y < r.y1; y++) ty[y] = cam->tan_up + (cam->tan_down - cam->tan_up) * ((float)y + 0.5f) / (float)cam->height;
    float ax[3], ay[3], az[3];
    const float ex[3] = { 1, 0, 0 }, ey[3] = { 0, 1, 0 }, ez[3] = { 0, 0, -1 };
    oxs_quat_rotate(eye.q, ex, ax);
    oxs_quat_rotate(eye.q, ey, ay);
    oxs_quat_rotate(eye.q, ez, az);
    for (int y = r.y0; y < r.y1; y++) {
        uint8_t *row = canvas + (size_t)y * cam->width * 4;
        float base[3];
        for (int i = 0; i < 3; i++) base[i] = az[i] + ty[y] * ay[i];
        for (int x = r.x0; x < r.x1; x++) {
            float d[3];
            for (int i = 0; i < 3; i++) d[i] = base[i] + tx[x] * ax[i];
            float u, v;
            bool hit = false;
            switch (l->kind) {
                case OXS_QUAD: hit = hit_quad(l, eye.p, d, &u, &v); break;
                case OXS_CYLINDER: hit = hit_cylinder(l, eye.p, d, &u, &v); break;
                case OXS_EQUIRECT: hit = hit_equirect(l, eye.p, d, &u, &v); break;
                case OXS_PROJECTION: hit = hit_view(l, eye.p, d, &u, &v); break;
                default: break;
            }
            if (!hit) continue;
            float c[4];
            sample(&l->image, u, v, c);
            layer_colour(l, c);
            blend(row + (size_t)x * 4, c);
        }
    }
}
