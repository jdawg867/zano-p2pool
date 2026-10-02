#include "zano_p2pool/seed_nodes.hpp"
#include "test_check.hpp"

int main() {
    using namespace zano_p2pool;

    const auto testnet_seeds =
        default_p2p_seed_nodes(P2pNetwork::Testnet);

    CHECK(testnet_seeds.size() == 2);

    CHECK(testnet_seeds[0].host == "45.77.77.93");
    CHECK(testnet_seeds[0].port == 37888);

    CHECK(testnet_seeds[1].host == "68.232.175.242");
    CHECK(testnet_seeds[1].port == 37888);

    CHECK(
        default_p2p_seed_nodes(P2pNetwork::Mainnet)
            .empty());

    const auto defaults_only =
        p2p_bootstrap_nodes(
            P2pNetwork::Testnet,
            {});

    CHECK(defaults_only.size() == 2);
    CHECK(defaults_only[0].host == "45.77.77.93");
    CHECK(defaults_only[0].port == 37888);
    CHECK(defaults_only[1].host == "68.232.175.242");
    CHECK(defaults_only[1].port == 37888);

    const P2pEndpoint manual{
        "seed.example",
        40000,
    };

    const auto combined =
        p2p_bootstrap_nodes(
            P2pNetwork::Testnet,
            {
                manual,
                manual,
                testnet_seeds[0],
            });

    CHECK(combined.size() == 3);

    CHECK(combined[0].host == "seed.example");
    CHECK(combined[0].port == 40000);

    CHECK(combined[1].host == "45.77.77.93");
    CHECK(combined[1].port == 37888);

    CHECK(combined[2].host == "68.232.175.242");
    CHECK(combined[2].port == 37888);

    const auto disabled =
        p2p_bootstrap_nodes(
            P2pNetwork::Testnet,
            {manual},
            false);

    CHECK(disabled.size() == 1);
    CHECK(disabled[0].host == "seed.example");
    CHECK(disabled[0].port == 40000);

    const auto disabled_empty =
        p2p_bootstrap_nodes(
            P2pNetwork::Testnet,
            {},
            false);

    CHECK(disabled_empty.empty());

    const auto mainnet =
        p2p_bootstrap_nodes(
            P2pNetwork::Mainnet,
            {});

    CHECK(mainnet.empty());

    return 0;
}
