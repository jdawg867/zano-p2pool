#pragma once

#include "zano_p2pool/pplns_coinbase.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace zano_p2pool {

inline constexpr std::size_t kZanoFullRewardZoneBytes = 125000;

struct ZanoMinerTxResult {
    std::string tx_blob_hex;
    std::string miner_tx_tgc_json;
    std::uint64_t block_reward_without_fee{0};
    std::uint64_t block_reward{0};
};

[[nodiscard]] bool zano_miner_tx_builder_available() noexcept;

// Read the mandatory HF6 etc_coinbase_block_cumulative_size value from an
// exact daemon miner transaction. The value represents the serialized size of
// regular block transactions and must survive a PPLNS miner-tx replacement.
[[nodiscard]] std::uint64_t extract_zano_hf6_coinbase_cumulative_size(
    std::span<const std::uint8_t> tx_blob);

// Build a current HF6 PoW miner transaction through Zano's pinned canonical
// construct_miner_tx() implementation. The caller supplies an already-verified
// direct PPLNS destination plan. This first adapter is intentionally limited to
// candidates inside the 125 kB full-reward zone; callers must independently
// verify the final assembled block remains inside that zone before mining it.
//
// The returned miner_tx_tgc_json is Zano's canonical KV serialization of the
// exact tx_generation_context produced by construct_miner_tx(). It must travel
// with tx_blob_hex when the transaction is embedded into a mining template so
// peers can independently verify its key binding and confidential proofs.
//
// expected_block_reward is the reward advertised by the daemon template. The
// adapter refuses output unless Zano's own constructor computes exactly the same
// value, preventing a P2Pool/daemon reward-rule mismatch from becoming work.
// coinbase_block_cumulative_size must be copied from the daemon template's
// original HF6 miner transaction. Zano consensus requires this extra field even
// when its value is zero.
[[nodiscard]] ZanoMinerTxResult build_zano_hf6_pplns_miner_tx(
    std::uint64_t height,
    std::uint64_t fee,
    std::uint64_t expected_block_reward,
    const PplnsCoinbasePlan& plan,
    std::string_view extra_text = {},
    std::uint64_t coinbase_block_cumulative_size = 0);

}  // namespace zano_p2pool
