#pragma once

#include "zano_p2pool/share.hpp"
#include "zano_p2pool/sidechain_params.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace zano_p2pool {

inline constexpr std::array<std::uint8_t, 4> kP2pMagic{'Z', 'P', '2', 'P'};
inline constexpr std::uint8_t kP2pProtocolVersion = 2;
inline constexpr std::size_t kP2pEnvelopeHeaderSize = 12;
inline constexpr std::size_t kP2pMaxPayloadSize = 64 * 1024;
inline constexpr std::size_t kP2pHandshakePayloadSize = 115;

enum class P2pMessageType : std::uint8_t {
    Handshake = 1,
    ShareAnnounce = 2,
    ShareRequest = 3,
    ShareResponse = 4,
    TipAnnounce = 5,
    MiningContextAnnounce = 6,
};

enum class P2pNetwork : std::uint8_t {
    Testnet = 1,
    Mainnet = 2,
};

using NodeId = Hash256;

inline constexpr std::uint64_t kP2pCapabilityShareGossip = 1ULL << 0;
inline constexpr std::uint64_t kP2pCapabilityShareSync = 1ULL << 1;
inline constexpr std::uint64_t kP2pCapabilityMiningContext = 1ULL << 2;
inline constexpr std::uint64_t kP2pCapabilitiesV1 =
    kP2pCapabilityShareGossip |
    kP2pCapabilityShareSync |
    kP2pCapabilityMiningContext;

struct P2pEnvelope {
    std::uint8_t version{kP2pProtocolVersion};
    P2pMessageType type{P2pMessageType::Handshake};
    std::uint16_t flags{0};
    std::vector<std::uint8_t> payload;

    bool operator==(const P2pEnvelope&) const = default;
};

// NodeId is an opaque, non-zero, public 32-byte node identifier. Persistent
// generation/storage policy is intentionally left to a later checkpoint.
//
// sidechain_id may be left zero only for a locally constructed handshake. In
// that case serialization/transport resolves it to the canonical sidechain for
// `network`. A received wire handshake always carries an explicit non-zero ID.
struct P2pHandshake {
    P2pNetwork network{P2pNetwork::Testnet};
    NodeId node_id{};
    std::uint64_t capabilities{kP2pCapabilitiesV1};
    std::uint16_t listen_port{0};
    ShareId best_share_id{};
    std::uint64_t best_share_height{0};
    SidechainId sidechain_id{};

    bool operator==(const P2pHandshake&) const = default;
};

enum class P2pHandshakeStatus {
    Accept,
    WrongNetwork,
    WrongSidechain,
    SelfConnection,
};

[[nodiscard]] bool p2p_message_type_supported(
    std::uint8_t value) noexcept;

[[nodiscard]] SidechainId canonical_p2p_sidechain_id(P2pNetwork network);
[[nodiscard]] SidechainId resolved_p2p_sidechain_id(
    const P2pHandshake& handshake);

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
    const SidechainId& expected_sidechain_id,
    const NodeId& local_node_id) noexcept;

// Compatibility helper for callers using the canonical sidechain profile for
// the selected parent network.
[[nodiscard]] P2pHandshakeStatus validate_p2p_handshake(
    const P2pHandshake& peer,
    P2pNetwork expected_network,
    const NodeId& local_node_id);

[[nodiscard]] bool is_zero_node_id(const NodeId& node_id) noexcept;

}  // namespace zano_p2pool
