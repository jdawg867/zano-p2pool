#pragma once

#include "zano_p2pool/p2p_transport.hpp"

#include <vector>

namespace zano_p2pool {

inline constexpr std::uint16_t kDefaultP2pSeedPort = 37888;

[[nodiscard]] std::vector<P2pEndpoint> default_p2p_seed_nodes(
    P2pNetwork network);

[[nodiscard]] std::vector<P2pEndpoint> p2p_bootstrap_nodes(
    P2pNetwork network,
    const std::vector<P2pEndpoint>& explicit_peers,
    bool include_default_seeds = true);

}  // namespace zano_p2pool
