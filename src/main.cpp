#include "zano_p2pool/crypto_hash.hpp"
#include "zano_p2pool/mining_header.hpp"
#include "zano_p2pool/pow_target.hpp"
#include "zano_p2pool/progpowz.hpp"
#include "zano_p2pool/rpc_client.hpp"
#include "zano_p2pool/share.hpp"
#include "zano_p2pool/stratum_server.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {

enum class Network {
    Mainnet,
    Testnet,
};

constexpr const char* kMainnetRpcUrl =
    "http://127.0.0.1:11211/json_rpc";
constexpr const char* kTestnetRpcUrl =
    "http://127.0.0.1:12111/json_rpc";
constexpr std::uint16_t kDefaultStratumPort = 3333;
constexpr std::uint64_t kDefaultStratumDifficulty = 100000000;
constexpr std::uint64_t kDefaultTemplateRefreshSeconds = 5;

volatile std::sig_atomic_t g_stop_requested = 0;

void handle_signal(int) noexcept {
    g_stop_requested = 1;
}

struct Options {
    Network network{Network::Testnet};
    std::string rpc_url;
    std::string wallet;
    bool stratum{false};
    std::string stratum_bind{"127.0.0.1"};
    std::uint16_t stratum_port{kDefaultStratumPort};
    std::uint64_t stratum_difficulty{kDefaultStratumDifficulty};
    std::uint64_t template_refresh_seconds{kDefaultTemplateRefreshSeconds};
};

