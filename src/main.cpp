#include "zano_p2pool/block_submitter.hpp"
#include "zano_p2pool/crypto_hash.hpp"
#include "zano_p2pool/metrics_server.hpp"
#include "zano_p2pool/metrics_snapshot.hpp"
#include "zano_p2pool/mining_header.hpp"
#include "zano_p2pool/p2p_mining_context.hpp"
#include "zano_p2pool/p2p_node.hpp"
#include "zano_p2pool/p2p_runtime.hpp"
#include "zano_p2pool/p2p_share.hpp"
#include "zano_p2pool/p2p_sync.hpp"
#include "zano_p2pool/p2p_tip.hpp"
#include "zano_p2pool/pow_target.hpp"
#include "zano_p2pool/progpowz.hpp"
#include "zano_p2pool/pplns_template.hpp"
#include "zano_p2pool/rpc_client.hpp"
#include "zano_p2pool/share.hpp"
#include "zano_p2pool/share_store.hpp"
#include "zano_p2pool/sidechain_params.hpp"
#include "zano_p2pool/stratum_server.hpp"
#include "zano_p2pool/template_refresh.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

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
constexpr std::uint16_t kDefaultMetricsPort = 37890;
constexpr std::uint64_t kDefaultStratumDifficulty = 100000000;
constexpr std::uint64_t kDefaultTemplateRefreshSeconds = 5;

volatile std::sig_atomic_t g_stop_requested = 0;
std::atomic<bool> g_template_refresh_requested{false};
std::atomic<bool> g_persistence_failed{false};

void handle_signal(int) noexcept {
    g_stop_requested = 1;
}

void request_template_refresh() noexcept {
    g_template_refresh_requested.store(true, std::memory_order_release);
}

[[nodiscard]] bool runtime_stop_requested() noexcept {
    return g_stop_requested != 0 ||
           g_persistence_failed.load(std::memory_order_acquire);
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

    bool p2p{false};
    std::string p2p_bind{"127.0.0.1"};
    std::uint16_t p2p_port{0};
    std::vector<zano_p2pool::P2pEndpoint> p2p_peers;

    bool metrics{false};
    std::string metrics_bind{"127.0.0.1"};
    std::uint16_t metrics_port{kDefaultMetricsPort};

    bool no_share_store{false};
    std::optional<std::filesystem::path> share_store_path;
};

struct LiveTemplate {
    zano_p2pool::BlockTemplate block;
    zano_p2pool::MiningHeaderWork mining_work;
    zano_p2pool::Hash256 seed_hash{};
    zano_p2pool::Difficulty128 network_difficulty{};
    std::optional<zano_p2pool::PplnsCoinbasePlan> payout_plan;
};

const char* network_name(Network network) {
    return network == Network::Testnet ? "testnet" : "mainnet";
}

const char* default_rpc_url(Network network) {
    return network == Network::Testnet ? kTestnetRpcUrl : kMainnetRpcUrl;
}

std::filesystem::path default_share_store_path(Network network) {
    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') {
        throw std::runtime_error(
            "HOME is not set; use --share-store PATH or --no-share-store");
    }
    return std::filesystem::path(home) /
           ".zano-p2pool" /
           network_name(network) /
           "shares.dat";
}

zano_p2pool::P2pNetwork p2p_network(Network network) {
    return network == Network::Testnet
        ? zano_p2pool::P2pNetwork::Testnet
        : zano_p2pool::P2pNetwork::Mainnet;
}

zano_p2pool::SidechainParentNetwork sidechain_parent_network(Network network) {
    return network == Network::Testnet
        ? zano_p2pool::SidechainParentNetwork::Testnet
        : zano_p2pool::SidechainParentNetwork::Mainnet;
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
        throw std::runtime_error(
            std::string(option) + " requires an unsigned integer");
    }

    std::size_t parsed = 0;
    std::uint64_t result = 0;
    try {
        result = std::stoull(value, &parsed, 10);
    } catch (const std::exception&) {
        throw std::runtime_error(
            std::string(option) + " requires an unsigned integer");
    }
    if (parsed != value.size()) {
        throw std::runtime_error(
            std::string(option) + " requires an unsigned integer");
    }
    return result;
}

