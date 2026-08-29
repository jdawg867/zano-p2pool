#include "zano_p2pool/crypto_hash.hpp"
#include "zano_p2pool/p2p_sync.hpp"
#include "zano_p2pool/p2p_transport.hpp"
#include "test_check.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <future>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using namespace zano_p2pool;

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

template <typename Fn>
bool throws_runtime(Fn&& fn) {
    try {
        std::forward<Fn>(fn)();
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

}  // namespace

int main() {
    const Share parent = make_parent();
    const Share child = make_child(parent);
    const ShareId parent_id = share_id(parent);
    const ShareId child_id = share_id(child);

    // ShareRequest is exactly one non-zero ShareId.
    const P2pEnvelope request = make_p2p_share_request_envelope(parent_id);
    CHECK(request.type == P2pMessageType::ShareRequest);
    CHECK(request.payload.size() == kP2pShareRequestPayloadSize);
    CHECK(parse_p2p_share_request_envelope(request) == parent_id);

    ShareId zero_id{};
    CHECK(throws_runtime([&] { (void)make_p2p_share_request_envelope(zero_id); }));

    P2pEnvelope short_request = request;
    short_request.payload.pop_back();
    CHECK(throws_runtime([&] { (void)parse_p2p_share_request_envelope(short_request); }));

    // Found responses bind the requested ID to the exact canonical Share.
    const P2pEnvelope found =
        make_p2p_share_response_envelope(parent_id, &parent);
    CHECK(found.type == P2pMessageType::ShareResponse);
    CHECK(found.payload.size() == kP2pShareResponseFoundPayloadSize);
    const P2pShareResponse parsed_found =
        parse_p2p_share_response_envelope(found);
    CHECK(parsed_found.code == P2pShareResponseCode::Found);
    CHECK(parsed_found.requested_id == parent_id);
    CHECK(parsed_found.share.has_value());
    CHECK(*parsed_found.share == parent);

    CHECK(throws_runtime([&] {
        (void)make_p2p_share_response_envelope(child_id, &parent);
    }));

    P2pEnvelope tampered_found = found;
    tampered_found.payload.back() ^= 0x01;
    CHECK(throws_runtime([&] {
        (void)parse_p2p_share_response_envelope(tampered_found);
    }));

    const P2pEnvelope not_found =
        make_p2p_share_response_envelope(parent_id, nullptr);
    CHECK(not_found.payload.size() == kP2pShareResponseHeaderSize);
    const P2pShareResponse parsed_not_found =
        parse_p2p_share_response_envelope(not_found);
    CHECK(parsed_not_found.code == P2pShareResponseCode::NotFound);
    CHECK(parsed_not_found.requested_id == parent_id);
    CHECK(!parsed_not_found.share.has_value());

    P2pEnvelope invalid_status = not_found;
    invalid_status.payload[0] = 0xff;
    CHECK(throws_runtime([&] {
        (void)parse_p2p_share_response_envelope(invalid_status);
    }));

    // Provider chain represents a peer that already has the requested shares.
    ShareChain provider_chain;
#ifdef ZANO_P2POOL_HAVE_PROGPOWZ
    CHECK(provider_chain.submit_share(
              parent,
              context_for(parent),
              child.timestamp,
              ProgPowZContextMode::Light).disposition ==
          ShareDisposition::Connected);
    CHECK(provider_chain.submit_share(
              child,
              context_for(child),
              child.timestamp,
              ProgPowZContextMode::Light).disposition ==
          ShareDisposition::Connected);
#else
    CHECK(provider_chain.add_share_unchecked(parent).disposition ==
          ShareDisposition::Connected);
    CHECK(provider_chain.add_share_unchecked(child).disposition ==
          ShareDisposition::Connected);
#endif

    P2pHandshake no_sync = make_handshake(0x20);
    no_sync.capabilities &= ~kP2pCapabilityShareSync;
    CHECK(throws_runtime([&] {
        (void)answer_p2p_share_request(no_sync, request, provider_chain);
    }));

    // Establish a real TCP peer connection so request and response framing are
    // tested on the same bounded transport used by gossip.
    const P2pHandshake provider_handshake = make_handshake(0x40);
    const P2pHandshake requester_handshake = make_handshake(0x80);
    P2pTcpListener listener(
        P2pEndpoint{"127.0.0.1", 0}, provider_handshake);
    listener.start();

    auto accepted = std::async(std::launch::async, [&listener] {
        return listener.accept_peer();
    });

    P2pTcpConnection requester = connect_p2p_peer(
        P2pEndpoint{"127.0.0.1", listener.port()}, requester_handshake);
    CHECK(accepted.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    P2pTcpConnection provider = accepted.get();

    P2pTrustedWorkRegistry trusted_work;
    trusted_work.remember(context_for(parent));
    ShareChain requester_chain;
    P2pShareReceiver receiver(requester_chain, trusted_work);

#ifdef ZANO_P2POOL_HAVE_PROGPOWZ
    // Provider announces child first. It is valid PoW, but cannot contribute to
    // the connected chain until its parent arrives.
    provider.send_envelope(make_p2p_share_announce_envelope(child));
    const P2pShareReceiveResult child_result = receiver.receive(
        requester.peer_handshake(),
        requester.receive_envelope(),
        child.timestamp,
        ProgPowZContextMode::Light);
    CHECK(child_result.status == P2pShareReceiveStatus::Orphan);
    CHECK(child_result.chain_result.disposition == ShareDisposition::Orphan);
    CHECK(child_result.missing_parent_id.has_value());
    CHECK(*child_result.missing_parent_id == parent_id);
    CHECK(requester_chain.is_orphan(child_id));
    CHECK(requester_chain.connected_size() == 0);

    requester.send_envelope(
        make_p2p_share_request_envelope(*child_result.missing_parent_id));
    const P2pEnvelope provider_request = provider.receive_envelope();
    provider.send_envelope(answer_p2p_share_request(
        provider.peer_handshake(), provider_request, provider_chain));

    const P2pShareSyncReceiveResult sync_result =
        receive_p2p_share_response(
            receiver,
            requester.peer_handshake(),
            requester.receive_envelope(),
            child.timestamp,
            ProgPowZContextMode::Light);
    CHECK(sync_result.status == P2pShareSyncReceiveStatus::ShareProcessed);
    CHECK(sync_result.requested_id == parent_id);
    CHECK(sync_result.share_result.has_value());
    CHECK(sync_result.share_result->status == P2pShareReceiveStatus::Connected);
    CHECK(sync_result.share_result->chain_result.disposition ==
          ShareDisposition::Connected);
    CHECK(sync_result.share_result->chain_result.promoted_orphans == 1);
    CHECK(requester_chain.contains(parent_id));
    CHECK(requester_chain.contains(child_id));
    CHECK(!requester_chain.is_orphan(child_id));
    CHECK(requester_chain.best_tip() != nullptr);
    CHECK(requester_chain.best_tip()->id == child_id);

    // Replaying the same parent response is suppressed before another hash.
    const P2pShareSyncReceiveResult duplicate_sync =
        receive_p2p_share_response(
            receiver,
            requester.peer_handshake(),
            found,
            child.timestamp,
            ProgPowZContextMode::Light);
    CHECK(duplicate_sync.status == P2pShareSyncReceiveStatus::ShareProcessed);
    CHECK(duplicate_sync.share_result.has_value());
    CHECK(duplicate_sync.share_result->status == P2pShareReceiveStatus::Duplicate);
#else
    // The lightweight build exercises the same request/response transport but
    // refuses to admit the returned share without the exact ProgPoWZ backend.
    requester.send_envelope(request);
    const P2pEnvelope provider_request = provider.receive_envelope();
    provider.send_envelope(answer_p2p_share_request(
        provider.peer_handshake(), provider_request, provider_chain));

    const P2pShareSyncReceiveResult sync_result =
        receive_p2p_share_response(
            receiver,
            requester.peer_handshake(),
            requester.receive_envelope(),
            parent.timestamp,
            ProgPowZContextMode::Light);
    CHECK(sync_result.status == P2pShareSyncReceiveStatus::ShareProcessed);
    CHECK(sync_result.share_result.has_value());
    CHECK(sync_result.share_result->status == P2pShareReceiveStatus::Rejected);
    CHECK(sync_result.share_result->chain_result.reject_reason ==
          ShareRejectReason::PowBackendUnavailable);
    CHECK(requester_chain.connected_size() == 0);
#endif

    // Unknown IDs produce an explicit bounded NotFound response.
    ShareId unknown_id{};
    unknown_id[31] = 0xee;
    requester.send_envelope(make_p2p_share_request_envelope(unknown_id));
    const P2pEnvelope unknown_request = provider.receive_envelope();
    provider.send_envelope(answer_p2p_share_request(
        provider.peer_handshake(), unknown_request, provider_chain));

    const P2pShareSyncReceiveResult missing_result =
        receive_p2p_share_response(
            receiver,
            requester.peer_handshake(),
            requester.receive_envelope(),
            child.timestamp,
            ProgPowZContextMode::Light);
    CHECK(missing_result.status == P2pShareSyncReceiveStatus::NotFound);
    CHECK(missing_result.requested_id == unknown_id);
    CHECK(!missing_result.share_result.has_value());

    // A response from a peer that did not advertise sync cannot be admitted.
    const P2pShareSyncReceiveResult capability_result =
        receive_p2p_share_response(
            receiver,
            no_sync,
            found,
            child.timestamp,
            ProgPowZContextMode::Light);
    CHECK(capability_result.status == P2pShareSyncReceiveStatus::CapabilityMissing);

    requester.close();
    provider.close();
    listener.stop();

    CHECK(std::string(p2p_share_sync_receive_status_name(
              P2pShareSyncReceiveStatus::NotFound)) ==
          "not-found");

    return 0;
}
