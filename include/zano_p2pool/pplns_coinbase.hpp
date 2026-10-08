#pragma once

#include "zano_p2pool/pplns.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace zano_p2pool {

inline constexpr std::size_t kZanoHf6MaxCoinbaseOutputs = 32;

enum class PplnsCoinbasePlanStatus : std::uint8_t {
    Ready,
    EmptyWindow,
    IncompleteWindow,
    MissingPayoutIdentity,
    TooManyRecipients,
    ZeroPayout,
    RewardSumMismatch,
};

struct PplnsCoinbaseDestination {
    MinerId miner_id{};
    PayoutPublicKeys payout{};
    std::uint64_t amount{0};

    bool operator==(const PplnsCoinbaseDestination&) const = default;
};

struct PplnsCoinbasePlan {
    PplnsCoinbasePlanStatus status{PplnsCoinbasePlanStatus::EmptyWindow};
    std::uint64_t reward_atomic{0};
    std::vector<PplnsCoinbaseDestination> destinations;
};

// V4 canonical payout construction.
//
// The consensus operator fee is deducted first. The remaining reward is
// distributed through PPLNS. At most 31 distinct miner recipients are
// permitted so a distinct operator destination still fits Zano's 32-output
// limit.
//
// If the operator payout identity is also mining, its fee and mining payout
// are merged into one logical destination.
[[nodiscard]] PplnsCoinbasePlan make_pplns_coinbase_plan(
    const PplnsWindow& window,
    std::uint64_t reward_atomic,
    const SidechainParameters& params);

[[nodiscard]] const char* pplns_coinbase_plan_status_name(
    PplnsCoinbasePlanStatus status) noexcept;

}  // namespace zano_p2pool
