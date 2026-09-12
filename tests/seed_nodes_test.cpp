#include "zano_p2pool/seed_nodes.hpp"
#include "test_check.hpp"

#include <string>
#include <vector>

int main() {
    using namespace zano_p2pool;

    CHECK(default_p2p_seed_nodes(P2pNetwork::Testnet).empty());
    CHECK(default_p2p_seed_nodes(P2pNetwork::Mainnet).empty());

    const P2pEndpoint manual{"seed.example", 40000};
    const auto combined = p2p_bootstrap_nodes(
        P2pNetwork::Testnet,
        {manual, manual});
    CHECK(combined.size() == 1);
    CHECK(combined[0].host == manual.host);
    CHECK(combined[0].port == manual.port);

    const auto disabled = p2p_bootstrap_nodes(
        P2pNetwork::Testnet,
        {manual},
        false);
    CHECK(disabled.size() == 1);
    CHECK(disabled[0].host == manual.host);

    return 0;
}
