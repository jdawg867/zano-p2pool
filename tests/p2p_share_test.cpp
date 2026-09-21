#include "zano_p2pool/crypto_hash.hpp"
#include "zano_p2pool/p2p_share.hpp"
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

Share make_verified_share() {
    Share share;
    share.timestamp = 1'700'100'000;
    share.zano_height = 0;
    share.mining_header_hash = hash_from_hex(
        "ffeeddccbbaa9988776655443322110000112233445566778899aabbccddeeff");
    share.nonce = UINT64_C(0x123456789abcdef0);
    share.share_difficulty = difficulty128_from_decimal("3");
    share.network_difficulty = difficulty128_from_decimal("4");
    share.miner_id[31] = 0x52;
    return share;
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
    const Share share = make_verified_share();
    const ShareId id = share_id(share);

    // ShareAnnounce is exactly the existing canonical 165-byte Share encoding;
    // P2P adds framing, not a second share serialization format.
    const P2pEnvelope announce = make_p2p_share_announce_envelope(share);
    CHECK(announce.type == P2pMessageType::ShareAnnounce);
    CHECK(announce.payload.size() == kShareV1SerializedSize);
    CHECK(announce.payload == serialize_share(share));
    CHECK(parse_p2p_share_announce_envelope(announce) == share);

    const auto wire = serialize_p2p_envelope(announce);
    CHECK(wire.size() == kP2pEnvelopeHeaderSize + kShareV1SerializedSize);
    CHECK(wire[5] == static_cast<std::uint8_t>(P2pMessageType::ShareAnnounce));
    CHECK(wire[8] == 0x00);
    CHECK(wire[9] == 0x00);
    CHECK(wire[10] == 0x00);
    CHECK(wire[11] == 0xa5);
    CHECK(parse_p2p_share_announce_envelope(deserialize_p2p_envelope(wire)) == share);

    P2pEnvelope wrong_type = announce;
    wrong_type.type = P2pMessageType::Handshake;
    CHECK(throws_runtime([&] { (void)parse_p2p_share_announce_envelope(wrong_type); }));

    P2pEnvelope short_payload = announce;
    short_payload.payload.pop_back();
    CHECK(throws_runtime([&] { (void)parse_p2p_share_announce_envelope(short_payload); }));

    P2pTrustedWorkRegistry registry;
    CHECK(registry.size() == 0);
    registry.remember(context_for(share));
    CHECK(registry.size() == 1);
    registry.remember(context_for(share));
    CHECK(registry.size() == 1);
    CHECK(registry.find(share.zano_height, share.mining_header_hash) != nullptr);
    ShareId different_parent{};
    different_parent.back() = 0x01;
    CHECK(registry.find(
              share.zano_height,
              share.mining_header_hash,
              different_parent) == nullptr);

    // Compatibility insertion carries no independently established Zano
    // parent. A later production trust crossing may safely upgrade the exact
    // same work key with provenance, but conflicting provenance is rejected.
    CHECK(registry.find_zano_parent_hash(
              share.zano_height,
              share.mining_header_hash,
              ShareId{}) == nullptr);

    Hash256 canonical_parent{};
    canonical_parent.front() = 0x71;
    registry.remember(
        context_for(share),
        ShareId{},
        canonical_parent);

    const Hash256* recorded_parent =
        registry.find_zano_parent_hash(
            share.zano_height,
            share.mining_header_hash,
            ShareId{});
    CHECK(recorded_parent != nullptr);
    CHECK(*recorded_parent == canonical_parent);
    CHECK(registry.size() == 1);

    Hash256 conflicting_parent = canonical_parent;
    conflicting_parent.front() ^= 0x01U;
    CHECK(throws_runtime([&] {
        registry.remember(
            context_for(share),
            ShareId{},
            conflicting_parent);
    }));

    ShareWorkContext conflict = context_for(share);
    conflict.network_difficulty = difficulty128_from_decimal("5");
    CHECK(throws_runtime([&] { registry.remember(conflict); }));

    // An unavailable mining height after rollback must revoke every trusted
    // authorization at that height without disturbing work from another
    // height.
    P2pTrustedWorkRegistry revocation_registry;

    const ShareWorkContext height_zero = context_for(share);
    revocation_registry.remember(height_zero);

    ShareId bound_parent{};
    bound_parent.back() = 0x22;
    revocation_registry.remember(height_zero, bound_parent);

    ShareWorkContext height_one = height_zero;
    height_one.zano_height = height_zero.zano_height + 1;
    height_one.mining_header_hash[0] ^= 0x01U;
    revocation_registry.remember(height_one);

    CHECK(revocation_registry.size() == 3);
    CHECK(revocation_registry.erase_zano_height(
              height_zero.zano_height) == 2);
    CHECK(revocation_registry.size() == 1);
    CHECK(revocation_registry.find(
              height_zero.zano_height,
              height_zero.mining_header_hash) == nullptr);
    CHECK(revocation_registry.find(
              height_zero.zano_height,
              height_zero.mining_header_hash,
              bound_parent) == nullptr);
    CHECK(revocation_registry.find(
              height_one.zano_height,
              height_one.mining_header_hash) != nullptr);
    CHECK(revocation_registry.erase_zano_height(
              height_zero.zano_height) == 0);
    CHECK(revocation_registry.size() == 1);

    // Advanced-height reorg auditing must revoke only entries whose recorded
    // Zano parent disagrees with current canonical history. Work from the same
    // height that matches canonical history, compatibility entries with no
    // provenance, and entries from other heights must survive.
    P2pTrustedWorkRegistry selective_registry;

    ShareWorkContext selective_context = context_for(share);

    ShareId stale_sidechain_parent{};
    stale_sidechain_parent.back() = 0x31;

    ShareId canonical_sidechain_parent{};
    canonical_sidechain_parent.back() = 0x32;

    ShareId compatibility_sidechain_parent{};
    compatibility_sidechain_parent.back() = 0x33;

    Hash256 stale_zano_parent{};
    stale_zano_parent.front() = 0x41;

    Hash256 current_zano_parent{};
    current_zano_parent.front() = 0x42;

    selective_registry.remember(
        selective_context,
        stale_sidechain_parent,
        stale_zano_parent);

    selective_registry.remember(
        selective_context,
        canonical_sidechain_parent,
        current_zano_parent);

    selective_registry.remember(
        selective_context,
        compatibility_sidechain_parent);

    ShareWorkContext later_context = selective_context;
    later_context.zano_height += 1;
    later_context.mining_header_hash[0] ^= 0x01U;

    ShareId later_sidechain_parent{};
    later_sidechain_parent.back() = 0x34;

    selective_registry.remember(
        later_context,
        later_sidechain_parent,
        stale_zano_parent);

    CHECK(selective_registry.size() == 4);
    CHECK(selective_registry.erase_zano_parent_mismatch(
              selective_context.zano_height,
              current_zano_parent) == 1);
    CHECK(selective_registry.size() == 3);

    CHECK(selective_registry.find(
              selective_context.zano_height,
              selective_context.mining_header_hash,
              stale_sidechain_parent) == nullptr);

    CHECK(selective_registry.find(
              selective_context.zano_height,
              selective_context.mining_header_hash,
              canonical_sidechain_parent) != nullptr);

    CHECK(selective_registry.find(
              selective_context.zano_height,
              selective_context.mining_header_hash,
              compatibility_sidechain_parent) != nullptr);

    CHECK(selective_registry.find(
              later_context.zano_height,
              later_context.mining_header_hash,
              later_sidechain_parent) != nullptr);

    CHECK(selective_registry.erase_zano_parent_mismatch(
              selective_context.zano_height,
              current_zano_parent) == 0);

    const std::vector<std::uint64_t> provenance_heights =
        selective_registry.provenance_zano_heights();

    CHECK(provenance_heights.size() == 2);
    CHECK(provenance_heights[0] == selective_context.zano_height);
    CHECK(provenance_heights[1] == later_context.zano_height);

    P2pTrustedWorkRegistry compatibility_only_registry;
    compatibility_only_registry.remember(selective_context);
    CHECK(
        compatibility_only_registry
            .provenance_zano_heights()
            .empty());

    // A peer that did not advertise share-gossip capability cannot inject a
    // share even when the work context is otherwise known locally.
    P2pHandshake no_gossip = make_handshake(0x20);
    no_gossip.capabilities &= ~kP2pCapabilityShareGossip;
    ShareChain capability_chain;
    P2pShareReceiver capability_receiver(capability_chain, registry);
    const auto capability_result = capability_receiver.receive(
        no_gossip, announce, share.timestamp);
    CHECK(capability_result.status == P2pShareReceiveStatus::CapabilityMissing);
    CHECK(capability_chain.connected_size() == 0);

    // Unknown mining contexts are rejected before local ProgPoWZ. A peer's
    // header/difficulty claims are not promoted to trusted context implicitly.
    P2pTrustedWorkRegistry empty_registry;
    ShareChain unknown_chain;
    P2pShareReceiver unknown_receiver(unknown_chain, empty_registry);
    const auto unknown_result = unknown_receiver.receive(
        make_handshake(0x30), announce, share.timestamp);
    CHECK(unknown_result.status == P2pShareReceiveStatus::UnknownWorkContext);
    CHECK(unknown_chain.connected_size() == 0);

    // Duplicate suppression happens before context lookup/ProgPoWZ for shares
    // already present in the local share chain.
    ShareChain duplicate_chain;
    CHECK(duplicate_chain.add_share_unchecked(share).disposition ==
          ShareDisposition::Connected);
    P2pShareReceiver duplicate_receiver(duplicate_chain, empty_registry);
    const auto duplicate_result = duplicate_receiver.receive(
        make_handshake(0x40), announce, share.timestamp);
    CHECK(duplicate_result.status == P2pShareReceiveStatus::Duplicate);
    CHECK(duplicate_result.chain_result.id == id);

    // First real P2P data exchange: establish a TCP peer session, send the
    // ShareAnnounce frame, and route the received canonical Share through the
    // receiver's trusted-context/ShareChain admission path.
    const P2pHandshake server_handshake = make_handshake(0x50);
    const P2pHandshake client_handshake = make_handshake(0x90);
    P2pTcpListener listener(
        P2pEndpoint{"127.0.0.1", 0}, server_handshake);
    listener.start();

    auto accepted = std::async(std::launch::async, [&listener] {
        return listener.accept_peer();
    });

    P2pTcpConnection outbound = connect_p2p_peer(
        P2pEndpoint{"127.0.0.1", listener.port()}, client_handshake);
    CHECK(accepted.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    P2pTcpConnection inbound = accepted.get();

    outbound.send_envelope(announce);
    const P2pEnvelope received = inbound.receive_envelope();

    ShareChain live_chain;
    P2pShareReceiver live_receiver(live_chain, registry);
    const auto live_result = live_receiver.receive(
        inbound.peer_handshake(),
        received,
        share.timestamp,
        ProgPowZContextMode::Light);

#ifdef ZANO_P2POOL_HAVE_PROGPOWZ
    CHECK(live_result.status == P2pShareReceiveStatus::Connected);
    CHECK(live_result.chain_result.disposition == ShareDisposition::Connected);
    CHECK(live_chain.contains(id));
    const ConnectedShare* connected = live_chain.find(id);
    CHECK(connected != nullptr);
    CHECK(connected->pow_validation.has_value());
    CHECK(connected->pow_validation->meets_share_difficulty);
    CHECK(!connected->pow_validation->meets_network_difficulty);

    // The same wire share is suppressed as a duplicate before another hash.
    outbound.send_envelope(announce);
    const auto repeated = live_receiver.receive(
        inbound.peer_handshake(),
        inbound.receive_envelope(),
        share.timestamp,
        ProgPowZContextMode::Light);
    CHECK(repeated.status == P2pShareReceiveStatus::Duplicate);
#else
    CHECK(live_result.status == P2pShareReceiveStatus::Rejected);
    CHECK(live_result.chain_result.reject_reason ==
          ShareRejectReason::PowBackendUnavailable);
    CHECK(live_chain.connected_size() == 0);
#endif

    outbound.close();
    inbound.close();
    listener.stop();

    CHECK(std::string(p2p_share_receive_status_name(
              P2pShareReceiveStatus::UnknownWorkContext)) ==
          "unknown-work-context");

    return 0;
}
