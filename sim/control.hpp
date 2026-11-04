// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
#pragma once

// THE SIMULATOR DRIVEN BY A PROGRAM (DESIGN.md, "The control socket"): what openxr-sim-mcp talks to. A
// headset-mode simulator listens on an ABSTRACT Unix socket, `\0openxr-sim/<uid>/<id>`, and answers
// one line for every line it is sent: a command in, one JSON object out.
//
// ABSTRACT, because the simulator is the application's child and so inside the application's sandbox: a
// confined application may not be able to make a file in $XDG_RUNTIME_DIR, and the sandbox has no network
// namespace, so an abstract name is reachable from outside it. It is also reachable by every other user on
// the machine -- so a connection whose peer is not this user is closed at once (SO_PEERCRED).
//
// Found by name in /proc/net/unix, which lists abstract sockets with a leading '@'.

#include <functional>
#include <string>
#include <vector>

namespace oxs {

class ControlSocket {
public:
    using Handler = std::function<std::string(const std::string& line)>;

    ControlSocket() = default;
    ~ControlSocket();
    ControlSocket(const ControlSocket&) = delete;
    ControlSocket& operator=(const ControlSocket&) = delete;

    // Listens. False, with why; the simulator goes on without it.
    bool open(std::string* error);
    int  listen_fd() const { return listen_; }
    const std::string& name() const { return name_; }   // without the leading NUL

    // The listening socket is readable: the new client's descriptor, or -1 (refused, or nothing there).
    int accept_client();
    // A client's descriptor is readable: each whole line handed to `handle`, each answer written back.
    // False once the client has gone, and its descriptor closed -- stop watching it.
    bool serve(int fd, const Handler& handle);

private:
    struct Client { int fd; std::string in; };
    std::vector<Client> clients_;
    int listen_ = -1;
    std::string name_;
};

}  // namespace oxs
