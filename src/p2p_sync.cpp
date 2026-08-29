#include "zano_p2pool/p2p_sync.hpp"

#include <algorithm>
#include <stdexcept>

namespace zano_p2pool {
namespace {

void require_envelope_type(
    const P2pEnvelope& envelope,
    P2pMessageType expected_type,
    const char* message) {
    if (envelope.version != kP2pProtocolVersion) {
        throw std::runtime_error("unsupported P2P protocol version");
    }
    if (envelope.type != expected_type) {
        throw std::runtime_error(message);
    }
    if (envelope.flags != 0) {
        throw std::runtime_error("unsupported P2P envelope flags");
    }
}

}  // namespace

P2pEnvelope make_p2p_share_request_envelope(const ShareId& requested_id) {
    if (is_zero_share_id(requested_id)) {
        throw std::runtime_error("cannot request zero P2P share id");
    }

    P2pEnvelope envelope;
    envelope.type = P2pMessageType::ShareRequest;
    envelope.payload.assign(requested_id.begin(), requested_id.end());
    return envelope;
}

ShareId parse_p2p_share_request_envelope(const P2pEnvelope& envelope) {
    require_envelope_type(
        envelope,
        P2pMessageType::ShareRequest,
        "P2P envelope is not a share request");
    if (envelope.payload.size() != kP2pShareRequestPayloadSize) {
        throw std::runtime_error("invalid P2P share request payload size");
    }

    ShareId requested_id{};
    std::copy(envelope.payload.begin(), envelope.payload.end(), requested_id.begin());
    if (is_zero_share_id(requested_id)) {
        throw std::runtime_error("cannot request zero P2P share id");
    }
    return requested_id;
}

P2pEnvelope make_p2p_share_response_envelope(
    const ShareId& requested_id,
    const Share* share) {
    if (is_zero_share_id(requested_id)) {
        throw std::runtime_error("cannot respond for zero P2P share id");
    }

    P2pEnvelope envelope;
    envelope.type = P2pMessageType::ShareResponse;

    if (share == nullptr) {
        envelope.payload.reserve(kP2pShareResponseHeaderSize);
        envelope.payload.push_back(
            static_cast<std::uint8_t>(P2pShareResponseCode::NotFound));
        envelope.payload.insert(
            envelope.payload.end(), requested_id.begin(), requested_id.end());
        return envelope;
    }

    if (share_id(*share) != requested_id) {
        throw std::runtime_error("P2P share response does not match requested id");
    }

    const auto serialized = serialize_share(*share);
    envelope.payload.reserve(kP2pShareResponseHeaderSize + serialized.size());
    envelope.payload.push_back(
        static_cast<std::uint8_t>(P2pShareResponseCode::Found));
    envelope.payload.insert(
        envelope.payload.end(), requested_id.begin(), requested_id.end());
    envelope.payload.insert(
        envelope.payload.end(), serialized.begin(), serialized.end());
    return envelope;
}

P2pShareResponse parse_p2p_share_response_envelope(
    const P2pEnvelope& envelope) {
    require_envelope_type(
        envelope,
        P2pMessageType::ShareResponse,
        "P2P envelope is not a share response");
    if (envelope.payload.size() < kP2pShareResponseHeaderSize) {
        throw std::runtime_error("truncated P2P share response payload");
    }

    P2pShareResponse response;
    const std::uint8_t code = envelope.payload[0];
    std::copy_n(
        envelope.payload.begin() + 1,
        response.requested_id.size(),
        response.requested_id.begin());
    if (is_zero_share_id(response.requested_id)) {
        throw std::runtime_error("P2P share response has zero requested id");
    }

    if (code == static_cast<std::uint8_t>(P2pShareResponseCode::NotFound)) {
        if (envelope.payload.size() != kP2pShareResponseHeaderSize) {
            throw std::runtime_error("invalid not-found P2P share response size");
        }
        response.code = P2pShareResponseCode::NotFound;
        return response;
    }

    if (code != static_cast<std::uint8_t>(P2pShareResponseCode::Found)) {
        throw std::runtime_error("unsupported P2P share response status");
    }

    const std::size_t serialized_size =
        envelope.payload.size() - kP2pShareResponseHeaderSize;
    if (serialized_size != kShareV1SerializedSize &&
        serialized_size != kShareV2SerializedSize) {
        throw std::runtime_error("invalid found P2P share response size");
    }

    response.code = P2pShareResponseCode::Found;
    response.share = deserialize_share(std::span<const std::uint8_t>(
        envelope.payload.data() + kP2pShareResponseHeaderSize,
        serialized_size));
    if (share_id(*response.share) != response.requested_id) {
        throw std::runtime_error("P2P share response id mismatch");
    }
    return response;
}

P2pEnvelope answer_p2p_share_request(
    const P2pHandshake& peer,
    const P2pEnvelope& request,
    const ShareChain& chain) {
    if ((peer.capabilities & kP2pCapabilityShareSync) == 0) {
        throw std::runtime_error("P2P peer lacks share-sync capability");
    }

    const ShareId requested_id = parse_p2p_share_request_envelope(request);
    const ConnectedShare* connected = chain.find(requested_id);
    return make_p2p_share_response_envelope(
        requested_id,
        connected == nullptr ? nullptr : &connected->share);
}

P2pShareSyncReceiveResult receive_p2p_share_response(
    P2pShareReceiver& receiver,
    const P2pHandshake& peer,
    const P2pEnvelope& response_envelope,
    std::uint64_t now,
    ProgPowZContextMode mode) {
    const P2pShareResponse response =
        parse_p2p_share_response_envelope(response_envelope);

    P2pShareSyncReceiveResult result;
    result.requested_id = response.requested_id;

    if ((peer.capabilities & kP2pCapabilityShareSync) == 0) {
        result.status = P2pShareSyncReceiveStatus::CapabilityMissing;
        return result;
    }

    if (!response.share.has_value()) {
        result.status = P2pShareSyncReceiveStatus::NotFound;
        return result;
    }

    result.status = P2pShareSyncReceiveStatus::ShareProcessed;
    result.share_result = receiver.receive_share(
        peer,
        *response.share,
        kP2pCapabilityShareSync,
        now,
        mode);
    return result;
}

const char* p2p_share_sync_receive_status_name(
    P2pShareSyncReceiveStatus status) noexcept {
    switch (status) {
    case P2pShareSyncReceiveStatus::ShareProcessed:
        return "share-processed";
    case P2pShareSyncReceiveStatus::NotFound:
        return "not-found";
    case P2pShareSyncReceiveStatus::CapabilityMissing:
        return "capability-missing";
    }
    return "unknown";
}

}  // namespace zano_p2pool
