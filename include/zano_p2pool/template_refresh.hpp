#pragma once

#include "zano_p2pool/block_template.hpp"
#include "zano_p2pool/mining_header.hpp"

#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace zano_p2pool {

enum class CanonicalReorgKind {
    None,
    SameHeightReplacement,
    AdvancedReplacement,
    Rollback,
};

[[nodiscard]] CanonicalReorgKind classify_canonical_reorg(
    const BlockTemplate& current_block,
    const BlockTemplate& next_block,
    bool advanced_parent_replacement) noexcept;

struct CanonicalReorgAuditPlan {
    CanonicalReorgKind kind{CanonicalReorgKind::None};
    std::uint64_t maximum_work_height{0};
    std::vector<std::pair<std::uint64_t, Hash256>> canonical_parents;
};

// For a confirmed canonical reorg, stop miner-facing publication before any
// broad historical canonical-parent observations are attempted. All lookups
// complete before this function returns a reconciliation plan; therefore a
// lookup exception cannot expose a partially prepared mutation plan.
[[nodiscard]] CanonicalReorgAuditPlan prepare_canonical_reorg_audit(
    CanonicalReorgKind kind,
    std::uint64_t maximum_work_height,
    const std::function<void()>& stop_publication,
    const std::function<std::vector<std::uint64_t>()>&
        provenance_heights,
    const std::function<Hash256(std::uint64_t)>&
        canonical_parent_for_work_height);

// Returns true when a newly fetched daemon template represents materially new
// mining work for connected miners. Zano may randomize coinbase/header bytes on
// repeated getblocktemplate calls at the same chain tip; those differences alone
// must not churn Stratum jobs.
[[nodiscard]] bool should_refresh_stratum_template(
    const BlockTemplate& current_block,
    const MiningHeaderWork& current_work,
    const BlockTemplate& next_block,
    const MiningHeaderWork& next_work) noexcept;

}  // namespace zano_p2pool
