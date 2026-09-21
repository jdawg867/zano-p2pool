#include "zano_p2pool/template_refresh.hpp"

namespace zano_p2pool {

CanonicalReorgKind classify_canonical_reorg(
    const BlockTemplate& current_block,
    const BlockTemplate& next_block,
    bool advanced_parent_replacement) noexcept {
    if (next_block.height < current_block.height) {
        return CanonicalReorgKind::Rollback;
    }

    if (next_block.height == current_block.height &&
        next_block.prev_hash != current_block.prev_hash) {
        return CanonicalReorgKind::SameHeightReplacement;
    }

    if (next_block.height > current_block.height &&
        advanced_parent_replacement) {
        return CanonicalReorgKind::AdvancedReplacement;
    }

    return CanonicalReorgKind::None;
}

CanonicalReorgAuditPlan prepare_canonical_reorg_audit(
    CanonicalReorgKind kind,
    std::uint64_t maximum_work_height,
    const std::function<void()>& stop_publication,
    const std::function<std::vector<std::uint64_t>()>&
        provenance_heights,
    const std::function<Hash256(std::uint64_t)>&
        canonical_parent_for_work_height) {
    CanonicalReorgAuditPlan plan;
    plan.kind = kind;
    plan.maximum_work_height = maximum_work_height;

    if (kind == CanonicalReorgKind::None) {
        return plan;
    }

    // This ordering is the fail-closed boundary: after reorg evidence exists,
    // miner-facing publication is stopped before any later audit lookup can
    // fail or observe a moving chain.
    stop_publication();

    const std::vector<std::uint64_t> heights =
        provenance_heights();

    plan.canonical_parents.reserve(heights.size());

    for (const std::uint64_t height : heights) {
        if (height == 0 || height > maximum_work_height) {
            continue;
        }

        plan.canonical_parents.emplace_back(
            height,
            canonical_parent_for_work_height(height));
    }

    return plan;
}

bool should_refresh_stratum_template(
    const BlockTemplate& current_block,
    const MiningHeaderWork& current_work,
    const BlockTemplate& next_block,
    const MiningHeaderWork& next_work) noexcept {
    if (current_block.height != next_block.height ||
        current_block.prev_hash != next_block.prev_hash ||
        current_block.difficulty != next_block.difficulty ||
        current_block.seed != next_block.seed ||
        current_block.block_reward != next_block.block_reward ||
        current_block.block_reward_without_fee != next_block.block_reward_without_fee ||
        current_block.txs_fee != next_block.txs_fee) {
        return true;
    }

    // At an unchanged chain tip, refresh only when the regular transaction set
    // changes. Differences confined to the daemon-generated miner transaction,
    // timestamp, or other per-request template randomness are intentionally
    // ignored so miners are not needlessly restarted every poll interval.
    return current_work.tx_hashes.hashes != next_work.tx_hashes.hashes;
}

}  // namespace zano_p2pool
