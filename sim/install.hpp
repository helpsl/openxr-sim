// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
#pragma once

// THE ACTIVE OPENXR RUNTIME: which one the OpenXR loader gives applications, and making it this one.
// install_win32.cpp and install_posix.cpp; docs/NOTES.md, "Making it the active runtime", says why each
// works the way it does.
//
//   Windows  The loader reads HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime, the path of a manifest, and
//            nothing per user. So making it active is installing it: this .exe copies itself and unpacks
//            the runtime it carries (openxr_sim.dll, a resource of this build: sim/CMakeLists.txt) into
//            %ProgramFiles%\openxr-sim\<version>\, writes the manifest beside them, points ActiveRuntime
//            there and adds an entry to Settings > Apps. That needs an administrator: the window starts
//            this program again elevated (a UAC prompt) with --register-runtime, and waits for it.
//   Linux    The loader reads $XDG_CONFIG_HOME/openxr/1/active_runtime.json before the system's, so making
//            it active is a link there to the installed manifest, for this user only, with no rights asked.
//
// Either way the runtime it replaces is remembered, and restoring puts that one back.

#include <string>

namespace oxs {

struct RuntimeState {
    bool can_register = false;   // there is an installed runtime to point at (Linux), or one to install (Windows)
    bool active_is_ours = false;
    bool ours_is_this_build = false;   // the registered copy is this version (Windows: an update is offered)
    std::string active_name;     // the active runtime's name from its manifest; empty when there is none
    std::string ours_version;    // the registered copy's version, when it is ours
    std::string previous_name;   // what restoring would make active again; empty when there is nothing
    std::string why_not;         // when !can_register: why
    // Linux: the active runtime is the system's choice (/etc/xdg), not one this user made -- nothing here
    // can take it back, so nothing is offered. Its file is `active_path`.
    bool system_choice = false;
    std::string active_path;
};
RuntimeState runtime_state();

enum class RegisterResult { Done, Cancelled, Failed };
// From the window: may ask for rights (Windows' UAC prompt) and waits for the answer, so it is called off
// the window's thread. `error` says what went wrong when the result is Failed.
RegisterResult make_runtime_active(std::string* error);
RegisterResult restore_previous_runtime(std::string* error);

// THE COMMAND LINE (main.cpp): --register-runtime, --unregister-runtime, and --unregister-runtime --remove
// (Windows: also the files and the Settings > Apps entry -- what that entry's Uninstall runs). Each asks for
// rights itself when it has none. The exit status is a RegisterExit.
enum RegisterExit { kRegisterOk = 0, kRegisterFailed = 1, kRegisterNoRights = 2, kRegisterInUse = 3, kRegisterCancelled = 4 };
int register_runtime_command(bool elevated_child);
int unregister_runtime_command(bool remove, bool elevated_child);

}  // namespace oxs
