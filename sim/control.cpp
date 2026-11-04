// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
#include "control.hpp"

#include <fcntl.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <format>

namespace oxs {

namespace {

constexpr size_t kLineMax = 64 * 1024;   // a command is a few words; anything longer is not one of ours

bool write_all(int fd, const std::string& s) {
    size_t done = 0;
    while (done < s.size()) {
        const ssize_t n = ::send(fd, s.data() + done, s.size() - done, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        done += static_cast<size_t>(n);
    }
    return true;
}

}  // namespace

ControlSocket::~ControlSocket() {
    for (const Client& c : clients_) ::close(c.fd);
    if (listen_ >= 0) ::close(listen_);
}

bool ControlSocket::open(std::string* error) {
    // THE NAME: this user, then a random id -- the simulator's pid is the application's namespace's, not
    // necessarily the one the reader sees.
    unsigned char id[8];
    if (::getrandom(id, sizeof id, 0) != static_cast<ssize_t>(sizeof id)) {
        *error = std::format("getrandom: {}", std::strerror(errno));
        return false;
    }
    name_ = std::format("openxr-sim/{}/", ::getuid());
    for (unsigned char b : id) name_ += std::format("{:02x}", b);

    listen_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (listen_ < 0) { *error = std::format("socket: {}", std::strerror(errno)); return false; }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path + 1, name_.data(), name_.size());   // sun_path[0] = 0: abstract
    const socklen_t len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name_.size());
    if (::bind(listen_, reinterpret_cast<sockaddr*>(&addr), len) != 0 || ::listen(listen_, 4) != 0) {
        *error = std::format("the control socket: {}", std::strerror(errno));
        ::close(listen_);
        listen_ = -1;
        return false;
    }
    return true;
}

int ControlSocket::accept_client() {
    const int fd = ::accept4(listen_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (fd < 0) return -1;
    ucred cred{};
    socklen_t cl = sizeof cred;
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &cl) != 0 || cred.uid != ::getuid()) {
        ::close(fd);
        return -1;
    }
    clients_.push_back({ fd, {} });
    return fd;
}

bool ControlSocket::serve(int fd, const Handler& handle) {
    auto it = std::find_if(clients_.begin(), clients_.end(), [fd](const Client& c) { return c.fd == fd; });
    if (it == clients_.end()) return false;
    bool alive = true;
    for (;;) {
        char buf[4096];
        const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
        if (n > 0) { it->in.append(buf, static_cast<size_t>(n)); continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        alive = false;   // closed, or an error
        break;
    }
    for (size_t nl; alive && (nl = it->in.find('\n')) != std::string::npos;) {
        std::string line = it->in.substr(0, nl);
        it->in.erase(0, nl + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        alive = write_all(fd, handle(line) + "\n");
    }
    if (alive && it->in.size() > kLineMax) alive = false;
    if (!alive) {
        ::close(fd);
        clients_.erase(it);
    }
    return alive;
}

}  // namespace oxs
