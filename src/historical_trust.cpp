#include "zano_p2pool/historical_trust.hpp"

#include "zano_p2pool/mining_header.hpp"

#include <stdexcept>

namespace zano_p2pool {

HistoricalTrustResult promote_historical_mining_context(
    P2pTrustedWorkRegistry& trusted_work,
    const ShareChain& chain,
    const SidechainParameters& params,
    const Share& candidate_share,
    const P2pHandshake& peer,
    const P2pMiningContextProposal& proposal,
    std::span<const P2pMiningAnchor> local_observations,
    const std::function<RpcCanonicalHeader(std::uint64_t)>& lookup,
    const std::function<std::optional<RpcHistoricalPowContext>(
        std::uint64_t)>& historical_pow_lookup) {
    HistoricalTrustResult result;

    // share_id() canonical-serializes the candidate. Malformed peer shares
    // such as a v2 MinerId/payout mismatch are candidate failures, not
    // exceptions that may escape the historical trust crossing.
    try {
        result.candidate_id = share_id(candidate_share);
    } catch (const std::invalid_argument&) {
        result.status = HistoricalTrustStatus::CandidateMismatch;
        return result;
    }

    // Structural validation derives the header from the transported block blob
    // instead of trusting either proposal metadata or the candidate share.
    const Hash256 proposal_header =
        validate_p2p_mining_context_structure(proposal);
    if (candidate_share.zano_height != proposal.zano_height ||
        candidate_share.mining_header_hash != proposal_header ||
        candidate_share.network_difficulty != proposal.network_difficulty) {
        result.status = HistoricalTrustStatus::CandidateMismatch;
        return result;
    }

    const bool bootstrap_root =
        is_zero_share_id(candidate_share.parent_id);

    // A zero-parent work item may bypass historical PPLNS reconstruction only
    // for the actual first v2 sidechain share. Require its payout identity to
    // be self-consistent before any peer-provided work can enter the trusted
    // registry. The payout identity seeds later sidechain accounting; it does
    // not authorize the older bootstrap template's node-specific coinbase.
    if (bootstrap_root &&
        (candidate_share.share_height != 0 ||
         candidate_share.version != kShareVersion2 ||
         !candidate_share.payout.has_value() ||
         candidate_share.miner_id !=
             miner_id_from_payout(*candidate_share.payout))) {
        result.status = HistoricalTrustStatus::CandidateMismatch;
        return result;
    }

    result.initial_anchor = audit_historical_local_anchor(
        proposal,
        local_observations,
        lookup,
        historical_pow_lookup);
    if (result.initial_anchor.status !=
        HistoricalAnchorStatus::AnchorMatchedUntrusted) {
        result.status = HistoricalTrustStatus::AnchorRejected;
        return result;
    }
    if (result.initial_anchor.mining_header_hash != proposal_header) {
        result.status = HistoricalTrustStatus::CandidateMismatch;
        return result;
    }

    // The successful local-anchor audit establishes that these proposal fields
    // match a locally captured daemon observation. HF6 burns fees, and the
    // existing payout/proof verifier independently requires block_reward to
    // equal block_reward_without_fee before accepting the miner transaction.
    if (!bootstrap_root) {
        result.payout = derive_historical_payout_plan(
            chain,
            params,
            candidate_share.parent_id,
            proposal.network_difficulty,
            proposal.block_reward_without_fee);
        if (result.payout.status != HistoricalPayoutStatus::PlanDerived) {
            result.status = HistoricalTrustStatus::PayoutRejected;
            return result;
        }
    } else {
        // No sidechain payout history exists before the root. Mark this
        // explicitly rather than leaving the payout result looking like a
        // missing-parent failure. Miner-tx trust still crosses only through
        // the bootstrap accounting/balance/range-proof path below.
        result.payout.status = HistoricalPayoutStatus::BootstrapRoot;
        result.payout.parent_id = candidate_share.parent_id;
    }

    // Re-sample the local daemon immediately before the only API below that can
    // insert trusted work. This catches a parent reorg/change that happened
    // while the branch-relative payout plan was being reconstructed.
    result.final_anchor = audit_historical_local_anchor(
        proposal,
        local_observations,
        lookup,
        historical_pow_lookup);
    if (result.final_anchor.status !=
            HistoricalAnchorStatus::AnchorMatchedUntrusted ||
        result.final_anchor.mining_header_hash !=
            result.initial_anchor.mining_header_hash) {
        result.status =
            HistoricalTrustStatus::AnchorChangedBeforePromotion;
        return result;
    }

    const P2pMiningAnchor local_anchor{
        proposal.zano_height,
        proposal.prev_hash,
        proposal.network_difficulty,
        proposal.seed,
        proposal.block_reward_without_fee,
    };

    if (bootstrap_root) {
        result.promotion =
            promote_bootstrap_p2p_mining_context(
                trusted_work,
                peer,
                make_p2p_mining_context_envelope(proposal),
                local_anchor);
    } else {
        result.promotion = promote_p2p_mining_context(
            trusted_work,
            peer,
            make_p2p_mining_context_envelope(proposal),
            local_anchor,
            candidate_share.parent_id,
            result.payout.plan);
    }

    if (result.promotion.status != P2pMiningContextTrustStatus::Trusted) {
        result.status = HistoricalTrustStatus::PromotionRejected;
        return result;
    }

    result.status = HistoricalTrustStatus::Trusted;
    return result;
}

const char* historical_trust_status_name(
    HistoricalTrustStatus status) noexcept {
    switch (status) {
    case HistoricalTrustStatus::Trusted:
        return "trusted";
    case HistoricalTrustStatus::CandidateMismatch:
        return "candidate-mismatch";
    case HistoricalTrustStatus::AnchorRejected:
        return "anchor-rejected";
    case HistoricalTrustStatus::PayoutRejected:
        return "payout-rejected";
    case HistoricalTrustStatus::AnchorChangedBeforePromotion:
        return "anchor-changed-before-promotion";
    case HistoricalTrustStatus::PromotionRejected:
        return "promotion-rejected";
    }
    return "unknown";
}

}  // namespace zano_p2pool
