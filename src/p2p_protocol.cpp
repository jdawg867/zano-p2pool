#include "zano_p2pool/p2p_protocol.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <string>

namespace zano_p2pool {
namespace {

[[nodiscard]] bool is_supported_network(std::uint8_t value) noexcept {
    return value == static_cast<std::uint8_t>(P2pNetwork::Testnet) ||
           value == static_cast<std::uint8_t>(P2pNetwork::Mainnet);
}

void append_u16_be(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xffU));
    out.push_back(static_cast<std::uint8_t>(value & 0xffU));
}

void append_u32_be(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xffU));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xffU));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xffU));
    out.push_back(static_cast<std::uint8_t>(value & 0xffU));
}

void append_u64_be(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

[[nodiscard]] std::uint16_t read_u16_be(
    std::span<const std::uint8_t> bytes,
    std::size_t offset) {
    if (offset + 2 > bytes.size()) {
        throw std::runtime_error("truncated P2P uint16");
    }
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(bytes[offset]) << 8) |
        static_cast<std::uint16_t>(bytes[offset + 1]));
}

[[nodiscard]] std::uint32_t read_u32_be(
    std::span<const std::uint8_t> bytes,
    std::size_t offset) {
    if (offset + 4 > bytes.size()) {
        throw std::runtime_error("truncated P2P uint32");
    }
    return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
           static_cast<std::uint32_t>(bytes[offset + 3]);
}

[[nodiscard]] std::uint64_t read_u64_be(
    std::span<const std::uint8_t> bytes,
    std::size_t offset) {
    if (offset + 8 > bytes.size()) {
        throw std::runtime_error("truncated P2P uint64");
    }

    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value = (value << 8) | static_cast<std::uint64_t>(bytes[offset + i]);
    }
    return value;
}

void require_valid_handshake(const P2pHandshake& handshake) {
    const auto network_value = static_cast<std::uint8_t>(handshake.network);
    if (!is_supported_network(network_value)) {
        throw std::runtime_error("unsupported P2P network");
    }
    if (is_zero_node_id(handshake.node_id)) {
        throw std::runtime_error("P2P node id must be non-zero");
    }
    if (is_zero_share_id(handshake.best_share_id) &&
        handshake.best_share_height != 0) {
        throw std::runtime_error(
            "zero best-share id requires zero best-share height");
    }
}

}  // namespace

bool p2p_message_type_supported(std::uint8_t value) noexcept {
    return value == static_cast<std::uint8_t>(P2pMessageType::Handshake) ||
           value == static_cast<std::uint8_t>(P2pMessageType::ShareAnnounce) ||
           value == static_cast<std::uint8_t>(P2pMessageType::ShareRequest) ||
           value == static_cast<std::uint8_t>(P2pMessageType::ShareResponse) ||
           value == static_cast<std::uint8_t>(P2pMessageType::TipAnnounce) ||
           value == static_cast<std::uint8_t>(P2pMessageType::MiningContextAnnounce);
}

bool is_zero_node_id(const NodeId& node_id) noexcept {
    return std::all_of(
        node_id.begin(), node_id.end(), [](std::uint8_t byte) { return byte == 0; });
}

SidechainId canonical_p2p_sidechain_id(P2pNetwork network) {
    switch (network) {
    case P2pNetwork::Testnet:
        return sidechain_id(canonical_sidechain_parameters(
            SidechainParentNetwork::Testnet));
    case P2pNetwork::Mainnet:
        return sidechain_id(canonical_sidechain_parameters(
            SidechainParentNetwork::Mainnet));
    }
    throw std::invalid_argument("unsupported P2P network");
}

SidechainId resolved_p2p_sidechain_id(const P2pHandshake& handshake) {
    if (!is_zero_sidechain_id(handshake.sidechain_id)) {
        return handshake.sidechain_id;
    }
    return canonical_p2p_sidechain_id(handshake.network);
}

std::vector<std::uint8_t> serialize_p2p_envelope(
    const P2pEnvelope& envelope) {
    if (envelope.version != kP2pProtocolVersion) {
        throw std::runtime_error("unsupported P2P protocol version");
    }
    if (!p2p_message_type_supported(static_cast<std::uint8_t>(envelope.type))) {
        throw std::runtime_error("unsupported P2P message type");
    }
    if (envelope.flags != 0) {
        throw std::runtime_error("unsupported P2P envelope flags");
    }
    if (envelope.payload.size() > kP2pMaxPayloadSize) {
        throw std::runtime_error("P2P payload exceeds maximum size");
    }

    std::vector<std::uint8_t> out;
    out.reserve(kP2pEnvelopeHeaderSize + envelope.payload.size());
    out.insert(out.end(), kP2pMagic.begin(), kP2pMagic.end());
    out.push_back(envelope.version);
    out.push_back(static_cast<std::uint8_t>(envelope.type));
    append_u16_be(out, envelope.flags);
    append_u32_be(out, static_cast<std::uint32_t>(envelope.payload.size()));
    out.insert(out.end(), envelope.payload.begin(), envelope.payload.end());
    return out;
}

