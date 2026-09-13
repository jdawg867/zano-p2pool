#include "zano_p2pool/restart_recovery.hpp"

#include "zano_p2pool/p2p_mining_context.hpp"

#include <algorithm>
#include <map>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace zano_p2pool {
namespace {

using RecoveryWorkKey =
    std::tuple<std::uint64_t, Hash256, Difficulty128>;

RecoveryWorkKey work_key(
    std::uint64_t zano_height,
    const Hash256& mining_header_hash,
    const Difficulty128& network_difficulty) {
    return {
        zano_height,
        mining_header_hash,
        network_difficulty,
    };
}

}  // namespace

RestartRecoveryResult recover_replayed_history(
    ShareChain& chain,
    const SidechainParameters& params,
    const MiningWorkArchive& local_archive,
    const std::function<RpcCanonicalHeader(std::uint64_t)>& lookup,
    std::uint64_t now,
    ProgPowZContextMode mode,
    std::size_t max_archive_records) {
    RestartRecoveryResult result;

    if (!chain.matches_sidechain_parameters(params)) {
        throw std::runtime_error(
            "restart recovery sidechain parameter mismatch");
    }

    // Validate and enumerate the entire archive before mutating any recovered
    // share state. This prevents a malformed late record from leaving a
    // partially recovered chain merely because directory iteration reached
    // earlier records first.
    const std::vector<Hash256> proposal_ids =
        local_archive.list_ids(max_archive_records);
    result.archive_records = proposal_ids.size();

    const std::vector<P2pMiningAnchor> observations =
        load_local_mining_anchors(
            local_archive,
            max_archive_records);
    if (observations.size() != proposal_ids.size()) {
        throw std::runtime_error(
            "mining-work archive enumeration changed during recovery");
    }

    // More than one immutable local record can map to the same share work
    // context. The proposal's miner transaction is outside restart authority,
    // so for identical (height, header, network difficulty) contexts choose the
    // smallest content ID deterministically. Conflicting local anchor metadata
    // is still caught by audit_historical_local_anchor() over the full archive.
    std::map<RecoveryWorkKey, P2pMiningContextId> work_index;
    for (const Hash256& proposal_id : proposal_ids) {
        const P2pMiningContextProposal proposal =
            deserialize_p2p_mining_context_payload(
                local_archive.read(proposal_id));
        const Hash256 mining_header_hash =
            validate_p2p_mining_context_structure(proposal);
        const RecoveryWorkKey key = work_key(
            proposal.zano_height,
            mining_header_hash,
            proposal.network_difficulty);
        auto [it, inserted] =
            work_index.emplace(key, proposal_id);
        if (!inserted && proposal_id < it->second) {
            it->second = proposal_id;
        }
    }

    std::vector<ShareId> share_ids = chain.connected_share_ids();
    std::sort(
        share_ids.begin(),
        share_ids.end(),
        [&chain](const ShareId& left, const ShareId& right) {
            const ConnectedShare* lhs = chain.find(left);
            const ConnectedShare* rhs = chain.find(right);
            if (lhs == nullptr || rhs == nullptr) {
                throw std::runtime_error(
                    "connected share disappeared during restart recovery");
            }
            if (lhs->share.share_height != rhs->share.share_height) {
                return lhs->share.share_height < rhs->share.share_height;
            }
            return left < right;
        });

    for (const ShareId& share_id : share_ids) {
        ++result.connected_considered;

        const ConnectedShare* connected = chain.find(share_id);
        if (connected == nullptr) {
            throw std::runtime_error(
                "connected share disappeared during restart recovery");
        }
        if (connected->validated_ancestry) {
            ++result.already_validated;
            continue;
        }

        const Share candidate = connected->share;
        const auto work_it = work_index.find(work_key(
            candidate.zano_height,
            candidate.mining_header_hash,
            candidate.network_difficulty));
        if (work_it == work_index.end()) {
            ++result.missing_local_work;
            continue;
        }

        const RestartRevalidationResult crossing =
            revalidate_recovered_share(
                chain,
                params,
                share_id,
                local_archive,
                work_it->second,
                observations,
                lookup,
                now,
                mode);

        switch (crossing.status) {
        case RestartRevalidationStatus::Revalidated:
            ++result.revalidated;
            break;
        case RestartRevalidationStatus::AlreadyValidated:
            ++result.already_validated;
            break;
        case RestartRevalidationStatus::ParentUnvalidated:
            ++result.parent_unvalidated;
            break;
        case RestartRevalidationStatus::ShareMissing:
        case RestartRevalidationStatus::ParameterMismatch:
        case RestartRevalidationStatus::CandidateMismatch:
        case RestartRevalidationStatus::AnchorRejected:
        case RestartRevalidationStatus::AnchorChangedBeforeRevalidation:
        case RestartRevalidationStatus::ShareRejected:
            ++result.rejected;
            break;
        }
    }

    return result;
}

}  // namespace zano_p2pool
