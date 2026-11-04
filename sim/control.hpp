// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
#pragma once

// THE SIMULATOR DRIVEN BY A PROGRAM (DESIGN.md, "The control socket"): what openxr-sim-mcp talks to. A
// headset-mode simulator listens on a Unix socket and answers one line for every line it is sent: a
// command in, one JSON object out.
//
// ON LINUX the socket is ABSTRACT, `\0openxr-sim/<uid>/<id>`, because the simulator is the application's
// child and so inside the application's sandbox: a confined application may not be able to make a file in
// $XDG_RUNTIME_DIR, and the sandbox has no network namespace, so an abstract name is reachable from outside
// it. It is also reachable by every other user on the machine -- so a connection whose peer is not this
// user is closed at once (SO_PEERCRED). Found by name in /proc/net/unix, which lists abstract sockets with a
// leading '@'.
//
// ON WINDOWS (AF_UNIX, Windows 10 1803 and later) it is a file, %LOCALAPPDATA%\openxr-sim\<id>.sock, in a
// directory only this user can open; the MCP server lists the directory.
//
// ITS OWN THREAD accepts and reads; each whole line is handed to the window's thread (run_on_window) and the
// answer waited for, so a command runs between frames, never beside one.

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace oxs {

class ControlSocket {
public:
    // Runs `line` on the window's thread and returns the answer: the window queues it and wakes its loop.
    using Handler = std::function<std::string(const std::string& line)>;

    ControlSocket() = default;
    ~ControlSocket();
    ControlSocket(const ControlSocket&) = delete;
    ControlSocket& operator=(const ControlSocket&) = delete;

    // Listens, and starts the thread. False, with why; the simulator goes on without it.
    bool start(Handler run_on_window, std::string* error);
    void stop();
    // The name the MCP server finds it by: the abstract name without its NUL, or the file's path.
    const std::string& name() const { return name_; }

private:
    void serve();
    Handler handler_;
    std::thread thread_;
    std::atomic<bool> stop_{ false };
    intptr_t listen_ = -1;
    std::string name_;
    std::string path_;   // Windows: the socket's file, removed when it stops
};

}  // namespace oxs
