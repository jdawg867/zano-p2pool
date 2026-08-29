#include "zano_p2pool/p2p_node.hpp"

#include <optional>

namespace zano_p2pool {

P2pNodeProtocol::P2pNodeProtocol(
    ShareChain& chain,
    P2pTrustedWorkRegistry& trusted_work,
    std::mutex& state_mutex) noexcept
    : chain_(chain),
      trusted_work_(trusted_work),
      state_mutex_(state_mutex) {}

P2pNodeMessageResult P2pNodeProtocol::handle(
    P2pRuntime& runtime,
    const P2pHandshake& peer,
    const P2pEnvelope& envelope,
    std::uint64_t now,
    ProgPowZContextMode mode) {
    P2pNodeMessageResult result;
    std::optional<P2pEnvelope> followup;

    switch (envelope.type) {
    case P2pMessageType::ShareAnnounce: {
        std::lock_guard lock(state_mutex_);
        P2pShareReceiver receiver(chain_, trusted_work_);
        const P2pShareReceiveResult receive =
            receiver.receive(peer, envelope, now, mode);
        result.status = P2pNodeMessageStatus::ShareProcessed;
        result.share_status = receive.status;
        if (receive.missing_parent_id.has_value()) {
            followup = make_p2p_share_request_envelope(
                *receive.missing_parent_id);
        }
        break;
    }
    case P2pMessageType::ShareRequest: {
        std::lock_guard lock(state_mutex_);
        followup = answer_p2p_share_request(peer, envelope, chain_);
        result.status = P2pNodeMessageStatus::ShareRequestAnswered;
        break;
    }
    case P2pMessageType::ShareResponse: {
        std::lock_guard lock(state_mutex_);
        P2pShareReceiver receiver(chain_, trusted_work_);
        const P2pShareSyncReceiveResult sync =
            receive_p2p_share_response(receiver, peer, envelope, now, mode);
        result.status = P2pNodeMessageStatus::ShareResponseProcessed;
        result.sync_status = sync.status;
        if (sync.share_result.has_value()) {
            result.share_status = sync.share_result->status;
            if (sync.share_result->missing_parent_id.has_value()) {
                followup = make_p2p_share_request_envelope(
                    *sync.share_result->missing_parent_id);
            }
        }
        break;
    }
    case P2pMessageType::TipAnnounce: {
        std::lock_guard lock(state_mutex_);
        const P2pTipHint hint = parse_p2p_tip_announce_envelope(envelope);
        const P2pTipSyncDecision decision =
            plan_p2p_tip_sync(peer, hint, chain_);
        result.status = P2pNodeMessageStatus::TipProcessed;
        result.tip_status = decision.status;
        if (decision.requested_id.has_value()) {
            followup = make_p2p_share_request_envelope(*decision.requested_id);
        }
        break;
    }
    case P2pMessageType::MiningContextAnnounce: {
        std::lock_guard lock(state_mutex_);
        if (!local_mining_anchor_.has_value() || !expected_payout_.has_value()) {
            result.status = P2pNodeMessageStatus::MiningContextDeferred;
            return result;
        }

        const P2pMiningContextTrustResult trust =
            promote_p2p_mining_context(
                trusted_work_,
                peer,
                envelope,
                *local_mining_anchor_,
                *expected_payout_);
        result.status = P2pNodeMessageStatus::MiningContextProcessed;
        result.mining_context_status = trust.status;
        result.mining_context_registry_inserted = trust.registry_inserted;
        return result;
    }
    case P2pMessageType::Handshake:
        // Handshakes are consumed by P2pTcpConnection before a runtime peer is
        // established; another handshake on an established session is not a
        // node-protocol message.
        result.status = P2pNodeMessageStatus::UnexpectedHandshake;
        return result;
    }

    if (followup.has_value()) {
        result.sent_followup = runtime.send_to(peer.node_id, *followup);
    }
    return result;
}

void P2pNodeProtocol::remember_trusted_work(
    const ShareWorkContext& context) {
    std::lock_guard lock(state_mutex_);
    trusted_work_.remember(context);
}

void P2pNodeProtocol::set_local_mining_anchor(
    const P2pMiningAnchor& anchor) {
    std::lock_guard lock(state_mutex_);
    local_mining_anchor_ = anchor;
}

void P2pNodeProtocol::set_expected_payout(
    const P2pPayoutAddress& payout) {
    std::lock_guard lock(state_mutex_);
    expected_payout_ = payout;
}

void P2pNodeProtocol::clear_expected_payout() noexcept {
    std::lock_guard lock(state_mutex_);
    expected_payout_.reset();
}

std::size_t P2pNodeProtocol::trusted_work_count() const noexcept {
    std::lock_guard lock(state_mutex_);
    return trusted_work_.size();
}

std::size_t P2pNodeProtocol::connected_share_count() const noexcept {
    std::lock_guard lock(state_mutex_);
    return chain_.connected_size();
}

P2pTipHint P2pNodeProtocol::local_tip() const noexcept {
    std::lock_guard lock(state_mutex_);
    return p2p_tip_hint_from_chain(chain_);
}

bool P2pNodeProtocol::mining_context_trust_ready() const noexcept {
    std::lock_guard lock(state_mutex_);
    return local_mining_anchor_.has_value() && expected_payout_.has_value();
}

const char* p2p_node_message_status_name(
    P2pNodeMessageStatus status) noexcept {
    switch (status) {
    case P2pNodeMessageStatus::ShareProcessed:
        return "share-processed";
    case P2pNodeMessageStatus::ShareRequestAnswered:
        return "share-request-answered";
    case P2pNodeMessageStatus::ShareResponseProcessed:
        return "share-response-processed";
    case P2pNodeMessageStatus::TipProcessed:
        return "tip-processed";
    case P2pNodeMessageStatus::MiningContextProcessed:
        return "mining-context-processed";
    case P2pNodeMessageStatus::MiningContextDeferred:
        return "mining-context-deferred";
    case P2pNodeMessageStatus::UnexpectedHandshake:
        return "unexpected-handshake";
    }
    return "unknown";
}

}  // namespace zano_p2pool
