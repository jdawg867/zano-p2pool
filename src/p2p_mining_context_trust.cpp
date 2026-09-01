#include "zano_p2pool/p2p_mining_context_trust.hpp"

#include <utility>

namespace zano_p2pool {
namespace {

template <typename ProofVerifier>
P2pMiningContextTrustResult promote_p2p_mining_context_impl(
    P2pTrustedWorkRegistry& trusted_work,
    const P2pHandshake& peer,
    const P2pEnvelope& envelope,
    const P2pMiningAnchor& local_anchor,
    ProofVerifier&& verify_proofs) {
    P2pMiningContextTrustResult result;

    const P2pMiningContextCheckResult check =
        inspect_p2p_mining_context(peer, envelope, local_anchor);
    result.check_status = check.status;
    result.proposal_id = check.proposal_id;

    if (check.status == P2pMiningContextCheckStatus::CapabilityMissing) {
        result.status = P2pMiningContextTrustStatus::CapabilityMissing;
        return result;
    }
    if (check.status !=
        P2pMiningContextCheckStatus::AnchoredUnverifiedMinerTx) {
        result.status = P2pMiningContextTrustStatus::AnchorMismatch;
        return result;
    }

    // Reparse the exact envelope that passed anchoring. The proof verifier
    // independently binds this proposal back to check.proposal_id before any
    // cryptographic result can be accepted.
    const P2pMiningContextProposal proposal =
        parse_p2p_mining_context_envelope(envelope);
    const P2pMinerTxProofResult proof =
        std::forward<ProofVerifier>(verify_proofs)(proposal, check);
    result.proof_status = proof.status;
    result.payout_status = proof.payout_status;

    if (proof.status != P2pMinerTxProofStatus::ProofsVerified) {
        result.status = P2pMiningContextTrustStatus::ProofsRejected;
        return result;
    }

    result.trusted_context = ShareWorkContext{
        local_anchor.zano_height,
        check.mining_header_hash,
        local_anchor.network_difficulty,
    };

    const bool already_trusted = trusted_work.find(
        result.trusted_context.zano_height,
        result.trusted_context.mining_header_hash) != nullptr;

    trusted_work.remember(result.trusted_context);
    result.registry_inserted = !already_trusted;
    result.status = P2pMiningContextTrustStatus::Trusted;
    return result;
}

}  // namespace

P2pMiningContextTrustResult promote_p2p_mining_context(
    P2pTrustedWorkRegistry& trusted_work,
    const P2pHandshake& peer,
    const P2pEnvelope& envelope,
    const P2pMiningAnchor& local_anchor,
    const P2pPayoutAddress& expected_payout) {
    return promote_p2p_mining_context_impl(
        trusted_work,
        peer,
        envelope,
        local_anchor,
        [&expected_payout](
            const P2pMiningContextProposal& proposal,
            const P2pMiningContextCheckResult& check) {
            return verify_p2p_mining_context_proofs(
                proposal, check, expected_payout);
        });
}

P2pMiningContextTrustResult promote_p2p_mining_context(
    P2pTrustedWorkRegistry& trusted_work,
    const P2pHandshake& peer,
    const P2pEnvelope& envelope,
    const P2pMiningAnchor& local_anchor,
    const PplnsCoinbasePlan& expected_plan) {
    return promote_p2p_mining_context_impl(
        trusted_work,
        peer,
        envelope,
        local_anchor,
        [&expected_plan](
            const P2pMiningContextProposal& proposal,
            const P2pMiningContextCheckResult& check) {
            return verify_p2p_mining_context_proofs(
                proposal, check, expected_plan);
        });
}

const char* p2p_mining_context_trust_status_name(
    P2pMiningContextTrustStatus status) noexcept {
    switch (status) {
    case P2pMiningContextTrustStatus::Trusted:
        return "trusted";
    case P2pMiningContextTrustStatus::CapabilityMissing:
        return "capability-missing";
    case P2pMiningContextTrustStatus::AnchorMismatch:
        return "anchor-mismatch";
    case P2pMiningContextTrustStatus::ProofsRejected:
        return "proofs-rejected";
    }
    return "unknown";
}

}  // namespace zano_p2pool
