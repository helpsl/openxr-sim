// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
//
// sim/install.hpp ON WINDOWS: this .exe installs itself and the runtime it carries, and registers them.
//
// WHAT IS WRITTEN, all in the 64-bit view of HKEY_LOCAL_MACHINE (the runtime is a 64-bit DLL, which only a
// 64-bit application can load; a 32-bit one reads WOW6432Node and would need a 32-bit build):
//   SOFTWARE\Khronos\OpenXR\1  ActiveRuntime      REG_SZ, the manifest's absolute path. REG_SZ and never
//                                                 REG_EXPAND_SZ: the loader asks RegGetValueW for RRF_RT_REG_SZ
//                                                 and then wants a regular file at the path as it stands.
//                              AvailableRuntimes\<manifest> = 0, which runtime switchers list (the loader
//                                                 does not read it)
//   SOFTWARE\openxr-sim        Version, InstallDir, and PreviousRuntime -- the ActiveRuntime this replaced
//   SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\openxr-sim   Settings > Apps
// and the files, in %ProgramFiles%\openxr-sim\<version>\: openxr-sim.exe, openxr_sim.dll, openxr_sim.json.
//
// A VERSIONED FOLDER because an application that is running holds the DLL it loaded, and Windows will not
// replace a file in use: an update is unpacked beside the old one, ActiveRuntime moves, and the old folder is
// removed then or, if something still has it open, at the next restart.
#include "install.hpp"

#include "os.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <cstring>
#include <fstream>
#include <regex>
#include <vector>

#ifndef OPENXR_SIM_VERSION
#define OPENXR_SIM_VERSION "0"
#endif

namespace oxs {

namespace {

constexpr const wchar_t* kKhronosKey = L"SOFTWARE\\Khronos\\OpenXR\\1";
constexpr const wchar_t* kAvailableKey = L"SOFTWARE\\Khronos\\OpenXR\\1\\AvailableRuntimes";
constexpr const wchar_t* kOurKey = L"SOFTWARE\\openxr-sim";
constexpr const wchar_t* kUninstallKey = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\openxr-sim";
constexpr REGSAM k64 = KEY_WOW64_64KEY;

std::wstring widen(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

bool same_path(const std::wstring& a, const std::wstring& b) { return lower(a) == lower(b); }
bool under(const std::wstring& path, const std::wstring& dir) {
    const std::wstring p = lower(path), d = lower(dir);
    return !d.empty() && p.size() > d.size() && p.compare(0, d.size(), d) == 0 && (p[d.size()] == L'\\' || d.back() == L'\\');
}

std::wstring read_string(HKEY root, const wchar_t* key, const wchar_t* value) {
    DWORD size = 0;
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, nullptr, &size) != ERROR_SUCCESS || size == 0)
        return {};
    std::wstring s(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, s.data(), &size) != ERROR_SUCCESS) return {};
    s.resize(wcsnlen(s.c_str(), s.size()));
    return s;
}

LSTATUS write_string(HKEY key, const wchar_t* value, const std::wstring& s) {
    return RegSetValueExW(key, value, 0, REG_SZ, reinterpret_cast<const BYTE*>(s.c_str()),
                          static_cast<DWORD>((s.size() + 1) * sizeof(wchar_t)));
}

LSTATUS write_dword(HKEY key, const wchar_t* value, DWORD v) {
    return RegSetValueExW(key, value, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof v);
}

// A key opened for writing, closed when it goes.
struct Key {
    HKEY h = nullptr;
    LSTATUS status = ERROR_SUCCESS;
    explicit Key(const wchar_t* path) {
        status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, path, 0, nullptr, 0, KEY_READ | KEY_WRITE | k64, nullptr, &h, nullptr);
        if (status != ERROR_SUCCESS) h = nullptr;
    }
    ~Key() { if (h != nullptr) RegCloseKey(h); }
    Key(const Key&) = delete;
    Key& operator=(const Key&) = delete;
};

std::wstring program_files() {
    PWSTR p = nullptr;
    std::wstring out;
    if (SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &p) == S_OK) out = p;
    CoTaskMemFree(p);
    return out;
}

