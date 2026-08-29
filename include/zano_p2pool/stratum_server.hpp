#pragma once

#include "zano_p2pool/share_chain.hpp"
#include "zano_p2pool/stratum_session.hpp"
#include "zano_p2pool/stratum_submission.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace zano_p2pool {

struct StratumServerConfig {
    std::string bind_address{"127.0.0.1"};
    std::uint16_t port{0};
    std::size_t max_line_bytes{64 * 1024};
    StratumSessionConfig sessions{};
};

// Development Stratum listener. It defaults to loopback only. The transport is
// newline-delimited JSON-RPC 2.0, matching current Zano miner expectations.
// Session/share-chain state is serialized behind a mutex even though clients are
// handled concurrently, keeping consensus-facing mutations deterministic.
class StratumTcpServer {
public:
    explicit StratumTcpServer(StratumServerConfig config = {});
    ~StratumTcpServer();

    StratumTcpServer(const StratumTcpServer&) = delete;
    StratumTcpServer& operator=(const StratumTcpServer&) = delete;

    void start();
    void stop() noexcept;

    [[nodiscard]] bool running() const noexcept { return running_.load(); }
    [[nodiscard]] std::uint16_t bound_port() const noexcept {
        return bound_port_.load();
    }
    [[nodiscard]] const StratumServerConfig& config() const noexcept {
        return config_;
    }

    // Publish trusted, locally derived work for subsequent login/getWork calls.
    [[nodiscard]] std::uint64_t publish_template(
        const Hash256& header_hash,
        const Hash256& seed_hash,
        std::uint64_t height,
        const Difficulty128& network_difficulty);

    [[nodiscard]] std::size_t connected_share_count() const noexcept;
    [[nodiscard]] const ConnectedShare* best_tip() const noexcept;

private:
    void accept_loop();
    void client_loop(int client_fd, std::uint64_t session_id) noexcept;
    [[nodiscard]] std::string handle_line(
        std::uint64_t session_id,
        std::string_view line);
    [[nodiscard]] std::string handle_request(
        std::uint64_t session_id,
        const StratumRequest& request);

    void register_client_fd(int fd);
    void unregister_client_fd(int fd) noexcept;
    void close_all_clients() noexcept;

    StratumServerConfig config_{};
    mutable std::mutex state_mutex_;
    StratumSessionRegistry sessions_;
    ShareChain share_chain_;
    StratumSubmissionRouter submissions_;

    std::atomic<bool> running_{false};
    std::atomic<std::uint16_t> bound_port_{0};
    int listen_fd_{-1};
    std::thread accept_thread_;

    mutable std::mutex clients_mutex_;
    std::set<int> client_fds_;
    std::vector<std::thread> client_threads_;
};

}  // namespace zano_p2pool
