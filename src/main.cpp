#include "zano_p2pool/pow_target.hpp"
#include "zano_p2pool/progpowz.hpp"
#include "zano_p2pool/rpc_client.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

enum class Network {
    Mainnet,
    Testnet,
};

constexpr const char* kMainnetRpcUrl =
    "http://127.0.0.1:11211/json_rpc";
constexpr const char* kTestnetRpcUrl =
    "http://127.0.0.1:12111/json_rpc";

struct Options {
    Network network{Network::Testnet};
    std::string rpc_url;
    std::string wallet;
};

const char* network_name(Network network) {
    return network == Network::Testnet ? "testnet" : "mainnet";
}

const char* default_rpc_url(Network network) {
    return network == Network::Testnet ? kTestnetRpcUrl : kMainnetRpcUrl;
}

Network parse_network(const std::string& value) {
    if (value == "testnet") {
        return Network::Testnet;
    }
    if (value == "mainnet") {
        return Network::Mainnet;
    }

    throw std::runtime_error(
        "--network must be either 'testnet' or 'mainnet'");
}

void print_usage(const char* program) {
    std::cout
        << "Usage: " << program
        << " --wallet ZANO_ADDRESS"
        << " [--network testnet|mainnet]"
        << " [--rpc-url URL]\n\n"
        << "Defaults:\n"
        << "  network: testnet\n"
        << "  testnet RPC: " << kTestnetRpcUrl << '\n'
        << "  mainnet RPC: " << kMainnetRpcUrl << '\n';
}

Options parse_args(int argc, char** argv) {
    Options options;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            std::exit(0);
        }

        if (arg == "--wallet") {
            if (++i >= argc) {
                throw std::runtime_error("--wallet requires a value");
            }
            options.wallet = argv[i];
            continue;
        }

        if (arg == "--network") {
            if (++i >= argc) {
                throw std::runtime_error("--network requires a value");
            }
            options.network = parse_network(argv[i]);
            continue;
        }

        if (arg == "--rpc-url") {
            if (++i >= argc) {
                throw std::runtime_error("--rpc-url requires a value");
            }
            options.rpc_url = argv[i];
            continue;
        }

        throw std::runtime_error("unknown argument: " + arg);
    }

    if (options.wallet.empty()) {
        throw std::runtime_error("--wallet is required");
    }

    if (options.rpc_url.empty()) {
        options.rpc_url = default_rpc_url(options.network);
    }

    return options;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse_args(argc, argv);

        std::cout << "zano-p2pool v0.1.0-dev\n";
        std::cout << "Network: " << network_name(options.network) << '\n';
        std::cout << "RPC: " << options.rpc_url << '\n';
        std::cout << "ProgPoWZ backend: "
                  << (zano_p2pool::progpowz_available()
                          ? zano_p2pool::progpowz_revision()
                          : "disabled")
                  << "\n\n";

        if (options.network == Network::Mainnet) {
            std::cerr
                << "WARNING: mainnet mode is experimental and not yet "
                   "recommended for production mining.\n";
        }

        const zano_p2pool::RpcClient rpc(options.rpc_url);
        const auto block = rpc.get_block_template(
            options.wallet,
            "zano-p2pool/0.1.0-dev");
        const auto target =
            zano_p2pool::difficulty_to_target(block.difficulty);

        std::cout << "Template status: " << block.status << '\n';
        std::cout << "Height:          " << block.height << '\n';
        std::cout << "ProgPoWZ epoch:  "
                  << zano_p2pool::progpowz_epoch(block.height) << '\n';
        std::cout << "Previous hash:   " << block.prev_hash << '\n';
        std::cout << "Difficulty:      " << block.difficulty << '\n';
        std::cout << "Target:          " << target.hex() << '\n';
        std::cout << "Block reward:    " << block.block_reward << '\n';
        std::cout << "ProgPoWZ seed:   " << block.seed << '\n';
        std::cout << "Blob bytes:      " << block.blob_bytes() << '\n';

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}
