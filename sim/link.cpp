// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
#include "link.hpp"

#include "composite.h"

#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>

namespace oxs {

namespace {

// LINEAR LIGHT TO sRGB, for an application that drew into a UNORM image: its values are light, and
// shown as they are every mid-tone would be too dark.
const std::array<uint8_t, 256>& srgb_of_linear() {
    static const std::array<uint8_t, 256> table = [] {
        std::array<uint8_t, 256> t{};
        for (int i = 0; i < 256; i++) {
            const double l = i / 255.0;
            const double s = l <= 0.0031308 ? l * 12.92 : 1.055 * std::pow(l, 1 / 2.4) - 0.055;
            t[static_cast<size_t>(i)] = static_cast<uint8_t>(std::lround(std::clamp(s, 0.0, 1.0) * 255));
        }
        return t;
    }();
    return table;
}

}  // namespace

// TWO BY TWO SAMPLES per pixel shown, spread over the source pixels it covers: smooth enough to read an
// application's text at half size, and four reads a pixel whatever the source -- a full average of a
// 2048-pixel eye at every frame would cost more than the application's own copy.
void convert_eye(const uint8_t* src, const oxs_shm_eye& eye, int max_w, int max_h, EyePixels& out) {
    const int sw = static_cast<int>(eye.width), sh = static_cast<int>(eye.height);
    out.source_w = sw;
    out.source_h = sh;
    if (sw <= 0 || sh <= 0 || max_w <= 0 || max_h <= 0) { out.width = out.height = 0; out.argb.clear(); return; }
    const double fit = std::min({ 1.0, static_cast<double>(max_w) / sw, static_cast<double>(max_h) / sh });
    const int tw = std::max(1, static_cast<int>(std::lround(sw * fit)));
    const int th = std::max(1, static_cast<int>(std::lround(sh * fit)));
    out.width = tw;
    out.height = th;
    out.argb.resize(static_cast<size_t>(tw) * th * 4);
    const bool bgra = (eye.flags & OXS_EYE_BGRA) != 0;
    const bool flipped = (eye.flags & OXS_EYE_FLIPPED) != 0;
    const bool linear = (eye.flags & OXS_EYE_LINEAR) != 0;
    const auto& lut = srgb_of_linear();
    // Source column of each sample, worked out once per row's worth.
    std::vector<int> cols(static_cast<size_t>(tw) * 2);
    for (int x = 0; x < tw; x++)
        for (int k = 0; k < 2; k++)
            cols[static_cast<size_t>(x) * 2 + k] = std::min(sw - 1, static_cast<int>((x * 2 + k + 0.5) * sw / (2.0 * tw)));
    for (int y = 0; y < th; y++) {
        const uint8_t* rows[2];
        for (int k = 0; k < 2; k++) {
            int sy = std::min(sh - 1, static_cast<int>((y * 2 + k + 0.5) * sh / (2.0 * th)));
            if (flipped) sy = sh - 1 - sy;
            rows[k] = src + static_cast<size_t>(sy) * eye.stride;
        }
        uint8_t* o = out.argb.data() + static_cast<size_t>(y) * tw * 4;
        for (int x = 0; x < tw; x++, o += 4) {
            unsigned c0 = 0, c1 = 0, c2 = 0;
            for (int r = 0; r < 2; r++)
                for (int k = 0; k < 2; k++) {
                    const uint8_t* p = rows[r] + static_cast<size_t>(cols[static_cast<size_t>(x) * 2 + k]) * 4;
                    c0 += p[0]; c1 += p[1]; c2 += p[2];
                }
            uint8_t a = static_cast<uint8_t>((c0 + 2) / 4), b = static_cast<uint8_t>((c1 + 2) / 4),
                    c = static_cast<uint8_t>((c2 + 2) / 4);
            if (linear) { a = lut[a]; b = lut[b]; c = lut[c]; }
            // ARGB32 in memory is B G R A. Opaque: an opaque blend mode ignores what the application left in alpha.
            o[0] = bgra ? a : c;
            o[1] = b;
            o[2] = bgra ? c : a;
            o[3] = 255;
        }
    }
}

namespace {

// Over nothing, an opaque or premultiplied layer is its own colour (composite.c).
bool plain(const oxs_layer& l) { return !(l.source_alpha && l.unpremultiplied) && !l.scale_bias; }

}  // namespace

// A COMPOSITED EYE (sim_protocol.h): at the size the window shows it -- the application's eye fitted as
// convert_eye fits it -- the layers drawn in order. The eye the frame is seen through, when it comes first
// and plain, is converted exactly as an eye alone is; everything after it is drawn over it.
void Link::compose_eye(const oxs_shm_slot& s, uint32_t slot, uint32_t e, const oxs_shm_eye& eye, int max_w, int max_h,
                       EyePixels& out) {
    oxs_camera cam = s.camera[e];
    const int sw = static_cast<int>(cam.width), sh = static_cast<int>(cam.height);
    out.source_w = sw;
    out.source_h = sh;
    if (sw <= 0 || sh <= 0 || max_w <= 0 || max_h <= 0) { out.width = out.height = 0; out.argb.clear(); return; }
    const double fit = std::min({ 1.0, static_cast<double>(max_w) / sw, static_cast<double>(max_h) / sh });
    cam.width = static_cast<uint32_t>(std::max(1L, std::lround(sw * fit)));
    cam.height = static_cast<uint32_t>(std::max(1L, std::lround(sh * fit)));
    const uint8_t* eye_px = oxs_shm_pixels(shm_, slot, e);
    const uint8_t* arena = oxs_shm_arena(shm_, slot);
    const uint32_t n = std::min<uint32_t>(s.nlayers[e], OXS_LAYERS_MAX);
    uint32_t i = 0;
    const oxs_shm_layer& first = s.layer[e][0];
    if (first.offset == OXS_OFFSET_EYE && plain(first.layer) && static_cast<int>(eye.width) == sw &&
        static_cast<int>(eye.height) == sh) {
        convert_eye(eye_px, eye, static_cast<int>(cam.width), static_cast<int>(cam.height), out);
        i = 1;
    } else {
        out.width = static_cast<int>(cam.width);
        out.height = static_cast<int>(cam.height);
        out.argb.resize(static_cast<size_t>(cam.width) * cam.height * 4);
        composite_clear(out.argb.data(), &cam);
    }
    for (; i < n; i++) {
        oxs_layer l = s.layer[e][i].layer;
        const uint64_t offset = s.layer[e][i].offset;
        if (offset == OXS_OFFSET_EYE) {
            l.image = oxs_eye{ eye_px, eye.width, eye.height, eye.width * 4, (eye.flags & OXS_EYE_BGRA) != 0,
                                  (eye.flags & OXS_EYE_LINEAR) != 0, (eye.flags & OXS_EYE_FLIPPED) != 0 };
        } else {
            // What the runtime wrote, checked against the arena it had: nothing is read from outside it.
            if (l.image.width == 0 || l.image.height == 0 || l.image.width > OXS_EYE_MAX || l.image.height > OXS_EYE_MAX ||
                offset > OXS_ARENA || static_cast<uint64_t>(l.image.width) * l.image.height * 4 > OXS_ARENA - offset)
                continue;
            l.image.pixels = arena + offset;
            l.image.stride = l.image.width * 4;
        }
        composite_layer(out.argb.data(), &cam, &l);
    }
}

Link::~Link() {
    if (shm_ != nullptr) munmap(shm_, size_);
    if (sock_ >= 0) close(sock_);
}

bool Link::attach(int shm_fd, int socket_fd, std::string* error) {
    struct stat st{};
    if (fstat(shm_fd, &st) != 0) { *error = "no shared memory on descriptor 3 -- the OpenXR runtime starts this with --headset"; return false; }
    if (!S_ISREG(st.st_mode) || static_cast<uint64_t>(st.st_size) < oxs_shm_size()) {
        *error = "descriptor 3 is not the OpenXR runtime's shared memory -- the runtime starts this with --headset";
        return false;
    }
    void* m = mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (m == MAP_FAILED) { *error = std::string("could not map the shared memory: ") + strerror(errno); return false; }
    auto* shm = static_cast<oxs_shm*>(m);
    if (shm->magic != OXS_SHM_MAGIC || shm->version != OXS_SHM_VERSION || shm->slots != OXS_SLOTS ||
        shm->eye_max != OXS_EYE_MAX || shm->size > static_cast<uint64_t>(st.st_size)) {
        munmap(m, static_cast<size_t>(st.st_size));
        *error = "the shared memory is not this version's (openxr-sim and openxr-sim must come from one package)";
        return false;
    }
    close(shm_fd);
    shm_ = shm;
    size_ = static_cast<size_t>(st.st_size);
    sock_ = socket_fd;
    return true;
}

bool Link::drain() {
    if (sock_ < 0) return false;
    char buf[64];
    for (;;) {
        const ssize_t r = recv(sock_, buf, sizeof buf, MSG_DONTWAIT);
        if (r > 0) continue;
        if (r == 0) return false;
        return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
    }
}

bool Link::take_frame(int max_w, int max_h, EyePixels out[2]) {
    if (shm_ == nullptr) return false;
    // Claim the published slot, then make sure it was still the published one when claimed: the runtime
    // never writes the slot named in `reading` (sim_protocol.h).
    uint32_t ready = 0;
    for (;;) {
        ready = oxs_load(&shm_->ready);
        if (ready == 0 || ready > OXS_SLOTS) return false;
        oxs_store(&shm_->reading, ready);
        if (oxs_load(&shm_->ready) == ready) break;
    }
    const uint32_t slot = ready - 1;
    const oxs_shm_slot& s = shm_->slot[slot];
    bool fresh = s.number != last_number_;
    if (fresh) {
        for (uint32_t e = 0; e < 2; e++) {
            oxs_shm_eye eye = s.eye[e];
            eye.width = std::min(eye.width, OXS_EYE_MAX);
            eye.height = std::min(eye.height, OXS_EYE_MAX);
            eye.stride = eye.width * 4;   // the runtime packs each eye's rows (device_sim.c, sim_present)
            if (s.nlayers[e] > 0) compose_eye(s, slot, e, eye, max_w, max_h, out[e]);
            else convert_eye(oxs_shm_pixels(shm_, slot, e), eye, max_w, max_h, out[e]);
        }
        last_number_ = s.number;
        taken_++;
    }
    oxs_store(&shm_->reading, 0);
    return fresh;
}

bool Link::status(oxs_shm_status& out) const {
    if (shm_ == nullptr) return false;
    if (!oxs_seq_read(&shm_->status_seq, &out, &shm_->status, sizeof out)) return false;
    out.app[sizeof out.app - 1] = '\0';
    out.profile[sizeof out.profile - 1] = '\0';
    return true;
}

void Link::write_tracking(const oxs_tracking& t) {
    if (shm_ == nullptr) return;
    oxs_seq_write(&shm_->tracking_seq, &shm_->tracking, &t, sizeof t);
    oxs_store(&shm_->tracking_valid, 1);
}

}  // namespace oxs
