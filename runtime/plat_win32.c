// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
//
// plat.h ON WINDOWS. The child is given what plat_posix.c gives it as descriptors 3 and 4, as two inherited
// handles named on its command line (--link SHM,PIPE, which sim/link.cpp reads): a pagefile-backed file
// mapping for the region, and the read end of an anonymous pipe for the wake channel. Only those two are
// inherited -- PROC_THREAD_ATTRIBUTE_HANDLE_LIST -- not whatever else the application left inheritable.
#include "plat.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

// ---- time -----------------------------------------------------------------------------------------

static int64_t qpc_freq(void)
{
    static int64_t f;
    if (f == 0) {
        LARGE_INTEGER li;
        QueryPerformanceFrequency(&li);
        f = li.QuadPart;
    }
    return f;
}

// QueryPerformanceCounter in nanoseconds: what XR_KHR_win32_convert_performance_counter_time converts.
int64_t plat_now_ns(void)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    const int64_t f = qpc_freq();
    return (c.QuadPart / f) * 1000000000 + (c.QuadPart % f) * 1000000000 / f;
}

// A high-resolution waitable timer for all but the last half millisecond, which is spun: Sleep's own
// granularity is the system tick, 15.6 ms unless someone raised it.
void plat_sleep_until_ns(int64_t t)
{
    static HANDLE timer;
    if (timer == NULL) {
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (timer == NULL) timer = CreateWaitableTimerW(NULL, TRUE, NULL);   // before Windows 10 1803
    }
    for (;;) {
        const int64_t left = t - plat_now_ns();
        if (left <= 0) return;
        if (left > 500000 && timer != NULL) {
            LARGE_INTEGER due = { .QuadPart = -((left - 500000) / 100) };   // relative, in 100 ns
            if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(timer, INFINITE);
            else Sleep(0);
        } else {
            YieldProcessor();
        }
    }
}

void plat_sleep_ms(int ms) { Sleep(ms > 0 ? (DWORD)ms : 0); }

// ---- libraries and files --------------------------------------------------------------------------

void *plat_symbol(const char *const libs[], const char *name)
{
    for (int i = 0; libs[i] != NULL; i++) {
        HMODULE m = GetModuleHandleA(libs[i]);
        if (m == NULL) m = LoadLibraryA(libs[i]);
        if (m == NULL) continue;
        FARPROC f = GetProcAddress(m, name);
        if (f != NULL) return (void *)f;
    }
    return NULL;
}

static bool widen(const char *s, wchar_t *out, int n)
{
    return MultiByteToWideChar(CP_UTF8, 0, s, -1, out, n) > 0;
}

// %APPDATA%\openxr-sim\config. There is no second, older file on Windows: LOS never ran there.
bool plat_config_path(int which, char *out, size_t n)
{
    if (which != 0) return false;
    const wchar_t *appdata = _wgetenv(L"APPDATA");
    if (appdata == NULL || appdata[0] == L'\0') return false;
    wchar_t path[MAX_PATH * 2];
    _snwprintf(path, sizeof path / sizeof path[0], L"%ls\\openxr-sim\\config", appdata);
    path[sizeof path / sizeof path[0] - 1] = L'\0';
    return WideCharToMultiByte(CP_UTF8, 0, path, -1, out, (int)n, NULL, NULL) > 0;
}

FILE *plat_fopen_read(const char *path)
{
    wchar_t w[MAX_PATH * 2];
    if (!widen(path, w, MAX_PATH * 2)) return NULL;
    return _wfopen(w, L"rN");   // N: not inherited by the child
}

// openxr-sim.exe BESIDE THIS DLL: they are installed together, and the manifest names the DLL by its
// relative path, so where the DLL is, is where the runtime is.
const char *plat_default_sim_path(void)
{
    static char path[MAX_PATH * 3];
    if (path[0] != '\0') return path;
    HMODULE self = NULL;
    wchar_t w[MAX_PATH];
    // A path that filled the buffer was cut short (GetModuleFileNameW then answers its size, and sets an error
    // that GetLastError after a success would not be the one to read).
    DWORD len = 0;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)(void *)plat_default_sim_path, &self) ||
        (len = GetModuleFileNameW(self, w, MAX_PATH)) == 0 || len >= MAX_PATH) {
        strcpy(path, "openxr-sim.exe");
        return path;
    }
    wchar_t *slash = wcsrchr(w, L'\\');
    if (slash == NULL) slash = w; else slash++;
    wcscpy_s(slash, MAX_PATH - (size_t)(slash - w), L"openxr-sim.exe");
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, path, (int)sizeof path, NULL, NULL) == 0) strcpy(path, "openxr-sim.exe");
    return path;
}

