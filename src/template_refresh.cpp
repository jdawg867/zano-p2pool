#include "zano_p2pool/template_refresh.hpp"

namespace zano_p2pool {

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
