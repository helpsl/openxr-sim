// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
//
// THE SIMULATED HEADSET, as the runtime sees it: a child process, openxr-sim, started when the session is
// created and given a shared memory region and a wake channel (sim_protocol.h; starting it, plat.h). Frames
// go into the region, poses and buttons come out of it; the channel closing is the window closed. DESIGN.md,
// "Two parts, two processes", says why a child and not a service found by name.
#include "internal.h"
#include "sim_protocol.h"
#include "xrmath.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    struct oxs_shm *shm;
    bool gone;
} sim;

static const char *sim_path(void)
{
    const char *e = getenv("OPENXR_SIM_EXE");
    return e != NULL && e[0] != '\0' ? e : plat_default_sim_path();
}

// CONNECTED WHEN IT CAN BE SHOWN: the simulator installed. Anything else is a headset that is not there.
static int sim_get_system(int form_factor, uint64_t *id)
{
    if (form_factor != XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY) return XR_ERROR_FORM_FACTOR_UNSUPPORTED;
    if (!plat_is_executable(sim_path())) {
        say("the simulator (%s) is not there: no headset", sim_path());
        return XR_ERROR_FORM_FACTOR_UNAVAILABLE;
    }
    *id = OXS_SYSTEM_ID;
    return XR_SUCCESS;
}

static void sim_close(void)
{
    plat_sim_stop();
    sim.shm = NULL;
    sim.gone = false;
}

static void stamp(void *m)
{
    struct oxs_shm *shm = m;
    shm->magic = OXS_SHM_MAGIC;
    shm->version = OXS_SHM_VERSION;
    shm->eye_max = OXS_EYE_MAX;
    shm->slots = OXS_SLOTS;
    shm->pixels_offset = OXS_SHM_HEADER;
    shm->size = oxs_shm_size();
}

static bool sim_open(const char *app, char *why, size_t n)
{
    sim_close();
    _Static_assert(sizeof(struct oxs_shm) <= OXS_SHM_HEADER, "the header must fit before the pixels");
    void *m = NULL;
    if (!plat_sim_start(sim_path(), app, (size_t)oxs_shm_size(), stamp, &m, why, n)) return false;
    sim.shm = m;
    say("simulator started: %s", sim_path());
    return true;
}

// GONE when the channel has closed: the window was closed, or the simulator died.
static bool alive(void)
{
    if (sim.shm == NULL || sim.gone) return false;
    if (!plat_sim_alive()) {
        say("the simulator's window was closed");
        sim.gone = true;
        return false;
    }
    return true;
}

static bool sim_track(struct oxs_tracking *out)
{
    if (!alive()) return false;
    struct oxs_tracking t;
    if (oxs_load(&sim.shm->tracking_valid) && oxs_seq_read(&sim.shm->tracking_seq, &t, &sim.shm->tracking, sizeof t))
        *out = t;
    else
        oxs_tracking_home(out);
    return true;
}

// A sub-image into the arena, packed, every `step`th pixel each way so that neither side is over
// OXS_EYE_MAX; false when it does not fit what is left.
static bool put_image(const struct oxs_eye *im, uint8_t *arena, uint64_t *used, uint64_t *offset, struct oxs_eye *out)
{
    const uint32_t big = im->width > im->height ? im->width : im->height;
    const uint32_t step = (big + OXS_EYE_MAX - 1) / OXS_EYE_MAX;
    const uint32_t w = (im->width + step - 1) / step, h = (im->height + step - 1) / step;
    const uint64_t bytes = (uint64_t)w * h * 4;
    if (*used + bytes > OXS_ARENA) return false;
    uint8_t *dst = arena + *used;
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *src = im->pixels + (size_t)y * step * im->stride;
        if (step == 1) memcpy(dst + (size_t)y * w * 4, src, (size_t)w * 4);
        else
            for (uint32_t x = 0; x < w; x++) memcpy(dst + ((size_t)y * w + x) * 4, src + (size_t)x * step * 4, 4);
    }
    *out = *im;
    out->pixels = NULL;
    out->width = w;
    out->height = h;
    out->stride = w * 4;
    *offset = *used;
    *used += bytes;
    return true;
}

