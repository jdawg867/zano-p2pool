#include "zano_p2pool/metrics_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace zano_p2pool {
namespace {

[[nodiscard]] std::string socket_error(const char* operation) {
    return std::string(operation) + ": " + std::strerror(errno);
}

bool send_all(int fd, std::string_view data) noexcept {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const ssize_t sent = ::send(
            fd,
            data.data() + static_cast<std::ptrdiff_t>(offset),
            data.size() - offset,
            MSG_NOSIGNAL);
        if (sent > 0) {
            offset += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

[[nodiscard]] std::string http_response(
    std::string_view status,
    std::string_view content_type,
    std::string_view body,
    std::string_view extra_headers = {}) {
    std::string response;
    response.reserve(128 + body.size() + extra_headers.size());
    response += "HTTP/1.1 ";
    response += status;
    response += "\r\nContent-Type: ";
    response += content_type;
    response += "\r\nContent-Length: ";
    response += std::to_string(body.size());
    response += "\r\nConnection: close\r\n";
    response += extra_headers;
    response += "\r\n";
    response += body;
    return response;
}

struct RequestLine {
    std::string_view method;
    std::string_view path;
    std::string_view version;
};

[[nodiscard]] bool parse_request_line(
    std::string_view request,
    RequestLine& parsed) noexcept {
    const std::size_t line_end = request.find("\r\n");
    if (line_end == std::string_view::npos) {
        return false;
    }

    const std::string_view line = request.substr(0, line_end);
    const std::size_t first_space = line.find(' ');
    if (first_space == std::string_view::npos) {
        return false;
    }
    const std::size_t second_space = line.find(' ', first_space + 1);
    if (second_space == std::string_view::npos ||
        line.find(' ', second_space + 1) != std::string_view::npos) {
        return false;
    }

    parsed.method = line.substr(0, first_space);
    parsed.path = line.substr(first_space + 1, second_space - first_space - 1);
    parsed.version = line.substr(second_space + 1);
    return !parsed.method.empty() &&
           !parsed.path.empty() &&
           (parsed.version == "HTTP/1.1" || parsed.version == "HTTP/1.0");
}

}  // namespace

MetricsHttpServer::MetricsHttpServer(
    MetricsServerConfig config,
    MetricsSnapshotProvider snapshot_provider)
    : config_(std::move(config)),
      snapshot_provider_(std::move(snapshot_provider)) {
    if (config_.bind_address.empty()) {
        throw std::runtime_error("metrics bind address must not be empty");
    }
    if (config_.max_request_bytes == 0) {
        throw std::runtime_error("metrics max request size must be nonzero");
    }
    if (!snapshot_provider_) {
        throw std::runtime_error("metrics snapshot provider must be configured");
    }
}

MetricsHttpServer::~MetricsHttpServer() {
    stop();
}

void MetricsHttpServer::start() {
    if (running_.load()) {
        throw std::runtime_error("metrics server is already running");
    }

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error(socket_error("socket"));
    }

    int reuse = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
        const std::string error = socket_error("setsockopt(SO_REUSEADDR)");
        ::close(fd);
        throw std::runtime_error(error);
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(config_.port);
    if (::inet_pton(AF_INET, config_.bind_address.c_str(), &address.sin_addr) != 1) {
        ::close(fd);
        throw std::runtime_error("invalid IPv4 metrics bind address");
    }

    if (::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        const std::string error = socket_error("bind");
        ::close(fd);
        throw std::runtime_error(error);
    }
    if (::listen(fd, SOMAXCONN) != 0) {
        const std::string error = socket_error("listen");
        ::close(fd);
        throw std::runtime_error(error);
    }

    sockaddr_in bound{};
    socklen_t bound_size = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &bound_size) != 0) {
        const std::string error = socket_error("getsockname");
        ::close(fd);
        throw std::runtime_error(error);
    }

    listen_fd_ = fd;
    bound_port_.store(ntohs(bound.sin_port));
    running_.store(true);

    try {
        accept_thread_ = std::thread(&MetricsHttpServer::accept_loop, this);
    } catch (...) {
        running_.store(false);
        bound_port_.store(0);
        ::close(listen_fd_);
        listen_fd_ = -1;
        throw;
    }
}

void MetricsHttpServer::stop() noexcept {
    if (!running_.exchange(false)) {
        return;
    }

    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }

    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
    bound_port_.store(0);
}

void MetricsHttpServer::accept_loop() noexcept {
    while (running_.load()) {
        sockaddr_in peer{};
        socklen_t peer_size = sizeof(peer);
        const int client_fd = ::accept(
            listen_fd_,
            reinterpret_cast<sockaddr*>(&peer),
            &peer_size);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (!running_.load() || errno == EBADF || errno == EINVAL) {
                break;
            }
            continue;
        }

        const timeval timeout{2, 0};
        static_cast<void>(::setsockopt(
            client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
        static_cast<void>(::setsockopt(
            client_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)));

        handle_client(client_fd);
        ::shutdown(client_fd, SHUT_RDWR);
        ::close(client_fd);
    }
}

void MetricsHttpServer::handle_client(int client_fd) noexcept {
    try {
        std::string request;
        request.reserve(std::min<std::size_t>(config_.max_request_bytes, 1024));
        std::array<char, 1024> buffer{};

        while (request.find("\r\n\r\n") == std::string::npos) {
            const ssize_t received = ::recv(
                client_fd,
                buffer.data(),
                buffer.size(),
                0);
            if (received > 0) {
                const std::size_t count = static_cast<std::size_t>(received);
                if (count > config_.max_request_bytes - request.size()) {
                    static_cast<void>(send_all(
                        client_fd,
                        http_response(
                            "413 Payload Too Large",
                            "text/plain; charset=utf-8",
                            "request too large\n")));
                    return;
                }
                request.append(buffer.data(), count);
                continue;
            }
            if (received < 0 && errno == EINTR) {
                continue;
            }
            return;
        }

        RequestLine parsed;
        if (!parse_request_line(request, parsed)) {
            static_cast<void>(send_all(
                client_fd,
                http_response(
                    "400 Bad Request",
                    "text/plain; charset=utf-8",
                    "bad request\n")));
            return;
        }

        if (parsed.method != "GET") {
            static_cast<void>(send_all(
                client_fd,
                http_response(
                    "405 Method Not Allowed",
                    "text/plain; charset=utf-8",
                    "method not allowed\n",
                    "Allow: GET\r\n")));
            return;
        }

        if (parsed.path == "/healthz") {
            static_cast<void>(send_all(
                client_fd,
                http_response(
                    "200 OK",
                    "text/plain; charset=utf-8",
                    "ok\n")));
            return;
        }

        if (parsed.path != "/metrics") {
            static_cast<void>(send_all(
                client_fd,
                http_response(
                    "404 Not Found",
                    "text/plain; charset=utf-8",
                    "not found\n")));
            return;
        }

        std::string body = snapshot_provider_();
        if (!body.empty() && body.back() != '\n') {
            body.push_back('\n');
        }
        static_cast<void>(send_all(
            client_fd,
            http_response(
                "200 OK",
                "text/plain; version=0.0.4; charset=utf-8",
                body)));
    } catch (...) {
        static_cast<void>(send_all(
            client_fd,
            http_response(
                "500 Internal Server Error",
                "text/plain; charset=utf-8",
                "metrics snapshot failed\n")));
    }
}

}  // namespace zano_p2pool