std::uint16_t parse_port(
    std::string_view option,
    const std::string& value,
    bool allow_zero = false) {
    const std::uint64_t port = parse_u64_option(option, value);
    if ((!allow_zero && port == 0) ||
        port > std::numeric_limits<std::uint16_t>::max()) {
        throw std::runtime_error(
            std::string(option) +
            (allow_zero
                 ? " must be between 0 and 65535"
                 : " must be between 1 and 65535"));
    }
    return static_cast<std::uint16_t>(port);
}

zano_p2pool::P2pEndpoint parse_p2p_endpoint(const std::string& value) {
    if (value.empty()) {
        throw std::runtime_error("--p2p-peer requires HOST:PORT");
    }

    std::string host;
    std::string port_text;
    if (value.front() == '[') {
        const std::size_t close = value.find(']');
        if (close == std::string::npos ||
            close + 1 >= value.size() ||
            value[close + 1] != ':') {
            throw std::runtime_error(
                "--p2p-peer IPv6 endpoints must use [HOST]:PORT");
        }
        host = value.substr(1, close - 1);
        port_text = value.substr(close + 2);
    } else {
        const std::size_t colon = value.rfind(':');
        if (colon == std::string::npos) {
            throw std::runtime_error("--p2p-peer requires HOST:PORT");
        }
        host = value.substr(0, colon);
        port_text = value.substr(colon + 1);
    }

    if (host.empty()) {
        throw std::runtime_error("--p2p-peer host must not be empty");
    }
    return zano_p2pool::P2pEndpoint{
        host,
        parse_port("--p2p-peer port", port_text),
    };
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

zano_p2pool::NodeId generate_node_id() {
    std::random_device random;
    zano_p2pool::NodeId id{};
    for (std::uint8_t& byte : id) {
        byte = static_cast<std::uint8_t>(random() & 0xffU);
    }
    if (zano_p2pool::is_zero_node_id(id)) {
        id.back() = 1;
    }
    return id;
}

std::uint64_t unix_time_seconds() noexcept {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(now).count());
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
        << " [--p2p]"
        << " [--p2p-bind ADDRESS]"
        << " [--p2p-port PORT]"
        << " [--p2p-peer HOST:PORT]..."
        << " [--metrics]"
        << " [--metrics-bind IPv4]"
        << " [--metrics-port PORT]"
        << " [--share-store PATH]"
        << " [--no-share-store]"
        << " [--template-refresh-seconds SECONDS]\n\n"
        << "Defaults:\n"
        << "  network: testnet\n"
        << "  testnet RPC: " << kTestnetRpcUrl << '\n'
        << "  mainnet RPC: " << kMainnetRpcUrl << '\n'
        << "  Stratum bind: 127.0.0.1\n"
        << "  Stratum port: " << kDefaultStratumPort << '\n'
        << "  Stratum difficulty: " << kDefaultStratumDifficulty << '\n'
        << "  P2P bind: 127.0.0.1\n"
        << "  P2P port: 0 (ephemeral development port)\n"
        << "  metrics: disabled\n"
        << "  metrics bind: 127.0.0.1\n"
        << "  metrics port: " << kDefaultMetricsPort << '\n'
        << "  share store: ~/.zano-p2pool/<network>/shares.dat\n"
        << "  template refresh: " << kDefaultTemplateRefreshSeconds
        << " seconds\n";
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
            options.stratum_port = parse_port("--stratum-port", argv[i]);
            continue;
        }

        if (arg == "--stratum-difficulty") {
            if (++i >= argc) {
                throw std::runtime_error(
                    "--stratum-difficulty requires a value");
            }
            options.stratum_difficulty =
                parse_u64_option("--stratum-difficulty", argv[i]);
            if (options.stratum_difficulty == 0) {
                throw std::runtime_error(
                    "--stratum-difficulty must be nonzero");
            }
            continue;
        }

        if (arg == "--p2p") {
            options.p2p = true;
            continue;
        }

        if (arg == "--p2p-bind") {
            if (++i >= argc) {
                throw std::runtime_error("--p2p-bind requires a value");
            }
            options.p2p_bind = argv[i];
            continue;
        }

        if (arg == "--p2p-port") {
            if (++i >= argc) {
                throw std::runtime_error("--p2p-port requires a value");
            }
            options.p2p_port = parse_port("--p2p-port", argv[i], true);
            continue;
        }

        if (arg == "--p2p-peer") {
            if (++i >= argc) {
                throw std::runtime_error("--p2p-peer requires HOST:PORT");
            }
            options.p2p = true;
            options.p2p_peers.push_back(parse_p2p_endpoint(argv[i]));
            continue;
        }

        if (arg == "--metrics") {
            options.metrics = true;
            continue;
        }

        if (arg == "--metrics-bind") {
            if (++i >= argc) {
                throw std::runtime_error("--metrics-bind requires a value");
            }
            options.metrics = true;
            options.metrics_bind = argv[i];
            continue;
        }

        if (arg == "--metrics-port") {
            if (++i >= argc) {
                throw std::runtime_error("--metrics-port requires a value");
            }
            options.metrics = true;
            options.metrics_port = parse_port("--metrics-port", argv[i]);
            continue;
        }

        if (arg == "--share-store") {
            if (++i >= argc || argv[i][0] == '\0') {
                throw std::runtime_error("--share-store requires PATH");
            }
            options.share_store_path = std::filesystem::path(argv[i]);
            continue;
        }

        if (arg == "--no-share-store") {
            options.no_share_store = true;
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

    if (options.no_share_store && options.share_store_path.has_value()) {
        throw std::runtime_error(
            "--share-store and --no-share-store cannot be used together");
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

bool apply_canonical_pplns_template(
    LiveTemplate& live,
    zano_p2pool::ShareChain& chain,
    std::mutex& state_mutex,
    const zano_p2pool::SidechainParameters& params) {
    std::lock_guard lock(state_mutex);
    if (chain.connected_size() == 0) {
        live.payout_plan.reset();
        return false;
    }

    zano_p2pool::PplnsTemplateResult rebuilt =
        zano_p2pool::build_canonical_pplns_template(
            live.block,
            chain,
            params,
            "zano-p2pool");
    if (rebuilt.status != zano_p2pool::PplnsTemplateStatus::Ready) {
        std::string error = "canonical PPLNS template unavailable: ";
        error += zano_p2pool::pplns_template_status_name(rebuilt.status);
        if (!rebuilt.error.empty()) {
            error += " (" + rebuilt.error + ")";
        }
        throw std::runtime_error(error);
    }

    live.block = std::move(rebuilt.block);
    live.mining_work = std::move(rebuilt.mining_work);
    live.payout_plan = std::move(rebuilt.plan);
    return true;
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
    std::cout << "Regular txs:     "
              << live.mining_work.tx_hashes.hashes.size() << '\n';
    std::cout << "Mining blob:     "
              << live.mining_work.hashing_blob.size() << " bytes\n";
    std::cout << "Mining header:   "
              << zano_p2pool::hash_to_hex(live.mining_work.header_hash)
              << '\n';
}

bool wait_for_refresh(std::uint64_t seconds) {
    constexpr auto kSlice = std::chrono::milliseconds(100);
    const std::uint64_t slices = seconds * 10;
    for (std::uint64_t i = 0;
         i < slices && !runtime_stop_requested();
         ++i) {
        if (g_template_refresh_requested.exchange(
                false,
                std::memory_order_acq_rel)) {
            return true;
        }
        std::this_thread::sleep_for(kSlice);
    }
    return false;
}

zano_p2pool::ShareWorkContext trusted_context_from_live(
    const LiveTemplate& live) {
    return zano_p2pool::ShareWorkContext{
        live.block.height,
        live.mining_work.header_hash,
        live.network_difficulty,
    };
}

void set_local_p2p_context(
    zano_p2pool::P2pNodeProtocol& protocol,
    const LiveTemplate& live) {
    const zano_p2pool::P2pMiningAnchor anchor =
        zano_p2pool::p2p_mining_anchor_from_template(live.block);
    const zano_p2pool::P2pMiningContextProposal proposal =
        zano_p2pool::p2p_mining_context_proposal_from_template(live.block);

    if (live.payout_plan.has_value()) {
        protocol.set_local_mining_context(anchor, proposal, *live.payout_plan);
        return;
    }

    // Empty-sidechain bootstrap has no deterministic historical PPLNS plan yet.
    // Keep peer trust deferred rather than treating the node operator wallet as a
    // consensus payout identity shared by every peer.
    protocol.clear_expected_payout();
    protocol.set_local_mining_context(anchor, proposal);
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

        if ((options.stratum || options.p2p) &&
            !zano_p2pool::progpowz_available()) {
            throw std::runtime_error(
                "Stratum/P2P runtime requires the exact Zano ProgPoWZ backend; "
                "configure with ZANO_P2POOL_ZANO_SOURCE_DIR");
        }
        if (options.stratum && !zano_p2pool::zano_miner_tx_builder_available()) {
            throw std::runtime_error(
                "Stratum runtime requires the exact Zano miner-tx backend for "
                "canonical PPLNS block construction");
        }

        const zano_p2pool::RpcClient rpc(options.rpc_url);
        LiveTemplate live = fetch_live_template(rpc, options);
        print_template(live);

        if (!options.stratum && !options.p2p && !options.metrics) {
            return 0;
        }

        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);

        const zano_p2pool::SidechainParameters sidechain_parameters =
            zano_p2pool::canonical_sidechain_parameters(
                sidechain_parent_network(options.network));
        zano_p2pool::ShareChain node_chain(sidechain_parameters);
        zano_p2pool::P2pTrustedWorkRegistry trusted_work;
        std::mutex node_state_mutex;

        std::atomic<std::uint64_t> observed_zano_height{live.block.height};
        std::atomic<std::uint64_t> stratum_accepted_shares_total{0};
        std::atomic<std::uint64_t> p2p_admitted_shares_total{0};
        std::atomic<std::uint64_t> block_candidates_total{0};
        std::atomic<std::uint64_t> blocks_submitted_total{0};
        std::atomic<std::uint64_t> block_submission_failures_total{0};
        std::atomic<std::uint64_t> template_refresh_failures_total{0};

        std::unique_ptr<zano_p2pool::ShareStore> share_store;
        if (!options.no_share_store) {
            const std::filesystem::path store_path =
                options.share_store_path.value_or(
                    default_share_store_path(options.network));
            share_store = std::make_unique<zano_p2pool::ShareStore>(
                store_path,
                zano_p2pool::sidechain_id(sidechain_parameters));
            const zano_p2pool::ShareStoreLoadResult recovered =
                share_store->load_into(node_chain);

            std::cout << "\nShare store:     " << store_path.string() << '\n';
            std::cout << "Recovered shares: records="
                      << recovered.records_loaded
                      << " connected=" << recovered.connected_shares
                      << " orphans=" << recovered.orphan_shares;
            if (recovered.best_tip_id.has_value()) {
                std::cout << " tip="
                          << zano_p2pool::hash_to_hex(*recovered.best_tip_id);
            }
            if (recovered.truncated_tail_repaired) {
                std::cout << " tail=repaired";
            }
            std::cout << '\n';
        } else {
            std::cout << "\nShare store:     disabled\n";
        }

        auto persist_share = [&](const zano_p2pool::Share& share) noexcept {
            if (!share_store || g_persistence_failed.load(std::memory_order_acquire)) {
                return;
            }
            try {
                share_store->append(share);
            } catch (const std::exception& e) {
                std::cerr
                    << "FATAL share-store append failed: "
                    << e.what()
                    << "; shutting down to preserve durable consensus history\n";
                g_persistence_failed.store(true, std::memory_order_release);
                request_template_refresh();
            } catch (...) {
                std::cerr
                    << "FATAL share-store append failed with unknown error; "
                       "shutting down to preserve durable consensus history\n";
                g_persistence_failed.store(true, std::memory_order_release);
                request_template_refresh();
            }
        };

        static_cast<void>(apply_canonical_pplns_template(
            live,
            node_chain,
            node_state_mutex,
            sidechain_parameters));

        zano_p2pool::P2pNodeProtocol p2p_protocol(
            node_chain, trusted_work, node_state_mutex);
        p2p_protocol.remember_trusted_work(trusted_context_from_live(live));
        set_local_p2p_context(p2p_protocol, live);

        std::unique_ptr<zano_p2pool::P2pRuntime> p2p_runtime;
        if (options.p2p) {
            zano_p2pool::P2pHandshake handshake;
            handshake.network = p2p_network(options.network);
            handshake.sidechain_id = zano_p2pool::sidechain_id(sidechain_parameters);
            handshake.node_id = generate_node_id();
            handshake.capabilities = zano_p2pool::kP2pCapabilitiesV1;
            const zano_p2pool::P2pTipHint tip = p2p_protocol.local_tip();
            handshake.best_share_id = tip.share_id;
            handshake.best_share_height = tip.share_height;

            p2p_runtime = std::make_unique<zano_p2pool::P2pRuntime>(
                zano_p2pool::P2pRuntimeConfig{
                    zano_p2pool::P2pEndpoint{
                        options.p2p_bind,
                        options.p2p_port,
                    },
                    handshake,
                },
                [&](const zano_p2pool::P2pHandshake& peer,
                    const zano_p2pool::P2pEnvelope& envelope) {
                    if (!p2p_runtime) {
                        return;
                    }
                    try {
                        const auto result = p2p_protocol.handle(
                            *p2p_runtime,
                            peer,
                            envelope,
                            unix_time_seconds(),
                            zano_p2pool::ProgPowZContextMode::Light);

                        const bool share_admitted =
                            (result.status ==
                                 zano_p2pool::P2pNodeMessageStatus::ShareProcessed ||
                             result.status ==
                                 zano_p2pool::P2pNodeMessageStatus::ShareResponseProcessed) &&
                            (result.share_status ==
                                 zano_p2pool::P2pShareReceiveStatus::Connected ||
                             result.share_status ==
                                 zano_p2pool::P2pShareReceiveStatus::Orphan);
                        if (share_admitted) {
                            p2p_admitted_shares_total.fetch_add(
                                1,
                                std::memory_order_relaxed);
                            if (result.status ==
                                zano_p2pool::P2pNodeMessageStatus::ShareProcessed) {
                                persist_share(
                                    zano_p2pool::parse_p2p_share_announce_envelope(
                                        envelope));
                            } else {
                                const zano_p2pool::P2pShareResponse response =
                                    zano_p2pool::parse_p2p_share_response_envelope(
                                        envelope);
                                if (response.share.has_value()) {
                                    persist_share(*response.share);
                                }
                            }
                        }

                        if ((result.status ==
                                 zano_p2pool::P2pNodeMessageStatus::ShareProcessed ||
                             result.status ==
                                 zano_p2pool::P2pNodeMessageStatus::ShareResponseProcessed) &&
                            result.share_status ==
                                zano_p2pool::P2pShareReceiveStatus::Connected) {
                            request_template_refresh();
                        }

                        if (result.status ==
                            zano_p2pool::P2pNodeMessageStatus::
                                MiningContextProcessed) {
                            std::cout
                                << "P2P mining context: "
                                << zano_p2pool::
                                       p2p_mining_context_trust_status_name(
                                           result.mining_context_status)
                                << (result.mining_context_registry_inserted
                                        ? " (trusted work inserted)"
                                        : "")
                                << '\n';

                            if (result.mining_context_registry_inserted) {
                                if (const auto local =
                                        p2p_protocol.
                                            local_mining_context_envelope();
                                    local.has_value()) {
                                    static_cast<void>(p2p_runtime->send_to(
                                        peer.node_id,
                                        *local));
                                }
                            }
                        } else if (
                            result.status ==
                            zano_p2pool::P2pNodeMessageStatus::
                                MiningContextDeferred) {
                            std::cerr
                                << "P2P mining context deferred: canonical payout "
                                   "plan is not currently established\n";
                        }
                    } catch (const std::exception& e) {
                        std::cerr
                            << "P2P message rejected: " << e.what() << '\n';
                    }
                });
            p2p_runtime->start();

            std::cout << "\nP2P listening: "
                      << options.p2p_bind << ':'
                      << p2p_runtime->listen_port() << '\n';
            std::cout << "P2P node id:    "
                      << zano_p2pool::hash_to_hex(
                             p2p_runtime->local_handshake().node_id)
                      << '\n';
            std::cout << "Mining-context trust: "
                      << (p2p_protocol.mining_context_trust_ready()
                              ? "ready"
                              : "deferred until canonical PPLNS payout history exists")
                      << '\n';

            for (const auto& peer : options.p2p_peers) {
                try {
                    p2p_runtime->connect_peer(peer);
                    std::cout << "P2P connected:  "
                              << peer.host << ':' << peer.port << '\n';
                } catch (const std::exception& e) {
                    std::cerr
                        << "P2P connect failed for "
                        << peer.host << ':' << peer.port
                        << ": " << e.what() << '\n';
                }
            }

            if (const auto local =
                    p2p_protocol.local_mining_context_envelope();
                local.has_value()) {
                p2p_runtime->broadcast(*local);
            }
        }

        std::unique_ptr<zano_p2pool::BlockCandidateSubmitter> block_submitter;
        if (options.stratum) {
            block_submitter =
                std::make_unique<zano_p2pool::BlockCandidateSubmitter>(
                    [&](const std::string& block_blob_hex) {
                        rpc.submit_block(block_blob_hex);
                    },
                    [&](const zano_p2pool::BlockSubmitEvent& event) {
                        if (event.status ==
                            zano_p2pool::BlockSubmitStatus::Submitted) {
                            blocks_submitted_total.fetch_add(
                                1,
                                std::memory_order_relaxed);
                            std::cout
                                << "Zano block submitted: height="
                                << event.candidate.zano_height
                                << " nonce=" << event.candidate.nonce
                                << " header="
                                << zano_p2pool::hash_to_hex(
                                       event.candidate.mining_header_hash)
                                << '\n';
                            request_template_refresh();
                            return;
                        }

                        block_submission_failures_total.fetch_add(
                            1,
                            std::memory_order_relaxed);
                        std::cerr
                            << "Zano block submission "
                            << zano_p2pool::block_submit_status_name(
                                   event.status)
                            << ": height=" << event.candidate.zano_height
                            << " nonce=" << event.candidate.nonce;
                        if (!event.error.empty()) {
                            std::cerr << " error=" << event.error;
                        }
                        std::cerr << '\n';
                        request_template_refresh();
                    },
                    8,
                    16);
            if (live.payout_plan.has_value()) {
                block_submitter->remember_template(
                    live.mining_work.header_hash,
                    live.block.blocktemplate_blob);
            }
            block_submitter->start();
        }

        std::unique_ptr<zano_p2pool::StratumTcpServer> server;
        std::uint64_t template_version = 0;
        if (options.stratum) {
            zano_p2pool::StratumServerConfig server_config;
            server_config.bind_address = options.stratum_bind;
            server_config.port = options.stratum_port;

            if (options.stratum_difficulty !=
                sidechain_parameters.minimum_share_difficulty) {
                std::cerr
                    << "WARNING: --stratum-difficulty is ignored in sidechain "
                       "consensus mode; miner targets follow the branch-relative "
                       "retarget policy.\n";
            }

            zano_p2pool::StratumAcceptedShareHandler accepted_share =
                [&](const zano_p2pool::Share& share,
                    bool block_candidate) {
                    stratum_accepted_shares_total.fetch_add(
                        1,
                        std::memory_order_relaxed);
                    if (block_candidate) {
                        block_candidates_total.fetch_add(
                            1,
                            std::memory_order_relaxed);
                    }
                    persist_share(share);

                    // The accepted share changed the local payout history. Until
                    // the main refresh installs the rebuilt plan, peer contexts
                    // must defer rather than be checked against a stale plan.
                    p2p_protocol.clear_expected_payout();

                    if (p2p_runtime && p2p_runtime->running()) {
                        p2p_runtime->broadcast(
                            zano_p2pool::make_p2p_share_announce_envelope(
                                share));
                        p2p_runtime->broadcast(
                            zano_p2pool::make_p2p_tip_announce_envelope(
                                p2p_protocol.local_tip()));
                    }

                    if (block_candidate && block_submitter) {
                        const zano_p2pool::BlockCandidateQueueResult queued =
                            block_submitter->enqueue(
                                zano_p2pool::BlockCandidate{
                                    share.zano_height,
                                    share.mining_header_hash,
                                    share.nonce,
                                });
                        if (queued ==
                            zano_p2pool::BlockCandidateQueueStatus::StaleTemplate) {
                            std::cerr
                                << "Zano block candidate not submitted: header is "
                                   "stale or was bootstrap work without a canonical "
                                   "PPLNS payout template\n";
                        } else if (!queued) {
                            std::cerr
                                << "Zano block candidate dropped: submit queue "
                                   "is unavailable or full\n";
                        }
                    }

                    request_template_refresh();
                };

            server = std::make_unique<zano_p2pool::StratumTcpServer>(
                server_config,
                &node_chain,
                &node_state_mutex,
                std::move(accepted_share));

            template_version = server->publish_template(
                live.mining_work.header_hash,
                live.seed_hash,
                live.block.height,
                live.network_difficulty);
            server->start();

            std::cout << "\nStratum listening: "
                      << server_config.bind_address << ':'
                      << server->bound_port() << '\n';
            std::cout << "Sidechain target interval: "
                      << sidechain_parameters.target_share_seconds
                      << " seconds\n";
            std::cout << "Minimum share difficulty: "
                      << sidechain_parameters.minimum_share_difficulty << '\n';
            std::cout << "Payout mode: "
                      << (live.payout_plan.has_value()
                              ? "canonical PPLNS"
                              : "bootstrap share establishment")
                      << '\n';
            std::cout << "Block submission: "
                      << (live.payout_plan.has_value()
                              ? "enabled for canonical PPLNS work"
                              : "deferred until the first sidechain share")
                      << '\n';
        }

        std::unique_ptr<zano_p2pool::MetricsHttpServer> metrics_server;
        if (options.metrics) {
            zano_p2pool::MetricsServerConfig metrics_config;
            metrics_config.bind_address = options.metrics_bind;
            metrics_config.port = options.metrics_port;

            metrics_server = std::make_unique<zano_p2pool::MetricsHttpServer>(
                metrics_config,
                [&]() {
                    zano_p2pool::MetricsSnapshot snapshot;
                    snapshot.zano_height = observed_zano_height.load(
                        std::memory_order_relaxed);

                    {
                        std::lock_guard lock(node_state_mutex);
                        snapshot.sidechain_connected_shares =
                            node_chain.connected_size();
                        snapshot.sidechain_orphan_shares =
                            node_chain.orphan_size();
                        if (const zano_p2pool::ConnectedShare* tip =
                                node_chain.best_tip();
                            tip != nullptr) {
                            snapshot.sidechain_tip_height = tip->share.share_height;
                        }
                    }

                    snapshot.p2p_peers = p2p_runtime
                        ? p2p_runtime->peer_count()
                        : 0;
                    snapshot.p2p_trusted_work_contexts =
                        p2p_protocol.trusted_work_count();
                    snapshot.stratum_connections = server
                        ? server->client_count()
                        : 0;
                    snapshot.stratum_template_version = server
                        ? server->current_template_version()
                        : 0;
                    snapshot.stratum_accepted_shares_total =
                        stratum_accepted_shares_total.load(
                            std::memory_order_relaxed);
                    snapshot.p2p_admitted_shares_total =
                        p2p_admitted_shares_total.load(
                            std::memory_order_relaxed);
                    snapshot.block_candidates_total =
                        block_candidates_total.load(std::memory_order_relaxed);
                    snapshot.blocks_submitted_total =
                        blocks_submitted_total.load(std::memory_order_relaxed);
                    snapshot.block_submission_failures_total =
                        block_submission_failures_total.load(
                            std::memory_order_relaxed);
                    snapshot.template_refresh_failures_total =
                        template_refresh_failures_total.load(
                            std::memory_order_relaxed);
                    snapshot.persistence_ok =
                        !g_persistence_failed.load(std::memory_order_acquire);
                    return zano_p2pool::render_prometheus_metrics(snapshot);
                });
            metrics_server->start();
            std::cout << "\nMetrics listening: "
                      << metrics_config.bind_address << ':'
                      << metrics_server->bound_port()
                      << " (/metrics, /healthz)\n";
        }

        std::cout << "Template refresh: "
                  << options.template_refresh_seconds << " seconds\n";
        std::cout << "Press Ctrl+C to stop.\n";

        while (!runtime_stop_requested()) {
            const bool forced_refresh =
                wait_for_refresh(options.template_refresh_seconds);
            if (runtime_stop_requested()) {
                break;
            }

            try {
                LiveTemplate next = fetch_live_template(rpc, options);
                observed_zano_height.store(
                    next.block.height,
                    std::memory_order_relaxed);
                const bool daemon_changed =
                    zano_p2pool::should_refresh_stratum_template(
                        live.block,
                        live.mining_work,
                        next.block,
                        next.mining_work);
                if (!forced_refresh && !daemon_changed) {
                    continue;
                }

                const bool canonical_pplns = apply_canonical_pplns_template(
                    next,
                    node_chain,
                    node_state_mutex,
                    sidechain_parameters);

                if (block_submitter && canonical_pplns) {
                    block_submitter->remember_template(
                        next.mining_work.header_hash,
                        next.block.blocktemplate_blob);
                }

                p2p_protocol.remember_trusted_work(
                    trusted_context_from_live(next));
                set_local_p2p_context(p2p_protocol, next);

                if (server) {
                    const std::uint64_t next_version =
                        server->publish_template(
                            next.mining_work.header_hash,
                            next.seed_hash,
                            next.block.height,
                            next.network_difficulty);
                    if (next_version != template_version) {
                        template_version = next_version;
                        std::cout
                            << "Stratum template v" << template_version
                            << ": height=" << next.block.height
                            << " header="
                            << zano_p2pool::hash_to_hex(
                                   next.mining_work.header_hash)
                            << " difficulty=" << next.block.difficulty
                            << " payout="
                            << (canonical_pplns ? "PPLNS" : "bootstrap")
                            << (next.payout_plan.has_value()
                                    ? " recipients=" + std::to_string(
                                          next.payout_plan->destinations.size())
                                    : std::string{})
                            << '\n';
                    }
                }

                if (p2p_runtime) {
                    if (const auto local =
                            p2p_protocol.local_mining_context_envelope();
                        local.has_value()) {
                        p2p_runtime->broadcast(*local);
                    }
                }
                live = std::move(next);
            } catch (const std::exception& e) {
                template_refresh_failures_total.fetch_add(
                    1,
                    std::memory_order_relaxed);
                std::cerr
                    << "template refresh failed: " << e.what() << '\n';
            }
        }

        if (metrics_server) {
            metrics_server->stop();
            std::cout << "Metrics stopped.\n";
        }
        if (server) {
            server->stop();
            std::cout
                << "Stratum stopped. Verified shares in memory: "
                << server->connected_share_count() << '\n';
        }
        if (block_submitter) {
            block_submitter->stop();
            std::cout << "Block submitter stopped.\n";
        }
        if (p2p_runtime) {
            p2p_runtime->stop();
            std::cout
                << "P2P stopped. Connected shares in memory: "
                << p2p_protocol.connected_share_count() << '\n';
        }

        if (g_persistence_failed.load(std::memory_order_acquire)) {
            std::cerr
                << "error: node stopped because sidechain persistence failed\n";
            return 1;
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}
