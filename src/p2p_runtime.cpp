#include "zano_p2pool/p2p_runtime.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

namespace zano_p2pool {
namespace {

[[nodiscard]] bool same_endpoint(
    const P2pEndpoint& left,
    const P2pEndpoint& right) noexcept {
    return left.host == right.host && left.port == right.port;
}

[[nodiscard]] std::chrono::milliseconds next_backoff(
    std::chrono::milliseconds current,
    std::chrono::milliseconds maximum) noexcept {
    if (current >= maximum) {
        return maximum;
    }

    const auto max_count = maximum.count();
    const auto current_count = current.count();
    if (current_count > max_count / 2) {
        return maximum;
    }
    return std::min(current * 2, maximum);
}

}  // namespace

struct P2pRuntime::Peer {
    explicit Peer(P2pTcpConnection value)
        : connection(std::move(value)) {}

    P2pTcpConnection connection;
    std::atomic<bool> alive{true};
    std::mutex send_mutex;
    std::thread reader_thread;
};

struct P2pRuntime::OutboundTarget {
    OutboundTarget(
        P2pEndpoint value,
        std::chrono::milliseconds initial_backoff)
        : endpoint(std::move(value)),
          backoff(initial_backoff),
          next_attempt(std::chrono::steady_clock::now()),
          attempt_in_progress(true) {}

    P2pEndpoint endpoint;
    std::shared_ptr<Peer> peer;
    std::optional<NodeId> node_id;
    std::chrono::milliseconds backoff;
    std::chrono::steady_clock::time_point next_attempt;
    bool attempt_in_progress{false};
};

P2pRuntime::P2pRuntime(
    P2pRuntimeConfig config,
    P2pMessageHandler handler)
    : config_(std::move(config)),
      handler_(std::move(handler)),
      peer_scores_(config_.peer_score) {}

P2pRuntime::~P2pRuntime() {
    stop();
}

void P2pRuntime::start() {
    if (running_.load()) {
        throw std::runtime_error("P2P runtime is already running");
    }
    if (config_.outbound_reconnect_initial.count() <= 0) {
        throw std::runtime_error(
            "P2P outbound reconnect initial delay must be positive");
    }
    if (config_.outbound_reconnect_max < config_.outbound_reconnect_initial) {
        throw std::runtime_error(
            "P2P outbound reconnect maximum delay must be >= initial delay");
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
        reconnect_thread_ = std::thread(&P2pRuntime::reconnect_loop, this);
    } catch (...) {
        running_.store(false);
        reconnect_cv_.notify_all();
        listener_->stop();
        if (accept_thread_.joinable()) {
            accept_thread_.join();
        }
        if (reconnect_thread_.joinable()) {
            reconnect_thread_.join();
        }
        listener_.reset();
        throw;
    }
}

void P2pRuntime::stop() noexcept {
    if (!running_.exchange(false)) {
        return;
    }

    reconnect_cv_.notify_all();
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
    if (reconnect_thread_.joinable()) {
        reconnect_thread_.join();
    }

    {
        std::lock_guard lock(peers_mutex_);
        peers = std::move(peers_);
        peers_.clear();
    }

    for (const auto& peer : peers) {
        peer->alive.store(false);
        peer->connection.shutdown();
        if (peer->reader_thread.joinable()) {
            peer->reader_thread.join();
        }
        peer->connection.close();
    }

    {
        std::lock_guard lock(reconnect_mutex_);
        outbound_targets_.clear();
    }
    listener_.reset();
}

void P2pRuntime::connect_peer(const P2pEndpoint& endpoint) {
    if (!running_.load()) {
        throw std::runtime_error("P2P runtime is not running");
    }

    auto target = std::make_shared<OutboundTarget>(
        endpoint,
        config_.outbound_reconnect_initial);
    {
        std::lock_guard lock(reconnect_mutex_);
        const bool duplicate_target = std::any_of(
            outbound_targets_.begin(),
            outbound_targets_.end(),
            [&](const std::shared_ptr<OutboundTarget>& existing) {
                return same_endpoint(existing->endpoint, endpoint);
            });
        if (duplicate_target) {
            throw std::runtime_error("P2P outbound endpoint is already managed");
        }
        outbound_targets_.push_back(target);
    }

    try {
        P2pTcpConnection connection = connect_p2p_peer(endpoint, config_.handshake);
        auto peer = add_peer(std::move(connection));
        if (!peer) {
            throw std::runtime_error(
                "P2P peer rejected: self-connection, duplicate, or banned node id");
        }
        const NodeId peer_node_id = peer->connection.peer_handshake().node_id;

        {
            std::lock_guard lock(reconnect_mutex_);
            target->peer = std::move(peer);
            target->node_id = peer_node_id;
            target->attempt_in_progress = false;
            target->backoff = config_.outbound_reconnect_initial;
            target->next_attempt =
                std::chrono::steady_clock::time_point::max();
        }
        reconnect_cv_.notify_all();
    } catch (...) {
        {
            std::lock_guard lock(reconnect_mutex_);
            target->attempt_in_progress = false;
            target->next_attempt =
                std::chrono::steady_clock::now() +
                config_.outbound_reconnect_initial;
            target->backoff = next_backoff(
                config_.outbound_reconnect_initial,
                config_.outbound_reconnect_max);
        }
        reconnect_cv_.notify_all();
        throw;
    }
}

void P2pRuntime::report_peer_misbehavior(
    const NodeId& peer_node_id,
    std::uint32_t penalty) noexcept {
    const P2pPeerPenaltyResult result =
        peer_scores_.penalize(peer_node_id, penalty);
    if (!result.ban_started) {
        return;
    }

    std::vector<std::shared_ptr<Peer>> banned_peers;
    {
        std::lock_guard lock(peers_mutex_);
        for (const auto& peer : peers_) {
            if (peer->alive.load() &&
                peer->connection.peer_handshake().node_id == peer_node_id) {
                banned_peers.push_back(peer);
            }
        }
    }

    for (const auto& peer : banned_peers) {
        peer->alive.store(false);
        std::lock_guard send_lock(peer->send_mutex);
        peer->connection.shutdown();
    }
    reconnect_cv_.notify_all();
}

std::uint32_t P2pRuntime::peer_score(
    const NodeId& peer_node_id) const noexcept {
    return peer_scores_.score(peer_node_id);
}

bool P2pRuntime::peer_banned(
    const NodeId& peer_node_id) const noexcept {
    return peer_scores_.banned(peer_node_id);
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
            static_cast<void>(add_peer(std::move(connection)));
        } catch (...) {
            if (!running_.load()) {
                break;
            }
        }
    }
}

