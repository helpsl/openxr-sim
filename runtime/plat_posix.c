// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
//
// plat.h ON LINUX (and any POSIX system with memfd_create and SOCK_SEQPACKET).
#define _GNU_SOURCE
#include "plat.h"

#include "sim_protocol.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#ifndef OPENXR_SIM_PATH
#define OPENXR_SIM_PATH "/usr/bin/openxr-sim"
#endif

int64_t plat_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

void plat_sleep_until_ns(int64_t t)
{
    const struct timespec ts = { .tv_sec = t / 1000000000, .tv_nsec = t % 1000000000 };
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
}

void plat_sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000 };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

void *plat_symbol(const char *const libs[], const char *name)
{
    void *f = dlsym(RTLD_DEFAULT, name);
    for (int i = 0; f == NULL && libs[i] != NULL; i++) {
        void *lib = dlopen(libs[i], RTLD_NOW | RTLD_GLOBAL);
        if (lib != NULL) f = dlsym(lib, name);
    }
    return f;
}

bool plat_config_path(int which, char *out, size_t n)
{
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    const char *rel = which == 0 ? "openxr-sim/config" : "los/openxr";
    if (xdg != NULL && xdg[0] == '/') snprintf(out, n, "%s/%s", xdg, rel);
    else if (home != NULL && home[0] != '\0') snprintf(out, n, "%s/.config/%s", home, rel);
    else return false;
    return true;
}

FILE *plat_fopen_read(const char *path) { return fopen(path, "re"); }

const char *plat_default_sim_path(void) { return OPENXR_SIM_PATH; }

bool plat_is_executable(const char *path) { return access(path, X_OK) == 0; }

// ---- the child ------------------------------------------------------------------------------------

static struct {
    void *shm;
    size_t size;
    int memfd, sock;
    pid_t pid;
} child = { .memfd = -1, .sock = -1, .pid = -1 };

// Above the standard descriptors and the two the child is given, so dup2 onto 3 and 4 never lands on
// itself (which would leave close-on-exec set).
static int lift(int fd)
{
    if (fd < 0) return fd;
    const int high = fcntl(fd, F_DUPFD_CLOEXEC, 10);
    close(fd);
    return high;
}

static void reap(void)
{
    if (child.pid <= 0) return;
    // The window closes when its socket does; a moment for it to go, then it is left to the application's
    // own exit rather than blocking a frame on it.
    for (int i = 0; i < 50; i++) {
        const pid_t r = waitpid(child.pid, NULL, WNOHANG);
        if (r == child.pid || (r < 0 && errno == ECHILD)) { child.pid = -1; return; }
        plat_sleep_ms(10);
    }
}

void plat_sim_stop(void)
{
    if (child.sock >= 0) { close(child.sock); child.sock = -1; }
    reap();
    if (child.shm != NULL) { munmap(child.shm, child.size); child.shm = NULL; }
    if (child.memfd >= 0) { close(child.memfd); child.memfd = -1; }
}

bool plat_sim_start(const char *exe, const char *app, size_t size, void (*prepare)(void *shm), void **shm, char *why,
                    size_t n)
{
    plat_sim_stop();
    child.memfd = lift(memfd_create("openxr-sim", MFD_CLOEXEC));
    if (child.memfd < 0 || ftruncate(child.memfd, (off_t)size) != 0) {
        snprintf(why, n, "no shared memory: %s", strerror(errno));
        plat_sim_stop();
        return false;
    }
    void *m = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, child.memfd, 0);
    if (m == MAP_FAILED) {
        snprintf(why, n, "could not map the shared memory: %s", strerror(errno));
        plat_sim_stop();
        return false;
    }
    child.shm = m;
    child.size = size;
    prepare(m);

    int pair[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) != 0) {
        snprintf(why, n, "no socket: %s", strerror(errno));
        plat_sim_stop();
        return false;
    }
    child.sock = lift(pair[0]);
    const int theirs = lift(pair[1]);
    fcntl(child.sock, F_SETFL, fcntl(child.sock, F_GETFL) | O_NONBLOCK);

    // THE ENVIRONMENT, less what points the dynamic linker elsewhere: a game's own LD_LIBRARY_PATH (the
    // Steam runtime's, a bundled copy of libstdc++) is for the game, and the simulator is a system program.
    size_t ne = 0;
    while (environ != NULL && environ[ne] != NULL) ne++;
    char **envp = calloc(ne + 1, sizeof *envp);
    size_t k = 0;
    for (size_t i = 0; i < ne && envp != NULL; i++)
        if (strncmp(environ[i], "LD_PRELOAD=", 11) != 0 && strncmp(environ[i], "LD_LIBRARY_PATH=", 16) != 0)
            envp[k++] = environ[i];

    char *argv[] = { (char *)exe, "--headset", "--app", (char *)(app != NULL ? app : ""), NULL };
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, child.memfd, OXS_SHM_FD);
    posix_spawn_file_actions_adddup2(&fa, theirs, OXS_SOCKET_FD);
    // The application's signal mask and ignored signals are not the simulator's.
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &all);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSID);
    const int rc = posix_spawn(&child.pid, exe, &fa, &attr, argv, envp != NULL ? envp : environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    free(envp);
    close(theirs);
    if (rc != 0) {
        child.pid = -1;
        snprintf(why, n, "could not start %s: %s", exe, strerror(rc));
        plat_sim_stop();
        return false;
    }
    *shm = m;
    return true;
}

bool plat_sim_alive(void)
{
    if (child.sock < 0) return false;
    struct pollfd p = { .fd = child.sock, .events = POLLIN };
    if (poll(&p, 1, 0) > 0) {
        char buf[64];
        const ssize_t r = recv(child.sock, buf, sizeof buf, MSG_DONTWAIT);
        if ((p.revents & (POLLHUP | POLLERR)) != 0 || r == 0) {
            reap();
            return false;
        }
    }
    return true;
}

void plat_sim_wake(void)
{
    const char wake = 'F';
    send(child.sock, &wake, 1, MSG_DONTWAIT | MSG_NOSIGNAL);   // a full queue already says "look"
}
