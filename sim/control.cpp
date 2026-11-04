// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
#include "control.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <afunix.h>
#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>
using sock_t = SOCKET;
constexpr sock_t kNoSocket = INVALID_SOCKET;
#define poll WSAPoll
static int last_error() { return WSAGetLastError(); }
static void close_socket(sock_t s) { closesocket(s); }
static bool would_block(int e) { return e == WSAEWOULDBLOCK; }
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
using sock_t = int;
constexpr sock_t kNoSocket = -1;
static int last_error() { return errno; }
static void close_socket(sock_t s) { ::close(s); }
static bool would_block(int e) { return e == EAGAIN || e == EWOULDBLOCK; }
#endif

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <format>
#include <vector>

namespace oxs {

namespace {

constexpr size_t kLineMax = 64 * 1024;   // a command is a few words; anything longer is not one of ours

bool write_all(sock_t fd, const std::string& s) {
    size_t done = 0;
    while (done < s.size()) {
#ifdef _WIN32
        const int n = ::send(fd, s.data() + done, static_cast<int>(s.size() - done), 0);
#else
        const ssize_t n = ::send(fd, s.data() + done, s.size() - done, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
#endif
        if (n <= 0) return false;
        done += static_cast<size_t>(n);
    }
    return true;
}

void set_nonblocking(sock_t s) {
#ifdef _WIN32
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
    fcntl(s, F_SETFD, FD_CLOEXEC);
#endif
}

bool random_id(std::string& out, std::string* error) {
    unsigned char id[8];
#ifdef _WIN32
    if (BCryptGenRandom(nullptr, id, sizeof id, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) { *error = "BCryptGenRandom failed"; return false; }
#else
    if (::getrandom(id, sizeof id, 0) != static_cast<ssize_t>(sizeof id)) {
        *error = std::format("getrandom: {}", std::strerror(errno));
        return false;
    }
#endif
    for (unsigned char b : id) out += std::format("{:02x}", b);
    return true;
}

// The peer is this user: always so on Windows, where only this user can reach the directory.
bool peer_is_us(sock_t fd) {
#ifdef _WIN32
    (void)fd;
    return true;
#else
    ucred cred{};
    socklen_t cl = sizeof cred;
    return ::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &cl) == 0 && cred.uid == ::getuid();
#endif
}

}  // namespace

ControlSocket::~ControlSocket() { stop(); }

bool ControlSocket::start(Handler run_on_window, std::string* error) {
    handler_ = std::move(run_on_window);
    std::string id;
    if (!random_id(id, error)) return false;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    int len = 0;
#ifdef _WIN32
    static const bool wsa = [] { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d) == 0; }();
    if (!wsa) { *error = "WSAStartup failed"; return false; }
    PWSTR base = nullptr;
    if (SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base) != S_OK) { *error = "no %LOCALAPPDATA%"; return false; }
    char narrow[MAX_PATH * 3] = {};
    WideCharToMultiByte(CP_UTF8, 0, base, -1, narrow, sizeof narrow, nullptr, nullptr);
    CoTaskMemFree(base);
    const std::string dir = std::string(narrow) + "\\openxr-sim";
    CreateDirectoryA(dir.c_str(), nullptr);
    path_ = dir + "\\" + id + ".sock";
    name_ = path_;
    if (path_.size() + 1 > sizeof addr.sun_path) { *error = "the socket's path is too long: " + path_; return false; }
    std::memcpy(addr.sun_path, path_.c_str(), path_.size() + 1);
    len = static_cast<int>(sizeof addr);
    const sock_t s = ::socket(AF_UNIX, SOCK_STREAM, 0);
#else
    // THE NAME: this user, then a random id -- the simulator's pid is the application's namespace's, not
    // necessarily the one the reader sees.
    name_ = std::format("openxr-sim/{}/{}", ::getuid(), id);
    std::memcpy(addr.sun_path + 1, name_.data(), name_.size());   // sun_path[0] = 0: abstract
    len = static_cast<int>(offsetof(sockaddr_un, sun_path) + 1 + name_.size());
    const sock_t s = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
#endif
    if (s == kNoSocket) { *error = std::format("socket: error {}", last_error()); return false; }
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), len) != 0 || ::listen(s, 4) != 0) {
        *error = std::format("the control socket: error {}", last_error());
        close_socket(s);
        return false;
    }
    set_nonblocking(s);
    listen_ = static_cast<intptr_t>(s);
    stop_ = false;
    thread_ = std::thread([this] { serve(); });
    return true;
}

void ControlSocket::stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    if (listen_ != -1) { close_socket(static_cast<sock_t>(listen_)); listen_ = -1; }
#ifdef _WIN32
    if (!path_.empty()) DeleteFileA(path_.c_str());
#endif
}

void ControlSocket::serve() {
    struct Client { sock_t fd; std::string in; };
    std::vector<Client> clients;
    const sock_t ls = static_cast<sock_t>(listen_);
    while (!stop_) {
        std::vector<pollfd> fds;
        fds.push_back({ ls, POLLIN, 0 });
        for (const Client& c : clients) fds.push_back({ c.fd, POLLIN, 0 });
        // A fifth of a second at most, so stop() is answered that soon.
        const int r = ::poll(fds.data(), static_cast<unsigned long>(fds.size()), 200);
        if (r <= 0) continue;
        if (fds[0].revents & POLLIN) {
            for (;;) {
                const sock_t fd = ::accept(ls, nullptr, nullptr);
                if (fd == kNoSocket) break;
                if (!peer_is_us(fd)) { close_socket(fd); continue; }
                set_nonblocking(fd);
                clients.push_back({ fd, {} });
            }
        }
        for (size_t i = 1; i < fds.size(); i++) {
            if (fds[i].revents == 0) continue;
            auto it = std::find_if(clients.begin(), clients.end(), [&](const Client& c) { return c.fd == fds[i].fd; });
            if (it == clients.end()) continue;
            bool alive = true;
            for (;;) {
                char buf[4096];
                const auto n = ::recv(it->fd, buf, sizeof buf, 0);
                if (n > 0) { it->in.append(buf, static_cast<size_t>(n)); continue; }
                if (n < 0 && would_block(last_error())) break;
#ifndef _WIN32
                if (n < 0 && errno == EINTR) continue;
#endif
                alive = false;   // closed, or an error
                break;
            }
            for (size_t nl; alive && !stop_ && (nl = it->in.find('\n')) != std::string::npos;) {
                std::string line = it->in.substr(0, nl);
                it->in.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;
                alive = write_all(it->fd, handler_(line) + "\n");
            }
            if (alive && it->in.size() > kLineMax) alive = false;
            if (!alive) {
                close_socket(it->fd);
                clients.erase(it);
            }
        }
    }
    for (const Client& c : clients) close_socket(c.fd);
}

}  // namespace oxs