P2pEnvelope deserialize_p2p_envelope(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kP2pEnvelopeHeaderSize) {
        throw std::runtime_error("truncated P2P envelope header");
    }
    if (!std::equal(kP2pMagic.begin(), kP2pMagic.end(), bytes.begin())) {
        throw std::runtime_error("invalid P2P envelope magic");
    }

    const std::uint8_t version = bytes[4];
    if (version != kP2pProtocolVersion) {
        throw std::runtime_error("unsupported P2P protocol version");
    }

    const std::uint8_t type_value = bytes[5];
    if (!p2p_message_type_supported(type_value)) {
        throw std::runtime_error("unsupported P2P message type");
    }

    const std::uint16_t flags = read_u16_be(bytes, 6);
    if (flags != 0) {
        throw std::runtime_error("unsupported P2P envelope flags");
    }

    const std::uint32_t payload_size = read_u32_be(bytes, 8);
    if (payload_size > kP2pMaxPayloadSize) {
        throw std::runtime_error("P2P payload exceeds maximum size");
    }

    const std::size_t expected_size =
        kP2pEnvelopeHeaderSize + static_cast<std::size_t>(payload_size);
    if (bytes.size() != expected_size) {
        throw std::runtime_error(
            bytes.size() < expected_size
                ? "truncated P2P envelope payload"
                : "trailing bytes after P2P envelope");
    }

    P2pEnvelope envelope;
    envelope.version = version;
    envelope.type = static_cast<P2pMessageType>(type_value);
    envelope.flags = flags;
    envelope.payload.assign(
        bytes.begin() + static_cast<std::ptrdiff_t>(kP2pEnvelopeHeaderSize),
        bytes.end());
    return envelope;
}

std::vector<std::uint8_t> serialize_p2p_handshake_payload(
    const P2pHandshake& handshake) {
    require_valid_handshake(handshake);
    const SidechainId effective_sidechain_id =
        resolved_p2p_sidechain_id(handshake);

    std::vector<std::uint8_t> out;
    out.reserve(kP2pHandshakePayloadSize);
    out.push_back(static_cast<std::uint8_t>(handshake.network));
    out.insert(
        out.end(),
        effective_sidechain_id.begin(),
        effective_sidechain_id.end());
    out.insert(out.end(), handshake.node_id.begin(), handshake.node_id.end());
    append_u64_be(out, handshake.capabilities);
    append_u16_be(out, handshake.listen_port);
    out.insert(
        out.end(), handshake.best_share_id.begin(), handshake.best_share_id.end());
    append_u64_be(out, handshake.best_share_height);

    if (out.size() != kP2pHandshakePayloadSize) {
        throw std::runtime_error("internal P2P handshake size mismatch");
    }
    return out;
}

P2pHandshake deserialize_p2p_handshake_payload(
    std::span<const std::uint8_t> bytes) {
    if (bytes.size() != kP2pHandshakePayloadSize) {
        throw std::runtime_error("invalid P2P handshake payload size");
    }

    const std::uint8_t network_value = bytes[0];
    if (!is_supported_network(network_value)) {
        throw std::runtime_error("unsupported P2P network");
    }

    P2pHandshake handshake;
    handshake.network = static_cast<P2pNetwork>(network_value);
    std::copy_n(
        bytes.begin() + 1,
        handshake.sidechain_id.size(),
        handshake.sidechain_id.begin());
    if (is_zero_sidechain_id(handshake.sidechain_id)) {
        throw std::runtime_error("P2P sidechain id must be non-zero");
    }
    std::copy_n(
        bytes.begin() + 33,
        handshake.node_id.size(),
        handshake.node_id.begin());
    handshake.capabilities = read_u64_be(bytes, 65);
    handshake.listen_port = read_u16_be(bytes, 73);
    std::copy_n(
        bytes.begin() + 75,
        handshake.best_share_id.size(),
        handshake.best_share_id.begin());
    handshake.best_share_height = read_u64_be(bytes, 107);

    require_valid_handshake(handshake);
    return handshake;
}

P2pEnvelope make_p2p_handshake_envelope(const P2pHandshake& handshake) {
    P2pEnvelope envelope;
    envelope.type = P2pMessageType::Handshake;
    envelope.payload = serialize_p2p_handshake_payload(handshake);
    return envelope;
}

P2pHandshake parse_p2p_handshake_envelope(const P2pEnvelope& envelope) {
    if (envelope.version != kP2pProtocolVersion) {
        throw std::runtime_error("unsupported P2P protocol version");
    }
    if (envelope.type != P2pMessageType::Handshake) {
        throw std::runtime_error("P2P envelope is not a handshake");
    }
    if (envelope.flags != 0) {
        throw std::runtime_error("unsupported P2P envelope flags");
    }
    return deserialize_p2p_handshake_payload(envelope.payload);
}

P2pHandshakeStatus validate_p2p_handshake(
    const P2pHandshake& peer,
    P2pNetwork expected_network,
    const SidechainId& expected_sidechain_id,
    const NodeId& local_node_id) noexcept {
    if (peer.network != expected_network) {
        return P2pHandshakeStatus::WrongNetwork;
    }
    if (peer.sidechain_id != expected_sidechain_id) {
        return P2pHandshakeStatus::WrongSidechain;
    }
    if (peer.node_id == local_node_id) {
        return P2pHandshakeStatus::SelfConnection;
    }
    return P2pHandshakeStatus::Accept;
}

P2pHandshakeStatus validate_p2p_handshake(
    const P2pHandshake& peer,
    P2pNetwork expected_network,
    const NodeId& local_node_id) {
    return validate_p2p_handshake(
        peer,
        expected_network,
        canonical_p2p_sidechain_id(expected_network),
        local_node_id);
}

}  // namespace zano_p2pool
