#include "zano_p2pool/historical_payout.hpp"

namespace zano_p2pool {
HistoricalPayoutResult derive_historical_payout_plan(
    const ShareChain& chain, const SidechainParameters& params,
    const ShareId& parent_id, const Difficulty128& network_difficulty,
    std::uint64_t reward_atomic) {
    HistoricalPayoutResult result;
    result.parent_id = parent_id;
    if (!chain.matches_sidechain_parameters(params)) {
        result.status = HistoricalPayoutStatus::ParameterMismatch;
        return result;
    }
    if (is_zero_share_id(parent_id)) {
        result.status = HistoricalPayoutStatus::BootstrapHistoryRequired;
        return result;
    }
    const auto* parent = chain.find(parent_id);
    if (!parent) return result;
    if (!parent->validated_ancestry) {
        result.status = HistoricalPayoutStatus::UnverifiedAncestry;
        return result;
    }
    result.plan = make_pplns_coinbase_plan(
        build_sidechain_pplns_window_at_parent(chain, parent_id, params, network_difficulty),
        reward_atomic);
    result.status = result.plan.status == PplnsCoinbasePlanStatus::Ready ?
        HistoricalPayoutStatus::PlanDerived : HistoricalPayoutStatus::PlanUnavailable;
    return result;
}

const char* historical_payout_status_name(
    HistoricalPayoutStatus status) noexcept {
    switch (status) {
    case HistoricalPayoutStatus::PlanDerived:
        return "plan-derived";
    case HistoricalPayoutStatus::ParameterMismatch:
        return "parameter-mismatch";
    case HistoricalPayoutStatus::BootstrapHistoryRequired:
        return "bootstrap-history-required";
    case HistoricalPayoutStatus::ParentMissing:
        return "parent-missing";
    case HistoricalPayoutStatus::UnverifiedAncestry:
        return "unverified-ancestry";
    case HistoricalPayoutStatus::PlanUnavailable:
        return "plan-unavailable";
    }
    return "unknown";
}

}
