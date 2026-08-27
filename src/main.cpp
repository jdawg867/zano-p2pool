#include "zano_p2pool/rpc_client.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

struct Options {
    std::string rpc_url{"http://127.0.0.1:11211/json_rpc"};
    std::string wallet;
};

void print_usage(const char* program) {
    std::cout
        << "Usage: " << program
        << " --wallet ZANO_ADDRESS"
        << " [--rpc-url http://127.0.0.1:11211/json_rpc]\n";
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

    return options;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse_args(argc, argv);

        std::cout << "zano-p2pool v0.1.0-dev\n";
        std::cout << "RPC: " << options.rpc_url << "\n\n";

        const zano_p2pool::RpcClient rpc(options.rpc_url);
        const auto block = rpc.get_block_template(
            options.wallet,
            "zano-p2pool/0.1.0-dev");

        std::cout << "Template status: " << block.status << '\n';
        std::cout << "Height:          " << block.height << '\n';
        std::cout << "Previous hash:   " << block.prev_hash << '\n';
        std::cout << "Difficulty:      " << block.difficulty << '\n';
        std::cout << "Block reward:    " << block.block_reward << '\n';
        std::cout << "ProgPoWZ seed:   " << block.seed << '\n';
        std::cout << "Blob bytes:      " << block.blob_bytes() << '\n';

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}
