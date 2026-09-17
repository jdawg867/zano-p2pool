#include "zano_p2pool/historical_work.hpp"
#include <stdexcept>
#include <algorithm>
#include <optional>
#include "zano_p2pool/progpowz.hpp"

namespace zano_p2pool {
HistoricalParentResult audit_historical_parent(
    const P2pMiningContextProposal& proposal,
    const std::function<RpcCanonicalHeader(std::uint64_t)>& lookup) {
    if (proposal.zano_height == 0)
        throw std::runtime_error("historical work has no parent height");
    HistoricalParentResult result;
    result.mining_header_hash = validate_p2p_mining_context_structure(proposal);
    const auto height = proposal.zano_height - 1;
    const auto first = lookup(height);
    const auto second = lookup(height);
    if (first.height != height || second.height != height ||
        first.hash == Hash256{} || second.hash == Hash256{})
        throw std::runtime_error("invalid canonical parent lookup");
    if (first.hash != second.hash) {
        result.status = HistoricalParentStatus::ParentChangedDuringCheck;
    } else if (first.hash == proposal.prev_hash) {
        result.status = HistoricalParentStatus::ParentMatchedUntrusted;
    }
    return result;
}

std::vector<P2pMiningAnchor> load_local_mining_anchors(
    const MiningWorkArchive& archive, std::size_t max_records) {
    std::vector<P2pMiningAnchor> anchors;
    for (const auto& entry : std::filesystem::directory_iterator(archive.path())) {
        if (entry.path().extension() != ".work") continue;
        if (anchors.size() >= max_records)
            throw std::runtime_error("local archive scan limit exceeded; no partial evidence accepted");
        const auto name = entry.path().stem().string();
        if (name.size() != 64 || !std::all_of(name.begin(), name.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        })) throw std::runtime_error("invalid local archive record name");
        const auto bytes = hex_to_bytes(name);
        Hash256 id{};
        std::copy(bytes.begin(), bytes.end(), id.begin());
        const auto proposal = deserialize_p2p_mining_context_payload(archive.read(id));
        static_cast<void>(validate_p2p_mining_context_structure(proposal));
        anchors.push_back({proposal.zano_height, proposal.prev_hash,
            proposal.network_difficulty, proposal.seed, proposal.block_reward_without_fee});
    }
    return anchors;
}

HistoricalAnchorResult audit_historical_local_anchor(
    const P2pMiningContextProposal& proposal,
    std::span<const P2pMiningAnchor> observations,
    const std::function<RpcCanonicalHeader(std::uint64_t)>& lookup,
    const std::function<std::optional<RpcHistoricalPowContext>(
        std::uint64_t)>& historical_pow_lookup) {
    HistoricalAnchorResult result;
    const auto seed = progpowz_seed(proposal.zano_height);
    const auto parent = audit_historical_parent(proposal, lookup);
    result.mining_header_hash = parent.mining_header_hash;
    if (parent.status != HistoricalParentStatus::ParentMatchedUntrusted) {
        result.status = parent.status == HistoricalParentStatus::ParentMismatch ?
            HistoricalAnchorStatus::ParentMismatch : HistoricalAnchorStatus::ParentChangedDuringCheck;
        return result;
    }
    if (proposal.seed != seed) {
        result.status = HistoricalAnchorStatus::SeedMismatch;
        return result;
    }
    std::optional<P2pMiningAnchor> expected;
    for (const auto& observation : observations) {
        if (observation.zano_height != proposal.zano_height ||
            observation.prev_hash != proposal.prev_hash) continue;
        ++result.matching_observations;
        if (observation.seed != seed || (expected && observation != *expected)) {
            result.status = HistoricalAnchorStatus::LocalObservationConflict;
            return result;
        }
        expected = observation;
    }
    if (expected) {
        if (proposal.network_difficulty != expected->network_difficulty ||
            proposal.block_reward_without_fee !=
                expected->block_reward_without_fee) {
            result.status =
                HistoricalAnchorStatus::LocalObservationMismatch;
            return result;
        }

        result.status =
            HistoricalAnchorStatus::AnchorMatchedUntrusted;
        return result;
    }

    // A fresh independent node may never have sampled this exact historical
    // template locally. In that case, and only in that case, allow the
    // operator's own canonical Zano daemon to reconstruct the consensus fields.
    //
    // Peer metadata is never used as authority here:
    //   * parent membership was already checked above by height,
    //   * the ProgPoW seed was derived locally from height,
    //   * next-PoW difficulty and base reward come from local canonical history.
    //
    // Existing matching local observations remain preferred. A local conflict
    // or mismatch above must never fall through to this oracle.
    if (!historical_pow_lookup) {
        result.status =
            HistoricalAnchorStatus::LocalObservationMissing;
        return result;
    }

    const auto historical =
        historical_pow_lookup(proposal.zano_height);

    if (!historical.has_value()) {
        result.status =
            HistoricalAnchorStatus::CanonicalPowContextUnavailable;
        return result;
    }

    if (historical->height != proposal.zano_height ||
        historical->parent_hash != proposal.prev_hash ||
        historical->network_difficulty !=
            proposal.network_difficulty ||
        historical->block_reward_without_fee !=
            proposal.block_reward_without_fee ||
        historical->confirming_pow_height <
            proposal.zano_height) {
        result.status =
            HistoricalAnchorStatus::CanonicalPowContextMismatch;
        return result;
    }

    result.status =
        HistoricalAnchorStatus::AnchorMatchedUntrusted;
    return result;
}

const char* historical_anchor_status_name(HistoricalAnchorStatus status) noexcept {
    switch (status) {
    case HistoricalAnchorStatus::AnchorMatchedUntrusted: return "matched-local-observation-untrusted";
    case HistoricalAnchorStatus::ParentMismatch: return "parent-mismatch";
    case HistoricalAnchorStatus::ParentChangedDuringCheck: return "parent-changed-during-check";
    case HistoricalAnchorStatus::SeedMismatch: return "seed-mismatch";
    case HistoricalAnchorStatus::LocalObservationMissing: return "local-observation-missing";
    case HistoricalAnchorStatus::LocalObservationConflict: return "local-observation-conflict";
    case HistoricalAnchorStatus::LocalObservationMismatch: return "local-observation-mismatch";
    case HistoricalAnchorStatus::CanonicalPowContextUnavailable: return "canonical-pow-context-unavailable";
    case HistoricalAnchorStatus::CanonicalPowContextMismatch: return "canonical-pow-context-mismatch";
    }
    return "unknown";
}
}
