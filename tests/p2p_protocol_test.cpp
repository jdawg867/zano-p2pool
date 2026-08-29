#include "test_check.hpp"

#include "zano_p2pool/crypto_hash.hpp"
#include "zano_p2pool/p2p_protocol.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

using zano_p2pool::NodeId;
using zano_p2pool::P2pEnvelope;
using zano_p2pool::P2pHandshake;
using zano_p2pool::P2pHandshakeStatus;
using zano_p2pool::P2pMessageType;
using zano_p2pool::P2pNetwork;
using zano_p2pool::ShareId;

[[nodiscard]] bool throws_runtime_error(const std::function<void()>& fn) {
    try {
        fn();
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

[[nodiscard]] P2pHandshake make_handshake() {
    P2pHandshake handshake;
    handshake.network = P2pNetwork::Testnet;
    for (std::size_t i = 0; i < handshake.node_id.size(); ++i) {
        handshake.node_id[i] = static_cast<std::uint8_t>(0x10U + i);
        handshake.best_share_id[i] = static_cast<std::uint8_t>(0xa0U + i);
    }
    handshake.capabilities = zano_p2pool::kP2pCapabilitiesV1;
    handshake.listen_port = 3334;
    handshake.best_share_height = 42;
    return handshake;
}

}  // namespace

int main() {
    const P2pHandshake handshake = make_handshake();

    const std::string expected_payload_hex =
        "01101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f"
        "0000000000000007"
        "0d06"
        "a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebf"
        "000000000000002a";

    const auto payload = zano_p2pool::serialize_p2p_handshake_payload(handshake);
    CHECK(payload.size() == zano_p2pool::kP2pHandshakePayloadSize);
    CHECK(zano_p2pool::bytes_to_hex(payload) == expected_payload_hex);
    CHECK(zano_p2pool::deserialize_p2p_handshake_payload(payload) == handshake);

    const P2pEnvelope envelope = zano_p2pool::make_p2p_handshake_envelope(handshake);
    CHECK(envelope.version == zano_p2pool::kP2pProtocolVersion);
    CHECK(envelope.type == P2pMessageType::Handshake);
    CHECK(envelope.flags == 0);
    CHECK(envelope.payload == payload);

    const std::string expected_frame_hex =
        "5a5032500101000000000053"
        "01101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f"
        "0000000000000007"
        "0d06"
        "a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebf"
        "000000000000002a";

    const auto frame = zano_p2pool::serialize_p2p_envelope(envelope);
    CHECK(frame.size() ==
          zano_p2pool::kP2pEnvelopeHeaderSize +
              zano_p2pool::kP2pHandshakePayloadSize);
    CHECK(zano_p2pool::bytes_to_hex(frame) == expected_frame_hex);

    const auto decoded_envelope = zano_p2pool::deserialize_p2p_envelope(frame);
    CHECK(decoded_envelope == envelope);
    CHECK(zano_p2pool::parse_p2p_handshake_envelope(decoded_envelope) == handshake);
    CHECK(zano_p2pool::serialize_p2p_envelope(decoded_envelope) == frame);

    CHECK((handshake.capabilities & zano_p2pool::kP2pCapabilityMiningContext) != 0);
    CHECK(zano_p2pool::p2p_message_type_supported(
        static_cast<std::uint8_t>(P2pMessageType::MiningContextAnnounce)));

    NodeId other_local_id{};
    other_local_id.fill(0x55);
    CHECK(zano_p2pool::validate_p2p_handshake(
              handshake, P2pNetwork::Testnet, other_local_id) ==
          P2pHandshakeStatus::Accept);
    CHECK(zano_p2pool::validate_p2p_handshake(
              handshake, P2pNetwork::Mainnet, other_local_id) ==
          P2pHandshakeStatus::WrongNetwork);
    CHECK(zano_p2pool::validate_p2p_handshake(
              handshake, P2pNetwork::Testnet, handshake.node_id) ==
          P2pHandshakeStatus::SelfConnection);

    NodeId zero_node{};
    CHECK(zano_p2pool::is_zero_node_id(zero_node));
    CHECK(!zano_p2pool::is_zero_node_id(handshake.node_id));

    P2pHandshake no_tip = handshake;
    no_tip.best_share_id.fill(0);
    no_tip.best_share_height = 0;
    CHECK(zano_p2pool::deserialize_p2p_handshake_payload(
              zano_p2pool::serialize_p2p_handshake_payload(no_tip)) == no_tip);

    P2pHandshake invalid_node = handshake;
    invalid_node.node_id.fill(0);
    CHECK(throws_runtime_error([&] {
        (void)zano_p2pool::serialize_p2p_handshake_payload(invalid_node);
    }));

    P2pHandshake invalid_tip = handshake;
    invalid_tip.best_share_id.fill(0);
    CHECK(throws_runtime_error([&] {
        (void)zano_p2pool::serialize_p2p_handshake_payload(invalid_tip);
    }));

    P2pHandshake invalid_network = handshake;
    invalid_network.network = static_cast<P2pNetwork>(99);
    CHECK(throws_runtime_error([&] {
        (void)zano_p2pool::serialize_p2p_handshake_payload(invalid_network);
    }));

    auto bad_magic = frame;
    bad_magic[0] ^= 0xffU;
    CHECK(throws_runtime_error([&] {
        (void)zano_p2pool::deserialize_p2p_envelope(bad_magic);
    }));

    auto bad_version = frame;
    bad_version[4] = 2;
    CHECK(throws_runtime_error([&] {
        (void)zano_p2pool::deserialize_p2p_envelope(bad_version);
    }));

    auto bad_type = frame;
    bad_type[5] = 0x7f;
    CHECK(throws_runtime_error([&] {
        (void)zano_p2pool::deserialize_p2p_envelope(bad_type);
    }));

    auto bad_flags = frame;
    bad_flags[7] = 1;
    CHECK(throws_runtime_error([&] {
        (void)zano_p2pool::deserialize_p2p_envelope(bad_flags);
    }));

    auto truncated_header = frame;
    truncated_header.resize(zano_p2pool::kP2pEnvelopeHeaderSize - 1);
    CHECK(throws_runtime_error([&] {
        (void)zano_p2pool::deserialize_p2p_envelope(truncated_header);
    }));

    auto truncated_payload = frame;
    truncated_payload.pop_back();
    CHECK(throws_runtime_error([&] {
        (void)zano_p2pool::deserialize_p2p_envelope(truncated_payload);
    }));

    auto trailing = frame;
    trailing.push_back(0);
    CHECK(throws_runtime_error([&] {
        (void)zano_p2pool::deserialize_p2p_envelope(trailing);
    }));

    auto bad_network_payload = payload;
    bad_network_payload[0] = 99;
    CHECK(throws_runtime_error([&] {
        (void)zano_p2pool::deserialize_p2p_handshake_payload(bad_network_payload);
    }));

    auto zero_node_payload = payload;
    std::fill(zero_node_payload.begin() + 1, zero_node_payload.begin() + 33, 0);
    CHECK(throws_runtime_error([&] {
        (void)zano_p2pool::deserialize_p2p_handshake_payload(zero_node_payload);
    }));

    auto inconsistent_tip_payload = payload;
    std::fill(
        inconsistent_tip_payload.begin() + 43,
        inconsistent_tip_payload.begin() + 75,
        0);
    CHECK(throws_runtime_error([&] {
        (void)zano_p2pool::deserialize_p2p_handshake_payload(
            inconsistent_tip_payload);
    }));

    auto short_handshake = payload;
    short_handshake.pop_back();
    CHECK(throws_runtime_error([&] {
        (void)zano_p2pool::deserialize_p2p_handshake_payload(short_handshake);
    }));

    P2pEnvelope oversized;
    oversized.payload.resize(zano_p2pool::kP2pMaxPayloadSize + 1);
    CHECK(throws_runtime_error([&] {
        (void)zano_p2pool::serialize_p2p_envelope(oversized);
    }));

    P2pEnvelope unsupported_type = envelope;
    unsupported_type.type = static_cast<P2pMessageType>(0x7f);
    CHECK(throws_runtime_error([&] {
        (void)zano_p2pool::serialize_p2p_envelope(unsupported_type);
    }));

    P2pEnvelope unsupported_version = envelope;
    unsupported_version.version = 2;
    CHECK(throws_runtime_error([&] {
        (void)zano_p2pool::serialize_p2p_envelope(unsupported_version);
    }));

    P2pEnvelope unsupported_flags = envelope;
    unsupported_flags.flags = 1;
    CHECK(throws_runtime_error([&] {
        (void)zano_p2pool::serialize_p2p_envelope(unsupported_flags);
    }));

    std::cout << "p2p protocol tests passed\n";
    return 0;
}
