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
//   * its exact sidechain parent.
//
// For non-root shares, that explicit parent is also the authority for
// reconstructing the canonical historical PPLNS payout plan from validated
// ancestry. A zero-parent v2 root has no preceding sidechain payout history;
// its template destination is therefore not sidechain consensus. The root
// instead uses the bootstrap miner-tx policy, while its own self-bound payout
// identity becomes the first payout-history entry after normal share admission.
//
// local_observations must contain only work captured from this node's own
// daemon. The lookup callback must query the operator's local daemon by height.
// historical_pow_lookup may reconstruct missing exact-template authority only
// from that same local daemon's canonical history.
//
// The local anchor is checked once before payout handling and again immediately
// before the miner-tx/proof trust crossing. The second check prevents a
// parent-chain change observed during reconstruction from silently authorizing
// stale work.
//
// No trusted-work insertion occurs unless:
//   1. candidate/proposal binding succeeds,
//   2. historical local anchoring succeeds,
//   3. either:
//        a. non-root PPLNS accounting is derived from validated ancestry, or
//        b. a canonical zero-parent v2 root passes the bootstrap shape gate,
//   4. the historical anchor still matches on recheck,
//   5. the applicable miner-tx accounting + consensus proofs verify, and
//   6. trusted work is inserted under the candidate's exact parent binding.
[[nodiscard]] HistoricalTrustResult promote_historical_mining_context(
    P2pTrustedWorkRegistry& trusted_work,
    const ShareChain& chain,
    const SidechainParameters& params,
    const Share& candidate_share,
    const P2pHandshake& peer,
    const P2pMiningContextProposal& proposal,
    std::span<const P2pMiningAnchor> local_observations,
    const std::function<RpcCanonicalHeader(std::uint64_t)>& lookup,
    const std::function<std::optional<RpcHistoricalPowContext>(
        std::uint64_t)>& historical_pow_lookup = {});

[[nodiscard]] const char* historical_trust_status_name(
    HistoricalTrustStatus status) noexcept;

}  // namespace zano_p2pool
