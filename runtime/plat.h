// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
//
// WHAT THE RUNTIME ASKS OF THE OPERATING SYSTEM, in one place: plat_posix.c and plat_win32.c answer it.
// Everything else in runtime/ is the same C on every platform.
//
// THE SIMULATOR CHILD is the largest part. The runtime makes a shared memory region of
// oxs_shm_size() bytes, starts the simulator with it and with its end of a WAKE CHANNEL, and
// afterwards only ever does three things with the child: wakes it ("a frame is ready"), asks whether it
// is still there, and stops it. On POSIX the channel is a SOCK_SEQPACKET pair and the two ends are
// descriptors 3 and 4 in the child; on Windows it is an anonymous pipe, the child is watched by its
// process handle, and the two handles are named on its command line (--link SHM,PIPE).
#ifndef OPENXR_SIM_PLAT_H
#define OPENXR_SIM_PLAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef _WIN32
#define OXS_EXPORT   // exported by runtime/openxr_sim.def
#define OXS_PRINTF(f, a)
#define strdup _strdup
#else
#define OXS_EXPORT __attribute__((visibility("default")))
#define OXS_PRINTF(f, a) __attribute__((format(printf, f, a)))
#endif

// A monotonic clock in nanoseconds (XrTime), and sleeping until a time on it.
int64_t plat_now_ns(void);
void plat_sleep_until_ns(int64_t t);
void plat_sleep_ms(int ms);

// A function out of a library the application has (or might have) loaded: first whatever is in the
// process already, then each of `libs` in turn, loaded if it has to be. NULL with none.
void *plat_symbol(const char *const libs[], const char *name);

// THE CONFIGURATION FILE, for `device =`: which = 0 for this project's own, 1 for the one LOS's runtime
// read before the split (until 2027-05-01). False when there is no such place.
bool plat_config_path(int which, char *out, size_t n);
FILE *plat_fopen_read(const char *path);

// The simulator's executable: where it is installed (beside this library on Windows), and whether it is there.
const char *plat_default_sim_path(void);
bool plat_is_executable(const char *path);

// The child. The region is made and mapped, handed to `prepare` (which stamps its header: the child checks
// it as it attaches), and only then is the child started; `shm` receives the mapping. False, with why, when
// any part of it failed, and nothing left behind.
bool plat_sim_start(const char *exe, const char *app, size_t size, void (*prepare)(void *shm), void **shm, char *why,
                    size_t n);
bool plat_sim_alive(void);   // false once the child's end has closed: its window, or the child, gone
void plat_sim_wake(void);
void plat_sim_stop(void);    // closes the channel, gives the child a moment to go, unmaps

#endif
