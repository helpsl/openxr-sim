// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
//
// BETWEEN THE RUNTIME AND THE SIMULATOR (DESIGN.md, "Two parts, two processes"). The runtime, inside the
// application, makes a memory file of this layout and a socket pair, and starts openxr-sim with the
// file as descriptor 3 and its end of the socket as descriptor 4.
//
// FRAMES go one way through three slots. The runtime writes a slot that is neither the one last
// published (`ready`) nor the one the simulator says it is reading (`reading`), publishes it, and sends
// one byte on the socket. The simulator sets `reading` to `ready`, reads `ready` again and starts over if
// it moved, then copies. Three slots always leave the runtime one to write, so neither side waits for the
// other. All of it sequentially consistent: the argument above needs every load and store in one order.
//
// TRACKING and STATUS go one way each, as seqlocks: the writer makes `seq` odd, writes, makes it even;
// the reader copies and keeps the copy only if `seq` was even and unchanged across it.
//
// A COMPOSITED FRAME (any frame but one plain projection layer; DESIGN.md, "Composition layers") also
// carries, in its slot, every layer for each eye -- resolved by the runtime, its pose in STAGE -- and each
// layer's sub-image in the slot's ARENA, OXS_ARENA bytes after the two eyes. A sub-image wider or taller
// than OXS_EYE_MAX is subsampled to fit; one layer shown to both eyes is copied once; the first
// projection layer's images are the slot's eyes, not copied again. A layer that does not fit what is left
// of the arena is left out of the frame, and the runtime says so once on stderr.
//
// The socket closing is the other side gone: the window was closed, or the application exited.
#ifndef OPENXR_SIM_SIM_PROTOCOL_H
#define OPENXR_SIM_SIM_PROTOCOL_H

#include "device.h"

#include <stdint.h>
#include <string.h>

#define OXS_SHM_MAGIC 0x584f534cu   // "LOSX": the value from before the split
#define OXS_SHM_VERSION 3u   // 2: composition layers; 3: openxr-sim, the handles named on the command line on Windows
#define OXS_EYE_MAX 2048u           // an eye wider or taller than this is cut to it
#define OXS_ARENA (32u << 20)       // per slot: the layers' sub-images, after the eyes
#define OXS_SLOTS 3u
#define OXS_SHM_FD 3
#define OXS_SOCKET_FD 4

enum { OXS_EYE_BGRA = 1, OXS_EYE_LINEAR = 2, OXS_EYE_FLIPPED = 4 };

struct oxs_shm_eye { uint32_t width, height, stride, flags; };

// A layer as the slot holds it: its `image.pixels` means nothing here; `offset` says where the pixels are.
#define OXS_OFFSET_EYE UINT64_MAX     // the slot's own eye, for the projection the eye is seen through
struct oxs_shm_layer {
    struct oxs_layer layer;
    uint64_t offset;                     // into the slot's arena
};

struct oxs_shm_slot {
    struct oxs_shm_eye eye[2];
    uint32_t nlayers[2];                 // 0: shown as it is; else composited from these
    struct oxs_shm_layer layer[2][OXS_LAYERS_MAX];
    struct oxs_camera camera[2];
    uint64_t number;
    int64_t display_time;
};

struct oxs_shm_status {
    char app[128];
    char profile[128];
    int32_t state;
    float fps;
    float haptic[2];
};

struct oxs_shm {
    uint32_t magic, version, eye_max, slots;
    uint64_t pixels_offset, size;
    // the runtime's
    uint32_t ready;     // 1 + the slot last published, 0 for none
    uint32_t reading;   // 1 + the slot the simulator is reading, 0 for none
    struct oxs_shm_slot slot[OXS_SLOTS];
    uint32_t status_seq;
    struct oxs_shm_status status;
    // the simulator's
    uint32_t tracking_seq;
    uint32_t tracking_valid;   // 0 until the simulator has written the poses once
    struct oxs_tracking tracking;
};

#define OXS_SHM_HEADER 65536u   // the structure above fits in it (device_sim.c asserts it)

