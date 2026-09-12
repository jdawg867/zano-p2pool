#include "zano_p2pool/p2p_node.hpp"

#include <optional>
#include <stdexcept>

namespace zano_p2pool {

P2pNodeProtocol::P2pNodeProtocol(
    ShareChain& chain,
    P2pTrustedWorkRegistry& trusted_work,
    std::mutex& state_mutex) noexcept
    : chain_(chain),
      trusted_work_(trusted_work),
      state_mutex_(state_mutex) {}

bool P2pNodeProtocol::historical_trust_sources_ready_unlocked() const noexcept {
    return historical_params_.has_value() &&
           static_cast<bool>(load_historical_observations_) &&
           static_cast<bool>(historical_parent_lookup_);
}

void P2pNodeProtocol::expire_pending_historical(std::uint64_t now) {
    for (auto it = pending_historical_.begin();
         it != pending_historical_.end();) {
        if (now < it->second.started ||
            now - it->second.started >= kMiningWorkRequestLifetime) {
            it = pending_historical_.erase(it);
        } else {
            ++it;
        }
    }
}

void P2pNodeProtocol::remember_pending_historical(
    const P2pHandshake& peer,
    const Share& share,
    std::uint64_t required_capability,
    std::uint64_t now) {
    expire_pending_historical(now);
    if (pending_historical_.size() >= kMiningWorkMaxPending) {
        return;
    }

    const MiningWorkKey work_key{
        share.zano_height,
        share.mining_header_hash,
    };
    pending_historical_.try_emplace(
        PendingHistoricalKey{peer.node_id, work_key},
        PendingHistoricalCandidate{
            share,
            required_capability,
            now,
        });
}

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
            if (receive.status == P2pShareReceiveStatus::UnknownWorkContext &&
                work_retrieval_) {
                const auto share =
                    parse_p2p_share_announce_envelope(envelope);
                auto work_request = work_retrieval_->begin(
                    peer,
                    {share.zano_height, share.mining_header_hash},
                    now);
                if (work_request.has_value()) {
                    followup = std::move(work_request);
                    if (historical_trust_sources_ready_unlocked()) {
                        remember_pending_historical(
                            peer,
                            share,
                            kP2pCapabilityShareGossip,
                            now);
                    }
                }
            }
            if (receive.chain_result.best_tip_changed) {
                expected_payout_.reset();
                expected_payout_plan_.reset();
                expected_payout_parent_id_.reset();
            }
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
                if (result.share_status ==
                        P2pShareReceiveStatus::UnknownWorkContext &&
                    work_retrieval_) {
                    const auto response =
                        parse_p2p_share_response_envelope(envelope);
                    if (response.share.has_value()) {
                        auto work_request = work_retrieval_->begin(
                            peer,
                            {
                                response.share->zano_height,
                                response.share->mining_header_hash,
                            },
                            now);
                        if (work_request.has_value()) {
                            followup = std::move(work_request);
                            if (historical_trust_sources_ready_unlocked()) {
                                remember_pending_historical(
                                    peer,
                                    *response.share,
                                    kP2pCapabilityShareSync,
                                    now);
                            }
                        }
                    }
                }
                if (sync.share_result->chain_result.best_tip_changed) {
                    expected_payout_.reset();
                    expected_payout_plan_.reset();
                    expected_payout_parent_id_.reset();
                }
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
                 !expected_payout_plan_.has_value()) ||
                !expected_payout_parent_id_.has_value()) {
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
                    *expected_payout_parent_id_,
                    *expected_payout_plan_);
            } else {
                trust = promote_p2p_mining_context(
                    trusted_work_,
                    peer,
                    envelope,
                    *local_mining_anchor_,
                    *expected_payout_parent_id_,
                    *expected_payout_);
            }
            result.status = P2pNodeMessageStatus::MiningContextProcessed;
            result.mining_context_status = trust.status;
            result.mining_context_registry_inserted = trust.registry_inserted;
            break;
        }
        case P2pMessageType::MiningWorkRequest: {
            if (!work_retrieval_) throw std::runtime_error("work retrieval is disabled");
            followup = work_retrieval_->answer(peer, envelope);
            result.status = P2pNodeMessageStatus::MiningWorkRequestAnswered;
            break;
        }
        case P2pMessageType::MiningWorkResponse: {
            if (!work_retrieval_) {
                throw std::runtime_error("work retrieval is disabled");
            }

            const MiningWorkResponse response =
                parse_mining_work_response(envelope);
            const MiningWorkKey work_key = response.request.key;
            MiningWorkReceiveResult received;
            try {
                received = work_retrieval_->receive(peer, envelope, now);
            } catch (...) {
                std::lock_guard lock(state_mutex_);
                pending_historical_.erase(
                    PendingHistoricalKey{peer.node_id, work_key});
                throw;
            }

            followup = received.followup;
            result.untrusted_work_received = received.received_id;
            result.status = P2pNodeMessageStatus::MiningWorkResponseProcessed;

            if (response.total_size == 0) {
                std::lock_guard lock(state_mutex_);
                pending_historical_.erase(
                    PendingHistoricalKey{peer.node_id, work_key});
                break;
            }
            if (!received.received_id.has_value()) {
                break;
            }

            std::optional<SidechainParameters> historical_params;
            std::function<std::vector<P2pMiningAnchor>()>
                load_observations;
            std::function<RpcCanonicalHeader(std::uint64_t)>
                parent_lookup;
            std::optional<PendingHistoricalCandidate> candidate;
            std::optional<std::vector<std::uint8_t>> work_bytes;

            {
                std::lock_guard lock(state_mutex_);
                expire_pending_historical(now);
                const auto pending_it = pending_historical_.find(
                    PendingHistoricalKey{peer.node_id, work_key});
                if (pending_it != pending_historical_.end() &&
                    historical_trust_sources_ready_unlocked()) {
                    historical_params = historical_params_;
                    load_observations =
                        load_historical_observations_;
                    parent_lookup = historical_parent_lookup_;
                    candidate = pending_it->second;
                    work_bytes =
                        work_retrieval_->take_untrusted(work_key);
                    pending_historical_.erase(pending_it);
                }
            }

            // If no exact waiting share is bound to this response, preserve
            // the structurally checked bytes in P2pWorkRetrieval's untrusted
            // cache for diagnostics/manual handling.
            if (!candidate.has_value()) {
                break;
            }
            if (!work_bytes.has_value()) {
                throw std::runtime_error(
                    "completed historical work is missing from untrusted cache");
            }

            const P2pMiningContextProposal proposal =
                deserialize_p2p_mining_context_payload(*work_bytes);
            const std::vector<P2pMiningAnchor> observations =
                load_observations();

            {
                // ShareChain and P2pTrustedWorkRegistry are runtime consensus
                // state. Keep them stable across payout reconstruction,
                // daemon rechecks, trusted-work insertion, and the exact-share
                // retry. The RPC callback can block, but correctness takes
                // precedence over concurrent admission at this trust boundary.
                std::lock_guard lock(state_mutex_);
                const HistoricalTrustResult trust =
                    promote_historical_mining_context(
                        trusted_work_,
                        chain_,
                        *historical_params,
                        candidate->share,
                        peer,
                        proposal,
                        observations,
                        parent_lookup);
                result.historical_trust_status = trust.status;

                if (trust.status != HistoricalTrustStatus::Trusted) {
                    break;
                }

                P2pShareReceiver receiver(chain_, trusted_work_);
                const P2pShareReceiveResult retried =
                    receiver.receive_share(
                        peer,
                        candidate->share,
                        candidate->required_capability,
                        now,
                        mode);
                result.historical_share = candidate->share;
                result.historical_share_retried = true;
                result.share_status = retried.status;

                if (retried.chain_result.best_tip_changed) {
                    expected_payout_.reset();
                    expected_payout_plan_.reset();
                    expected_payout_parent_id_.reset();
                }
                if (retried.missing_parent_id.has_value()) {
                    followup = make_p2p_share_request_envelope(
                        *retried.missing_parent_id);
                }
                if (retried.status == P2pShareReceiveStatus::Connected) {
                    relay_share = make_p2p_share_announce_envelope(
                        candidate->share);
                    relay_tip = make_p2p_tip_announce_envelope(
                        p2p_tip_hint_from_chain(chain_));
                }
            }
            break;
        }
        case P2pMessageType::Handshake:
            result.status = P2pNodeMessageStatus::UnexpectedHandshake;
            break;
        }

        const std::uint32_t penalty = p2p_node_message_penalty(result);
        if (penalty != 0) {
            runtime.report_peer_misbehavior(peer.node_id, penalty);
        }

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
        throw;
    }
}

