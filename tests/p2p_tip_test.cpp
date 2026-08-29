#include "zano_p2pool/p2p_tip.hpp"
#include "zano_p2pool/p2p_transport.hpp"
#include "test_check.hpp"

#include <chrono>
#include <cstdint>
#include <future>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {

using namespace zano_p2pool;

NodeId node_id_from(std::uint8_t seed) {
    NodeId id{};
    for (std::size_t i = 0; i < id.size(); ++i) {
        id[i] = static_cast<std::uint8_t>(seed + i);
    }
    return id;
}

ShareId share_id_from(std::uint8_t seed) {
    ShareId id{};
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

Share make_root(std::uint64_t timestamp, std::string_view difficulty, std::uint8_t miner_tag) {
    Share share;
    share.timestamp = timestamp;
    share.zano_height = 100;
    share.mining_header_hash[31] = miner_tag;
    share.nonce = miner_tag;
    share.share_difficulty = difficulty128_from_decimal(difficulty);
    share.network_difficulty = difficulty128_from_decimal("1000");
    share.miner_id[31] = miner_tag;
    return share;
}

Share make_child(
    const Share& parent,
    std::uint64_t timestamp,
    std::string_view difficulty,
    std::uint8_t miner_tag) {
    Share share = make_root(timestamp, difficulty, miner_tag);
    share.parent_id = share_id(parent);
    share.share_height = parent.share_height + 1;
    return share;
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
    ShareChain empty_chain;
    const P2pTipHint empty_hint = p2p_tip_hint_from_chain(empty_chain);
    CHECK(is_zero_share_id(empty_hint.share_id));
    CHECK(empty_hint.share_height == 0);

    const P2pEnvelope empty_announce = make_p2p_tip_announce_envelope(empty_hint);
    CHECK(empty_announce.type == P2pMessageType::TipAnnounce);
    CHECK(empty_announce.payload.size() == kP2pTipHintPayloadSize);
    CHECK(parse_p2p_tip_announce_envelope(empty_announce) == empty_hint);

    Share root = make_root(1000, "10", 0x11);
    Share child = make_child(root, 1001, "5", 0x12);
    Share stronger_root = make_root(1002, "20", 0x13);

    ShareChain chain;
    CHECK(chain.add_share_unchecked(root).disposition == ShareDisposition::Connected);
    CHECK(chain.add_share_unchecked(child).disposition == ShareDisposition::Connected);
    CHECK(chain.best_tip() != nullptr);
    CHECK(chain.best_tip()->id == share_id(child));

    // Best-tip announcement is derived from locally selected cumulative work.
    // A shorter competing root with more work becomes the actual local tip.
    CHECK(chain.add_share_unchecked(stronger_root).disposition == ShareDisposition::Connected);
    CHECK(chain.best_tip() != nullptr);
    CHECK(chain.best_tip()->id == share_id(stronger_root));
    const P2pTipHint local_hint = p2p_tip_hint_from_chain(chain);
    CHECK(local_hint.share_id == share_id(stronger_root));
    CHECK(local_hint.share_height == 0);

    const P2pEnvelope local_announce = make_p2p_tip_announce_envelope(chain);
    const auto local_wire = serialize_p2p_envelope(local_announce);
    CHECK(local_wire.size() == kP2pEnvelopeHeaderSize + kP2pTipHintPayloadSize);
    CHECK(local_wire[5] == static_cast<std::uint8_t>(P2pMessageType::TipAnnounce));
    CHECK(local_wire[8] == 0x00);
    CHECK(local_wire[9] == 0x00);
    CHECK(local_wire[10] == 0x00);
    CHECK(local_wire[11] == 0x28);
    CHECK(parse_p2p_tip_announce_envelope(deserialize_p2p_envelope(local_wire)) ==
          local_hint);

    P2pHandshake peer = make_handshake(0x40);

    auto decision = plan_p2p_tip_sync(peer, local_hint, chain);
    CHECK(decision.status == P2pTipSyncStatus::KnownConnectedTip);
    CHECK(!decision.requested_id.has_value());

    P2pTipHint wrong_known_height = local_hint;
    wrong_known_height.share_height = 99;
    decision = plan_p2p_tip_sync(peer, wrong_known_height, chain);
    CHECK(decision.status == P2pTipSyncStatus::HeightMismatch);
    CHECK(!decision.requested_id.has_value());

    // Claimed height does not determine trust or chain preference for an
    // unknown tip. Even UINT64_MAX only causes an exact ShareId fetch.
    P2pTipHint unknown_hint{
        share_id_from(0x80),
        std::numeric_limits<std::uint64_t>::max(),
    };
    decision = plan_p2p_tip_sync(peer, unknown_hint, chain);
    CHECK(decision.status == P2pTipSyncStatus::RequestAdvertisedTip);
    CHECK(decision.requested_id.has_value());
    CHECK(*decision.requested_id == unknown_hint.share_id);
    CHECK(chain.best_tip()->id == share_id(stronger_root));

    // If the advertised tip is already a verified orphan, request its exact
    // missing parent rather than re-requesting the orphan itself.
    Share orphan = make_root(1003, "3", 0x14);
    orphan.parent_id = share_id_from(0xa0);
    orphan.share_height = 7;
    ShareChain orphan_chain;
    CHECK(orphan_chain.add_share_unchecked(orphan).disposition == ShareDisposition::Orphan);
    const P2pTipHint orphan_hint{share_id(orphan), orphan.share_height};
    decision = plan_p2p_tip_sync(peer, orphan_hint, orphan_chain);
    CHECK(decision.status == P2pTipSyncStatus::RequestMissingParent);
    CHECK(decision.requested_id.has_value());
    CHECK(*decision.requested_id == orphan.parent_id);

    P2pHandshake no_sync = peer;
    no_sync.capabilities &= ~kP2pCapabilityShareSync;
    decision = plan_p2p_tip_sync(no_sync, unknown_hint, chain);
    CHECK(decision.status == P2pTipSyncStatus::CapabilityMissing);
    CHECK(!decision.requested_id.has_value());

    decision = plan_p2p_tip_sync(peer, empty_hint, chain);
    CHECK(decision.status == P2pTipSyncStatus::NoRemoteTip);

    P2pTipHint invalid_empty{};
    invalid_empty.share_height = 1;
    CHECK(throws_runtime([&] {
        (void)make_p2p_tip_announce_envelope(invalid_empty);
    }));

    P2pEnvelope short_tip = local_announce;
    short_tip.payload.pop_back();
    CHECK(throws_runtime([&] {
        (void)parse_p2p_tip_announce_envelope(short_tip);
    }));

    P2pEnvelope wrong_type = local_announce;
    wrong_type.type = P2pMessageType::ShareRequest;
    CHECK(throws_runtime([&] {
        (void)parse_p2p_tip_announce_envelope(wrong_type);
    }));

    // The initial handshake best-share fields and later TipAnnounce messages
    // feed the same hint planner. Neither path carries cumulative work.
    P2pHandshake server_handshake = make_handshake(0x20);
    server_handshake.best_share_id = unknown_hint.share_id;
    server_handshake.best_share_height = unknown_hint.share_height;
    const P2pTipHint handshake_hint = p2p_tip_hint_from_handshake(server_handshake);
    CHECK(handshake_hint == unknown_hint);

    P2pTcpListener listener(
        P2pEndpoint{"127.0.0.1", 0}, server_handshake);
    listener.start();
    auto accepted = std::async(std::launch::async, [&listener] {
        return listener.accept_peer();
    });

    P2pTcpConnection outbound = connect_p2p_peer(
        P2pEndpoint{"127.0.0.1", listener.port()},
        make_handshake(0x60));
    CHECK(accepted.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    P2pTcpConnection inbound = accepted.get();

    const auto from_handshake = p2p_tip_hint_from_handshake(outbound.peer_handshake());
    decision = plan_p2p_tip_sync(outbound.peer_handshake(), from_handshake, empty_chain);
    CHECK(decision.status == P2pTipSyncStatus::RequestAdvertisedTip);
    CHECK(decision.requested_id == std::optional<ShareId>{unknown_hint.share_id});

    inbound.send_envelope(local_announce);
    const P2pTipHint received_tip =
        parse_p2p_tip_announce_envelope(outbound.receive_envelope());
    CHECK(received_tip == local_hint);

    outbound.close();
    inbound.close();
    listener.stop();

    CHECK(std::string(p2p_tip_sync_status_name(
              P2pTipSyncStatus::RequestAdvertisedTip)) ==
          "request-advertised-tip");

    return 0;
}
