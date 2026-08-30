#include "zano_p2pool/pplns_coinbase.hpp"

#include <limits>

namespace zano_p2pool {

PplnsCoinbasePlan make_pplns_coinbase_plan(
    const PplnsWindow& window,
    std::uint64_t reward_atomic) {
    PplnsCoinbasePlan result;
    result.reward_atomic = reward_atomic;

    if (window.miners.empty()) {
        result.status = PplnsCoinbasePlanStatus::EmptyWindow;
        return result;
    }
    if (!window.complete) {
        result.status = PplnsCoinbasePlanStatus::IncompleteWindow;
        return result;
    }

    const auto payouts = allocate_pplns_reward(window, reward_atomic);
    if (payouts.empty()) {
        result.status = PplnsCoinbasePlanStatus::EmptyWindow;
        return result;
    }
    if (payouts.size() > kZanoHf6MaxCoinbaseOutputs) {
        result.status = PplnsCoinbasePlanStatus::TooManyRecipients;
        return result;
    }

    std::uint64_t total = 0;
    result.destinations.reserve(payouts.size());
    for (const auto& payout : payouts) {
        if (!payout.payout.has_value()) {
            result.destinations.clear();
            result.status = PplnsCoinbasePlanStatus::MissingPayoutIdentity;
            return result;
        }
        if (payout.miner_id != miner_id_from_payout(*payout.payout)) {
            result.destinations.clear();
            result.status = PplnsCoinbasePlanStatus::MissingPayoutIdentity;
            return result;
        }
        if (payout.amount == 0) {
            result.destinations.clear();
            result.status = PplnsCoinbasePlanStatus::ZeroPayout;
            return result;
        }
        if (payout.amount > std::numeric_limits<std::uint64_t>::max() - total) {
            result.destinations.clear();
            result.status = PplnsCoinbasePlanStatus::RewardSumMismatch;
            return result;
        }
        total += payout.amount;
        result.destinations.push_back(PplnsCoinbaseDestination{
            payout.miner_id,
            *payout.payout,
            payout.amount,
        });
    }

    if (total != reward_atomic) {
        result.destinations.clear();
        result.status = PplnsCoinbasePlanStatus::RewardSumMismatch;
        return result;
    }

    result.status = PplnsCoinbasePlanStatus::Ready;
    return result;
}

const char* pplns_coinbase_plan_status_name(
    PplnsCoinbasePlanStatus status) noexcept {
    switch (status) {
    case PplnsCoinbasePlanStatus::Ready:
        return "ready";
    case PplnsCoinbasePlanStatus::EmptyWindow:
        return "empty-window";
    case PplnsCoinbasePlanStatus::IncompleteWindow:
        return "incomplete-window";
    case PplnsCoinbasePlanStatus::MissingPayoutIdentity:
        return "missing-payout-identity";
    case PplnsCoinbasePlanStatus::TooManyRecipients:
        return "too-many-recipients";
    case PplnsCoinbasePlanStatus::ZeroPayout:
        return "zero-payout";
    case PplnsCoinbasePlanStatus::RewardSumMismatch:
        return "reward-sum-mismatch";
    }
    return "reward-sum-mismatch";
}

}  // namespace zano_p2pool
