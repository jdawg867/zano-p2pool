#include "zano_p2pool/p2p_runtime.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace zano_p2pool {

struct P2pRuntime::Peer {
    explicit Peer(P2pTcpConnection value)
        : connection(std::move(value)) {}

    P2pTcpConnection connection;
    std::atomic<bool> alive{true};
    std::mutex send_mutex;
    std::thread reader_thread;
};

P2pRuntime::P2pRuntime(
    P2pRuntimeConfig config,
    P2pMessageHandler handler)
    : config_(std::move(config)),
      handler_(std::move(handler)) {}

P2pRuntime::~P2pRuntime() {
    stop();
}

void P2pRuntime::start() {
    if (running_.load()) {
        throw std::runtime_error("P2P runtime is already running");
    }

    auto listener = std::make_unique<P2pTcpListener>(
        config_.listen_endpoint,
        config_.handshake);
    listener->start();
    config_.handshake = listener->local_handshake();
    listener_ = std::move(listener);
    running_.store(true);

    try {
        accept_thread_ = std::thread(&P2pRuntime::accept_loop, this);
    } catch (...) {
        running_.store(false);
        listener_->stop();
        listener_.reset();
        throw;
    }
}

void P2pRuntime::stop() noexcept {
    if (!running_.exchange(false)) {
        return;
    }

    if (listener_) {
        listener_->stop();
    }

    std::vector<std::shared_ptr<Peer>> peers;
    {
        std::lock_guard lock(peers_mutex_);
        peers = peers_;
    }

    for (const auto& peer : peers) {
        peer->alive.store(false);
        std::lock_guard send_lock(peer->send_mutex);
        peer->connection.shutdown();
    }

    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }

    for (const auto& peer : peers) {
        if (peer->reader_thread.joinable()) {
            peer->reader_thread.join();
        }
        peer->connection.close();
    }

    {
        std::lock_guard lock(peers_mutex_);
        peers_.clear();
    }
    listener_.reset();
}

void P2pRuntime::connect_peer(const P2pEndpoint& endpoint) {
    if (!running_.load()) {
        throw std::runtime_error("P2P runtime is not running");
    }

    P2pTcpConnection connection = connect_p2p_peer(endpoint, config_.handshake);
    add_peer(std::move(connection));
}

bool P2pRuntime::send_to(
    const NodeId& peer_node_id,
    const P2pEnvelope& envelope) noexcept {
    std::vector<std::shared_ptr<Peer>> peers;
    {
        std::lock_guard lock(peers_mutex_);
        peers = peers_;
    }

    bool sent = false;
    for (const auto& peer : peers) {
        if (!peer->alive.load() ||
            peer->connection.peer_handshake().node_id != peer_node_id) {
            continue;
        }
        sent = send_peer(peer, envelope) || sent;
    }
    return sent;
}

void P2pRuntime::broadcast(const P2pEnvelope& envelope) noexcept {
    std::vector<std::shared_ptr<Peer>> peers;
    {
        std::lock_guard lock(peers_mutex_);
        peers = peers_;
    }

    for (const auto& peer : peers) {
        if (peer->alive.load()) {
            static_cast<void>(send_peer(peer, envelope));
        }
    }
}

void P2pRuntime::broadcast_except(
    const NodeId& excluded_node_id,
    const P2pEnvelope& envelope) noexcept {
    std::vector<std::shared_ptr<Peer>> peers;
    {
        std::lock_guard lock(peers_mutex_);
        peers = peers_;
    }

    for (const auto& peer : peers) {
        if (!peer->alive.load() ||
            peer->connection.peer_handshake().node_id == excluded_node_id) {
            continue;
        }
        static_cast<void>(send_peer(peer, envelope));
    }
}

bool P2pRuntime::running() const noexcept {
    return running_.load();
}

std::uint16_t P2pRuntime::listen_port() const noexcept {
    return listener_ ? listener_->port() : 0;
}

P2pHandshake P2pRuntime::local_handshake() const {
    return config_.handshake;
}

std::size_t P2pRuntime::peer_count() const noexcept {
    std::lock_guard lock(peers_mutex_);
    return static_cast<std::size_t>(std::count_if(
        peers_.begin(),
        peers_.end(),
        [](const std::shared_ptr<Peer>& peer) {
            return peer->alive.load();
        }));
}

void P2pRuntime::accept_loop() noexcept {
    while (running_.load()) {
        try {
            P2pTcpConnection connection = listener_->accept_peer();
            if (!running_.load()) {
                connection.close();
                break;
            }
            add_peer(std::move(connection));
        } catch (...) {
            if (!running_.load()) {
                break;
            }
        }
    }
}

void P2pRuntime::add_peer(P2pTcpConnection connection) {
    auto peer = std::make_shared<Peer>(std::move(connection));
    {
        std::lock_guard lock(peers_mutex_);
        peers_.push_back(peer);
    }

    try {
        peer->reader_thread = std::thread(&P2pRuntime::peer_loop, this, peer);
    } catch (...) {
        peer->alive.store(false);
        peer->connection.close();
        throw;
    }
}

void P2pRuntime::peer_loop(const std::shared_ptr<Peer>& peer) noexcept {
    while (running_.load() && peer->alive.load()) {
        try {
            const P2pEnvelope envelope = peer->connection.receive_envelope();
            if (handler_) {
                handler_(peer->connection.peer_handshake(), envelope);
            }
        } catch (...) {
            break;
        }
    }

    peer->alive.store(false);
    peer->connection.shutdown();
}

bool P2pRuntime::send_peer(
    const std::shared_ptr<Peer>& peer,
    const P2pEnvelope& envelope) noexcept {
    try {
        std::lock_guard send_lock(peer->send_mutex);
        if (!peer->alive.load()) {
            return false;
        }
        peer->connection.send_envelope(envelope);
        return true;
    } catch (...) {
        peer->alive.store(false);
        peer->connection.shutdown();
        return false;
    }
}

}  // namespace zano_p2pool
