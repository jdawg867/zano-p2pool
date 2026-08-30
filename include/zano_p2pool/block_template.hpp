#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace zano_p2pool {

struct BlockTemplate {
    std::uint64_t block_reward{};
    std::uint64_t block_reward_without_fee{};
    std::string blocktemplate_blob;
    std::string difficulty;
    std::uint64_t height{};
    // Canonical compact JSON for Zano's RPC miner_tx_tgc object when present.
    // This context contains miner-transaction generation data, not wallet spend
    // keys. It is retained so later P2P validation can audit a proposed template.
    std::string miner_tx_tgc_json;
    std::string prev_hash;
    std::string seed;
    std::string status;
    std::uint64_t txs_fee{};

    [[nodiscard]] std::size_t blob_bytes() const;
};

BlockTemplate parse_block_template_json(std::string_view json_text);

}  // namespace zano_p2pool
