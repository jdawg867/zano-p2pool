#pragma once

#include "zano_p2pool/p2p_protocol.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>

namespace zano_p2pool {

inline constexpr std::uint32_t kP2pProtocolViolationPenalty = 25;

struct P2pPeerScoreConfig {
    std::uint32_t ban_threshold{100};
    std::chrono::milliseconds ban_duration{std::chrono::minutes(5)};
};

struct P2pPeerPenaltyResult {
    std::uint32_t score{0};
    bool banned{false};
    bool ban_started{false};
    std::optional<std::chrono::steady_clock::time_point> banned_until;
};

// Thread-safe, identity-keyed peer scoring. Scores accumulate only from explicit
// protocol-level penalties reported by the caller; ordinary socket disconnects
// are deliberately not treated as abuse. Reaching the configured threshold
// starts a temporary ban. Once that ban expires, the score resets to zero.
class P2pPeerScoreBook {
public:
    explicit P2pPeerScoreBook(P2pPeerScoreConfig config = {})
        : config_(config) {
        if (config_.ban_threshold == 0) {
            throw std::runtime_error("P2P peer ban threshold must be nonzero");
        }
        if (config_.ban_duration.count() <= 0) {
            throw std::runtime_error("P2P peer ban duration must be positive");
        }
    }

    [[nodiscard]] P2pPeerPenaltyResult penalize(
        const NodeId& node_id,
        std::uint32_t penalty,
        std::chrono::steady_clock::time_point now =
            std::chrono::steady_clock::now()) noexcept {
        std::lock_guard lock(mutex_);
        Entry& entry = entries_[node_id];
        refresh_locked(entry, now);

        if (entry.banned_until.has_value()) {
            return result_locked(entry, false);
        }
        if (penalty == 0) {
            return result_locked(entry, false);
        }

        const std::uint32_t room =
            config_.ban_threshold > entry.score
                ? config_.ban_threshold - entry.score
                : 0;
        entry.score += std::min(penalty, room);

        bool ban_started = false;
        if (entry.score >= config_.ban_threshold) {
            entry.score = config_.ban_threshold;
            entry.banned_until = now + config_.ban_duration;
            ban_started = true;
        }
        return result_locked(entry, ban_started);
    }

    [[nodiscard]] std::uint32_t score(
        const NodeId& node_id,
        std::chrono::steady_clock::time_point now =
            std::chrono::steady_clock::now()) const noexcept {
        std::lock_guard lock(mutex_);
        const auto it = entries_.find(node_id);
        if (it == entries_.end()) {
            return 0;
        }
        refresh_locked(it->second, now);
        return it->second.score;
    }

    [[nodiscard]] bool banned(
        const NodeId& node_id,
        std::chrono::steady_clock::time_point now =
            std::chrono::steady_clock::now()) const noexcept {
        return banned_until(node_id, now).has_value();
    }

    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point>
    banned_until(
        const NodeId& node_id,
        std::chrono::steady_clock::time_point now =
            std::chrono::steady_clock::now()) const noexcept {
        std::lock_guard lock(mutex_);
        const auto it = entries_.find(node_id);
        if (it == entries_.end()) {
            return std::nullopt;
        }
        refresh_locked(it->second, now);
        return it->second.banned_until;
    }

    void clear(const NodeId& node_id) noexcept {
        std::lock_guard lock(mutex_);
        entries_.erase(node_id);
    }

    [[nodiscard]] const P2pPeerScoreConfig& config() const noexcept {
        return config_;
    }

private:
    struct Entry {
        std::uint32_t score{0};
        std::optional<std::chrono::steady_clock::time_point> banned_until;
    };

    static void refresh_locked(
        Entry& entry,
        std::chrono::steady_clock::time_point now) noexcept {
        if (entry.banned_until.has_value() && now >= *entry.banned_until) {
            entry.score = 0;
            entry.banned_until.reset();
        }
    }

    [[nodiscard]] static P2pPeerPenaltyResult result_locked(
        const Entry& entry,
        bool ban_started) noexcept {
        return P2pPeerPenaltyResult{
            entry.score,
            entry.banned_until.has_value(),
            ban_started,
            entry.banned_until,
        };
    }

    P2pPeerScoreConfig config_;
    mutable std::mutex mutex_;
    mutable std::map<NodeId, Entry> entries_;
};

}  // namespace zano_p2pool
