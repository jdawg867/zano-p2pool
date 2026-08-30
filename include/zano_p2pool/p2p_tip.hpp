#pragma once

#include "zano_p2pool/p2p_protocol.hpp"
#include "zano_p2pool/share_chain.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace zano_p2pool {

inline constexpr std::size_t kP2pTipHintPayloadSize = 40;

struct P2pTipHint {
    ShareId share_id{};
    std::uint64_t share_height{0};

    bool operator==(const P2pTipHint&) const = default;
};

[[nodiscard]] P2pTipHint p2p_tip_hint_from_chain(
    const ShareChain& chain) noexcept;
[[nodiscard]] P2pTipHint p2p_tip_hint_from_handshake(
    const P2pHandshake& handshake);

[[nodiscard]] P2pEnvelope make_p2p_tip_announce_envelope(
    const P2pTipHint& hint);
[[nodiscard]] P2pEnvelope make_p2p_tip_announce_envelope(
    const ShareChain& chain);
[[nodiscard]] P2pTipHint parse_p2p_tip_announce_envelope(
    const P2pEnvelope& envelope);

enum class P2pTipSyncStatus {
    NoRemoteTip,
    KnownConnectedTip,
    RequestAdvertisedTip,
    RequestMissingParent,
    HeightMismatch,
    CapabilityMissing,
};

struct P2pTipSyncDecision {
    P2pTipSyncStatus status{P2pTipSyncStatus::NoRemoteTip};
    std::optional<ShareId> requested_id;
};

// Tip IDs/heights are synchronization hints only. Unknown IDs are fetched and
// locally validated; claimed height never contributes work or chooses a tip.
[[nodiscard]] P2pTipSyncDecision plan_p2p_tip_sync(
    const P2pHandshake& peer,
    const P2pTipHint& hint,
    const ShareChain& chain) noexcept;

[[nodiscard]] const char* p2p_tip_sync_status_name(
    P2pTipSyncStatus status) noexcept;

}  // namespace zano_p2pool
