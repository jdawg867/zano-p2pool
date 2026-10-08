#include "zano_p2pool/pplns_coinbase.hpp"

#include <algorithm>
#include <limits>

namespace zano_p2pool {
namespace {

[[nodiscard]] bool calculate_basis_point_share(
    std::uint64_t amount,
    std::uint64_t basis_points,
    std::uint64_t& result) noexcept {
    if (basis_points >= kOperatorFeeBasisPointDenominator) {
        return false;
    }

    const std::uint64_t whole =
        amount / kOperatorFeeBasisPointDenominator;

    const std::uint64_t remainder =
        amount % kOperatorFeeBasisPointDenominator;

    if (basis_points != 0 &&
        whole >
            std::numeric_limits<std::uint64_t>::max() /
                basis_points) {
        return false;
    }

    const std::uint64_t whole_fee =
        whole * basis_points;

    // Both factors are below 10000.
    const std::uint64_t remainder_fee =
        (remainder * basis_points) /
        kOperatorFeeBasisPointDenominator;

    if (remainder_fee >
        std::numeric_limits<std::uint64_t>::max() -
            whole_fee) {
        return false;
    }

    result = whole_fee + remainder_fee;
    return true;
}

}  // namespace

PplnsCoinbasePlan make_pplns_coinbase_plan(
    const PplnsWindow& window,
    std::uint64_t reward_atomic,
    const SidechainParameters& params) {
    PplnsCoinbasePlan result;
    result.reward_atomic = reward_atomic;

    // Reuse canonical parameter validation so noncanonical/broken fee
    // parameters can never silently construct work.
    static_cast<void>(
        serialize_sidechain_parameters(params));

    if (window.miners.empty()) {
        result.status =
            PplnsCoinbasePlanStatus::EmptyWindow;
        return result;
    }

    if (!window.complete) {
        result.status =
            PplnsCoinbasePlanStatus::IncompleteWindow;
        return result;
    }

    std::uint64_t operator_fee = 0;

    if (!calculate_basis_point_share(
            reward_atomic,
            params.operator_fee_basis_points,
            operator_fee) ||
        operator_fee >= reward_atomic) {
        result.status =
            PplnsCoinbasePlanStatus::ZeroPayout;
        return result;
    }

    const std::uint64_t miner_reward =
        reward_atomic - operator_fee;

    const auto payouts =
        allocate_pplns_reward(
            window,
            miner_reward);

    if (payouts.empty()) {
        result.status =
            PplnsCoinbasePlanStatus::EmptyWindow;
        return result;
    }

    const std::size_t max_miner_recipients =
        kZanoHf6MaxCoinbaseOutputs - 1;

    if (payouts.size() > max_miner_recipients ||
        payouts.size() > params.pplns_window_shares) {
        result.status =
            PplnsCoinbasePlanStatus::TooManyRecipients;
        return result;
    }

    std::uint64_t total = 0;

    result.destinations.reserve(
        payouts.size() + 1);

    for (const auto& payout : payouts) {
        if (!payout.payout.has_value()) {
            result.destinations.clear();
            result.status =
                PplnsCoinbasePlanStatus::
                    MissingPayoutIdentity;
            return result;
        }

        if (payout.miner_id !=
            miner_id_from_payout(*payout.payout)) {
            result.destinations.clear();
            result.status =
                PplnsCoinbasePlanStatus::
                    MissingPayoutIdentity;
            return result;
        }

        if (payout.amount == 0) {
            result.destinations.clear();
            result.status =
                PplnsCoinbasePlanStatus::ZeroPayout;
            return result;
        }

        if (payout.amount >
            std::numeric_limits<std::uint64_t>::max() -
                total) {
            result.destinations.clear();
            result.status =
                PplnsCoinbasePlanStatus::
                    RewardSumMismatch;
            return result;
        }

        total += payout.amount;

        result.destinations.push_back(
            PplnsCoinbaseDestination{
                payout.miner_id,
                *payout.payout,
                payout.amount,
            });
    }

    if (total != miner_reward) {
        result.destinations.clear();
        result.status =
            PplnsCoinbasePlanStatus::
                RewardSumMismatch;
        return result;
    }

    if (operator_fee != 0) {
        auto operator_destination =
            std::find_if(
                result.destinations.begin(),
                result.destinations.end(),
                [&](const PplnsCoinbaseDestination&
                        destination) {
                    return destination.payout ==
                        params.operator_fee_payout;
                });

        if (operator_destination !=
            result.destinations.end()) {
            if (operator_fee >
                std::numeric_limits<std::uint64_t>::max() -
                    operator_destination->amount) {
                result.destinations.clear();
                result.status =
                    PplnsCoinbasePlanStatus::
                        RewardSumMismatch;
                return result;
            }

            operator_destination->amount +=
                operator_fee;
        } else {
            if (result.destinations.size() >=
                kZanoHf6MaxCoinbaseOutputs) {
                result.destinations.clear();
                result.status =
                    PplnsCoinbasePlanStatus::
                        TooManyRecipients;
                return result;
            }

            result.destinations.push_back(
                PplnsCoinbaseDestination{
                    miner_id_from_payout(
                        params.operator_fee_payout),
                    params.operator_fee_payout,
                    operator_fee,
                });
        }

    }

    if (operator_fee >
        std::numeric_limits<std::uint64_t>::max() -
            total) {
        result.destinations.clear();
        result.status =
            PplnsCoinbasePlanStatus::
                RewardSumMismatch;
        return result;
    }

    total += operator_fee;

    if (total != reward_atomic) {
        result.destinations.clear();
        result.status =
            PplnsCoinbasePlanStatus::
                RewardSumMismatch;
        return result;
    }

    result.status =
        PplnsCoinbasePlanStatus::Ready;

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
