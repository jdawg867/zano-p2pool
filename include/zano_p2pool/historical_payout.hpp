#pragma once
#include "zano_p2pool/pplns_coinbase.hpp"

namespace zano_p2pool {
enum class HistoricalPayoutStatus {
    PlanDerived,
    ParameterMismatch,
    BootstrapHistoryRequired,
    BootstrapRoot,
    ParentMissing,
    UnverifiedAncestry,
    PlanUnavailable,
};
struct HistoricalPayoutResult {
    HistoricalPayoutStatus status{HistoricalPayoutStatus::ParentMissing};
    ShareId parent_id{};
    PplnsCoinbasePlan plan{};
};
// Derive accounting for the explicit candidate-share parent, never today's tip.
// Difficulty/reward must come from an independently checked mining anchor.
// PlanDerived does not verify a miner transaction or promote work. The caller
// must bind the parent to the candidate share and run consensus proof checks.
[[nodiscard]] HistoricalPayoutResult derive_historical_payout_plan(
    const ShareChain& chain, const SidechainParameters& params,
    const ShareId& parent_id, const Difficulty128& network_difficulty,
    std::uint64_t reward_atomic);

[[nodiscard]] const char* historical_payout_status_name(
    HistoricalPayoutStatus status) noexcept;

}
