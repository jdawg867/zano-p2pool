#pragma once

#include "zano_p2pool/share.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace zano_p2pool {

inline constexpr std::array<std::uint8_t, 4> kP2pMagic{'Z', 'P', '2', 'P'};
inline constexpr std::uint8_t kP2pProtocolVersion = 1;
inline constexpr std::size_t kP2pEnvelopeHeaderSize = 12;
inline constexpr std::size_t kP2pMaxPayloadSize = 64 * 1024;
inline constexpr std::size_t kP2pHandshakePayloadSize = 83;

enum class P2pMessageType : std::uint8_t {
    Handshake = 1,
};

enum class P2pNetwork : std::uint8_t {
    Testnet = 1,
    Mainnet = 2,
};

using NodeId = Hash256;

inline constexpr std::uint64_t kP2pCapabilityShareGossip = 1ULL << 0;
inline constexpr std::uint64_t kP2pCapabilityShareSync = 1ULL << 1;
inline constexpr std::uint64_t kP2pCapabilitiesV1 =
    kP2pCapabilityShareGossip | kP2pCapabilityShareSync;

struct P2pEnvelope {
    std::uint8_t version{kP2pProtocolVersion};
    P2pMessageType type{P2pMessageType::Handshake};
    std::uint16_t flags{0};
    std::vector<std::uint8_t> payload;

    bool operator==(const P2pEnvelope&) const = default;
};

// NodeId is an opaque, non-zero, public 32-byte node identifier. Persistent
// generation/storage policy is intentionally left to a later checkpoint.
struct P2pHandshake {
    P2pNetwork network{P2pNetwork::Testnet};
    NodeId node_id{};
    std::uint64_t capabilities{kP2pCapabilitiesV1};
    std::uint16_t listen_port{0};
    ShareId best_share_id{};
    std::uint64_t best_share_height{0};

    bool operator==(const P2pHandshake&) const = default;
};

enum class P2pHandshakeStatus {
    Accept,
    WrongNetwork,
    SelfConnection,
};

[[nodiscard]] std::vector<std::uint8_t> serialize_p2p_envelope(
    const P2pEnvelope& envelope);
[[nodiscard]] P2pEnvelope deserialize_p2p_envelope(
    std::span<const std::uint8_t> bytes);

[[nodiscard]] std::vector<std::uint8_t> serialize_p2p_handshake_payload(
    const P2pHandshake& handshake);
[[nodiscard]] P2pHandshake deserialize_p2p_handshake_payload(
    std::span<const std::uint8_t> bytes);

[[nodiscard]] P2pEnvelope make_p2p_handshake_envelope(
    const P2pHandshake& handshake);
[[nodiscard]] P2pHandshake parse_p2p_handshake_envelope(
    const P2pEnvelope& envelope);

[[nodiscard]] P2pHandshakeStatus validate_p2p_handshake(
    const P2pHandshake& peer,
    P2pNetwork expected_network,
    const NodeId& local_node_id) noexcept;

[[nodiscard]] bool is_zero_node_id(const NodeId& node_id) noexcept;

}  // namespace zano_p2pool
