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
    if (envelope.payload.size() != kShareV1SerializedSize &&
        envelope.payload.size() != kShareV2SerializedSize) {
        throw std::runtime_error("invalid P2P share announcement payload size");
    }
    return deserialize_share(envelope.payload);
}

void P2pTrustedWorkRegistry::remember(const ShareWorkContext& context) {
    remember(context, ShareId{});
}

void P2pTrustedWorkRegistry::remember(
    const ShareWorkContext& context,
    const ShareId& parent_id) {
    if (difficulty128_is_zero(context.network_difficulty)) {
        throw std::runtime_error("trusted P2P work context has zero network difficulty");
    }

    const Key key{context.zano_height, context.mining_header_hash, parent_id};
    const auto it = contexts_.find(key);
    if (it != contexts_.end()) {
        if (it->second.context.network_difficulty !=
            context.network_difficulty) {
            throw std::runtime_error("conflicting trusted P2P work context");
        }
        return;
    }

    contexts_.emplace(
        key,
        Entry{context, std::nullopt});
}

void P2pTrustedWorkRegistry::remember(
    const ShareWorkContext& context,
    const ShareId& parent_id,
    const Hash256& zano_parent_hash) {
    if (difficulty128_is_zero(context.network_difficulty)) {
        throw std::runtime_error("trusted P2P work context has zero network difficulty");
    }

    const Key key{context.zano_height, context.mining_header_hash, parent_id};
    const auto it = contexts_.find(key);
    if (it != contexts_.end()) {
        if (it->second.context.network_difficulty !=
            context.network_difficulty) {
            throw std::runtime_error("conflicting trusted P2P work context");
        }

        if (it->second.zano_parent_hash.has_value() &&
            *it->second.zano_parent_hash != zano_parent_hash) {
            throw std::runtime_error(
                "conflicting trusted P2P Zano parent provenance");
        }

        it->second.zano_parent_hash = zano_parent_hash;
        return;
    }

    contexts_.emplace(
        key,
        Entry{context, zano_parent_hash});
}

const ShareWorkContext* P2pTrustedWorkRegistry::find(
    std::uint64_t zano_height,
    const Hash256& mining_header_hash) const noexcept {
    return find(zano_height, mining_header_hash, ShareId{});
}

const ShareWorkContext* P2pTrustedWorkRegistry::find(
    std::uint64_t zano_height,
    const Hash256& mining_header_hash,
    const ShareId& parent_id) const noexcept {
    const auto it = contexts_.find(
        Key{zano_height, mining_header_hash, parent_id});
    return it == contexts_.end() ? nullptr : &it->second.context;
}

std::size_t P2pTrustedWorkRegistry::erase_zano_height(
    std::uint64_t zano_height) {
    std::size_t erased = 0;

    for (auto it = contexts_.begin(); it != contexts_.end();) {
        if (std::get<0>(it->first) != zano_height) {
            ++it;
            continue;
        }

        it = contexts_.erase(it);
        ++erased;
    }

    return erased;
}

std::size_t P2pTrustedWorkRegistry::erase_zano_heights_above(
    std::uint64_t maximum_zano_height) {
    std::size_t erased = 0;

    for (auto it = contexts_.begin(); it != contexts_.end();) {
        if (std::get<0>(it->first) <= maximum_zano_height) {
            ++it;
            continue;
        }

        it = contexts_.erase(it);
        ++erased;
    }

    return erased;
}

std::size_t P2pTrustedWorkRegistry::erase_zano_parent_mismatch(
    std::uint64_t zano_height,
    const Hash256& canonical_parent_hash) {
    std::size_t erased = 0;

    for (auto it = contexts_.begin(); it != contexts_.end();) {
        if (std::get<0>(it->first) != zano_height ||
            !it->second.zano_parent_hash.has_value() ||
            *it->second.zano_parent_hash == canonical_parent_hash) {
            ++it;
            continue;
        }

        it = contexts_.erase(it);
        ++erased;
    }

    return erased;
}

std::vector<std::uint64_t>
P2pTrustedWorkRegistry::provenance_zano_heights() const {
    std::vector<std::uint64_t> heights;

    for (const auto& [key, entry] : contexts_) {
        if (!entry.zano_parent_hash.has_value()) {
            continue;
        }

        const std::uint64_t height = std::get<0>(key);
        if (heights.empty() || heights.back() != height) {
            heights.push_back(height);
        }
    }

    return heights;
}

const Hash256* P2pTrustedWorkRegistry::find_zano_parent_hash(
    std::uint64_t zano_height,
    const Hash256& mining_header_hash,
    const ShareId& parent_id) const noexcept {
    const auto it = contexts_.find(
        Key{zano_height, mining_header_hash, parent_id});
    if (it == contexts_.end() ||
        !it->second.zano_parent_hash.has_value()) {
        return nullptr;
    }

    return &*it->second.zano_parent_hash;
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
    return receive_share(
        peer,
        parse_p2p_share_announce_envelope(envelope),
        kP2pCapabilityShareGossip,
        now,
        mode);
}

P2pShareReceiveResult P2pShareReceiver::receive_share(
    const P2pHandshake& peer,
    const Share& share,
    std::uint64_t required_capability,
    std::uint64_t now,
    ProgPowZContextMode mode) {
    P2pShareReceiveResult result;
    result.chain_result.id = share_id(share);

    if (required_capability == 0 ||
        (peer.capabilities & required_capability) != required_capability) {
        result.status = P2pShareReceiveStatus::CapabilityMissing;
        return result;
    }

    if (const ConnectedShare* connected =
            chain_.find(result.chain_result.id);
        connected != nullptr) {
        // On a production consensus chain an unchecked persistence replay is
        // structurally known but is not yet authoritative. Route it through
        // historical-work recovery instead of suppressing it as an ordinary
        // duplicate. Generic/test chains retain the historical duplicate
        // behavior because they have no consensus difficulty policy.
        if (chain_.enforces_sidechain_difficulty() &&
            !connected->validated_ancestry) {
            result.status =
                P2pShareReceiveStatus::UnknownWorkContext;
            return result;
        }

        result.status = P2pShareReceiveStatus::Duplicate;
        result.chain_result.disposition =
            ShareDisposition::Duplicate;
        return result;
    }

    if (chain_.is_orphan(result.chain_result.id)) {
        result.status = P2pShareReceiveStatus::Duplicate;
        result.chain_result.disposition =
            ShareDisposition::Duplicate;
        return result;
    }

    const ShareWorkContext* trusted = trusted_work_.find(
        share.zano_height,
        share.mining_header_hash,
        share.parent_id);
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
        result.missing_parent_id = share.parent_id;
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