// THE LAYERS of a composited frame, and their sub-images in the slot's arena (sim_protocol.h).
static void put_layers(const struct oxs_frame *f, uint32_t slot, struct oxs_shm_slot *s)
{
    uint8_t *arena = oxs_shm_arena(sim.shm, slot);
    uint64_t used = 0;
    // What is in the arena already, by where it came from: a layer both eyes see is copied once.
    struct { const uint8_t *from; uint32_t width, height; uint64_t offset; struct oxs_eye image; } copied[2 * OXS_LAYERS_MAX];
    int ncopied = 0;
    static bool said;
    for (int e = 0; e < 2; e++) {
        s->nlayers[e] = 0;
        s->camera[e] = f->camera[e];
        for (uint32_t i = 0; i < f->nlayers[e] && i < OXS_LAYERS_MAX; i++) {
            const struct oxs_layer *l = &f->layer[e][i];
            struct oxs_shm_layer *out = &s->layer[e][s->nlayers[e]];
            out->layer = *l;
            out->layer.image.pixels = NULL;
            if (l->own_view) {
                out->offset = OXS_OFFSET_EYE;
            } else {
                int k = 0;
                while (k < ncopied && !(copied[k].from == l->image.pixels && copied[k].width == l->image.width &&
                                        copied[k].height == l->image.height))
                    k++;
                if (k == ncopied) {
                    if (!put_image(&l->image, arena, &used, &copied[k].offset, &copied[k].image)) {
                        if (!said) say("a layer's image (%ux%u) did not fit the frame's %u MiB; it is left out",
                                       l->image.width, l->image.height, OXS_ARENA >> 20);
                        said = true;
                        continue;
                    }
                    copied[k].from = l->image.pixels;
                    copied[k].width = l->image.width;
                    copied[k].height = l->image.height;
                    ncopied++;
                }
                out->offset = copied[k].offset;
                out->layer.image = copied[k].image;
            }
            s->nlayers[e]++;
        }
    }
}

static bool sim_present(const struct oxs_frame *f)
{
    if (!alive()) return false;
    const uint32_t ready = oxs_load(&sim.shm->ready), reading = oxs_load(&sim.shm->reading);
    uint32_t slot = 0;
    while (slot + 1 == ready || slot + 1 == reading) slot++;
    struct oxs_shm_slot *s = &sim.shm->slot[slot];
    for (int e = 0; e < 2; e++) {
        const struct oxs_eye *eye = &f->eye[e];
        const uint32_t w = eye->width < OXS_EYE_MAX ? eye->width : OXS_EYE_MAX;
        const uint32_t h = eye->height < OXS_EYE_MAX ? eye->height : OXS_EYE_MAX;
        uint8_t *dst = oxs_shm_pixels(sim.shm, slot, (uint32_t)e);
        if (eye->pixels != NULL)
            for (uint32_t y = 0; y < h; y++) memcpy(dst + (size_t)y * w * 4, eye->pixels + (size_t)y * eye->stride, (size_t)w * 4);
        s->eye[e] = (struct oxs_shm_eye){
            .width = eye->pixels != NULL ? w : 0, .height = eye->pixels != NULL ? h : 0, .stride = w * 4,
            .flags = (eye->bgra ? OXS_EYE_BGRA : 0) | (eye->linear ? OXS_EYE_LINEAR : 0) |
                     (eye->flipped ? OXS_EYE_FLIPPED : 0),
        };
    }
    put_layers(f, slot, s);
    s->number = f->number;
    s->display_time = f->display_time;
    oxs_store(&sim.shm->ready, slot + 1);
    plat_sim_wake();
    return true;
}

static void sim_status(const struct oxs_status *st)
{
    if (sim.shm == NULL) return;
    struct oxs_shm_status s = { .state = st->state, .fps = st->fps, .haptic = { st->haptic[0], st->haptic[1] } };
    snprintf(s.app, sizeof s.app, "%s", st->app != NULL ? st->app : "");
    snprintf(s.profile, sizeof s.profile, "%s", st->profile != NULL ? st->profile : "");
    oxs_seq_write(&sim.shm->status_seq, &sim.shm->status, &s, sizeof s);
}

const struct oxs_device oxs_device_sim = {
    .name = "sim",
    .system_name = "openxr-sim simulated headset",
    .get_system = sim_get_system,
    .open = sim_open,
    .close = sim_close,
    .track = sim_track,
    .present = sim_present,
    .status = sim_status,
};