std::wstring install_root() {
    const std::wstring pf = program_files();
    return pf.empty() ? std::wstring() : pf + L"\\openxr-sim";
}

std::wstring self_path() {
    std::wstring w(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, w.data(), static_cast<DWORD>(w.size()));
        if (n == 0) return {};
        if (n < w.size()) { w.resize(n); return w; }
        w.resize(w.size() * 2);
    }
}

// THE NAME IN A MANIFEST: runtime.name, which the specification makes optional, else the library's file
// name. Read with a pattern, not a JSON parser: a manifest is a handful of lines, and a name is all that is
// wanted from it.
std::string manifest_name(const std::wstring& path) {
    if (path.empty()) return {};
    std::ifstream in(narrow(path), std::ios::binary);
    if (!in) return {};
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::smatch m;
    if (std::regex_search(text, m, std::regex(R"re("name"\s*:\s*"([^"]*)")re")) && !m[1].str().empty()) return m[1].str();
    if (std::regex_search(text, m, std::regex(R"re("library_path"\s*:\s*"([^"]*)")re"))) {
        std::string lib = m[1].str();
        const size_t slash = lib.find_last_of("/\\");
        return slash == std::string::npos ? lib : lib.substr(slash + 1);
    }
    return narrow(path);
}

bool file_exists(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool is_elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION e{};
    DWORD size = 0;
    const bool ok = GetTokenInformation(token, TokenElevation, &e, sizeof e, &size) != 0;
    CloseHandle(token);
    return ok && e.TokenIsElevated != 0;
}

// THIS PROGRAM AGAIN, ELEVATED, with `args`: Windows asks the person (UAC) and the answer comes back as
// the child's exit status, since a windowed program has nowhere to print. --elevated marks the child so it
// never asks again -- under Wine, which has no UAC, "runas" just runs it, and a child that found itself
// unelevated would otherwise start another.
int run_elevated(const std::wstring& args) {
    const std::wstring self = self_path();
    const std::wstring params = args + L" --elevated";
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof sei;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"runas";
    sei.lpFile = self.c_str();
    sei.lpParameters = params.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) return GetLastError() == ERROR_CANCELLED ? kRegisterCancelled : kRegisterNoRights;
    if (sei.hProcess == nullptr) return kRegisterFailed;
    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD code = kRegisterFailed;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return static_cast<int>(code);
}

RegisterResult from_exit(int code, std::string* error) {
    switch (code) {
        case kRegisterOk: return RegisterResult::Done;
        case kRegisterCancelled: return RegisterResult::Cancelled;
        case kRegisterNoRights: *error = "Windows did not give this an administrator's rights"; break;
        case kRegisterInUse: *error = "an application has the runtime open: close it and try again"; break;
        default: *error = "the runtime could not be installed (exit " + std::to_string(code) + ")"; break;
    }
    return RegisterResult::Failed;
}

int status_exit(LSTATUS s) { return s == ERROR_ACCESS_DENIED ? kRegisterNoRights : kRegisterFailed; }

// The runtime this build carries, written to `path` through a temporary file, so a half-written DLL is never
// what the loader finds.
int unpack_runtime(const std::wstring& path) {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    HGLOBAL data = res != nullptr ? LoadResource(nullptr, res) : nullptr;
    const void* bytes = data != nullptr ? LockResource(data) : nullptr;
    const DWORD size = res != nullptr ? SizeofResource(nullptr, res) : 0;
    if (bytes == nullptr || size == 0) return kRegisterFailed;
    // THE SAME BYTES ALREADY THERE (this version registered before): nothing to do, and a running
    // application may hold the file.
    {
        std::ifstream have(narrow(path), std::ios::binary);
        if (have) {
            const std::string old((std::istreambuf_iterator<char>(have)), std::istreambuf_iterator<char>());
            if (old.size() == size && std::memcmp(old.data(), bytes, size) == 0) return kRegisterOk;
        }
    }
    const std::wstring tmp = path + L".new";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_ACCESS_DENIED ? kRegisterNoRights : kRegisterFailed;
    DWORD wrote = 0;
    const bool ok = WriteFile(f, bytes, size, &wrote, nullptr) && wrote == size;
    CloseHandle(f);
    if (!ok) { DeleteFileW(tmp.c_str()); return kRegisterFailed; }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        const DWORD e = GetLastError();
        DeleteFileW(tmp.c_str());
        return e == ERROR_SHARING_VIOLATION || e == ERROR_ACCESS_DENIED ? kRegisterInUse : kRegisterFailed;
    }
    return kRegisterOk;
}

int write_manifest(const std::wstring& path) {
    std::ofstream out(narrow(path), std::ios::binary | std::ios::trunc);
    out << "{\n    \"file_format_version\": \"1.0.0\",\n    \"runtime\": {\n        \"name\": \"openxr-sim\",\n"
           "        \"library_path\": \"./openxr_sim.dll\"\n    }\n}\n";
    out.close();
    return out ? kRegisterOk : kRegisterNoRights;
}

// Every file in `dir` and then `dir`; what is in use is deleted at the next restart instead.
void remove_dir(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (std::wcscmp(fd.cFileName, L".") == 0 || std::wcscmp(fd.cFileName, L"..") == 0) continue;
            const std::wstring p = dir + L"\\" + fd.cFileName;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) { remove_dir(p); continue; }
            if (!DeleteFileW(p.c_str())) MoveFileExW(p.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (!RemoveDirectoryW(dir.c_str())) MoveFileExW(dir.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
}

// The other versions' folders under the install root -- all of them when `keep` is empty.
void remove_other_versions(const std::wstring& root, const std::wstring& keep) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 || std::wcscmp(fd.cFileName, L".") == 0 ||
            std::wcscmp(fd.cFileName, L"..") == 0)
            continue;
        const std::wstring dir = root + L"\\" + fd.cFileName;
        if (!keep.empty() && same_path(dir, keep)) continue;
        remove_dir(dir);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// AvailableRuntimes' entries for any of our versions, so the list holds one.
void forget_available(const std::wstring& root) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kAvailableKey, 0, KEY_READ | KEY_WRITE | k64, &k) != ERROR_SUCCESS) return;
    std::vector<std::wstring> ours;
    wchar_t name[2048];
    for (DWORD i = 0;; i++) {
        DWORD n = 2048;
        if (RegEnumValueW(k, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        if (under(name, root)) ours.emplace_back(name);
    }
    for (const auto& v : ours) RegDeleteValueW(k, v.c_str());
    RegCloseKey(k);
}

int do_register() {
    const std::wstring root = install_root();
    if (root.empty()) return kRegisterFailed;
    const std::wstring dir = root + L"\\" + widen(OPENXR_SIM_VERSION);
    if (!make_parent_dirs(narrow(dir + L"\\x"))) return kRegisterNoRights;
    const std::wstring exe = dir + L"\\openxr-sim.exe", dll = dir + L"\\openxr_sim.dll", json = dir + L"\\openxr_sim.json";

    // THE FILES. This .exe copied unless it is the one there (the installed copy re-registering itself).
    const std::wstring self = self_path();
    if (!same_path(self, exe) && !CopyFileW(self.c_str(), exe.c_str(), FALSE)) {
        const DWORD e = GetLastError();
        // In use: the installed copy is running, and it is this version -- the same program.
        if (!(e == ERROR_SHARING_VIOLATION && file_exists(exe))) return e == ERROR_ACCESS_DENIED ? kRegisterNoRights : kRegisterFailed;
    }
    if (const int r = unpack_runtime(dll); r != kRegisterOk) return r;
    if (const int r = write_manifest(json); r != kRegisterOk) return r;

    // THE REGISTRATION. What was active before is kept, unless it was already us -- an update must not
    // remember the old version as the runtime to go back to.
    Key khronos(kKhronosKey);
    if (khronos.h == nullptr) return status_exit(khronos.status);
    Key ours(kOurKey);
    if (ours.h == nullptr) return status_exit(ours.status);
    const std::wstring before = read_string(HKEY_LOCAL_MACHINE, kKhronosKey, L"ActiveRuntime");
    if (!before.empty() && !under(before, root)) write_string(ours.h, L"PreviousRuntime", before);
    if (const LSTATUS s = write_string(khronos.h, L"ActiveRuntime", json); s != ERROR_SUCCESS) return status_exit(s);
    write_string(ours.h, L"Version", widen(OPENXR_SIM_VERSION));
    write_string(ours.h, L"InstallDir", dir);
    forget_available(root);
    {
        Key available(kAvailableKey);
        if (available.h != nullptr) write_dword(available.h, json.c_str(), 0);
    }
    {
        Key un(kUninstallKey);
        if (un.h != nullptr) {
            write_string(un.h, L"DisplayName", L"OpenXR Simulator");
            write_string(un.h, L"DisplayVersion", widen(OPENXR_SIM_VERSION));
            write_string(un.h, L"Publisher", L"The openxr-sim authors");
            write_string(un.h, L"InstallLocation", dir);
            write_string(un.h, L"DisplayIcon", exe);
            write_string(un.h, L"UninstallString", L"\"" + exe + L"\" --unregister-runtime --remove");
            write_dword(un.h, L"NoModify", 1);
            write_dword(un.h, L"NoRepair", 1);
        }
    }
    remove_other_versions(root, dir);
    return kRegisterOk;
}

int do_unregister(bool remove) {
    const std::wstring root = install_root();
    const std::wstring active = read_string(HKEY_LOCAL_MACHINE, kKhronosKey, L"ActiveRuntime");
    const std::wstring previous = read_string(HKEY_LOCAL_MACHINE, kOurKey, L"PreviousRuntime");
    if (!root.empty() && under(active, root)) {
        Key khronos(kKhronosKey);
        if (khronos.h == nullptr) return status_exit(khronos.status);
        // Back to the one it replaced, if that is still installed; else to none.
        const LSTATUS s = !previous.empty() && file_exists(previous) ? write_string(khronos.h, L"ActiveRuntime", previous)
                                                                     : RegDeleteValueW(khronos.h, L"ActiveRuntime");
        if (s != ERROR_SUCCESS && s != ERROR_FILE_NOT_FOUND) return status_exit(s);
    }
    {
        Key ours(kOurKey);
        if (ours.h != nullptr) RegDeleteValueW(ours.h, L"PreviousRuntime");
    }
    if (!remove) return kRegisterOk;
    forget_available(root);
    RegDeleteKeyExW(HKEY_LOCAL_MACHINE, kUninstallKey, k64, 0);
    RegDeleteKeyExW(HKEY_LOCAL_MACHINE, kOurKey, k64, 0);
    if (!root.empty()) remove_dir(root);   // this program among them, if it runs from there: at the restart
    return kRegisterOk;
}

}  // namespace

RuntimeState runtime_state() {
    RuntimeState st;
    st.can_register = true;
    const std::wstring root = install_root();
    const std::wstring active = read_string(HKEY_LOCAL_MACHINE, kKhronosKey, L"ActiveRuntime");
    st.active_name = file_exists(active) ? manifest_name(active) : std::string();
    st.active_is_ours = !root.empty() && under(active, root) && file_exists(active);
    if (st.active_is_ours) {
        st.ours_version = narrow(read_string(HKEY_LOCAL_MACHINE, kOurKey, L"Version"));
        st.ours_is_this_build = st.ours_version == OPENXR_SIM_VERSION;
        const std::wstring previous = read_string(HKEY_LOCAL_MACHINE, kOurKey, L"PreviousRuntime");
        if (file_exists(previous)) st.previous_name = manifest_name(previous);
    }
    return st;
}

RegisterResult make_runtime_active(std::string* error) { return from_exit(run_elevated(L"--register-runtime"), error); }
RegisterResult restore_previous_runtime(std::string* error) { return from_exit(run_elevated(L"--unregister-runtime"), error); }

int register_runtime_command(bool elevated_child) {
    if (!elevated_child && !is_elevated()) return run_elevated(L"--register-runtime");
    return do_register();
}

int unregister_runtime_command(bool remove, bool elevated_child) {
    if (!elevated_child && !is_elevated()) return run_elevated(remove ? L"--unregister-runtime --remove" : L"--unregister-runtime");
    return do_unregister(remove);
}

}  // namespace oxs
