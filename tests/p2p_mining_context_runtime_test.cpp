#define main p2p_miner_tx_proofs_fixture_main
#include "p2p_miner_tx_proofs_test.cpp"
#undef main

#include "zano_p2pool/p2p_node.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

namespace {

using namespace zano_p2pool;
using namespace std::chrono_literals;

NodeId node_id_from(std::uint8_t seed) {
    NodeId id{};
    for (std::size_t i = 0; i < id.size(); ++i) {
        id[i] = static_cast<std::uint8_t>(seed + i);
    }
    return id;
}

P2pHandshake make_runtime_handshake(std::uint8_t seed) {
    P2pHandshake handshake;
    handshake.network = P2pNetwork::Testnet;
    handshake.node_id = node_id_from(seed);
    handshake.capabilities = kP2pCapabilitiesV1;
    return handshake;
}

bool wait_for(
    const std::function<bool()>& predicate,
    std::chrono::milliseconds timeout = 3s) {
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
    using namespace zano_p2pool;

    const ZanoCurveKey scalar_one = key_from_hex(kScalarOneHex);
    const ZanoCurveKey basepoint = key_from_hex(kEd25519BasepointHex);
    const ZanoCurveKey native_asset = key_from_hex(kNativeCoinAssetId1Div8Hex);
    const P2pPayoutAddress payout{basepoint, basepoint};

    std::vector<ZanoCurveKey> stealths(2, basepoint);
    if (zano_curve_backend_available()) {
        CHECK(zano_derive_output_public_key(
            scalar_one,
            payout.spend_public_key,
            payout.view_public_key,
            0,
            stealths[0]));
        CHECK(zano_derive_output_public_key(
            scalar_one,
            payout.spend_public_key,
            payout.view_public_key,
            1,
            stealths[1]));
    }

    const auto prefix = make_prefix(basepoint, stealths, native_asset);
    const auto balance_proof = make_balance_proof(prefix, basepoint);
    const auto range_proof = make_valid_range_proof(prefix);
    const P2pMiningContextProposal proposal =
        make_proposal(prefix, balance_proof, range_proof);
    const P2pMiningAnchor anchor = anchor_for(proposal);
    const P2pMiningContextCheckResult inspected = inspect(proposal);

    ShareChain receiver_chain;
    P2pTrustedWorkRegistry receiver_work;
    std::mutex receiver_mutex;
    P2pNodeProtocol receiver_protocol(
        receiver_chain, receiver_work, receiver_mutex);
    receiver_protocol.set_local_mining_context(anchor, proposal);
    receiver_protocol.set_expected_payout(payout);

    std::atomic<std::size_t> mining_context_messages{0};
    std::atomic<std::size_t> share_messages{0};
    std::atomic<P2pMiningContextTrustStatus> context_status{
        P2pMiningContextTrustStatus::ProofsRejected};
    std::atomic<P2pShareReceiveStatus> share_status{
        P2pShareReceiveStatus::Rejected};

    P2pRuntime* receiver_runtime_ptr = nullptr;

    P2pRuntime sender_runtime(
        P2pRuntimeConfig{
            P2pEndpoint{"127.0.0.1", 0},
            make_runtime_handshake(0x20),
        },
        [](const P2pHandshake&, const P2pEnvelope&) {});

    P2pRuntime receiver_runtime(
        P2pRuntimeConfig{
            P2pEndpoint{"127.0.0.1", 0},
            make_runtime_handshake(0x70),
        },
        [&](const P2pHandshake& peer, const P2pEnvelope& envelope) {
            if (receiver_runtime_ptr == nullptr) {
                return;
            }
            const P2pNodeMessageResult result = receiver_protocol.handle(
                *receiver_runtime_ptr,
                peer,
                envelope,
                1'700'300'000,
                ProgPowZContextMode::Light);
            if (result.status ==
                P2pNodeMessageStatus::MiningContextProcessed) {
                ++mining_context_messages;
                context_status.store(result.mining_context_status);
            }
            if (result.status == P2pNodeMessageStatus::ShareProcessed) {
                ++share_messages;
                share_status.store(result.share_status);
            }
        });

    receiver_runtime_ptr = &receiver_runtime;

    sender_runtime.start();
    receiver_runtime.start();
    sender_runtime.connect_peer(
        P2pEndpoint{"127.0.0.1", receiver_runtime.listen_port()});

    CHECK(wait_for([&] {
        return sender_runtime.peer_count() == 1 &&
               receiver_runtime.peer_count() == 1;
    }));

    CHECK(receiver_protocol.trusted_work_count() == 0);

    sender_runtime.broadcast(make_p2p_mining_context_envelope(proposal));
    CHECK(wait_for([&] { return mining_context_messages.load() >= 1; }));

    if (!zano_curve_backend_available()) {
        CHECK(context_status.load() ==
              P2pMiningContextTrustStatus::ProofsRejected);
        CHECK(receiver_protocol.trusted_work_count() == 0);
        receiver_runtime.stop();
        sender_runtime.stop();
        return 0;
    }

    CHECK(context_status.load() == P2pMiningContextTrustStatus::Trusted);
    CHECK(receiver_protocol.trusted_work_count() == 1);

    Share share;
    share.version = kShareVersion1;
    share.timestamp = 1'700'300'000;
    share.zano_height = proposal.zano_height;
    share.mining_header_hash = inspected.mining_header_hash;
    share.nonce = UINT64_C(0x0102030405060708);
    share.share_difficulty = difficulty128_from_decimal("1");
    share.network_difficulty = proposal.network_difficulty;
    share.miner_id[31] = 0x55;

    sender_runtime.broadcast(make_p2p_share_announce_envelope(share));
    CHECK(wait_for([&] { return share_messages.load() >= 1; }));

#ifdef ZANO_P2POOL_HAVE_PROGPOWZ
    CHECK(share_status.load() == P2pShareReceiveStatus::Connected);
    {
        std::lock_guard lock(receiver_mutex);
        CHECK(receiver_chain.contains(share_id(share)));
        CHECK(receiver_chain.connected_size() == 1);
    }
#else
    CHECK(share_status.load() == P2pShareReceiveStatus::Rejected);
    {
        std::lock_guard lock(receiver_mutex);
        CHECK(receiver_chain.connected_size() == 0);
    }
#endif

    receiver_runtime.stop();
    sender_runtime.stop();
    return 0;
}
