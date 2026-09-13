#include "zano_p2pool/restart_revalidation.hpp"

namespace zano_p2pool {

RestartRevalidationResult revalidate_recovered_share(
    ShareChain& chain,
    const SidechainParameters& params,
    const ShareId& share_id,
    const MiningWorkArchive& local_archive,
    const P2pMiningContextId& proposal_id,
    std::span<const P2pMiningAnchor> local_observations,
    const std::function<RpcCanonicalHeader(std::uint64_t)>& lookup,
    std::uint64_t now,
    ProgPowZContextMode mode) {
    RestartRevalidationResult result;
    result.share_id = share_id;

    if (!chain.matches_sidechain_parameters(params)) {
        result.status = RestartRevalidationStatus::ParameterMismatch;
        return result;
    }

    const ConnectedShare* connected = chain.find(share_id);
    if (connected == nullptr) {
        result.status = RestartRevalidationStatus::ShareMissing;
        return result;
    }
    if (connected->validated_ancestry) {
        result.status = RestartRevalidationStatus::AlreadyValidated;
        return result;
    }

    // MiningWorkArchive::read() verifies the record sidechain, content ID
    // and checksum before returning bytes. This makes exact local archival
    // provenance part of the restart trust crossing rather than a caller
    // convention.
    const P2pMiningContextProposal proposal =
        deserialize_p2p_mining_context_payload(
            local_archive.read(proposal_id));

    const Share candidate = connected->share;
    const Hash256 proposal_header =
        validate_p2p_mining_context_structure(proposal);

    if (candidate.zano_height != proposal.zano_height ||
        candidate.mining_header_hash != proposal_header ||
        candidate.network_difficulty != proposal.network_difficulty) {
        result.status = RestartRevalidationStatus::CandidateMismatch;
        return result;
    }

    if (!is_zero_share_id(candidate.parent_id)) {
        const ConnectedShare* parent = chain.find(candidate.parent_id);
        if (parent == nullptr || !parent->validated_ancestry) {
            result.status = RestartRevalidationStatus::ParentUnvalidated;
            return result;
        }
    }

    result.initial_anchor = audit_historical_local_anchor(
        proposal,
        local_observations,
        lookup);
    if (result.initial_anchor.status !=
            HistoricalAnchorStatus::AnchorMatchedUntrusted ||
        result.initial_anchor.mining_header_hash != proposal_header) {
        result.status = RestartRevalidationStatus::AnchorRejected;
        return result;
    }

    result.final_anchor = audit_historical_local_anchor(
        proposal,
        local_observations,
        lookup);
    if (result.final_anchor.status !=
            HistoricalAnchorStatus::AnchorMatchedUntrusted ||
        result.final_anchor.mining_header_hash !=
            result.initial_anchor.mining_header_hash) {
        result.status =
            RestartRevalidationStatus::AnchorChangedBeforeRevalidation;
        return result;
    }

    const ShareWorkContext trusted_context{
        proposal.zano_height,
        proposal_header,
        proposal.network_difficulty,
    };

    result.share_result = chain.revalidate_connected_share(
        share_id,
        trusted_context,
        now,
        mode);

    switch (result.share_result.status) {
    case RevalidateShareStatus::Validated:
        result.status = RestartRevalidationStatus::Revalidated;
        break;
    case RevalidateShareStatus::AlreadyValidated:
        result.status = RestartRevalidationStatus::AlreadyValidated;
        break;
    case RevalidateShareStatus::NotConnected:
        result.status = RestartRevalidationStatus::ShareMissing;
        break;
    case RevalidateShareStatus::ParentUnvalidated:
        result.status = RestartRevalidationStatus::ParentUnvalidated;
        break;
    case RevalidateShareStatus::Rejected:
        result.status = RestartRevalidationStatus::ShareRejected;
        break;
    }

    return result;
}

const char* restart_revalidation_status_name(
    RestartRevalidationStatus status) noexcept {
    switch (status) {
    case RestartRevalidationStatus::Revalidated:
        return "revalidated";
    case RestartRevalidationStatus::AlreadyValidated:
        return "already-validated";
    case RestartRevalidationStatus::ShareMissing:
        return "share-missing";
    case RestartRevalidationStatus::ParameterMismatch:
        return "parameter-mismatch";
    case RestartRevalidationStatus::CandidateMismatch:
        return "candidate-mismatch";
    case RestartRevalidationStatus::ParentUnvalidated:
        return "parent-unvalidated";
    case RestartRevalidationStatus::AnchorRejected:
        return "anchor-rejected";
    case RestartRevalidationStatus::AnchorChangedBeforeRevalidation:
        return "anchor-changed-before-revalidation";
    case RestartRevalidationStatus::ShareRejected:
        return "share-rejected";
    }
    return "unknown";
}

}  // namespace zano_p2pool