bool plat_is_executable(const char *path)
{
    wchar_t w[MAX_PATH * 2];
    if (!widen(path, w, MAX_PATH * 2)) return false;
    const DWORD a = GetFileAttributesW(w);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// ---- the child ------------------------------------------------------------------------------------

static struct {
    void *shm;
    HANDLE mapping, pipe_write, process;
    bool broken;
} child;

static void close_handle(HANDLE *h)
{
    if (*h != NULL) { CloseHandle(*h); *h = NULL; }
}

void plat_sim_stop(void)
{
    close_handle(&child.pipe_write);   // the simulator's wait() sees a broken pipe and closes its window
    if (child.process != NULL) {
        // A moment for it to go, then it is left to finish on its own (plat_posix.c, reap).
        WaitForSingleObject(child.process, 500);
        close_handle(&child.process);
    }
    if (child.shm != NULL) { UnmapViewOfFile(child.shm); child.shm = NULL; }
    close_handle(&child.mapping);
    child.broken = false;
}

// One command-line argument, quoted the way CommandLineToArgvW and the C runtime split it: backslashes are
// literal except before a quote, where they are doubled.
static void append_arg(wchar_t *cmd, size_t cap, const wchar_t *arg)
{
    size_t at = wcslen(cmd);
    if (at > 0 && at + 1 < cap) cmd[at++] = L' ';
    if (at + 1 < cap) cmd[at++] = L'"';
    for (const wchar_t *p = arg;; p++) {
        size_t slashes = 0;
        while (*p == L'\\') { slashes++; p++; }
        const size_t times = *p == L'\0' ? slashes * 2 : *p == L'"' ? slashes * 2 + 1 : slashes;
        for (size_t i = 0; i < times && at + 1 < cap; i++) cmd[at++] = L'\\';
        if (*p == L'\0') break;
        if (at + 1 < cap) cmd[at++] = *p;
    }
    if (at + 1 < cap) cmd[at++] = L'"';
    cmd[at] = L'\0';
}

static void last_error(char *why, size_t n, const char *what)
{
    snprintf(why, n, "%s (error %lu)", what, (unsigned long)GetLastError());
}

bool plat_sim_start(const char *exe, const char *app, size_t size, void (*prepare)(void *shm), void **shm, char *why,
                    size_t n)
{
    plat_sim_stop();
    SECURITY_ATTRIBUTES inherit = { .nLength = sizeof inherit, .bInheritHandle = TRUE };
    child.mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &inherit, PAGE_READWRITE, (DWORD)((uint64_t)size >> 32),
                                       (DWORD)size, NULL);
    if (child.mapping == NULL) { last_error(why, n, "no shared memory"); plat_sim_stop(); return false; }
    child.shm = MapViewOfFile(child.mapping, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (child.shm == NULL) { last_error(why, n, "could not map the shared memory"); plat_sim_stop(); return false; }
    prepare(child.shm);

    HANDLE theirs = NULL;
    if (!CreatePipe(&theirs, &child.pipe_write, &inherit, 4096)) {
        last_error(why, n, "no pipe");
        plat_sim_stop();
        return false;
    }
    SetHandleInformation(child.pipe_write, HANDLE_FLAG_INHERIT, 0);
    // A write that would block returns at once instead: a full pipe already says "look" (plat_sim_wake).
    DWORD mode = PIPE_NOWAIT;
    SetNamedPipeHandleState(child.pipe_write, &mode, NULL, NULL);

    wchar_t wexe[MAX_PATH * 2], wapp[512], link[64];
    if (!widen(exe, wexe, MAX_PATH * 2)) { snprintf(why, n, "the simulator's path is not UTF-8"); CloseHandle(theirs); plat_sim_stop(); return false; }
    if (!widen(app != NULL ? app : "", wapp, 512)) wapp[0] = L'\0';
    _snwprintf(link, 64, L"%llu,%llu", (unsigned long long)(uintptr_t)child.mapping, (unsigned long long)(uintptr_t)theirs);
    link[63] = L'\0';
    static wchar_t cmd[4096];
    cmd[0] = L'\0';
    append_arg(cmd, 4096, wexe);
    append_arg(cmd, 4096, L"--headset");
    append_arg(cmd, 4096, L"--app");
    append_arg(cmd, 4096, wapp);
    append_arg(cmd, 4096, L"--link");
    append_arg(cmd, 4096, link);

    HANDLE handles[2] = { child.mapping, theirs };
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = malloc(attr_size);
    bool listed = attrs != NULL && InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size) &&
                  UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof handles, NULL, NULL);
    STARTUPINFOEXW si = { .StartupInfo.cb = sizeof si };
    si.lpAttributeList = listed ? attrs : NULL;
    PROCESS_INFORMATION pi = { 0 };
    // Its own process group, so the application's console Ctrl+C is not the simulator's.
    const BOOL ok = CreateProcessW(wexe, cmd, NULL, NULL, TRUE,
                                   (listed ? EXTENDED_STARTUPINFO_PRESENT : 0) | CREATE_NEW_PROCESS_GROUP, NULL, NULL,
                                   &si.StartupInfo, &pi);
    const DWORD err = GetLastError();
    if (listed) DeleteProcThreadAttributeList(attrs);
    free(attrs);
    CloseHandle(theirs);
    if (!ok) {
        snprintf(why, n, "could not start %s (error %lu)", exe, (unsigned long)err);
        plat_sim_stop();
        return false;
    }
    CloseHandle(pi.hThread);
    child.process = pi.hProcess;
    *shm = child.shm;
    return true;
}

// Gone when the process has exited, or its end of the pipe has closed (its window closed: sim/link.cpp,
// interrupt). The pipe cannot be asked from this end -- PeekNamedPipe wants read access, which the write
// end has not -- so that is what plat_sim_wake saw last, at the latest one frame ago.
bool plat_sim_alive(void)
{
    if (child.process == NULL || child.pipe_write == NULL || child.broken) return false;
    return WaitForSingleObject(child.process, 0) == WAIT_TIMEOUT;
}

void plat_sim_wake(void)
{
    if (child.pipe_write == NULL) return;
    const char wake = 'F';
    DWORD wrote = 0;
    if (!WriteFile(child.pipe_write, &wake, 1, &wrote, NULL)) {
        const DWORD e = GetLastError();
        if (e == ERROR_NO_DATA || e == ERROR_BROKEN_PIPE) child.broken = true;
    }
}
