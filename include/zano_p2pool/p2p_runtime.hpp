#pragma once

#include "zano_p2pool/p2p_peer_score.hpp"
#include "zano_p2pool/p2p_transport.hpp"
#include "zano_p2pool/token_bucket.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace zano_p2pool {

struct P2pRuntimeConfig {
    P2pEndpoint listen_endpoint{"127.0.0.1", 0};
    P2pHandshake handshake{};
    std::chrono::milliseconds outbound_reconnect_initial{250};
    std::chrono::milliseconds outbound_reconnect_max{5000};
    P2pPeerScoreConfig peer_score{};
    std::size_t max_peers{64};
    TokenBucketConfig inbound_message_rate_limit{512, 256.0};
};

using P2pMessageHandler = std::function<void(
    const P2pHandshake& peer,
    const P2pEnvelope& envelope)>;

// Long-lived transport runtime for a single P2Pool node. It owns the listener,
// inbound/outbound peer sockets, reader threads and shutdown ordering. Protocol
// semantics (share validation, sync, mining-context trust) deliberately remain
// outside this class and are supplied through the message callback.
class P2pRuntime {
public:
    explicit P2pRuntime(
        P2pRuntimeConfig config,
        P2pMessageHandler handler = {});
    ~P2pRuntime();

    P2pRuntime(const P2pRuntime&) = delete;
    P2pRuntime& operator=(const P2pRuntime&) = delete;

    void start();
    void stop() noexcept;

    // Establish one outbound peer using the same validated handshake path as
    // inbound connections. The initial dial remains synchronous so callers get
    // immediate success/failure feedback, but the endpoint is retained as a
    // managed outbound target. Startup failures and later disconnects are retried
    // in the background with bounded exponential backoff until stop().
    void connect_peer(const P2pEndpoint& endpoint);

    // Report a protocol-level violation attributable to a validated public node
    // id. Normal socket closure is not a violation and must not call this API.
    // Crossing the configured threshold immediately disconnects that identity
    // and suppresses managed outbound reconnects until the temporary ban expires.
    void report_peer_misbehavior(
        const NodeId& peer_node_id,
        std::uint32_t penalty = kP2pProtocolViolationPenalty) noexcept;

    [[nodiscard]] std::uint32_t peer_score(
        const NodeId& peer_node_id) const noexcept;
    [[nodiscard]] bool peer_banned(
        const NodeId& peer_node_id) const noexcept;

    // Send to live connections advertising the exact public node id. Returns
    // true if at least one matching peer accepted the frame for sending.
    [[nodiscard]] bool send_to(
        const NodeId& peer_node_id,
        const P2pEnvelope& envelope) noexcept;

    // Best-effort broadcast to currently live peers. A failed send disconnects
    // that peer; other peers still receive the message.
    void broadcast(const P2pEnvelope& envelope) noexcept;

    // Best-effort broadcast excluding every connection that advertises the
    // supplied node id. This is the relay primitive used by gossip so a frame
    // is never immediately echoed back to the peer that supplied it.
    void broadcast_except(
        const NodeId& excluded_node_id,
        const P2pEnvelope& envelope) noexcept;

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] std::uint16_t listen_port() const noexcept;
    [[nodiscard]] P2pHandshake local_handshake() const;
    [[nodiscard]] std::size_t peer_count() const noexcept;

private:
    struct Peer;
    struct OutboundTarget;

    void accept_loop() noexcept;
    void reconnect_loop() noexcept;
    [[nodiscard]] std::shared_ptr<Peer> add_peer(P2pTcpConnection connection);
    void reap_dead_peers() noexcept;
    void peer_loop(const std::shared_ptr<Peer>& peer) noexcept;
    [[nodiscard]] bool send_peer(
        const std::shared_ptr<Peer>& peer,
        const P2pEnvelope& envelope) noexcept;

    P2pRuntimeConfig config_;
    P2pMessageHandler handler_;
    P2pPeerScoreBook peer_scores_;

    std::atomic<bool> running_{false};
    std::unique_ptr<P2pTcpListener> listener_;
    std::thread accept_thread_;
    std::thread reconnect_thread_;

    mutable std::mutex peers_mutex_;
    std::vector<std::shared_ptr<Peer>> peers_;

    mutable std::mutex reconnect_mutex_;
    std::condition_variable reconnect_cv_;
    std::vector<std::shared_ptr<OutboundTarget>> outbound_targets_;
};

}  // namespace zano_p2pool
