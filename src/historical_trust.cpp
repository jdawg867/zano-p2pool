#include "zano_p2pool/historical_trust.hpp"

#include "zano_p2pool/mining_header.hpp"

namespace zano_p2pool {

HistoricalTrustResult promote_historical_mining_context(
    P2pTrustedWorkRegistry& trusted_work,
    const ShareChain& chain,
    const SidechainParameters& params,
    const Share& candidate_share,
    const P2pHandshake& peer,
    const P2pMiningContextProposal& proposal,
    std::span<const P2pMiningAnchor> local_observations,
    const std::function<RpcCanonicalHeader(std::uint64_t)>& lookup) {
    HistoricalTrustResult result;
    result.candidate_id = share_id(candidate_share);

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

    result.initial_anchor = audit_historical_local_anchor(
        proposal, local_observations, lookup);
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

    // Re-sample the local daemon immediately before the only API below that can
    // insert trusted work. This catches a parent reorg/change that happened
    // while the branch-relative payout plan was being reconstructed.
    result.final_anchor = audit_historical_local_anchor(
        proposal, local_observations, lookup);
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

    result.promotion = promote_p2p_mining_context(
        trusted_work,
        peer,
        make_p2p_mining_context_envelope(proposal),
        local_anchor,
        candidate_share.parent_id,
        result.payout.plan);
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
