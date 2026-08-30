#include "zano_p2pool/stratum_server.hpp"

#include "zano_p2pool/stratum_protocol.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace zano_p2pool {
namespace {

[[nodiscard]] std::uint64_t unix_time_seconds() noexcept {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(now).count());
}

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

[[nodiscard]] std::string submission_error_message(
    const StratumSubmissionResult& result) {
    switch (result.disposition) {
    case StratumSubmissionDisposition::Stale:
        return "stale work";
    case StratumSubmissionDisposition::Duplicate:
        return "duplicate work";
    case StratumSubmissionDisposition::UnknownWork:
        return "unknown work";
    case StratumSubmissionDisposition::Rejected:
        if (result.reject_reason != ShareRejectReason::None) {
            return share_reject_reason_name(result.reject_reason);
        }
        return "share rejected";
    case StratumSubmissionDisposition::AcceptedShare:
    case StratumSubmissionDisposition::BlockCandidate:
        break;
    }
    return "share rejected";
}

}  // namespace

StratumTcpServer::StratumTcpServer(
    StratumServerConfig config,
    ShareChain* shared_chain,
    std::mutex* shared_chain_mutex,
    StratumAcceptedShareHandler accepted_share)
    : config_(std::move(config)),
      sessions_(config_.sessions),
      owned_share_chain_(
          shared_chain == nullptr ? std::make_unique<ShareChain>() : nullptr),
      share_chain_(
          shared_chain != nullptr ? shared_chain : owned_share_chain_.get()),
      shared_chain_mutex_(shared_chain_mutex),
      submissions_(sessions_, *share_chain_),
      accepted_share_handler_(std::move(accepted_share)) {
    if (config_.bind_address.empty()) {
        throw std::runtime_error("Stratum bind address must not be empty");
    }
    if (config_.max_line_bytes == 0) {
        throw std::runtime_error("Stratum max line size must be nonzero");
    }
    if ((shared_chain == nullptr) != (shared_chain_mutex == nullptr)) {
        throw std::runtime_error(
            "shared Stratum chain and mutex must be supplied together");
    }
}

StratumTcpServer::~StratumTcpServer() {
    stop();
}

void StratumTcpServer::start() {
    if (running_.load()) {
        throw std::runtime_error("Stratum server is already running");
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
        throw std::runtime_error("invalid IPv4 Stratum bind address");
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
        accept_thread_ = std::thread(&StratumTcpServer::accept_loop, this);
    } catch (...) {
        running_.store(false);
        bound_port_.store(0);
        ::close(listen_fd_);
        listen_fd_ = -1;
        throw;
    }
}

void StratumTcpServer::stop() noexcept {
    if (!running_.exchange(false)) {
        return;
    }

    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    close_all_clients();

    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }

    for (std::thread& thread : client_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    client_threads_.clear();
    bound_port_.store(0);
}

std::uint64_t StratumTcpServer::publish_template(
    const Hash256& header_hash,
    const Hash256& seed_hash,
    std::uint64_t height,
    const Difficulty128& network_difficulty) {
    std::lock_guard lock(state_mutex_);
    return sessions_.publish_template(
        header_hash, seed_hash, height, network_difficulty);
}

std::size_t StratumTcpServer::connected_share_count() const noexcept {
    if (shared_chain_mutex_ != nullptr) {
        std::lock_guard lock(*shared_chain_mutex_);
        return share_chain_->connected_size();
    }

    std::lock_guard lock(state_mutex_);
    return share_chain_->connected_size();
}

void StratumTcpServer::accept_loop() {
    // start() publishes the descriptor before launching this thread. Keep a
    // private copy so stop() can reset the member after close without racing a
    // member read in the blocking accept call.
    const int listen_fd = listen_fd_;

    while (running_.load()) {
        sockaddr_in peer{};
        socklen_t peer_size = sizeof(peer);
        const int client_fd = ::accept(
            listen_fd, reinterpret_cast<sockaddr*>(&peer), &peer_size);
        if (client_fd < 0) {
            if (!running_.load()) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            continue;
        }

        if (!running_.load()) {
            ::close(client_fd);
            break;
        }

        std::uint64_t session_id = 0;
        try {
            {
                std::lock_guard lock(state_mutex_);
                session_id = sessions_.create_session();
            }
            register_client_fd(client_fd);
            client_threads_.emplace_back(
                &StratumTcpServer::client_loop, this, client_fd, session_id);
        } catch (...) {
            if (session_id != 0) {
                std::lock_guard lock(state_mutex_);
                sessions_.remove_session(session_id);
            }
            unregister_client_fd(client_fd);
            ::close(client_fd);
        }
    }
}

