#include "zano_p2pool/template_refresh.hpp"
#include "test_check.hpp"

#include <cstdint>
#include <stdexcept>
#include <vector>

namespace {

zano_p2pool::BlockTemplate make_block() {
    zano_p2pool::BlockTemplate block;
    block.height = 165177;
    block.prev_hash = "28e8ebe20d883d924815fddcc31dd5b7824d26db45eca815200aad1e90b85f87";
    block.difficulty = "1193434";
    block.seed = "f2e59013a0a379837166b59f871b20a8a0d101d1c355ea85d35329360e69c000";
    block.block_reward = 1'000'000'000'000ULL;
    block.block_reward_without_fee = 1'000'000'000'000ULL;
    block.txs_fee = 0;
    return block;
}

}  // namespace

int main() {
    using namespace zano_p2pool;

    const BlockTemplate current_block = make_block();
    MiningHeaderWork current_work;
    current_work.header_hash[0] = 0x11;

    // An unchanged template is not a canonical reorg.
    CHECK(
        classify_canonical_reorg(
            current_block,
            current_block,
            false) ==
        CanonicalReorgKind::None);

    // A normal height advance must not be treated as a reorg merely because
    // the daemon moved forward.
    BlockTemplate normal_advance = current_block;
    ++normal_advance.height;
    CHECK(
        classify_canonical_reorg(
            current_block,
            normal_advance,
            false) ==
        CanonicalReorgKind::None);

    // If the previously installed work's parent is later proven displaced,
    // an observed height advance represents a skipped/advanced reorg.
    CHECK(
        classify_canonical_reorg(
            current_block,
            normal_advance,
            true) ==
        CanonicalReorgKind::AdvancedReplacement);

    // A changed parent at the same mining height is direct reorg evidence.
    BlockTemplate same_height_replacement = current_block;
    same_height_replacement.prev_hash[0] =
        same_height_replacement.prev_hash[0] == '0' ? '1' : '0';
    CHECK(
        classify_canonical_reorg(
            current_block,
            same_height_replacement,
            false) ==
        CanonicalReorgKind::SameHeightReplacement);

    // Moving backwards in daemon template height is always handled as a
    // fail-closed rollback transition.
    BlockTemplate rollback = current_block;
    --rollback.height;
    CHECK(
        classify_canonical_reorg(
            current_block,
            rollback,
            false) ==
        CanonicalReorgKind::Rollback);

    // No reorg means no publication stop and no historical audit.
    bool stopped = false;
    std::size_t lookup_calls = 0;

    const CanonicalReorgAuditPlan no_reorg_plan =
        prepare_canonical_reorg_audit(
            CanonicalReorgKind::None,
            current_block.height,
            [&] {
                stopped = true;
            },
            [&] {
                CHECK(!stopped);
                return std::vector<std::uint64_t>{
                    current_block.height};
            },
            [&](std::uint64_t) {
                ++lookup_calls;
                return Hash256{};
            });

    CHECK(!stopped);
    CHECK(lookup_calls == 0);
    CHECK(no_reorg_plan.kind == CanonicalReorgKind::None);
    CHECK(no_reorg_plan.canonical_parents.empty());

    // Once reorg evidence is established, publication must already be stopped
    // when the first broad historical lookup begins. Height zero and work above
    // the new daemon height are deliberately omitted from the canonical audit.
    stopped = false;
    lookup_calls = 0;

    Hash256 parent_99{};
    parent_99[0] = 0x99;
    Hash256 parent_100{};
    parent_100[0] = 0xa0;

    const CanonicalReorgAuditPlan replacement_plan =
        prepare_canonical_reorg_audit(
            CanonicalReorgKind::SameHeightReplacement,
            100,
            [&] {
                CHECK(!stopped);
                stopped = true;
            },
            [&] {
                CHECK(stopped);
                return std::vector<std::uint64_t>{
                    0, 99, 100, 101};
            },
            [&](std::uint64_t height) {
                CHECK(stopped);
                ++lookup_calls;

                if (height == 99) {
                    return parent_99;
                }
                CHECK(height == 100);
                return parent_100;
            });

    CHECK(stopped);
    CHECK(lookup_calls == 2);
    CHECK(
        replacement_plan.kind ==
        CanonicalReorgKind::SameHeightReplacement);
    CHECK(replacement_plan.maximum_work_height == 100);
    CHECK(replacement_plan.canonical_parents.size() == 2);
    CHECK(replacement_plan.canonical_parents[0].first == 99);
    CHECK(replacement_plan.canonical_parents[0].second == parent_99);
    CHECK(replacement_plan.canonical_parents[1].first == 100);
    CHECK(replacement_plan.canonical_parents[1].second == parent_100);

    // Most importantly, an audit failure after reorg evidence occurs only
    // after publication has stopped. Because the helper never returns a plan,
    // the caller cannot begin reconciliation from a partial audit.
    stopped = false;
    lookup_calls = 0;
    bool audit_threw = false;
    bool reconciliation_started = false;

    try {
        const CanonicalReorgAuditPlan failed_plan =
            prepare_canonical_reorg_audit(
                CanonicalReorgKind::AdvancedReplacement,
                101,
                [&] {
                    stopped = true;
                },
                [&] {
                    CHECK(stopped);
                    return std::vector<std::uint64_t>{
                        100, 101};
                },
                [&](std::uint64_t height) -> Hash256 {
                    CHECK(stopped);
                    ++lookup_calls;

                    if (height == 100) {
                        Hash256 parent{};
                        parent[0] = 0xb0;
                        return parent;
                    }

                    CHECK(height == 101);
                    throw std::runtime_error(
                        "simulated canonical audit failure");
                });

        static_cast<void>(failed_plan);
        reconciliation_started = true;
    } catch (const std::runtime_error&) {
        audit_threw = true;
    }

    CHECK(audit_threw);
    CHECK(stopped);
    CHECK(lookup_calls == 2);
    CHECK(!reconciliation_started);

    // Repeated getblocktemplate calls at the same chain tip may randomize the
    // daemon-generated miner transaction/header. That alone must not churn jobs.
    BlockTemplate randomized_block = current_block;
    randomized_block.blocktemplate_blob = "different-randomized-template";
    MiningHeaderWork randomized_work = current_work;
    randomized_work.header_hash[0] = 0x22;
    randomized_work.miner_tx_prefix.hash[0] = 0x33;
    CHECK(!should_refresh_stratum_template(
        current_block, current_work, randomized_block, randomized_work));

    BlockTemplate new_height = current_block;
    ++new_height.height;
    CHECK(should_refresh_stratum_template(
        current_block, current_work, new_height, randomized_work));

    BlockTemplate reorg = current_block;
    reorg.prev_hash[0] = reorg.prev_hash[0] == '0' ? '1' : '0';
    CHECK(should_refresh_stratum_template(
        current_block, current_work, reorg, randomized_work));

    BlockTemplate new_difficulty = current_block;
    new_difficulty.difficulty = "1193435";
    CHECK(should_refresh_stratum_template(
        current_block, current_work, new_difficulty, randomized_work));

    BlockTemplate new_seed = current_block;
    new_seed.seed[0] = new_seed.seed[0] == '0' ? '1' : '0';
    CHECK(should_refresh_stratum_template(
        current_block, current_work, new_seed, randomized_work));

    BlockTemplate new_reward = current_block;
    ++new_reward.block_reward;
    CHECK(should_refresh_stratum_template(
        current_block, current_work, new_reward, randomized_work));

    MiningHeaderWork tx_change = current_work;
    Hash256 tx_hash{};
    tx_hash[31] = 0x42;
    tx_change.tx_hashes.hashes.push_back(tx_hash);
    CHECK(should_refresh_stratum_template(
        current_block, current_work, current_block, tx_change));

    return 0;
}
