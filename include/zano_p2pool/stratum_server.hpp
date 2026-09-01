#pragma once

#include "zano_p2pool/share_chain.hpp"
#include "zano_p2pool/stratum_session.hpp"
#include "zano_p2pool/stratum_submission.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace zano_p2pool {

struct StratumServerConfig {
    std::string bind_address{"127.0.0.1"};
    std::uint16_t port{3333};
    std::size_t max_line_bytes{64 * 1024};
    StratumSessionConfig sessions{};
};

using StratumAcceptedShareHandler =
    std::function<void(const Share& share, bool block_candidate)>;

// Development Stratum listener. It defaults to loopback only. The transport is
// newline-delimited JSON-RPC 2.0, matching current Zano miner expectations.
// Session/share-chain state is serialized behind a mutex even though clients are
// handled concurrently, keeping consensus-facing mutations deterministic.
class StratumTcpServer {
public:
    // When shared_chain is null the server preserves the standalone behavior
    // and owns an internal chain. A full P2Pool node supplies its node-wide
    // ShareChain and the mutex protecting that chain so Stratum and P2P operate
    // on exactly the same verified, serialized consensus state. accepted_share
    // runs only after a successful share is connected and all consensus locks
    // have been released.
    explicit StratumTcpServer(
        StratumServerConfig config = {},
        ShareChain* shared_chain = nullptr,
        std::mutex* shared_chain_mutex = nullptr,
        StratumAcceptedShareHandler accepted_share = {});
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

    // Publish trusted, locally derived work and proactively push a fresh work
    // notification to every currently logged-in miner connection.
    [[nodiscard]] std::uint64_t publish_template(
        const Hash256& header_hash,
        const Hash256& seed_hash,
        std::uint64_t height,
        const Difficulty128& network_difficulty);

    [[nodiscard]] std::size_t connected_share_count() const noexcept;

    // Read-only observability snapshots. These methods never issue work or
    // mutate session/consensus state.
    [[nodiscard]] std::size_t client_count() const;
    [[nodiscard]] std::uint64_t current_template_version() const;

private:
    void accept_loop();
    void client_loop(int client_fd, std::uint64_t session_id) noexcept;
    [[nodiscard]] std::string handle_line(
        std::uint64_t session_id,
        std::string_view line);
    [[nodiscard]] std::string handle_request(
        std::uint64_t session_id,
        const StratumRequest& request);

    // Called while state_mutex_ is held. If a consensus-configured shared chain
    // is present, this also snapshots that chain under its consensus mutex and
    // passes the exact branch-derived difficulty into the session work target.
    [[nodiscard]] StratumIssuedWork issue_work(std::uint64_t session_id);

    void register_client_fd(int fd, std::uint64_t session_id);
    void unregister_client_fd(int fd) noexcept;
    void close_all_clients() noexcept;

    StratumServerConfig config_{};
    mutable std::mutex state_mutex_;
    StratumSessionRegistry sessions_;
    std::unique_ptr<ShareChain> owned_share_chain_;
    ShareChain* share_chain_{nullptr};
    std::mutex* shared_chain_mutex_{nullptr};
    StratumSubmissionRouter submissions_;
    StratumAcceptedShareHandler accepted_share_handler_;

    std::atomic<bool> running_{false};
    std::atomic<std::uint16_t> bound_port_{0};
    int listen_fd_{-1};
    std::thread accept_thread_;

    mutable std::mutex clients_mutex_;
    std::map<int, std::uint64_t> client_sessions_;
    std::vector<std::thread> client_threads_;
};

}  // namespace zano_p2pool