struct LiveTemplate {
    zano_p2pool::BlockTemplate block;
    zano_p2pool::MiningHeaderWork mining_work;
    zano_p2pool::Hash256 seed_hash{};
    zano_p2pool::Difficulty128 network_difficulty{};
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

std::uint64_t parse_u64_option(
    std::string_view option,
    const std::string& value) {
    if (value.empty() || value.front() == '-') {
        throw std::runtime_error(std::string(option) + " requires an unsigned integer");
    }

    std::size_t parsed = 0;
    std::uint64_t result = 0;
    try {
        result = std::stoull(value, &parsed, 10);
    } catch (const std::exception&) {
        throw std::runtime_error(std::string(option) + " requires an unsigned integer");
    }
    if (parsed != value.size()) {
        throw std::runtime_error(std::string(option) + " requires an unsigned integer");
    }
    return result;
}

std::uint16_t parse_port(const std::string& value) {
    const std::uint64_t port = parse_u64_option("--stratum-port", value);
    if (port == 0 || port > std::numeric_limits<std::uint16_t>::max()) {
        throw std::runtime_error("--stratum-port must be between 1 and 65535");
    }
    return static_cast<std::uint16_t>(port);
}

zano_p2pool::Hash256 parse_hash256(std::string_view hex) {
    if (hex.starts_with("0x") || hex.starts_with("0X")) {
        hex.remove_prefix(2);
    }
    const auto bytes = zano_p2pool::hex_to_bytes(hex);
    if (bytes.size() != 32) {
        throw std::runtime_error("expected a 32-byte hash from Zano RPC");
    }

    zano_p2pool::Hash256 hash{};
    std::copy(bytes.begin(), bytes.end(), hash.begin());
    return hash;
}

void print_usage(const char* program) {
    std::cout
        << "Usage: " << program
        << " --wallet ZANO_ADDRESS"
        << " [--network testnet|mainnet]"
        << " [--rpc-url URL]"
        << " [--stratum]"
        << " [--stratum-bind IPv4]"
        << " [--stratum-port PORT]"
        << " [--stratum-difficulty DIFFICULTY]"
        << " [--template-refresh-seconds SECONDS]\n\n"
        << "Defaults:\n"
        << "  network: testnet\n"
        << "  testnet RPC: " << kTestnetRpcUrl << '\n'
        << "  mainnet RPC: " << kMainnetRpcUrl << '\n'
        << "  Stratum bind: 127.0.0.1\n"
        << "  Stratum port: " << kDefaultStratumPort << '\n'
        << "  Stratum difficulty: " << kDefaultStratumDifficulty << '\n'
        << "  template refresh: " << kDefaultTemplateRefreshSeconds << " seconds\n";
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

        if (arg == "--stratum") {
            options.stratum = true;
            continue;
        }

        if (arg == "--stratum-bind") {
            if (++i >= argc) {
                throw std::runtime_error("--stratum-bind requires a value");
            }
            options.stratum_bind = argv[i];
            continue;
        }

        if (arg == "--stratum-port") {
            if (++i >= argc) {
                throw std::runtime_error("--stratum-port requires a value");
            }
            options.stratum_port = parse_port(argv[i]);
            continue;
        }

        if (arg == "--stratum-difficulty") {
            if (++i >= argc) {
                throw std::runtime_error("--stratum-difficulty requires a value");
            }
            options.stratum_difficulty =
                parse_u64_option("--stratum-difficulty", argv[i]);
            if (options.stratum_difficulty == 0) {
                throw std::runtime_error("--stratum-difficulty must be nonzero");
            }
            continue;
        }

        if (arg == "--template-refresh-seconds") {
            if (++i >= argc) {
                throw std::runtime_error(
                    "--template-refresh-seconds requires a value");
            }
            options.template_refresh_seconds =
                parse_u64_option("--template-refresh-seconds", argv[i]);
            if (options.template_refresh_seconds == 0) {
                throw std::runtime_error(
                    "--template-refresh-seconds must be nonzero");
            }
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

LiveTemplate fetch_live_template(
    const zano_p2pool::RpcClient& rpc,
    const Options& options) {
    LiveTemplate result;
    result.block = rpc.get_block_template(
        options.wallet,
        "zano-p2pool/0.1.0-dev");
    const auto block_blob =
        zano_p2pool::hex_to_bytes(result.block.blocktemplate_blob);
    result.mining_work = zano_p2pool::derive_mining_header_work(block_blob);
    result.seed_hash = parse_hash256(result.block.seed);
    result.network_difficulty =
        zano_p2pool::difficulty128_from_decimal(result.block.difficulty);
    return result;
}

void print_template(const LiveTemplate& live) {
    const auto target =
        zano_p2pool::difficulty_to_target(live.block.difficulty);

    std::cout << "Template status: " << live.block.status << '\n';
    std::cout << "Height:          " << live.block.height << '\n';
    std::cout << "ProgPoWZ epoch:  "
              << zano_p2pool::progpowz_epoch(live.block.height) << '\n';
    std::cout << "Previous hash:   " << live.block.prev_hash << '\n';
    std::cout << "Difficulty:      " << live.block.difficulty << '\n';
    std::cout << "Target:          " << target.hex() << '\n';
    std::cout << "Block reward:    " << live.block.block_reward << '\n';
    std::cout << "ProgPoWZ seed:   " << live.block.seed << '\n';
    std::cout << "Blob bytes:      " << live.block.blob_bytes() << '\n';
    std::cout << "Regular txs:     " << live.mining_work.tx_hashes.hashes.size() << '\n';
    std::cout << "Mining blob:     " << live.mining_work.hashing_blob.size()
              << " bytes\n";
    std::cout << "Mining header:   "
              << zano_p2pool::hash_to_hex(live.mining_work.header_hash) << '\n';
}

void wait_for_refresh(std::uint64_t seconds) {
    constexpr auto kSlice = std::chrono::milliseconds(100);
    const std::uint64_t slices = seconds * 10;
    for (std::uint64_t i = 0; i < slices && !g_stop_requested; ++i) {
        std::this_thread::sleep_for(kSlice);
    }
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

        if (options.stratum && !zano_p2pool::progpowz_available()) {
            throw std::runtime_error(
                "Stratum mode requires the exact Zano ProgPoWZ backend; "
                "configure with ZANO_P2POOL_ZANO_SOURCE_DIR");
        }

        const zano_p2pool::RpcClient rpc(options.rpc_url);
        LiveTemplate live = fetch_live_template(rpc, options);
        print_template(live);

        if (!options.stratum) {
            return 0;
        }

        zano_p2pool::StratumServerConfig server_config;
        server_config.bind_address = options.stratum_bind;
        server_config.port = options.stratum_port;
        server_config.sessions.default_share_difficulty =
            options.stratum_difficulty;
        server_config.sessions.maximum_share_difficulty = std::max(
            server_config.sessions.maximum_share_difficulty,
            options.stratum_difficulty);

        zano_p2pool::StratumTcpServer server(server_config);
        std::uint64_t template_version = server.publish_template(
            live.mining_work.header_hash,
            live.seed_hash,
            live.block.height,
            live.network_difficulty);
        server.start();

        std::cout << "\nStratum listening: "
                  << server_config.bind_address << ':' << server.bound_port() << '\n';
        std::cout << "Default share difficulty: "
                  << options.stratum_difficulty << '\n';
        std::cout << "Template refresh: "
                  << options.template_refresh_seconds << " seconds\n";
        std::cout << "Press Ctrl+C to stop.\n";

        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);

        while (!g_stop_requested) {
            wait_for_refresh(options.template_refresh_seconds);
            if (g_stop_requested) {
                break;
            }

            try {
                LiveTemplate next = fetch_live_template(rpc, options);
                const std::uint64_t next_version = server.publish_template(
                    next.mining_work.header_hash,
                    next.seed_hash,
                    next.block.height,
                    next.network_difficulty);
                if (next_version != template_version) {
                    template_version = next_version;
                    std::cout << "Stratum template v" << template_version
                              << ": height=" << next.block.height
                              << " header="
                              << zano_p2pool::hash_to_hex(
                                     next.mining_work.header_hash)
                              << " difficulty=" << next.block.difficulty
                              << '\n';
                }
                live = std::move(next);
            } catch (const std::exception& e) {
                std::cerr << "template refresh failed: " << e.what() << '\n';
            }
        }

        server.stop();
        std::cout << "Stratum stopped. Verified shares in memory: "
                  << server.connected_share_count() << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}
