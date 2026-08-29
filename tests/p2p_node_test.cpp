#include "zano_p2pool/crypto_hash.hpp"
#include "zano_p2pool/p2p_node.hpp"
#include "test_check.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <thread>

namespace {

using namespace zano_p2pool;
using namespace std::chrono_literals;

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

Share make_parent() {
    Share share;
    share.timestamp = 1'700'200'000;
    share.zano_height = 0;
    share.mining_header_hash = hash_from_hex(
        "ffeeddccbbaa9988776655443322110000112233445566778899aabbccddeeff");
    share.nonce = UINT64_C(0x123456789abcdef0);
    share.share_difficulty = difficulty128_from_decimal("3");
    share.network_difficulty = difficulty128_from_decimal("4");
    share.miner_id[31] = 0x61;
    return share;
}

Share make_child(const Share& parent) {
    Share child = parent;
    child.parent_id = share_id(parent);
    child.share_height = parent.share_height + 1;
    child.timestamp = parent.timestamp + 1;
    child.miner_id[31] = 0x62;
    return child;
}

ShareWorkContext context_for(const Share& share) {
    return ShareWorkContext{
        share.zano_height,
        share.mining_header_hash,
        share.network_difficulty,
    };
}

bool wait_for(
    const std::function<bool()>& predicate,
    std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return predicate();
}

}  // namespace

int main() {
    const Share parent = make_parent();
    const Share child = make_child(parent);
    const ShareId parent_id = share_id(parent);
    const ShareId child_id = share_id(child);
    const ShareWorkContext trusted = context_for(parent);

    ShareChain provider_chain;
    ShareChain requester_chain;
    P2pTrustedWorkRegistry provider_work;
    P2pTrustedWorkRegistry requester_work;
    std::mutex provider_mutex;
    std::mutex requester_mutex;

    provider_work.remember(trusted);
    requester_work.remember(trusted);

#ifdef ZANO_P2POOL_HAVE_PROGPOWZ
    CHECK(provider_chain.submit_share(
              parent,
              trusted,
              child.timestamp,
              ProgPowZContextMode::Light).disposition ==
          ShareDisposition::Connected);
    CHECK(provider_chain.submit_share(
              child,
              trusted,
              child.timestamp,
              ProgPowZContextMode::Light).disposition ==
          ShareDisposition::Connected);
#else
    CHECK(provider_chain.add_share_unchecked(parent).disposition ==
          ShareDisposition::Connected);
    CHECK(provider_chain.add_share_unchecked(child).disposition ==
          ShareDisposition::Connected);
#endif

    P2pNodeProtocol provider_protocol(
        provider_chain, provider_work, provider_mutex);
    P2pNodeProtocol requester_protocol(
        requester_chain, requester_work, requester_mutex);

    P2pRuntime* provider_runtime_ptr = nullptr;
    P2pRuntime* requester_runtime_ptr = nullptr;

    P2pRuntime provider_runtime(
        P2pRuntimeConfig{
            P2pEndpoint{"127.0.0.1", 0},
            make_handshake(0x20),
        },
        [&](const P2pHandshake& peer, const P2pEnvelope& envelope) {
            CHECK(provider_runtime_ptr != nullptr);
            static_cast<void>(provider_protocol.handle(
                *provider_runtime_ptr,
                peer,
                envelope,
                child.timestamp,
                ProgPowZContextMode::Light));
        });

    P2pRuntime requester_runtime(
        P2pRuntimeConfig{
            P2pEndpoint{"127.0.0.1", 0},
            make_handshake(0x70),
        },
        [&](const P2pHandshake& peer, const P2pEnvelope& envelope) {
            CHECK(requester_runtime_ptr != nullptr);
            static_cast<void>(requester_protocol.handle(
                *requester_runtime_ptr,
                peer,
                envelope,
                child.timestamp,
                ProgPowZContextMode::Light));
        });

    provider_runtime_ptr = &provider_runtime;
    requester_runtime_ptr = &requester_runtime;

    provider_runtime.start();
    requester_runtime.start();
    requester_runtime.connect_peer(
        P2pEndpoint{"127.0.0.1", provider_runtime.listen_port()});

    CHECK(wait_for([&] {
        return provider_runtime.peer_count() == 1 &&
               requester_runtime.peer_count() == 1;
    }));

    // Child-first gossip must trigger the full targeted sync path:
    // requester orphan -> ShareRequest(parent) -> provider response -> local
    // parent revalidation -> deterministic child promotion.
    provider_runtime.broadcast(make_p2p_share_announce_envelope(child));

#ifdef ZANO_P2POOL_HAVE_PROGPOWZ
    CHECK(wait_for([&] {
        std::lock_guard lock(requester_mutex);
        return requester_chain.contains(parent_id) &&
               requester_chain.contains(child_id) &&
               !requester_chain.is_orphan(child_id);
    }));

    {
        std::lock_guard lock(requester_mutex);
        CHECK(requester_chain.connected_size() == 2);
        CHECK(requester_chain.orphan_size() == 0);
        CHECK(requester_chain.best_tip() != nullptr);
        CHECK(requester_chain.best_tip()->id == child_id);
    }
#else
    // Lightweight builds exercise the same live message/request/response path
    // but fail closed at local PoW admission without the exact backend.
    CHECK(wait_for([&] {
        std::lock_guard lock(requester_mutex);
        return requester_chain.connected_size() == 0;
    }));
    {
        std::lock_guard lock(requester_mutex);
        CHECK(!requester_chain.contains(parent_id));
        CHECK(!requester_chain.contains(child_id));
    }
#endif

    requester_runtime.stop();
    provider_runtime.stop();
    return 0;
}
