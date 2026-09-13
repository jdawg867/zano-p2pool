#pragma once

#include "zano_p2pool/historical_payout.hpp"
#include "zano_p2pool/historical_work.hpp"
#include "zano_p2pool/p2p_mining_context_trust.hpp"

#include <cstdint>
#include <functional>
#include <span>

namespace zano_p2pool {

// End-to-end historical trust crossing. Every status before Trusted is
// side-effect free with respect to P2pTrustedWorkRegistry.
enum class HistoricalTrustStatus : std::uint8_t {
    Trusted,
    CandidateMismatch,
    AnchorRejected,
    PayoutRejected,
    AnchorChangedBeforePromotion,
    PromotionRejected,
};

struct HistoricalTrustResult {
    HistoricalTrustStatus status{HistoricalTrustStatus::CandidateMismatch};
    ShareId candidate_id{};
    HistoricalAnchorResult initial_anchor{};
    HistoricalAnchorResult final_anchor{};
    HistoricalPayoutResult payout{};
    P2pMiningContextTrustResult promotion{};
};

// Promote one retrieved historical mining proposal for one exact candidate
// share. The candidate binds the proposal to:
//   * the exact Zano height,
//   * the locally derived mining-header hash,
//   * the exact network difficulty, and
//   * the exact sidechain parent used for historical PPLNS accounting.
//
// local_observations must contain only work captured from this node's own
// daemon. The lookup callback must query the operator's local daemon by height.
//
// The local anchor is checked once before payout reconstruction and again
// immediately before the existing miner-tx/proof trust crossing. The second
// check prevents a parent-chain change observed during reconstruction from
// silently authorizing stale work.
//
// No trusted-work insertion occurs unless:
//   1. candidate/proposal binding succeeds,
//   2. historical local anchoring succeeds,
//   3. the explicit-parent payout plan is derived from validated ancestry,
//   4. the historical anchor still matches on recheck, and
//   5. the existing payout + consensus proof gate returns Trusted.
[[nodiscard]] HistoricalTrustResult promote_historical_mining_context(
    P2pTrustedWorkRegistry& trusted_work,
    const ShareChain& chain,
    const SidechainParameters& params,
    const Share& candidate_share,
    const P2pHandshake& peer,
    const P2pMiningContextProposal& proposal,
    std::span<const P2pMiningAnchor> local_observations,
    const std::function<RpcCanonicalHeader(std::uint64_t)>& lookup);

[[nodiscard]] const char* historical_trust_status_name(
    HistoricalTrustStatus status) noexcept;

}  // namespace zano_p2pool
