#pragma once

#include "zano_p2pool/pplns.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace zano_p2pool {

// Current Zano HF6 consensus limit. This matches CURRENCY_TX_MAX_ALLOWED_OUTS
// and the Bulletproof+ aggregation maximum in the pinned Zano source.
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

// Convert an already-defined PPLNS work window into a direct HF6 coinbase
// destination set. This first implementation deliberately fails closed rather
// than inventing implicit deferred balances: every credited miner must carry a
// v2 payout identity, the window must be complete, recipient count must fit
// Zano's hard 32-output limit, and every output must be non-zero.
[[nodiscard]] PplnsCoinbasePlan make_pplns_coinbase_plan(
    const PplnsWindow& window,
    std::uint64_t reward_atomic);

[[nodiscard]] const char* pplns_coinbase_plan_status_name(
    PplnsCoinbasePlanStatus status) noexcept;

}  // namespace zano_p2pool
