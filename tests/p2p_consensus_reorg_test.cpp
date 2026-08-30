#include "zano_p2pool/crypto_hash.hpp"
#include "zano_p2pool/p2p_node.hpp"
#include "zano_p2pool/share_validation.hpp"
#include "test_check.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace {

using namespace zano_p2pool;
using namespace std::chrono_literals;

constexpr std::uint64_t kBaseTimestamp = 1'700'300'000;
constexpr std::uint64_t kNow = kBaseTimestamp + 100;

Hash256 hash_from_hex(std::string_view hex) {
    const auto bytes = hex_to_bytes(hex);
    CHECK(bytes.size() == 32);
    Hash256 hash{};
    std::copy(bytes.begin(), bytes.end(), hash.begin());
    return hash;
}

NodeId node_id_from(std::uint8_t seed) {
    NodeId id{};
    for (std::size_t i = 0; i < id.size(); ++i) {
        id[i] = static_cast<std::uint8_t>(seed + i);
    }
    return id;
}

P2pHandshake make_handshake(std::uint8_t seed) {
    P2pHandshake handshake;
    handshake.network = P2pNetwork::Testnet;
    handshake.node_id = node_id_from(seed);
    handshake.capabilities = kP2pCapabilitiesV1;
    return handshake;
}

Share make_root(std::string_view difficulty, std::uint8_t miner_tag) {
    Share share;
    share.timestamp = kBaseTimestamp;
    share.zano_height = 0;
    share.mining_header_hash = hash_from_hex(
        "ffeeddccbbaa9988776655443322110000112233445566778899aabbccddeeff");
    share.share_difficulty = difficulty128_from_decimal(difficulty);
    share.network_difficulty = difficulty128_from_decimal("100");
    share.miner_id[31] = miner_tag;
    return share;
}

Share make_child(
    const Share& parent,
    std::string_view difficulty,
    std::uint8_t miner_tag) {
    Share child = parent;
    child.parent_id = share_id(parent);
    child.share_height = parent.share_height + 1;
    child.timestamp = parent.timestamp + 1;
    child.share_difficulty = difficulty128_from_decimal(difficulty);
    child.miner_id[31] = miner_tag;
    child.nonce = 0;
    return child;
}

ShareWorkContext context_for(const Share& share) {
    return ShareWorkContext{
        share.zano_height,
        share.mining_header_hash,
        share.network_difficulty,
    };
}

Share with_valid_nonce(Share share, std::uint64_t first_nonce) {
#ifdef ZANO_P2POOL_HAVE_PROGPOWZ
    const std::string share_difficulty =
        difficulty128_to_decimal(share.share_difficulty);
    const std::string network_difficulty =
        difficulty128_to_decimal(share.network_difficulty);

    constexpr std::uint64_t kMaxAttempts = 10'000;
    for (std::uint64_t offset = 0; offset < kMaxAttempts; ++offset) {
        const std::uint64_t nonce = first_nonce + offset;
        const CandidateValidation validation = validate_candidate(
            share.zano_height,
            share.mining_header_hash,
            nonce,
            share_difficulty,
            network_difficulty,
            ProgPowZContextMode::Light);
        if (validation.meets_share_difficulty) {
            share.nonce = nonce;
            return share;
        }
    }
    CHECK(false && "could not find deterministic low-difficulty ProgPoWZ nonce");
#else
    share.nonce = first_nonce;
#endif
    return share;
}

bool wait_for(
    const std::function<bool()>& predicate,
    std::chrono::milliseconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return predicate();
}

bool wait_for_chain_state(
    ShareChain& chain,
    std::mutex& mutex,
    const ShareId& expected_tip,
    std::size_t expected_connected) {
    return wait_for([&] {
        std::lock_guard lock(mutex);
        const ConnectedShare* tip = chain.best_tip();
        return chain.connected_size() == expected_connected &&
               tip != nullptr && tip->id == expected_tip;
    });
}

void send_share_to(
    P2pRuntime& provider,
    const NodeId& collector,
    const Share& share) {
    CHECK(provider.send_to(
        collector,
        make_p2p_share_announce_envelope(share)));
}

}  // namespace

