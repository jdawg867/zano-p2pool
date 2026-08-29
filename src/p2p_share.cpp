#include "zano_p2pool/p2p_share.hpp"

#include <stdexcept>

namespace zano_p2pool {

P2pEnvelope make_p2p_share_announce_envelope(const Share& share) {
    P2pEnvelope envelope;
    envelope.type = P2pMessageType::ShareAnnounce;
    envelope.payload = serialize_share(share);
    return envelope;
}

Share parse_p2p_share_announce_envelope(const P2pEnvelope& envelope) {
    if (envelope.version != kP2pProtocolVersion) {
        throw std::runtime_error("unsupported P2P protocol version");
    }
    if (envelope.type != P2pMessageType::ShareAnnounce) {
        throw std::runtime_error("P2P envelope is not a share announcement");
    }
    if (envelope.flags != 0) {
        throw std::runtime_error("unsupported P2P envelope flags");
    }
    if (envelope.payload.size() != kShareV1SerializedSize) {
        throw std::runtime_error("invalid P2P share announcement payload size");
    }
    return deserialize_share(envelope.payload);
}

void P2pTrustedWorkRegistry::remember(const ShareWorkContext& context) {
    if (difficulty128_is_zero(context.network_difficulty)) {
        throw std::runtime_error("trusted P2P work context has zero network difficulty");
    }

    const Key key{context.zano_height, context.mining_header_hash};
    const auto it = contexts_.find(key);
    if (it != contexts_.end()) {
        if (it->second.network_difficulty != context.network_difficulty) {
            throw std::runtime_error("conflicting trusted P2P work context");
        }
        return;
    }
    contexts_.emplace(key, context);
}

const ShareWorkContext* P2pTrustedWorkRegistry::find(
    std::uint64_t zano_height,
    const Hash256& mining_header_hash) const noexcept {
    const auto it = contexts_.find(Key{zano_height, mining_header_hash});
    return it == contexts_.end() ? nullptr : &it->second;
}

std::size_t P2pTrustedWorkRegistry::size() const noexcept {
    return contexts_.size();
}

P2pShareReceiver::P2pShareReceiver(
    ShareChain& chain,
    const P2pTrustedWorkRegistry& trusted_work) noexcept
    : chain_(chain), trusted_work_(trusted_work) {}

P2pShareReceiveResult P2pShareReceiver::receive(
    const P2pHandshake& peer,
    const P2pEnvelope& envelope,
    std::uint64_t now,
    ProgPowZContextMode mode) {
    const Share share = parse_p2p_share_announce_envelope(envelope);

    P2pShareReceiveResult result;
    result.chain_result.id = share_id(share);

    if ((peer.capabilities & kP2pCapabilityShareGossip) == 0) {
        result.status = P2pShareReceiveStatus::CapabilityMissing;
        return result;
    }

    // Suppress already connected/orphaned shares before trusted-context lookup
    // or expensive ProgPoWZ verification.
    if (chain_.contains(result.chain_result.id) ||
        chain_.is_orphan(result.chain_result.id)) {
        result.status = P2pShareReceiveStatus::Duplicate;
        result.chain_result.disposition = ShareDisposition::Duplicate;
        return result;
    }

    const ShareWorkContext* trusted = trusted_work_.find(
        share.zano_height,
        share.mining_header_hash);
    if (trusted == nullptr) {
        result.status = P2pShareReceiveStatus::UnknownWorkContext;
        return result;
    }

    result.chain_result = chain_.submit_share(share, *trusted, now, mode);
    switch (result.chain_result.disposition) {
    case ShareDisposition::Connected:
        result.status = P2pShareReceiveStatus::Connected;
        break;
    case ShareDisposition::Orphan:
        result.status = P2pShareReceiveStatus::Orphan;
        break;
    case ShareDisposition::Duplicate:
        result.status = P2pShareReceiveStatus::Duplicate;
        break;
    case ShareDisposition::Rejected:
        result.status = P2pShareReceiveStatus::Rejected;
        break;
    }
    return result;
}

const char* p2p_share_receive_status_name(
    P2pShareReceiveStatus status) noexcept {
    switch (status) {
    case P2pShareReceiveStatus::Connected:
        return "connected";
    case P2pShareReceiveStatus::Orphan:
        return "orphan";
    case P2pShareReceiveStatus::Duplicate:
        return "duplicate";
    case P2pShareReceiveStatus::Rejected:
        return "rejected";
    case P2pShareReceiveStatus::UnknownWorkContext:
        return "unknown-work-context";
    case P2pShareReceiveStatus::CapabilityMissing:
        return "capability-missing";
    }
    return "unknown";
}

}  // namespace zano_p2pool
