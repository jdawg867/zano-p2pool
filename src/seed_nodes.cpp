#include "zano_p2pool/seed_nodes.hpp"

#include <algorithm>

namespace zano_p2pool {
namespace {

[[nodiscard]] bool same_endpoint(
    const P2pEndpoint& left,
    const P2pEndpoint& right) noexcept {
    return left.host == right.host && left.port == right.port;
}

void append_unique(
    std::vector<P2pEndpoint>& result,
    const P2pEndpoint& endpoint) {
    if (std::none_of(
            result.begin(),
            result.end(),
            [&](const P2pEndpoint& existing) {
                return same_endpoint(existing, endpoint);
            })) {
        result.push_back(endpoint);
    }
}

}  // namespace

std::vector<P2pEndpoint> default_p2p_seed_nodes(P2pNetwork) {
    return {};
}

std::vector<P2pEndpoint> p2p_bootstrap_nodes(
    P2pNetwork network,
    const std::vector<P2pEndpoint>& explicit_peers,
    bool include_default_seeds) {
    std::vector<P2pEndpoint> result;
    result.reserve(
        explicit_peers.size() + (include_default_seeds ? 1U : 0U));

    for (const auto& peer : explicit_peers) {
        append_unique(result, peer);
    }
    if (include_default_seeds) {
        for (const auto& seed : default_p2p_seed_nodes(network)) {
            append_unique(result, seed);
        }
    }
    return result;
}

}  // namespace zano_p2pool
