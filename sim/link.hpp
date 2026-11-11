// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
#pragma once

// THE SIMULATOR'S END OF common/sim_protocol.h: the shared memory the runtime made, and the wake channel
// that says "a frame is ready" and, by closing, "the application has gone" (runtime/plat.h says what each is
// on each platform). Frames are read straight out of the shared slot into the window's own (smaller)
// pixels, so a 2048-pixel eye is never copied whole; a frame of composition layers is composited here, at
// that size (common/composite.c).

#include "sim_protocol.h"

#include <cstdint>
#include <string>
#include <vector>

namespace oxs {

// ONE EYE AS THE WINDOW DRAWS IT: B G R A in memory (sim/canvas.hpp's order), opaque, rows top to bottom,
// sRGB -- whatever order, direction and encoding the application drew in.
struct EyePixels {
    std::vector<uint8_t> argb;
    int width = 0, height = 0;       // what was converted: at most the size asked for, never larger than the source
    int source_w = 0, source_h = 0;  // what the application drew
};

// Converts one eye of a frame to `out`, at most `max_w` x `max_h` (the proportions kept; never enlarged).
void convert_eye(const uint8_t* src, const oxs_shm_eye& eye, int max_w, int max_h, EyePixels& out);

class Link {
public:
    Link() = default;
    ~Link();
    Link(const Link&) = delete;
    Link& operator=(const Link&) = delete;

    // What the runtime handed over: on POSIX descriptors OXS_SHM_FD and OXS_SOCKET_FD; on Windows the two
    // handles in `link` (--link SHM,PIPE). False, and why, for something that is not one of ours -- started
    // by hand with --headset, or by a runtime of another version.
    bool attach(const std::string& link, std::string* error);
    bool attached() const { return shm_ != nullptr; }

    // WAITS for the runtime to say a frame is ready, at most `timeout_ms` (-1: for ever), and takes every
    // wake already sent: 1 when one came, 0 when the time ran out, -1 once the runtime has gone (the
    // application exited, or ended its session). The only reader of the channel: the window's link thread,
    // or the headless loop.
    int wait(int timeout_ms);
    // Makes a wait() blocked in another thread return -1, for shutting down.
    void interrupt();

    // The newest frame, if there is one newer than the last taken: both eyes converted at most max_w x max_h.
    bool take_frame(int max_w, int max_h, EyePixels out[2]);
    uint64_t frames() const { return taken_; }

    // What the application is doing (the runtime's seqlock); false while it is mid-write.
    bool status(oxs_shm_status& out) const;
    void write_tracking(const oxs_tracking&);
    void write_room(const oxs_room&);

private:
    void compose_eye(const oxs_shm_slot& s, uint32_t slot, uint32_t e, const oxs_shm_eye& eye, int max_w, int max_h,
                     EyePixels& out);

    oxs_shm* shm_ = nullptr;
    size_t      size_ = 0;
#ifdef _WIN32
    void*       pipe_ = nullptr;     // HANDLE: the read end of the runtime's pipe
#else
    int         sock_ = -1;
#endif
    uint64_t    last_number_ = 0;
    uint64_t    taken_ = 0;
};

}  // namespace oxs
