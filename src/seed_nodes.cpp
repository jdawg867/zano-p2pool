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

std::vector<P2pEndpoint> default_p2p_seed_nodes(P2pNetwork network) {
    switch (network) {
    case P2pNetwork::Testnet:
        // The public beta.2 testnet seed VPSs were retired after completing
        // their validation role. Keep defaults empty until replacement public
        // testnet infrastructure is intentionally provisioned and validated.
        return {};

    case P2pNetwork::Mainnet:
        return {};
    }

    return {};
}

std::vector<P2pEndpoint> p2p_bootstrap_nodes(
    P2pNetwork network,
    const std::vector<P2pEndpoint>& explicit_peers,
    bool include_default_seeds) {
    const auto seeds = include_default_seeds
        ? default_p2p_seed_nodes(network)
        : std::vector<P2pEndpoint>{};

    std::vector<P2pEndpoint> result;
    result.reserve(explicit_peers.size() + seeds.size());

    for (const auto& peer : explicit_peers) {
        append_unique(result, peer);
    }

    for (const auto& seed : seeds) {
        append_unique(result, seed);
    }

    return result;
}

}  // namespace zano_p2pool
