#include "zano_p2pool/runtime_network.hpp"

#include <stdexcept>
#include <string>

namespace zano_p2pool {

RuntimeNetwork parse_runtime_network(std::string_view value) {
    if (value == "testnet") {
        return RuntimeNetwork::Testnet;
    }
    if (value == "mainnet") {
        return RuntimeNetwork::Mainnet;
    }

    throw std::runtime_error(
        "--network must be either 'testnet' or 'mainnet'");
}

std::string_view runtime_network_name(RuntimeNetwork network) {
    switch (network) {
    case RuntimeNetwork::Testnet:
        return "testnet";
    case RuntimeNetwork::Mainnet:
        return "mainnet";
    }

    throw std::invalid_argument("unsupported runtime network");
}

std::string_view runtime_default_rpc_url(RuntimeNetwork network) {
    switch (network) {
    case RuntimeNetwork::Testnet:
        return kRuntimeTestnetRpcUrl;
    case RuntimeNetwork::Mainnet:
        return kRuntimeMainnetRpcUrl;
    }

    throw std::invalid_argument("unsupported runtime network");
}

std::filesystem::path runtime_default_share_store_path(
    RuntimeNetwork network,
    const std::filesystem::path& home) {
    if (home.empty()) {
        throw std::invalid_argument(
            "home path must not be empty for default share store");
    }

    return home /
           ".zano-p2pool" /
           std::string(runtime_network_name(network)) /
           "shares.dat";
}

P2pNetwork runtime_p2p_network(RuntimeNetwork network) {
    switch (network) {
    case RuntimeNetwork::Testnet:
        return P2pNetwork::Testnet;
    case RuntimeNetwork::Mainnet:
        return P2pNetwork::Mainnet;
    }

    throw std::invalid_argument("unsupported runtime network");
}

SidechainParentNetwork runtime_sidechain_parent_network(
    RuntimeNetwork network) {
    switch (network) {
    case RuntimeNetwork::Testnet:
        return SidechainParentNetwork::Testnet;
    case RuntimeNetwork::Mainnet:
        return SidechainParentNetwork::Mainnet;
    }

    throw std::invalid_argument("unsupported runtime network");
}

bool runtime_network_operation_allowed(
    RuntimeNetwork network,
    bool long_lived_runtime,
    bool experimental_mainnet_opt_in) noexcept {
    if (network != RuntimeNetwork::Mainnet) {
        return true;
    }
    if (!long_lived_runtime) {
        return true;
    }
    return experimental_mainnet_opt_in;
}

}  // namespace zano_p2pool
