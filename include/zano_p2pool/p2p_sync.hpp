#pragma once

#include "zano_p2pool/p2p_share.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace zano_p2pool {

inline constexpr std::size_t kP2pShareRequestPayloadSize = 32;
inline constexpr std::size_t kP2pShareResponseHeaderSize = 33;
inline constexpr std::size_t kP2pShareResponseFoundV1PayloadSize =
    kP2pShareResponseHeaderSize + kShareV1SerializedSize;
inline constexpr std::size_t kP2pShareResponseFoundV2PayloadSize =
    kP2pShareResponseHeaderSize + kShareV2SerializedSize;
// Compatibility alias for v1-era tests/callers.
inline constexpr std::size_t kP2pShareResponseFoundPayloadSize =
    kP2pShareResponseFoundV1PayloadSize;

enum class P2pShareResponseCode : std::uint8_t {
    Found = 1,
    NotFound = 2,
};

struct P2pShareResponse {
    P2pShareResponseCode code{P2pShareResponseCode::NotFound};
    ShareId requested_id{};
    std::optional<Share> share;
};

[[nodiscard]] P2pEnvelope make_p2p_share_request_envelope(
    const ShareId& requested_id);
[[nodiscard]] ShareId parse_p2p_share_request_envelope(
    const P2pEnvelope& envelope);

[[nodiscard]] P2pEnvelope make_p2p_share_response_envelope(
    const ShareId& requested_id,
    const Share* share);
[[nodiscard]] P2pShareResponse parse_p2p_share_response_envelope(
    const P2pEnvelope& envelope);

// Answers only from shares that are already connected in the local share
// chain. The receiver still independently verifies any returned share; a
// response is data transport, never a trust transfer.
[[nodiscard]] P2pEnvelope answer_p2p_share_request(
    const P2pHandshake& peer,
    const P2pEnvelope& request,
    const ShareChain& chain);

enum class P2pShareSyncReceiveStatus {
    ShareProcessed,
    NotFound,
    CapabilityMissing,
};

struct P2pShareSyncReceiveResult {
    P2pShareSyncReceiveStatus status{P2pShareSyncReceiveStatus::NotFound};
    ShareId requested_id{};
    std::optional<P2pShareReceiveResult> share_result;
};

[[nodiscard]] P2pShareSyncReceiveResult receive_p2p_share_response(
    P2pShareReceiver& receiver,
    const P2pHandshake& peer,
    const P2pEnvelope& response,
    std::uint64_t now,
    ProgPowZContextMode mode = ProgPowZContextMode::Light);

[[nodiscard]] const char* p2p_share_sync_receive_status_name(
    P2pShareSyncReceiveStatus status) noexcept;

}  // namespace zano_p2pool
