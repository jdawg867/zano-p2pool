#include "zano_p2pool/template_refresh.hpp"
#include "test_check.hpp"

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
