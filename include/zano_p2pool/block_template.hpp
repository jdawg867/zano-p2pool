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
    std::string prev_hash;
    std::string seed;
    std::string status;
    std::uint64_t txs_fee{};

    [[nodiscard]] std::size_t blob_bytes() const;
};

BlockTemplate parse_block_template_json(std::string_view json_text);

}  // namespace zano_p2pool
