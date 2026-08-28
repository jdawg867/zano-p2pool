#include "zano_p2pool/block_template.hpp"

#include <cassert>
#include <iostream>
#include <string_view>

int main() {
    constexpr std::string_view sample = R"json({
        "block_reward": 1000000000000,
        "block_reward_without_fee": 1000000000000,
        "blocktemplate_blob": "03010203",
        "difficulty": "12936195379842",
        "height": 2555002,
        "prev_hash": "ae73338b7927df71b6ed477937625c230172219306750ba97995fb5109dda703",
        "seed": "0518e1373ff88ccabb28493cac10cb0731313135d880dae0d846be6016ab9acf",
        "status": "OK",
        "txs_fee": 0
    })json";

    const auto block = zano_p2pool::parse_block_template_json(sample);

    assert(block.height == 2555002ULL);
    assert(block.difficulty == "12936195379842");
    assert(block.block_reward == 1000000000000ULL);
    assert(block.blob_bytes() == 4);
    assert(block.prev_hash.size() == 64);
    assert(block.seed.size() == 64);
    assert(block.status == "OK");

    std::cout << "block_template_test: PASS\n";
    return 0;
}
