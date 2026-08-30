#pragma once

#include "zano_p2pool/share_chain.hpp"
#include "zano_p2pool/sidechain_params.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace zano_p2pool {

struct PplnsMinerWork {
    MinerId miner_id{};
    ChainWork work{};
    std::optional<PayoutPublicKeys> payout;

    bool operator==(const PplnsMinerWork&) const = default;
};

struct PplnsWindow {
    ChainWork requested_work{};
    ChainWork covered_work{};
    std::size_t included_shares{0};
    bool complete{false};
    std::vector<PplnsMinerWork> miners;
};

struct PplnsPayout {
    MinerId miner_id{};
    ChainWork work{};
    std::uint64_t amount{0};
    std::optional<PayoutPublicKeys> payout;

    bool operator==(const PplnsPayout&) const = default;
};

// Walk the locally verified best chain newest-to-oldest until exactly
// requested_work is covered. If the oldest included share would cross the
// boundary, only the work needed to fill the window is credited. Public payout
// keys from v2 shares are retained with the MinerId so the winning block can be
// constructed directly from consensus history.
[[nodiscard]] PplnsWindow build_pplns_window(
    const ShareChain& chain,
    const ChainWork& requested_work);

// Build the canonical sidechain payout window. The effective requested work is
// the lesser of (a) all work in the newest params.pplns_window_shares best-chain
// shares and (b) current Zano network difficulty multiplied by
// params.pplns_max_network_difficulty_multiplier. This keeps bootstrap windows
// complete while guaranteeing the direct-payout share-count/output bound.
[[nodiscard]] PplnsWindow build_sidechain_pplns_window(
    const ShareChain& chain,
    const SidechainParameters& params,
    const Difficulty128& network_difficulty);

// Divide reward_atomic proportionally to credited work using exact integer
// arithmetic. Floor allocations are followed by deterministic largest-remainder
// apportionment; ties are resolved by lexicographic MinerId. The returned rows
// remain sorted by MinerId and sum exactly to reward_atomic for a non-empty
// window.
[[nodiscard]] std::vector<PplnsPayout> allocate_pplns_reward(
    const PplnsWindow& window,
    std::uint64_t reward_atomic);

}  // namespace zano_p2pool
