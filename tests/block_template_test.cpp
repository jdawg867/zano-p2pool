#include "zano_p2pool/block_template.hpp"
#include "test_check.hpp"

#include <iostream>
#include <string_view>

int main() {
    constexpr std::string_view sample = R"json({
        "block_reward": 1000000000000,
        "block_reward_without_fee": 1000000000000,
        "blocktemplate_blob": "03010203",
        "difficulty": "12936195379842",
        "height": 2555002,
        "miner_tx_tgc": {
            "tx_key": "0011",
            "tx_pub_key_p": "2233",
            "tx_outs_attr": 0
        },
        "prev_hash": "ae73338b7927df71b6ed477937625c230172219306750ba97995fb5109dda703",
        "seed": "0518e1373ff88ccabb28493cac10cb0731313135d880dae0d846be6016ab9acf",
        "status": "OK",
        "txs_fee": 0
    })json";

    const auto block = zano_p2pool::parse_block_template_json(sample);

    CHECK(block.height == 2555002ULL);
    CHECK(block.difficulty == "12936195379842");
    CHECK(block.block_reward == 1000000000000ULL);
    CHECK(block.blob_bytes() == 4);
    CHECK(block.miner_tx_tgc_json.find("\"tx_key\":\"0011\"") !=
          std::string::npos);
    CHECK(block.prev_hash.size() == 64);
    CHECK(block.seed.size() == 64);
    CHECK(block.status == "OK");

    // Sanitized/older fixtures may intentionally omit miner_tx_tgc; parsing the
    // rest of the daemon template remains backwards compatible.
    constexpr std::string_view without_tgc = R"json({
        "block_reward": 1,
        "block_reward_without_fee": 1,
        "blocktemplate_blob": "00",
        "difficulty": "1",
        "height": 1,
        "prev_hash": "0000000000000000000000000000000000000000000000000000000000000000",
        "seed": "0000000000000000000000000000000000000000000000000000000000000000",
        "status": "OK",
        "txs_fee": 0
    })json";
    CHECK(zano_p2pool::parse_block_template_json(without_tgc)
              .miner_tx_tgc_json.empty());

    std::cout << "block_template_test: PASS\n";
    return 0;
}
