#include "zano_p2pool/seed_nodes.hpp"
#include "test_check.hpp"

#include <string>
#include <vector>

int main() {
    using namespace zano_p2pool;

    const auto testnet = default_p2p_seed_nodes(P2pNetwork::Testnet);
    CHECK(testnet.size() == 1);
    CHECK(testnet[0].host == "207.148.30.120");
    CHECK(testnet[0].port == kDefaultP2pSeedPort);
    CHECK(default_p2p_seed_nodes(P2pNetwork::Mainnet).empty());

    const P2pEndpoint manual{"seed.example", 40000};
    const auto combined = p2p_bootstrap_nodes(
        P2pNetwork::Testnet,
        {manual, testnet[0], manual});
    CHECK(combined.size() == 2);
    CHECK(combined[0].host == manual.host);
    CHECK(combined[0].port == manual.port);
    CHECK(combined[1].host == testnet[0].host);
    CHECK(combined[1].port == testnet[0].port);

    const auto disabled = p2p_bootstrap_nodes(
        P2pNetwork::Testnet,
        {manual},
        false);
    CHECK(disabled.size() == 1);
    CHECK(disabled[0].host == manual.host);

    return 0;
}
