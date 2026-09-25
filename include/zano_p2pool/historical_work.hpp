#pragma once

#include "zano_p2pool/p2p_mining_context.hpp"
#include "zano_p2pool/rpc_client.hpp"
#include <functional>
#include <optional>
#include "zano_p2pool/mining_work_archive.hpp"

namespace zano_p2pool {
enum class HistoricalParentStatus {
    ParentMatchedUntrusted,
    ParentMismatch,
    ParentChangedDuringCheck,
};
struct HistoricalParentResult {
    HistoricalParentStatus status{HistoricalParentStatus::ParentMismatch};
    Hash256 mining_header_hash{};
};
// The lookup must query the operator's local daemon by height, never by a
// peer-supplied hash. Two samples detect a change during this check; they do
// not establish permanent finality or authorize trusted-work insertion.
[[nodiscard]] HistoricalParentResult audit_historical_parent(
    const P2pMiningContextProposal& proposal,
    const std::function<RpcCanonicalHeader(std::uint64_t)>& lookup);

// Only feed this API records captured from this node's own daemon. Archive
// checksums establish integrity, not provenance; peer records are not evidence
// of locally observed difficulty or reward.
[[nodiscard]] std::vector<P2pMiningAnchor> load_local_mining_anchors(
    const MiningWorkArchive& archive, std::size_t max_records = 10000);

enum class HistoricalAnchorStatus {
    AnchorMatchedUntrusted,
    ParentMismatch,
    ParentChangedDuringCheck,
    SeedMismatch,
    LocalObservationMissing,
    LocalObservationConflict,
    LocalObservationMismatch,
    CanonicalPowContextUnavailable,
    CanonicalPowContextMismatch,
};
struct HistoricalAnchorResult {
    HistoricalAnchorStatus status{HistoricalAnchorStatus::LocalObservationMissing};
    Hash256 mining_header_hash{};
    std::size_t matching_observations{};
};
// Locally archived observations remain the preferred authority. If none
// match, historical_pow_lookup may independently reconstruct the historical
// PoW context from the operator's own canonical Zano daemon. This audit still
// has no authority to insert trusted work or admit shares.
[[nodiscard]] HistoricalAnchorResult audit_historical_local_anchor(
    const P2pMiningContextProposal& proposal,
    std::span<const P2pMiningAnchor> local_observations,
    const std::function<RpcCanonicalHeader(std::uint64_t)>& lookup,
    const std::function<std::optional<RpcHistoricalPowContext>(
        std::uint64_t)>& historical_pow_lookup = {});
[[nodiscard]] const char* historical_anchor_status_name(HistoricalAnchorStatus status) noexcept;
}
