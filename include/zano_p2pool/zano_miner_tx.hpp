#pragma once

#include "zano_p2pool/pplns_coinbase.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace zano_p2pool {

inline constexpr std::size_t kZanoFullRewardZoneBytes = 125000;

struct ZanoMinerTxResult {
    std::string tx_blob_hex;
    std::uint64_t block_reward_without_fee{0};
    std::uint64_t block_reward{0};
};

[[nodiscard]] bool zano_miner_tx_builder_available() noexcept;

// Build a current HF6 PoW miner transaction through Zano's pinned canonical
// construct_miner_tx() implementation. The caller supplies an already-verified
// direct PPLNS destination plan. This first adapter is intentionally limited to
// candidates inside the 125 kB full-reward zone; callers must independently
// verify the final assembled block remains inside that zone before mining it.
//
// expected_block_reward is the reward advertised by the daemon template. The
// adapter refuses output unless Zano's own constructor computes exactly the same
// value, preventing a P2Pool/daemon reward-rule mismatch from becoming work.
[[nodiscard]] ZanoMinerTxResult build_zano_hf6_pplns_miner_tx(
    std::uint64_t height,
    std::uint64_t fee,
    std::uint64_t expected_block_reward,
    const PplnsCoinbasePlan& plan,
    std::string_view extra_text = {});

}  // namespace zano_p2pool
