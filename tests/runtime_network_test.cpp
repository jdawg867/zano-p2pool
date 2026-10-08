#include "zano_p2pool/runtime_network.hpp"
#include "zano_p2pool/p2p_protocol.hpp"
#include "zano_p2pool/sidechain_params.hpp"
#include "test_check.hpp"

#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>

namespace {

bool throws_runtime_error(const std::function<void()>& fn) {
    try {
        fn();
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

}  // namespace

int main() {
    using namespace zano_p2pool;

    CHECK(parse_runtime_network("testnet") == RuntimeNetwork::Testnet);
    CHECK(parse_runtime_network("mainnet") == RuntimeNetwork::Mainnet);
    CHECK(throws_runtime_error([] {
        static_cast<void>(parse_runtime_network("production"));
    }));

    CHECK(runtime_network_name(RuntimeNetwork::Testnet) == "testnet");
    CHECK(runtime_network_name(RuntimeNetwork::Mainnet) == "mainnet");

    CHECK(
        runtime_default_rpc_url(RuntimeNetwork::Testnet) ==
        "http://127.0.0.1:12111/json_rpc");
    CHECK(
        runtime_default_rpc_url(RuntimeNetwork::Mainnet) ==
        "http://127.0.0.1:11211/json_rpc");

    const std::filesystem::path home{"/srv/zano-p2pool-test-home"};
    CHECK(
        runtime_default_share_store_path(
            RuntimeNetwork::Testnet,
            home) ==
        home / ".zano-p2pool" / "testnet" / "shares.dat");
    CHECK(
        runtime_default_share_store_path(
            RuntimeNetwork::Mainnet,
            home) ==
        home / ".zano-p2pool" / "mainnet" / "shares.dat");
    CHECK(
        runtime_default_share_store_path(
            RuntimeNetwork::Testnet,
            home) !=
        runtime_default_share_store_path(
            RuntimeNetwork::Mainnet,
            home));

    CHECK(
        runtime_p2p_network(RuntimeNetwork::Testnet) ==
        P2pNetwork::Testnet);
    CHECK(
        runtime_p2p_network(RuntimeNetwork::Mainnet) ==
        P2pNetwork::Mainnet);

    CHECK(
        runtime_sidechain_parent_network(RuntimeNetwork::Testnet) ==
        SidechainParentNetwork::Testnet);
    CHECK(
        runtime_sidechain_parent_network(RuntimeNetwork::Mainnet) ==
        SidechainParentNetwork::Mainnet);

    const SidechainId testnet_id =
        canonical_p2p_sidechain_id(
            runtime_p2p_network(RuntimeNetwork::Testnet));
    const SidechainId mainnet_id =
        canonical_p2p_sidechain_id(
            runtime_p2p_network(RuntimeNetwork::Mainnet));

    CHECK(
        testnet_id ==
        sidechain_id(canonical_sidechain_parameters(
            runtime_sidechain_parent_network(
                RuntimeNetwork::Testnet))));
    CHECK(
        mainnet_id ==
        sidechain_id(canonical_sidechain_parameters(
            runtime_sidechain_parent_network(
                RuntimeNetwork::Mainnet))));
    CHECK(testnet_id != mainnet_id);

    CHECK(runtime_network_operation_allowed(
        RuntimeNetwork::Testnet,
        false,
        false));
    CHECK(runtime_network_operation_allowed(
        RuntimeNetwork::Testnet,
        true,
        false));
    CHECK(runtime_network_operation_allowed(
        RuntimeNetwork::Mainnet,
        false,
        false));
    CHECK(!runtime_network_operation_allowed(
        RuntimeNetwork::Mainnet,
        true,
        false));
    CHECK(runtime_network_operation_allowed(
        RuntimeNetwork::Mainnet,
        true,
        true));

    return 0;
}