int main() {
    // Both branches share a verified root. Branch A reaches cumulative work 5
    // first. Branch B is initially weaker at cumulative work 3, then its final
    // share raises it to work 6 and must deterministically trigger a reorg.
    const Share root = with_valid_nonce(make_root("1", 0x01), 100);
    const Share branch_a1 = with_valid_nonce(make_child(root, "3", 0x11), 200);
    const Share branch_a2 = with_valid_nonce(make_child(branch_a1, "1", 0x12), 300);
    const Share branch_b1 = with_valid_nonce(make_child(root, "1", 0x21), 400);
    const Share branch_b2 = with_valid_nonce(make_child(branch_b1, "1", 0x22), 500);
    const Share branch_b3 = with_valid_nonce(make_child(branch_b2, "3", 0x23), 600);

    const ShareId root_id = share_id(root);
    const ShareId branch_a1_id = share_id(branch_a1);
    const ShareId branch_a2_id = share_id(branch_a2);
    const ShareId branch_b1_id = share_id(branch_b1);
    const ShareId branch_b2_id = share_id(branch_b2);
    const ShareId branch_b3_id = share_id(branch_b3);
    const ShareWorkContext trusted = context_for(root);

    P2pRuntime provider_a(
        P2pRuntimeConfig{
            P2pEndpoint{"127.0.0.1", 0},
            make_handshake(0x20),
        });
    P2pRuntime provider_b(
        P2pRuntimeConfig{
            P2pEndpoint{"127.0.0.1", 0},
            make_handshake(0x40),
        });

    ShareChain collector1_chain;
    ShareChain collector2_chain;
    P2pTrustedWorkRegistry collector1_work;
    P2pTrustedWorkRegistry collector2_work;
    std::mutex collector1_mutex;
    std::mutex collector2_mutex;
    collector1_work.remember(trusted);
    collector2_work.remember(trusted);
    P2pNodeProtocol collector1_protocol(
        collector1_chain, collector1_work, collector1_mutex);
    P2pNodeProtocol collector2_protocol(
        collector2_chain, collector2_work, collector2_mutex);

    std::atomic<std::size_t> collector1_messages{0};
    std::atomic<std::size_t> collector2_messages{0};
    P2pRuntime* collector1_runtime_ptr = nullptr;
    P2pRuntime* collector2_runtime_ptr = nullptr;

    P2pRuntime collector1_runtime(
        P2pRuntimeConfig{
            P2pEndpoint{"127.0.0.1", 0},
            make_handshake(0x60),
        },
        [&](const P2pHandshake& peer, const P2pEnvelope& envelope) {
            ++collector1_messages;
            if (collector1_runtime_ptr != nullptr) {
                static_cast<void>(collector1_protocol.handle(
                    *collector1_runtime_ptr,
                    peer,
                    envelope,
                    kNow,
                    ProgPowZContextMode::Light));
            }
        });

    P2pRuntime collector2_runtime(
        P2pRuntimeConfig{
            P2pEndpoint{"127.0.0.1", 0},
            make_handshake(0x80),
        },
        [&](const P2pHandshake& peer, const P2pEnvelope& envelope) {
            ++collector2_messages;
            if (collector2_runtime_ptr != nullptr) {
                static_cast<void>(collector2_protocol.handle(
                    *collector2_runtime_ptr,
                    peer,
                    envelope,
                    kNow,
                    ProgPowZContextMode::Light));
            }
        });

    collector1_runtime_ptr = &collector1_runtime;
    collector2_runtime_ptr = &collector2_runtime;

    provider_a.start();
    provider_b.start();
    collector1_runtime.start();
    collector2_runtime.start();

    collector1_runtime.connect_peer(
        P2pEndpoint{"127.0.0.1", provider_a.listen_port()});
    collector1_runtime.connect_peer(
        P2pEndpoint{"127.0.0.1", provider_b.listen_port()});
    collector2_runtime.connect_peer(
        P2pEndpoint{"127.0.0.1", provider_a.listen_port()});
    collector2_runtime.connect_peer(
        P2pEndpoint{"127.0.0.1", provider_b.listen_port()});

    CHECK(wait_for([&] {
        return provider_a.peer_count() == 2 &&
               provider_b.peer_count() == 2 &&
               collector1_runtime.peer_count() == 2 &&
               collector2_runtime.peer_count() == 2;
    }));

#ifdef ZANO_P2POOL_HAVE_PROGPOWZ
    // Collector 1 sees branch A first and must select A2 at cumulative work 5.
    send_share_to(
        provider_a,
        collector1_runtime.local_handshake().node_id,
        root);
    send_share_to(
        provider_a,
        collector1_runtime.local_handshake().node_id,
        branch_a1);
    send_share_to(
        provider_a,
        collector1_runtime.local_handshake().node_id,
        branch_a2);
    CHECK(wait_for_chain_state(
        collector1_chain, collector1_mutex, branch_a2_id, 3));

    {
        std::lock_guard lock(collector1_mutex);
        CHECK(chain_work_hex(collector1_chain.best_tip()->cumulative_work) ==
              "0000000000000000000000000000000000000000000000000000000000000005");
        CHECK(collector1_chain.is_on_best_chain(branch_a1_id));
        CHECK(collector1_chain.is_on_best_chain(branch_a2_id));
    }

    // Collector 2 sees the eventual winning branch first.
    send_share_to(
        provider_b,
        collector2_runtime.local_handshake().node_id,
        root);
    send_share_to(
        provider_b,
        collector2_runtime.local_handshake().node_id,
        branch_b1);
    send_share_to(
        provider_b,
        collector2_runtime.local_handshake().node_id,
        branch_b2);
    send_share_to(
        provider_b,
        collector2_runtime.local_handshake().node_id,
        branch_b3);
    CHECK(wait_for_chain_state(
        collector2_chain, collector2_mutex, branch_b3_id, 4));

    // Feed only the weaker prefix of branch B to collector 1. The active tip
    // must remain A2 because cumulative work is still 5 versus 3.
    send_share_to(
        provider_b,
        collector1_runtime.local_handshake().node_id,
        root);
    send_share_to(
        provider_b,
        collector1_runtime.local_handshake().node_id,
        branch_b1);
    send_share_to(
        provider_b,
        collector1_runtime.local_handshake().node_id,
        branch_b2);
    CHECK(wait_for_chain_state(
        collector1_chain, collector1_mutex, branch_a2_id, 5));

    // The final B share raises cumulative work to 6 and triggers the reorg.
    send_share_to(
        provider_b,
        collector1_runtime.local_handshake().node_id,
        branch_b3);
    CHECK(wait_for_chain_state(
        collector1_chain, collector1_mutex, branch_b3_id, 6));

    // Now deliver the losing branch A to collector 2. Arrival order must not
    // matter: both collectors converge on the same higher-work B3 tip.
    send_share_to(
        provider_a,
        collector2_runtime.local_handshake().node_id,
        root);
    send_share_to(
        provider_a,
        collector2_runtime.local_handshake().node_id,
        branch_a1);
    send_share_to(
        provider_a,
        collector2_runtime.local_handshake().node_id,
        branch_a2);
    CHECK(wait_for_chain_state(
        collector2_chain, collector2_mutex, branch_b3_id, 6));

    for (auto* pair : {
             &collector1_chain,
             &collector2_chain,
         }) {
        std::mutex* mutex =
            pair == &collector1_chain ? &collector1_mutex : &collector2_mutex;
        std::lock_guard lock(*mutex);
        CHECK(pair->best_tip() != nullptr);
        CHECK(pair->best_tip()->id == branch_b3_id);
        CHECK(chain_work_hex(pair->best_tip()->cumulative_work) ==
              "0000000000000000000000000000000000000000000000000000000000000006");
        CHECK(pair->is_on_best_chain(root_id));
        CHECK(pair->is_on_best_chain(branch_b1_id));
        CHECK(pair->is_on_best_chain(branch_b2_id));
        CHECK(pair->is_on_best_chain(branch_b3_id));
        CHECK(pair->is_stale(branch_a1_id));
        CHECK(pair->is_stale(branch_a2_id));
    }

    CHECK(collector1_messages.load() >= 7);
    CHECK(collector2_messages.load() >= 7);
#else
    // Lightweight builds cannot cross the verified P2P consensus boundary.
    // They still exercise the socket/dispatcher path and must fail closed
    // without allowing an unverified share to influence the active chain.
    send_share_to(
        provider_a,
        collector1_runtime.local_handshake().node_id,
        root);
    CHECK(wait_for([&] { return collector1_messages.load() >= 1; }));
    {
        std::lock_guard lock(collector1_mutex);
        CHECK(collector1_chain.connected_size() == 0);
        CHECK(collector1_chain.best_tip() == nullptr);
    }
#endif

    collector2_runtime.stop();
    collector1_runtime.stop();
    provider_b.stop();
    provider_a.stop();
    return 0;
}
