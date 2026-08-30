#pragma once

#include "zano_p2pool/p2p_transport.hpp"

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace zano_p2pool {

struct P2pRuntimeConfig {
    P2pEndpoint listen_endpoint{"127.0.0.1", 0};
    P2pHandshake handshake{};
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
    // inbound connections, then start its managed reader loop.
    void connect_peer(const P2pEndpoint& endpoint);

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

    void accept_loop() noexcept;
    void add_peer(P2pTcpConnection connection);
    void peer_loop(const std::shared_ptr<Peer>& peer) noexcept;
    [[nodiscard]] bool send_peer(
        const std::shared_ptr<Peer>& peer,
        const P2pEnvelope& envelope) noexcept;

    P2pRuntimeConfig config_;
    P2pMessageHandler handler_;

    std::atomic<bool> running_{false};
    std::unique_ptr<P2pTcpListener> listener_;
    std::thread accept_thread_;

    mutable std::mutex peers_mutex_;
    std::vector<std::shared_ptr<Peer>> peers_;
};

}  // namespace zano_p2pool
