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
    try {
        P2pNodeMessageResult result;
        std::optional<P2pEnvelope> followup;
        std::optional<P2pEnvelope> relay_share;
        std::optional<P2pEnvelope> relay_tip;

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
            if (receive.status == P2pShareReceiveStatus::Connected) {
                relay_share = envelope;
                relay_tip = make_p2p_tip_announce_envelope(
                    p2p_tip_hint_from_chain(chain_));
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
                if (sync.share_result->status == P2pShareReceiveStatus::Connected) {
                    const P2pShareResponse response =
                        parse_p2p_share_response_envelope(envelope);
                    if (response.share.has_value()) {
                        relay_share = make_p2p_share_announce_envelope(
                            *response.share);
                        relay_tip = make_p2p_tip_announce_envelope(
                            p2p_tip_hint_from_chain(chain_));
                    }
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
            if (!local_mining_anchor_.has_value() ||
                (!expected_payout_.has_value() &&
                 !expected_payout_plan_.has_value())) {
                result.status = P2pNodeMessageStatus::MiningContextDeferred;
                break;
            }

            P2pMiningContextTrustResult trust;
            if (expected_payout_plan_.has_value()) {
                trust = promote_p2p_mining_context(
                    trusted_work_,
                    peer,
                    envelope,
                    *local_mining_anchor_,
                    *expected_payout_plan_);
            } else {
                trust = promote_p2p_mining_context(
                    trusted_work_,
                    peer,
                    envelope,
                    *local_mining_anchor_,
                    *expected_payout_);
            }
            result.status = P2pNodeMessageStatus::MiningContextProcessed;
            result.mining_context_status = trust.status;
            result.mining_context_registry_inserted = trust.registry_inserted;
            break;
        }
        case P2pMessageType::Handshake:
            // Handshakes are consumed by P2pTcpConnection before a runtime peer is
            // established; another handshake on an established session is a
            // scoreable node-protocol violation.
            result.status = P2pNodeMessageStatus::UnexpectedHandshake;
            break;
        }

        const std::uint32_t penalty = p2p_node_message_penalty(result);
        if (penalty != 0) {
            runtime.report_peer_misbehavior(peer.node_id, penalty);
        }

        // Network I/O deliberately happens after the node-state critical section.
        // Freshly connected shares are forwarded to every peer except the source.
        // Duplicate/orphan/rejected shares never fan out, which provides natural
        // loop suppression without trusting peer-provided seen sets.
        if (followup.has_value()) {
            result.sent_followup = runtime.send_to(peer.node_id, *followup);
        }
        if (relay_share.has_value()) {
            runtime.broadcast_except(peer.node_id, *relay_share);
            result.relayed_share = true;
        }
        if (relay_tip.has_value()) {
            runtime.broadcast_except(peer.node_id, *relay_tip);
            result.relayed_tip = true;
        }
        return result;
    } catch (...) {
        // Generic exceptions are deliberately reputation-neutral. Existing
        // parsers and validation paths use broad exception types for both
        // peer-attributable malformed input and local/internal failures; until
        // those failures are typed explicitly, charging here risks banning a
        // peer for a local fault.
        throw;
    }
}

std::uint32_t p2p_node_message_penalty(
    const P2pNodeMessageResult& result) noexcept {
    switch (result.status) {
    case P2pNodeMessageStatus::ShareProcessed:
        // Rejected is intentionally neutral because it can represent a local
        // inability to validate (for example an unavailable exact PoW backend),
        // not necessarily malicious peer behavior.
        return result.share_status == P2pShareReceiveStatus::CapabilityMissing
                   ? kP2pProtocolViolationPenalty
                   : 0;

    case P2pNodeMessageStatus::ShareRequestAnswered:
        return 0;

    case P2pNodeMessageStatus::ShareResponseProcessed:
        if (result.sync_status == P2pShareSyncReceiveStatus::CapabilityMissing) {
            return kP2pProtocolViolationPenalty;
        }
        if (result.sync_status == P2pShareSyncReceiveStatus::ShareProcessed &&
            result.share_status == P2pShareReceiveStatus::CapabilityMissing) {
            return kP2pProtocolViolationPenalty;
        }
        return 0;

    case P2pNodeMessageStatus::TipProcessed:
        return (result.tip_status == P2pTipSyncStatus::HeightMismatch ||
                result.tip_status == P2pTipSyncStatus::CapabilityMissing)
                   ? kP2pProtocolViolationPenalty
                   : 0;

    case P2pNodeMessageStatus::MiningContextProcessed:
        return (result.mining_context_status ==
                    P2pMiningContextTrustStatus::CapabilityMissing ||
                result.mining_context_status ==
                    P2pMiningContextTrustStatus::ProofsRejected)
                   ? kP2pProtocolViolationPenalty
                   : 0;

    case P2pNodeMessageStatus::MiningContextDeferred:
        return 0;

    case P2pNodeMessageStatus::UnexpectedHandshake:
        return kP2pProtocolViolationPenalty;
    }
    return 0;
}

void P2pNodeProtocol::remember_trusted_work(
    const ShareWorkContext& context) {
    std::lock_guard lock(state_mutex_);
    trusted_work_.remember(context);
}

void P2pNodeProtocol::set_local_mining_context(
    const P2pMiningAnchor& anchor,
    const P2pMiningContextProposal& proposal) {
    std::lock_guard lock(state_mutex_);
    local_mining_anchor_ = anchor;
    local_mining_context_ = proposal;
}

void P2pNodeProtocol::set_expected_payout(
    const P2pPayoutAddress& payout) {
    std::lock_guard lock(state_mutex_);
    expected_payout_ = payout;
    expected_payout_plan_.reset();
}

void P2pNodeProtocol::set_expected_payout_plan(
    const PplnsCoinbasePlan& plan) {
    std::lock_guard lock(state_mutex_);
    expected_payout_plan_ = plan;
    expected_payout_.reset();
}

void P2pNodeProtocol::clear_expected_payout() noexcept {
    std::lock_guard lock(state_mutex_);
    expected_payout_.reset();
    expected_payout_plan_.reset();
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
    return local_mining_anchor_.has_value() &&
           local_mining_context_.has_value() &&
           (expected_payout_.has_value() || expected_payout_plan_.has_value());
}

std::optional<P2pEnvelope>
P2pNodeProtocol::local_mining_context_envelope() const {
    std::lock_guard lock(state_mutex_);
    if (!local_mining_context_.has_value()) {
        return std::nullopt;
    }
    return make_p2p_mining_context_envelope(*local_mining_context_);
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