// Each slot's pixels: its two eyes, then its arena.
static inline uint64_t oxs_slot_bytes(void) { return (uint64_t)2 * OXS_EYE_MAX * OXS_EYE_MAX * 4 + OXS_ARENA; }

static inline uint64_t oxs_shm_size(void)
{
    return OXS_SHM_HEADER + (uint64_t)OXS_SLOTS * oxs_slot_bytes();
}

static inline uint8_t *oxs_shm_pixels(struct oxs_shm *m, uint32_t slot, uint32_t eye)
{
    return (uint8_t *)m + m->pixels_offset + slot * oxs_slot_bytes() + (uint64_t)eye * m->eye_max * m->eye_max * 4;
}

static inline uint8_t *oxs_shm_arena(struct oxs_shm *m, uint32_t slot)
{
    return oxs_shm_pixels(m, slot, 2);
}

// THE ATOMICS: GCC's and Clang's builtins, or MSVC's. Under MSVC, x86-64 only: there an aligned 32-bit load
// or store is atomic, and loads are not reordered with loads nor stores with stores (TSO), so acquire and
// release need only keep the compiler from moving them; a sequentially consistent store is an exchange.
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#if !defined(_M_X64)
#error "the MSVC atomics here are written for x86-64"
#endif
#define OXS_LOAD_ACQUIRE(p) oxs_msvc_load(p)
#define OXS_LOAD_RELAXED(p) (*(const volatile uint32_t *)(p))
#define OXS_STORE_RELAXED(p, v) (*(volatile uint32_t *)(p) = (v))
#define OXS_STORE_RELEASE(p, v) do { _ReadWriteBarrier(); *(volatile uint32_t *)(p) = (v); } while (0)
#define OXS_FENCE_RELEASE() _ReadWriteBarrier()
#define OXS_FENCE_ACQUIRE() _ReadWriteBarrier()
static inline uint32_t oxs_msvc_load(const uint32_t *p)
{
    const uint32_t v = *(const volatile uint32_t *)p;
    _ReadWriteBarrier();
    return v;
}
static inline uint32_t oxs_load(const uint32_t *p) { return oxs_msvc_load(p); }
static inline void oxs_store(uint32_t *p, uint32_t v) { _InterlockedExchange((volatile long *)p, (long)v); }
#else
#define OXS_LOAD_ACQUIRE(p) __atomic_load_n(p, __ATOMIC_ACQUIRE)
#define OXS_LOAD_RELAXED(p) __atomic_load_n(p, __ATOMIC_RELAXED)
#define OXS_STORE_RELAXED(p, v) __atomic_store_n(p, v, __ATOMIC_RELAXED)
#define OXS_STORE_RELEASE(p, v) __atomic_store_n(p, v, __ATOMIC_RELEASE)
#define OXS_FENCE_RELEASE() __atomic_thread_fence(__ATOMIC_RELEASE)
#define OXS_FENCE_ACQUIRE() __atomic_thread_fence(__ATOMIC_ACQUIRE)
static inline uint32_t oxs_load(const uint32_t *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static inline void oxs_store(uint32_t *p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }
#endif

// The seqlock's two halves, for any plain struct.
static inline void oxs_seq_write(uint32_t *seq, void *dst, const void *src, size_t n)
{
    const uint32_t s = OXS_LOAD_RELAXED(seq);
    OXS_STORE_RELAXED(seq, s + 1);
    OXS_FENCE_RELEASE();
    memcpy(dst, src, n);
    OXS_STORE_RELEASE(seq, s + 2);
}

static inline int oxs_seq_read(const uint32_t *seq, void *dst, const void *src, size_t n)
{
    for (int tries = 0; tries < 1000; tries++) {
        const uint32_t a = OXS_LOAD_ACQUIRE(seq);
        if (a & 1) continue;
        memcpy(dst, src, n);
        OXS_FENCE_ACQUIRE();
        if (OXS_LOAD_RELAXED(seq) == a) return 1;
    }
    return 0;
}

#endif
