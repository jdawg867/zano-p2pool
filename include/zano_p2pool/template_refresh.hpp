#pragma once

#include "zano_p2pool/block_template.hpp"
#include "zano_p2pool/mining_header.hpp"

namespace zano_p2pool {

// Returns true when a newly fetched daemon template represents materially new
// mining work for connected miners. Zano may randomize coinbase/header bytes on
// repeated getblocktemplate calls at the same chain tip; those differences alone
// must not churn Stratum jobs.
[[nodiscard]] bool should_refresh_stratum_template(
    const BlockTemplate& current_block,
    const MiningHeaderWork& current_work,
    const BlockTemplate& next_block,
    const MiningHeaderWork& next_work) noexcept;

}  // namespace zano_p2pool