std::uint32_t p2p_node_message_penalty(
    const P2pNodeMessageResult& result) noexcept {
    switch (result.status) {
    case P2pNodeMessageStatus::ShareProcessed:
        return result.share_status == P2pShareReceiveStatus::CapabilityMissing
                   ? kP2pProtocolViolationPenalty
                   : 0;
    case P2pNodeMessageStatus::ShareRequestAnswered:
    case P2pNodeMessageStatus::MiningWorkRequestAnswered:
    case P2pNodeMessageStatus::MiningWorkResponseProcessed:
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

void P2pNodeProtocol::set_historical_trust_sources(
    const SidechainParameters& params,
    std::function<std::vector<P2pMiningAnchor>()> load_local_observations,
    std::function<RpcCanonicalHeader(std::uint64_t)> lookup) {
    if (!load_local_observations || !lookup) {
        throw std::invalid_argument(
            "historical trust sources must be callable");
    }

    std::lock_guard lock(state_mutex_);
    if (!chain_.matches_sidechain_parameters(params)) {
        throw std::runtime_error(
            "historical trust sidechain parameters do not match chain");
    }
    historical_params_ = params;
    load_historical_observations_ =
        std::move(load_local_observations);
    historical_parent_lookup_ = std::move(lookup);
}

void P2pNodeProtocol::remember_trusted_work(
    const ShareWorkContext& context) {
    std::lock_guard lock(state_mutex_);
    const ConnectedShare* parent = chain_.best_tip();
    trusted_work_.remember(
        context, parent == nullptr ? ShareId{} : parent->id);
}

void P2pNodeProtocol::set_local_mining_context(
    const P2pMiningAnchor& anchor,
    const P2pMiningContextProposal& proposal) {
    std::lock_guard lock(state_mutex_);
    local_mining_anchor_ = anchor;
    local_mining_context_ = proposal;
}

void P2pNodeProtocol::set_local_mining_context(
    const P2pMiningAnchor& anchor,
    const P2pMiningContextProposal& proposal,
    const P2pPayoutAddress& payout) {
    std::lock_guard lock(state_mutex_);
    local_mining_anchor_ = anchor;
    local_mining_context_ = proposal;
    expected_payout_ = payout;
    expected_payout_plan_.reset();
    const ConnectedShare* parent = chain_.best_tip();
    expected_payout_parent_id_ = parent == nullptr ? ShareId{} : parent->id;
}

void P2pNodeProtocol::set_local_mining_context(
    const P2pMiningAnchor& anchor,
    const P2pMiningContextProposal& proposal,
    const PplnsCoinbasePlan& plan) {
    std::lock_guard lock(state_mutex_);
    local_mining_anchor_ = anchor;
    local_mining_context_ = proposal;
    expected_payout_plan_ = plan;
    expected_payout_.reset();
    const ConnectedShare* parent = chain_.best_tip();
    expected_payout_parent_id_ = parent == nullptr ? ShareId{} : parent->id;
}

void P2pNodeProtocol::set_expected_payout(
    const P2pPayoutAddress& payout) {
    std::lock_guard lock(state_mutex_);
    expected_payout_ = payout;
    expected_payout_plan_.reset();
    const ConnectedShare* parent = chain_.best_tip();
    expected_payout_parent_id_ = parent == nullptr ? ShareId{} : parent->id;
}

void P2pNodeProtocol::set_expected_payout_plan(
    const PplnsCoinbasePlan& plan) {
    std::lock_guard lock(state_mutex_);
    expected_payout_plan_ = plan;
    expected_payout_.reset();
    const ConnectedShare* parent = chain_.best_tip();
    expected_payout_parent_id_ = parent == nullptr ? ShareId{} : parent->id;
}

void P2pNodeProtocol::clear_expected_payout() noexcept {
    std::lock_guard lock(state_mutex_);
    expected_payout_.reset();
    expected_payout_plan_.reset();
    expected_payout_parent_id_.reset();
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
           (expected_payout_.has_value() || expected_payout_plan_.has_value()) &&
           expected_payout_parent_id_.has_value();
}

std::optional<P2pEnvelope>
P2pNodeProtocol::local_mining_context_envelope() const {
    std::lock_guard lock(state_mutex_);
    if (!local_mining_context_.has_value() ||
        (!expected_payout_.has_value() && !expected_payout_plan_.has_value()) ||
        !expected_payout_parent_id_.has_value()) {
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
    case P2pNodeMessageStatus::MiningWorkRequestAnswered:
        return "mining-work-request-answered";
    case P2pNodeMessageStatus::MiningWorkResponseProcessed:
        return "mining-work-response-processed";
    case P2pNodeMessageStatus::UnexpectedHandshake:
        return "unexpected-handshake";
    }
    return "unknown";
}

}  // namespace zano_p2pool
