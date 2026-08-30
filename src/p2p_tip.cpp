#include "zano_p2pool/p2p_tip.hpp"

#include <algorithm>
#include <stdexcept>

namespace zano_p2pool {
namespace {

void append_u64_be(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

[[nodiscard]] std::uint64_t read_u64_be(
    std::span<const std::uint8_t> bytes,
    std::size_t offset) {
    if (offset + 8 > bytes.size()) {
        throw std::runtime_error("truncated P2P tip height");
    }
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value = (value << 8) | static_cast<std::uint64_t>(bytes[offset + i]);
    }
    return value;
}

void require_valid_tip_hint(const P2pTipHint& hint) {
    if (is_zero_share_id(hint.share_id) && hint.share_height != 0) {
        throw std::runtime_error(
            "zero P2P tip id requires zero share height");
    }
}

}  // namespace

P2pTipHint p2p_tip_hint_from_chain(const ShareChain& chain) noexcept {
    const ConnectedShare* tip = chain.best_tip();
    if (tip == nullptr) {
        return {};
    }
    return P2pTipHint{tip->id, tip->share.share_height};
}

P2pTipHint p2p_tip_hint_from_handshake(const P2pHandshake& handshake) {
    P2pTipHint hint{handshake.best_share_id, handshake.best_share_height};
    require_valid_tip_hint(hint);
    return hint;
}

P2pEnvelope make_p2p_tip_announce_envelope(const P2pTipHint& hint) {
    require_valid_tip_hint(hint);

    P2pEnvelope envelope;
    envelope.type = P2pMessageType::TipAnnounce;
    envelope.payload.reserve(kP2pTipHintPayloadSize);
    envelope.payload.insert(
        envelope.payload.end(), hint.share_id.begin(), hint.share_id.end());
    append_u64_be(envelope.payload, hint.share_height);
    return envelope;
}

P2pEnvelope make_p2p_tip_announce_envelope(const ShareChain& chain) {
    return make_p2p_tip_announce_envelope(p2p_tip_hint_from_chain(chain));
}

P2pTipHint parse_p2p_tip_announce_envelope(const P2pEnvelope& envelope) {
    if (envelope.version != kP2pProtocolVersion) {
        throw std::runtime_error("unsupported P2P protocol version");
    }
    if (envelope.type != P2pMessageType::TipAnnounce) {
        throw std::runtime_error("P2P envelope is not a tip announcement");
    }
    if (envelope.flags != 0) {
        throw std::runtime_error("unsupported P2P envelope flags");
    }
    if (envelope.payload.size() != kP2pTipHintPayloadSize) {
        throw std::runtime_error("invalid P2P tip announcement payload size");
    }

    P2pTipHint hint;
    std::copy_n(
        envelope.payload.begin(), hint.share_id.size(), hint.share_id.begin());
    hint.share_height = read_u64_be(envelope.payload, hint.share_id.size());
    require_valid_tip_hint(hint);
    return hint;
}

P2pTipSyncDecision plan_p2p_tip_sync(
    const P2pHandshake& peer,
    const P2pTipHint& hint,
    const ShareChain& chain) noexcept {
    if ((peer.capabilities & kP2pCapabilityShareSync) == 0) {
        return {P2pTipSyncStatus::CapabilityMissing, std::nullopt};
    }

    if (is_zero_share_id(hint.share_id)) {
        if (hint.share_height != 0) {
            return {P2pTipSyncStatus::HeightMismatch, std::nullopt};
        }
        return {P2pTipSyncStatus::NoRemoteTip, std::nullopt};
    }

    if (const ConnectedShare* connected = chain.find(hint.share_id)) {
        if (connected->share.share_height != hint.share_height) {
            return {P2pTipSyncStatus::HeightMismatch, std::nullopt};
        }
        return {P2pTipSyncStatus::KnownConnectedTip, std::nullopt};
    }

    if (const Share* orphan = chain.find_orphan_share(hint.share_id)) {
        if (orphan->share_height != hint.share_height ||
            is_zero_share_id(orphan->parent_id)) {
            return {P2pTipSyncStatus::HeightMismatch, std::nullopt};
        }
        return {
            P2pTipSyncStatus::RequestMissingParent,
            orphan->parent_id,
        };
    }

    // The advertised height is deliberately not used to decide whether the
    // peer is "ahead" or how much work it has. Fetch the unknown ID and let
    // local share validation/cumulative work decide whether it matters.
    return {
        P2pTipSyncStatus::RequestAdvertisedTip,
        hint.share_id,
    };
}

const char* p2p_tip_sync_status_name(P2pTipSyncStatus status) noexcept {
    switch (status) {
    case P2pTipSyncStatus::NoRemoteTip:
        return "no-remote-tip";
    case P2pTipSyncStatus::KnownConnectedTip:
        return "known-connected-tip";
    case P2pTipSyncStatus::RequestAdvertisedTip:
        return "request-advertised-tip";
    case P2pTipSyncStatus::RequestMissingParent:
        return "request-missing-parent";
    case P2pTipSyncStatus::HeightMismatch:
        return "height-mismatch";
    case P2pTipSyncStatus::CapabilityMissing:
        return "capability-missing";
    }
    return "unknown";
}

}  // namespace zano_p2pool