void P2pRuntime::reconnect_loop() noexcept {
    using namespace std::chrono_literals;

    while (running_.load()) {
        reap_dead_peers();

        std::shared_ptr<OutboundTarget> target;
        {
            std::unique_lock lock(reconnect_mutex_);
            const auto now = std::chrono::steady_clock::now();

            for (const auto& candidate : outbound_targets_) {
                if (candidate->peer && !candidate->peer->alive.load()) {
                    candidate->node_id =
                        candidate->peer->connection.peer_handshake().node_id;
                    candidate->peer.reset();
                    candidate->next_attempt =
                        now + config_.outbound_reconnect_initial;
                    candidate->backoff = next_backoff(
                        config_.outbound_reconnect_initial,
                        config_.outbound_reconnect_max);
                }

                if (!candidate->peer && candidate->node_id.has_value()) {
                    if (const auto banned_until =
                            peer_scores_.banned_until(*candidate->node_id, now);
                        banned_until.has_value()) {
                        candidate->next_attempt = *banned_until;
                        continue;
                    }
                }

                if (!candidate->peer &&
                    !candidate->attempt_in_progress &&
                    candidate->next_attempt <= now) {
                    candidate->attempt_in_progress = true;
                    target = candidate;
                    break;
                }
            }

            if (!target) {
                reconnect_cv_.wait_for(lock, 50ms, [&] {
                    return !running_.load();
                });
                continue;
            }
        }

        try {
            P2pTcpConnection connection = connect_p2p_peer(
                target->endpoint,
                config_.handshake);
            auto peer = add_peer(std::move(connection));
            if (!peer) {
                throw std::runtime_error(
                    "P2P peer rejected: self-connection, duplicate, or banned node id");
            }
            const NodeId peer_node_id = peer->connection.peer_handshake().node_id;

            std::lock_guard lock(reconnect_mutex_);
            if (!running_.load()) {
                peer->alive.store(false);
                peer->connection.shutdown();
                target->attempt_in_progress = false;
                continue;
            }
            target->peer = std::move(peer);
            target->node_id = peer_node_id;
            target->attempt_in_progress = false;
            target->backoff = config_.outbound_reconnect_initial;
            target->next_attempt =
                std::chrono::steady_clock::time_point::max();
        } catch (...) {
            std::lock_guard lock(reconnect_mutex_);
            target->attempt_in_progress = false;
            if (!running_.load()) {
                continue;
            }
            const auto delay = target->backoff;
            target->next_attempt =
                std::chrono::steady_clock::now() + delay;
            target->backoff = next_backoff(
                delay,
                config_.outbound_reconnect_max);
        }
    }

    reap_dead_peers();
}

std::shared_ptr<P2pRuntime::Peer> P2pRuntime::add_peer(
    P2pTcpConnection connection) {
    if (!running_.load()) {
        connection.close();
        return {};
    }

    const NodeId peer_node_id = connection.peer_handshake().node_id;
    if (peer_node_id == config_.handshake.node_id ||
        peer_scores_.banned(peer_node_id)) {
        connection.close();
        return {};
    }

    auto peer = std::make_shared<Peer>(std::move(connection));
    {
        std::lock_guard lock(peers_mutex_);
        const bool duplicate = std::any_of(
            peers_.begin(),
            peers_.end(),
            [&](const std::shared_ptr<Peer>& existing) {
                return existing->alive.load() &&
                       existing->connection.peer_handshake().node_id ==
                           peer_node_id;
            });
        if (duplicate) {
            peer->alive.store(false);
            peer->connection.close();
            return {};
        }
        peers_.push_back(peer);
    }

    try {
        peer->reader_thread = std::thread(&P2pRuntime::peer_loop, this, peer);
    } catch (...) {
        peer->alive.store(false);
        peer->connection.close();
        throw;
    }
    return peer;
}

void P2pRuntime::reap_dead_peers() noexcept {
    std::vector<std::shared_ptr<Peer>> dead;
    {
        std::lock_guard lock(peers_mutex_);
        auto it = peers_.begin();
        while (it != peers_.end()) {
            if ((*it)->alive.load()) {
                ++it;
                continue;
            }
            dead.push_back(*it);
            it = peers_.erase(it);
        }
    }

    for (const auto& peer : dead) {
        peer->connection.shutdown();
        if (peer->reader_thread.joinable()) {
            peer->reader_thread.join();
        }
        peer->connection.close();
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
    reconnect_cv_.notify_all();
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
        reconnect_cv_.notify_all();
        return false;
    }
}

}  // namespace zano_p2pool
