#pragma once

// Live telemetry over HTTP, for a second screen. A background thread serves:
//   GET /          the dashboard page (telemetry_page.h)
//   GET /stream    Server-Sent Events: the latest JSON snapshot, 20 times a second
//   GET /snapshot  the latest JSON snapshot once
//   GET /3d        the 3D view of the ground (view3d_page.h)
//   any path given to publish_resource(), e.g. /terrain.bin
// The game calls publish() every frame. No dependencies beyond POSIX sockets; on Windows it is a stub.

#include <atomic>
#include <cstdint>
#include <map>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <poll.h>
#endif

#include "telemetry_page.h"
#include "view3d_page.h"

namespace telemetry {

class Server {
public:
    Server() = default;
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    ~Server() { stop(); }

    // Port 0 picks a free one. lan = false listens on 127.0.0.1 only; true on every interface.
    bool start(int port, bool lan);
    void stop();
    int port() const { return port_; }
    bool running() const { return running_; }

    void publish(std::string json) {
        std::lock_guard<std::mutex> lock(mu_);
        json_ = std::move(json);
        ++version_;
    }
    // Any other data the game wants to serve, replaced whole each time (e.g. the ground for the 3D view).
    void publish_resource(const std::string& path, std::string content_type, std::string body) {
        std::lock_guard<std::mutex> lock(mu_);
        resources_[path] = {std::move(content_type), std::move(body)};
    }

private:
    std::string latest(std::uint64_t* version) {
        std::lock_guard<std::mutex> lock(mu_);
        if (version) *version = version_;
        return json_;
    }
    void accept_loop();
    void serve(int fd);

    struct Resource {
        std::string type, body;
    };
    bool resource(const std::string& path, Resource& out) {
        std::lock_guard<std::mutex> lock(mu_);
        const auto it = resources_.find(path);
        if (it == resources_.end()) return false;
        out = it->second;
        return true;
    }

    std::mutex mu_;
    std::map<std::string, Resource> resources_;
    std::string json_ = "{}";
    std::uint64_t version_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<int> clients_{0};
    int listen_fd_ = -1;
    int port_ = 0;
    std::thread acceptor_;
};

#ifdef _WIN32

inline bool Server::start(int, bool) { return false; }
inline void Server::stop() {}

#else

namespace detail {

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;  // a viewer closing its tab must not kill the game with SIGPIPE
#else
constexpr int kSendFlags = 0;
#endif

inline bool send_all(int fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, kSendFlags);
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

inline void respond(int fd, const char* status, const char* type, const std::string& body) {
    send_all(fd, std::string("HTTP/1.1 ") + status + "\r\nContent-Type: " + type +
                     "\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: " + std::to_string(body.size()) +
                     "\r\n\r\n" + body);
}

}  // namespace detail

inline bool Server::start(int port, bool lan) {
    if (running_) return true;
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) return false;
    const int yes = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    addr.sin_addr.s_addr = htonl(lan ? INADDR_ANY : INADDR_LOOPBACK);
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || ::listen(listen_fd_, 8) != 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    socklen_t len = sizeof addr;
    ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
    port_ = ntohs(addr.sin_port);
    running_ = true;
    acceptor_ = std::thread([this] { accept_loop(); });
    return true;
}

inline void Server::stop() {
    if (!running_) return;
    running_ = false;
    if (acceptor_.joinable()) acceptor_.join();
    ::close(listen_fd_);
    listen_fd_ = -1;
    // Clients notice within about a second: the stream checks running_ every 50 ms, a request read within one recv
    // timeout (1 s), a stalled send within the send timeout (1 s).
    while (clients_ > 0) std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

inline void Server::accept_loop() {
    constexpr int kMaxClients = 16;
    while (running_) {
        pollfd p{listen_fd_, POLLIN, 0};
        if (::poll(&p, 1, 100) <= 0) continue;
        const int fd = ::accept(listen_fd_, nullptr, nullptr);
        if (fd < 0) continue;
        if (clients_ >= kMaxClients) {
            ::close(fd);
            continue;
        }
#ifdef SO_NOSIGPIPE
        const int yes = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof yes);
#endif
        timeval timeout{1, 0};  // a stalled viewer cannot hold a thread for long
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
        ++clients_;
        std::thread([this, fd] {
            serve(fd);
            ::close(fd);
            --clients_;
        }).detach();
    }
}

inline void Server::serve(int fd) {
    // Read the request head, giving up after 2 s or on shutdown, so a client trickling bytes cannot hold a
    // thread (or stop()) for longer than that.
    std::string request;
    char buf[1024];
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (request.find("\r\n\r\n") == std::string::npos && request.size() < 8192) {
        if (!running_ || std::chrono::steady_clock::now() > deadline) return;
        const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
        if (n <= 0) return;
        request.append(buf, static_cast<std::size_t>(n));
    }
    const std::size_t sp1 = request.find(' '), sp2 = request.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos || request.compare(0, sp1, "GET") != 0) {
        detail::respond(fd, "405 Method Not Allowed", "text/plain", "GET only\n");
        return;
    }
    std::string path = request.substr(sp1 + 1, sp2 - sp1 - 1);
    path = path.substr(0, path.find('?'));

    Resource res;
    if (path == "/" || path == "/index.html") {
        detail::respond(fd, "200 OK", "text/html; charset=utf-8", kPage);
    } else if (path == "/3d") {
        detail::respond(fd, "200 OK", "text/html; charset=utf-8", kView3dPage);
    } else if (resource(path, res)) {
        detail::respond(fd, "200 OK", res.type.c_str(), res.body);
    } else if (path == "/snapshot") {
        detail::respond(fd, "200 OK", "application/json", latest(nullptr));
    } else if (path == "/stream") {
        if (!detail::send_all(fd, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-store\r\n"
                                  "Connection: keep-alive\r\n\r\nretry: 1000\n\n"))
            return;
        std::uint64_t sent = ~0ull;
        auto quiet = std::chrono::steady_clock::now();
        while (running_) {
            std::uint64_t version = 0;
            const std::string json = latest(&version);
            const auto now = std::chrono::steady_clock::now();
            if (version != sent) {
                if (!detail::send_all(fd, "data: " + json + "\n\n")) return;
                sent = version;
                quiet = now;
            } else if (now - quiet > std::chrono::seconds(5)) {  // keep the connection alive while the game is paused
                if (!detail::send_all(fd, ": idle\n\n")) return;
                quiet = now;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    } else {
        detail::respond(fd, "404 Not Found", "text/plain", "not found\n");
    }
}

#endif

}  // namespace telemetry