void StratumTcpServer::client_loop(
    int client_fd,
    std::uint64_t session_id) noexcept {
    std::string pending;
    pending.reserve(4096);
    std::array<char, 4096> buffer{};
    bool keep_running = true;

    while (running_.load() && keep_running) {
        const ssize_t received = ::recv(client_fd, buffer.data(), buffer.size(), 0);
        if (received == 0) {
            break;
        }
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        pending.append(buffer.data(), static_cast<std::size_t>(received));
        if (pending.size() > config_.max_line_bytes && pending.find('\n') == std::string::npos) {
            const std::string error = stratum_error_json(
                StratumId{}, kStratumErrorParse, "Stratum request line too large");
            static_cast<void>(send_all(client_fd, error));
            break;
        }

        while (true) {
            const std::size_t newline = pending.find('\n');
            if (newline == std::string::npos) {
                break;
            }
            if (newline > config_.max_line_bytes) {
                const std::string error = stratum_error_json(
                    StratumId{}, kStratumErrorParse, "Stratum request line too large");
                static_cast<void>(send_all(client_fd, error));
                keep_running = false;
                break;
            }

            std::string line = pending.substr(0, newline);
            pending.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.empty()) {
                continue;
            }

            const std::string response = handle_line(session_id, line);
            if (!response.empty() && !send_all(client_fd, response)) {
                keep_running = false;
                break;
            }
        }
    }

    {
        std::lock_guard lock(state_mutex_);
        sessions_.remove_session(session_id);
    }
    unregister_client_fd(client_fd);
    ::close(client_fd);
}

std::string StratumTcpServer::handle_line(
    std::uint64_t session_id,
    std::string_view line) {
    try {
        const StratumRequest request = parse_stratum_request(line);
        return handle_request(session_id, request);
    } catch (const std::exception& e) {
        return stratum_error_json(StratumId{}, kStratumErrorParse, e.what());
    }
}

std::string StratumTcpServer::handle_request(
    std::uint64_t session_id,
    const StratumRequest& request) {
    try {
        std::unique_lock state_lock(state_mutex_);

        switch (stratum_method(request)) {
        case StratumMethod::SubmitLogin: {
            const StratumLogin login = parse_stratum_login(request);
            sessions_.login(session_id, login);
            const StratumIssuedWork work = sessions_.issue_work(session_id);
            return stratum_success_json(request.id) +
                   stratum_work_notification_json(work.wire_work);
        }
        case StratumMethod::GetWork: {
            const StratumIssuedWork work = sessions_.issue_work(session_id);
            return stratum_work_json(request.id, work.wire_work);
        }
        case StratumMethod::SubmitHashrate: {
            const StratumSession* session = sessions_.find_session(session_id);
            if (session == nullptr || !session->logged_in) {
                return stratum_error_json(
                    request.id, kStratumErrorDefault, "session is not logged in");
            }
            return stratum_success_json(request.id);
        }
        case StratumMethod::SubmitWork: {
            const StratumSubmission submission = parse_stratum_submission(request);
            std::unique_lock<std::mutex> chain_lock;
            if (shared_chain_mutex_ != nullptr) {
                chain_lock = std::unique_lock<std::mutex>(*shared_chain_mutex_);
            }

            const StratumSubmissionResult result = submissions_.submit(
                session_id,
                submission,
                unix_time_seconds(),
                ProgPowZContextMode::Light);
            if (result.disposition == StratumSubmissionDisposition::AcceptedShare ||
                result.disposition == StratumSubmissionDisposition::BlockCandidate) {
                std::optional<Share> accepted_share;
                if (accepted_share_handler_) {
                    if (const ConnectedShare* connected =
                            share_chain_->find(result.share_id);
                        connected != nullptr) {
                        accepted_share = connected->share;
                    }
                }
                const bool block_candidate =
                    result.disposition == StratumSubmissionDisposition::BlockCandidate;

                if (chain_lock.owns_lock()) {
                    chain_lock.unlock();
                }
                state_lock.unlock();

                if (accepted_share.has_value() && accepted_share_handler_) {
                    try {
                        accepted_share_handler_(*accepted_share, block_candidate);
                    } catch (...) {
                        // Gossip/observer failures cannot revoke a share that
                        // has already passed consensus admission.
                    }
                }
                return stratum_success_json(request.id);
            }
            return stratum_error_json(
                request.id,
                kStratumErrorDefault,
                submission_error_message(result));
        }
        case StratumMethod::Unknown:
            return stratum_error_json(
                request.id,
                kStratumErrorMethodNotFound,
                "unknown method");
        }
    } catch (const std::exception& e) {
        return stratum_error_json(request.id, kStratumErrorDefault, e.what());
    }

    return stratum_error_json(
        request.id, kStratumErrorMethodNotFound, "unknown method");
}

void StratumTcpServer::register_client_fd(int fd) {
    std::lock_guard lock(clients_mutex_);
    client_fds_.insert(fd);
}

void StratumTcpServer::unregister_client_fd(int fd) noexcept {
    std::lock_guard lock(clients_mutex_);
    client_fds_.erase(fd);
}

void StratumTcpServer::close_all_clients() noexcept {
    std::lock_guard lock(clients_mutex_);
    for (const int fd : client_fds_) {
        ::shutdown(fd, SHUT_RDWR);
    }
}

}  // namespace zano_p2pool
