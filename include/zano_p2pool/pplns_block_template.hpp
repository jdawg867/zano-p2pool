#pragma once

#include "zano_p2pool/block_template.hpp"
#include "zano_p2pool/mining_header.hpp"

#include <string_view>

namespace zano_p2pool {

struct PplnsBlockTemplate {
    BlockTemplate block;
    MiningHeaderWork mining_work;
};

// Replace only block.miner_tx in an existing current-HF6 RPC template. The
// original serialized block header and regular transaction-hash trailer are
// preserved byte-for-byte. The returned mining work is re-derived from the
// rebuilt block, so miners never receive the daemon template's old header hash
// after payout destinations change.
[[nodiscard]] PplnsBlockTemplate replace_hf6_miner_tx(
    const BlockTemplate& base,
    std::string_view miner_tx_blob_hex);

}  // namespace zano_p2pool
