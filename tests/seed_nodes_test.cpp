#include "zano_p2pool/seed_nodes.hpp"
#include "test_check.hpp"

int main() {
    using namespace zano_p2pool;

    // No retired or unvalidated infrastructure may be advertised as a
    // built-in default. Both networks currently require explicit peers until
    // replacement public seed infrastructure is provisioned and validated.
    const auto testnet_seeds =
        default_p2p_seed_nodes(P2pNetwork::Testnet);

    CHECK(testnet_seeds.empty());

    CHECK(
        default_p2p_seed_nodes(P2pNetwork::Mainnet)
            .empty());

    const auto defaults_only =
        p2p_bootstrap_nodes(
            P2pNetwork::Testnet,
            {});

    CHECK(defaults_only.empty());

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
            });

    CHECK(combined.size() == 1);

    CHECK(combined[0].host == "seed.example");
    CHECK(combined[0].port == 40000);

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
