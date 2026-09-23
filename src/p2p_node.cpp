#include "zano_p2pool/p2p_node.hpp"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <vector>

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
           static_cast<bool>(historical_parent_lookup_) &&
           static_cast<bool>(historical_pow_lookup_);
}

void P2pNodeProtocol::expire_historical_state(std::uint64_t now) {
    for (auto it = pending_historical_.begin();
         it != pending_historical_.end();) {
        if (now < it->second.started ||
            now - it->second.started >= kMiningWorkRequestLifetime) {
            it = pending_historical_.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = historical_evidence_.begin();
         it != historical_evidence_.end();) {
        if (now < it->second.received ||
            now - it->second.received >= kMiningWorkRequestLifetime) {
            it = historical_evidence_.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = deferred_historical_.begin();
         it != deferred_historical_.end();) {
        if (now < it->second.last_progress ||
            now - it->second.last_progress >=
                kMiningWorkRequestLifetime) {
            it = deferred_historical_.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = retryable_historical_.begin();
         it != retryable_historical_.end();) {
        if (now < it->second.started ||
            now - it->second.started >= kMiningWorkRequestLifetime) {
            it = retryable_historical_.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = replay_frontier_attempts_.begin();
         it != replay_frontier_attempts_.end();) {
        if (now < it->second ||
            now - it->second >= kMiningWorkRequestLifetime) {
            it = replay_frontier_attempts_.erase(it);
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
    expire_historical_state(now);
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

void P2pNodeProtocol::remember_historical_evidence(
    const MiningWorkKey& key,
    HistoricalEvidence evidence,
    std::uint64_t now) {
    expire_historical_state(now);

    const auto existing =
        historical_evidence_.find(key);
    if (existing != historical_evidence_.end()) {
        // Reuse never renews the lifetime of peer-supplied evidence. The
        // bounded cache expires relative to the first retained receipt.
        evidence.received = existing->second.received;
    } else {
        evidence.received = now;
    }

    if (existing == historical_evidence_.end() &&
        historical_evidence_.size() >= kMiningWorkMaxReceived) {
        historical_evidence_.erase(historical_evidence_.begin());
    }
    historical_evidence_.insert_or_assign(key, std::move(evidence));
}

bool P2pNodeProtocol::remember_deferred_historical(
    const Share& share,
    const P2pHandshake& candidate_peer,
    const HistoricalEvidence& evidence,
    std::uint64_t required_capability,
    std::uint64_t now) {
    expire_historical_state(now);
    if ((candidate_peer.capabilities & kP2pCapabilityShareSync) == 0) {
        return false;
    }

    const ShareId id = share_id(share);

    // Historical recovery walks ancestry sequentially and is not equivalent to
    // concurrent mining-work pressure. Bound the deferred ancestry path by the
    // locally configured consensus history horizon rather than the much smaller
    // network-request concurrency limit. If no validated prefix is reachable
    // within that horizon, recovery remains fail-closed.
    if (!historical_params_.has_value()) {
        return false;
    }
    const std::size_t max_deferred =
        static_cast<std::size_t>(
            historical_params_->difficulty_window_shares);
    if (!deferred_historical_.contains(id) &&
        deferred_historical_.size() >= max_deferred) {
        return false;
    }

    // A newly audited parent is positive progress for only the recovery walk
    // whose deferred child points directly at it. Carry the original recovery
    // root backward and refresh every still-live member of that exact walk.
    //
    // This deliberately occurs after expire_historical_state(): a session that
    // has already been inactive for the full lifetime is not resurrected by a
    // later message.
    ShareId recovery_root = id;
    std::vector<ShareId> joined_roots;

    for (const auto& [deferred_id, deferred] :
         deferred_historical_) {
        static_cast<void>(deferred_id);

        if (deferred.candidate_peer.node_id ==
                candidate_peer.node_id &&
            deferred.share.parent_id == id) {
            joined_roots.push_back(
                deferred.recovery_root);
        }
    }

    if (!joined_roots.empty()) {
        recovery_root = joined_roots.front();

        for (auto& [deferred_id, deferred] :
             deferred_historical_) {
            static_cast<void>(deferred_id);

            if (deferred.candidate_peer.node_id !=
                candidate_peer.node_id) {
                continue;
            }

            for (const ShareId& joined_root :
                 joined_roots) {
                if (deferred.recovery_root !=
                    joined_root) {
                    continue;
                }

                // If two deferred branches converge on the same exact parent,
                // merge them into one recovery session from this point back.
                deferred.recovery_root =
                    recovery_root;
                deferred.last_progress = now;
                break;
            }
        }
    }

    deferred_historical_.insert_or_assign(
        id,
        DeferredHistoricalCandidate{
            share,
            candidate_peer,
            evidence,
            required_capability,
            recovery_root,
            now,
        });
    return true;
}

void P2pNodeProtocol::remember_retryable_historical(
    const Share& share,
    const P2pHandshake& candidate_peer,
    std::uint64_t required_capability,
    std::uint64_t now) {
    expire_historical_state(now);

    if (required_capability != kP2pCapabilityShareGossip &&
        required_capability != kP2pCapabilityShareSync) {
        throw std::logic_error(
            "unsupported historical retry capability");
    }

    const ShareId id = share_id(share);

    const auto existing =
        retryable_historical_.find(id);
    const std::uint64_t started =
        existing != retryable_historical_.end()
            ? existing->second.started
            : now;

    if (existing == retryable_historical_.end() &&
        retryable_historical_.size() >= kMiningWorkMaxReceived) {
        auto oldest = retryable_historical_.begin();
        for (auto it = retryable_historical_.begin();
             it != retryable_historical_.end();
             ++it) {
            if (it->second.started < oldest->second.started) {
                oldest = it;
            }
        }
        retryable_historical_.erase(oldest);
    }

    retryable_historical_.insert_or_assign(
        id,
        RetryableHistoricalCandidate{
            share,
            candidate_peer,
            required_capability,
            started,
        });
}

P2pHistoricalRetrySummary
P2pNodeProtocol::retry_historical_pow_unavailable(
    P2pRuntime& runtime,
    std::uint64_t now,
    ProgPowZContextMode mode) {

    std::vector<RetryableHistoricalCandidate> retry;

    {
        std::lock_guard lock(state_mutex_);
        expire_historical_state(now);

        retry.reserve(retryable_historical_.size());
        for (const auto& [candidate_id, candidate] :
             retryable_historical_) {
            static_cast<void>(candidate_id);
            retry.push_back(candidate);
        }

        // Leave registrations in place while processing the snapshot.
        // attempt_historical() consumes the exact ShareId when its full trust
        // crossing begins and re-registers it only when canonical PoW context
        // remains temporarily unavailable. This also means an exception cannot
        // silently discard candidates that have not been attempted yet.
    }

    P2pHistoricalRetrySummary summary;
    summary.attempted = retry.size();

    for (const RetryableHistoricalCandidate& candidate : retry) {
        P2pEnvelope envelope;

        if (candidate.required_capability ==
            kP2pCapabilityShareGossip) {
            envelope =
                make_p2p_share_announce_envelope(candidate.share);
        } else if (
            candidate.required_capability ==
            kP2pCapabilityShareSync) {
            const ShareId requested_id =
                share_id(candidate.share);
            envelope =
                make_p2p_share_response_envelope(
                    requested_id,
                    &candidate.share);
        } else {
            throw std::logic_error(
                "unsupported historical retry capability");
        }

        P2pNodeMessageResult result;
        try {
            result =
                handle(
                    runtime,
                    candidate.candidate_peer,
                    envelope,
                    now,
                    mode);
        } catch (...) {
            // attempt_historical() removes the registration immediately before
            // crossing local trust. Restore the original bounded candidate if
            // an exceptional local/RPC failure interrupted that crossing.
            std::lock_guard lock(state_mutex_);
            if (now >= candidate.started &&
                now - candidate.started <
                    kMiningWorkRequestLifetime &&
                !retryable_historical_.contains(
                    share_id(candidate.share))) {
                retryable_historical_.insert_or_assign(
                    share_id(candidate.share),
                    candidate);
            }
            throw;
        }

        if (!result.historical_trust_status.has_value() &&
            result.share_status !=
                P2pShareReceiveStatus::UnknownWorkContext) {
            // A concurrent path may already have admitted or rejected this
            // exact share before the synthetic retry reached historical trust.
            // It no longer needs an oracle-retry registration.
            std::lock_guard lock(state_mutex_);
            retryable_historical_.erase(
                share_id(candidate.share));
        }

        if (result.historical_trust_status.has_value() &&
            *result.historical_trust_status ==
                HistoricalTrustStatus::Trusted) {
            ++summary.trusted;
        }

        if (result.historical_share_connected) {
            ++summary.connected;
        }
    }

    {
        std::lock_guard lock(state_mutex_);
        expire_historical_state(now);
        summary.remaining = retryable_historical_.size();
    }

    return summary;
}

P2pReplayRecoverySummary
P2pNodeProtocol::advance_replay_recovery(
    P2pRuntime& runtime,
    std::uint64_t now,
    ProgPowZContextMode mode) {

    P2pReplayRecoverySummary summary;

    // retry_historical_pow_unavailable() already establishes the precedent
    // that autonomous local recovery may re-enter handle() with a synthetic
    // ShareResponse while preserving the real authenticated peer identity.
    // Do the same for structurally replayed frontiers, but select that identity
    // only from connections the transport currently reports as live.
    const std::vector<P2pHandshake> peers =
        runtime.peer_handshakes();

    for (const P2pHandshake& peer : peers) {
        if ((peer.capabilities &
             kP2pCapabilityShareSync) == 0) {
            continue;
        }

        std::optional<Share> candidate;
        std::optional<ReplayFrontierKey> attempt_key;

        {
            std::lock_guard lock(state_mutex_);
            expire_historical_state(now);

            if (!work_retrieval_ ||
                !historical_trust_sources_ready_unlocked()) {
                break;
            }

            std::vector<ShareId> replay_ids =
                chain_.connected_share_ids();

            std::sort(
                replay_ids.begin(),
                replay_ids.end(),
                [this](const ShareId& left,
                       const ShareId& right) {
                    const ConnectedShare* lhs =
                        chain_.find(left);
                    const ConnectedShare* rhs =
                        chain_.find(right);

                    if (lhs == nullptr ||
                        rhs == nullptr) {
                        throw std::logic_error(
                            "connected replay share disappeared "
                            "during periodic recovery scheduling");
                    }

                    if (lhs->share.share_height !=
                        rhs->share.share_height) {
                        return lhs->share.share_height <
                               rhs->share.share_height;
                    }

                    return left < right;
                });

            for (const ShareId& candidate_id :
                 replay_ids) {
                const ConnectedShare* connected =
                    chain_.find(candidate_id);

                if (connected == nullptr ||
                    connected->validated_ancestry) {
                    continue;
                }

                bool parent_ready = false;

                if (is_zero_share_id(
                        connected->share.parent_id)) {
                    parent_ready = true;
                } else {
                    const ConnectedShare* parent =
                        chain_.find(
                            connected->share.parent_id);

                    parent_ready =
                        parent != nullptr &&
                        parent->validated_ancestry;
                }

                if (!parent_ready) {
                    continue;
                }

                const ReplayFrontierKey key{
                    peer.node_id,
                    candidate_id,
                };

                if (replay_frontier_attempts_.contains(
                        key)) {
                    continue;
                }

                candidate = connected->share;
                attempt_key = key;

                // Reserve this exact peer/frontier before re-entering handle().
                // handle() may itself expose more replay work, and this keeps
                // the selected frontier from being immediately selected again
                // by the ancillary scheduler at the end of that same pass.
                replay_frontier_attempts_.
                    insert_or_assign(
                        key,
                        now);

                break;
            }
        }

        if (!candidate.has_value() ||
            !attempt_key.has_value()) {
            continue;
        }

        ++summary.attempted;

        const ShareId candidate_id =
            share_id(*candidate);

        summary.attempted_share_id =
            candidate_id;
        summary.attempted_parent_id =
            candidate->parent_id;

        try {
            const P2pEnvelope replay =
                make_p2p_share_response_envelope(
                    candidate_id,
                    &*candidate);

            const P2pNodeMessageResult result =
                handle(
                    runtime,
                    peer,
                    replay,
                    now,
                    mode);

            summary.historical_trust_status =
                result.historical_trust_status;
            summary.historical_anchor_status =
                result.historical_anchor_status;
            summary.historical_payout_status =
                result.historical_payout_status;
            summary.pruned_connected_shares +=
                result.historical_pruned_connected_shares;

            if (result.historical_share_connected) {
                ++summary.connected;
            }
        } catch (...) {
            // Exceptional local/RPC failure did not complete the periodic
            // attempt. Remove the reservation so a later refresh may retry
            // immediately rather than waiting for the normal attempt timeout.
            std::lock_guard lock(state_mutex_);
            replay_frontier_attempts_.erase(
                *attempt_key);
            throw;
        }

        // Keep one explicit periodic trigger bounded. handle() may itself
        // advance additional immediately eligible local frontiers using the
        // existing kMiningWorkMaxPending budget.
        break;
    }

    {
        std::lock_guard lock(state_mutex_);
        expire_historical_state(now);

        for (const ShareId& id :
             chain_.connected_share_ids()) {
            const ConnectedShare* connected =
                chain_.find(id);

            if (connected != nullptr &&
                !connected->validated_ancestry) {
                ++summary.remaining;
            }
        }
    }

    return summary;
}

std::optional<P2pEnvelope>
P2pNodeProtocol::initial_sync_request(
    const P2pHandshake& peer) {
    std::lock_guard lock(state_mutex_);

    const P2pTipHint hint =
        p2p_tip_hint_from_handshake(peer);
    const P2pTipSyncDecision decision =
        plan_p2p_tip_sync(peer, hint, chain_);

    if (decision.requested_id.has_value()) {
        return make_p2p_share_request_envelope(
            *decision.requested_id);
    }

    if (decision.status ==
            P2pTipSyncStatus::KnownConnectedTip &&
        work_retrieval_ &&
        historical_trust_sources_ready_unlocked()) {
        const ConnectedShare* connected =
            chain_.find(hint.share_id);

        if (connected != nullptr &&
            !connected->validated_ancestry) {
            // Structural replay is not synchronization completion. Re-request
            // the exact known tip so historical recovery can walk backward
            // through UnverifiedAncestry until it reaches a validated parent
            // boundary.
            return make_p2p_share_request_envelope(
                hint.share_id);
        }
    }

    // A transport handshake is only a connection-time snapshot. A long-lived
    // node may have advanced its sidechain substantially since that handshake
    // was created. When the peer's snapshot gives us nothing to request,
    // advertise our current application-level tip so the peer can make its own
    // synchronization decision from fresh state.
    if ((peer.capabilities & kP2pCapabilityShareSync) != 0 &&
        (decision.status == P2pTipSyncStatus::NoRemoteTip ||
         decision.status == P2pTipSyncStatus::KnownConnectedTip)) {
        const P2pTipHint local_hint =
            p2p_tip_hint_from_chain(chain_);
        if (!is_zero_share_id(local_hint.share_id)) {
            return make_p2p_tip_announce_envelope(
                local_hint);
        }
    }

    return std::nullopt;
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
        NodeId followup_peer = peer.node_id;
        std::optional<P2pEnvelope> fresh_tip_followup;
        std::optional<P2pEnvelope> relay_share;
        std::optional<P2pEnvelope> relay_tip;

        const auto attempt_historical =
            [&](const Share& candidate_share,
                const P2pHandshake& candidate_peer,
                const HistoricalEvidence& evidence,
                std::uint64_t required_capability,
                bool allow_parent_defer)
                -> std::optional<ShareId> {
            if (!historical_trust_sources_ready_unlocked()) {
                return std::nullopt;
            }

            const ShareId candidate_id =
                share_id(candidate_share);

            // Keep per-attempt diagnostics aligned with the exact candidate.
            // Aggregate admission fields intentionally remain cumulative for
            // the complete outer message handling pass.
            result.historical_attempt_share_id =
                candidate_id;
            result.historical_attempt_parent_id =
                candidate_share.parent_id;
            result.historical_attempt_zano_height =
                candidate_share.zano_height;
            result.historical_attempt_mining_header_hash =
                candidate_share.mining_header_hash;

            result.historical_anchor_status.reset();
            result.historical_payout_status.reset();
            result.historical_promotion_status.reset();
            result.historical_proof_status.reset();
            result.historical_payout_policy_status.reset();

            const std::vector<P2pMiningAnchor> observations =
                load_historical_observations_();
            const HistoricalTrustResult trust =
                promote_historical_mining_context(
                    trusted_work_,
                    chain_,
                    *historical_params_,
                    candidate_share,
                    evidence.source_peer,
                    evidence.proposal,
                    observations,
                    historical_parent_lookup_,
                    historical_pow_lookup_);
            result.historical_trust_status = trust.status;

            if (trust.status ==
                HistoricalTrustStatus::PromotionRejected) {
                result.historical_promotion_status =
                    trust.promotion.status;
                result.historical_proof_status =
                    trust.promotion.proof_status;
                result.historical_payout_policy_status =
                    trust.promotion.payout_status;
            }

            if (trust.status ==
                HistoricalTrustStatus::AnchorRejected) {
                result.historical_anchor_status =
                    trust.initial_anchor.status;
            } else if (
                trust.status ==
                HistoricalTrustStatus::AnchorChangedBeforePromotion) {
                result.historical_anchor_status =
                    trust.final_anchor.status;
            }

            if (trust.status ==
                HistoricalTrustStatus::PayoutRejected) {
                result.historical_payout_status =
                    trust.payout.status;
            }

            const MiningWorkKey candidate_work_key{
                candidate_share.zano_height,
                candidate_share.mining_header_hash,
            };

            if (trust.status != HistoricalTrustStatus::Trusted) {
                const bool parent_blocked =
                    allow_parent_defer &&
                    !is_zero_share_id(candidate_share.parent_id) &&
                    trust.status ==
                        HistoricalTrustStatus::PayoutRejected &&
                    (trust.payout.status ==
                         HistoricalPayoutStatus::ParentMissing ||
                     trust.payout.status ==
                         HistoricalPayoutStatus::UnverifiedAncestry);
                const bool canonical_pow_temporarily_unavailable =
                    (trust.status ==
                         HistoricalTrustStatus::AnchorRejected &&
                     trust.initial_anchor.status ==
                         HistoricalAnchorStatus::
                             CanonicalPowContextUnavailable) ||
                    (trust.status ==
                         HistoricalTrustStatus::
                             AnchorChangedBeforePromotion &&
                     trust.final_anchor.status ==
                         HistoricalAnchorStatus::
                             CanonicalPowContextUnavailable);

                const bool stable_parent_mismatch =
                    trust.status ==
                        HistoricalTrustStatus::AnchorRejected &&
                    trust.initial_anchor.status ==
                        HistoricalAnchorStatus::ParentMismatch;

                if (stable_parent_mismatch) {
                    const ConnectedShare* replayed =
                        chain_.find(candidate_id);

                    if (replayed != nullptr &&
                        !replayed->validated_ancestry) {
                        const std::size_t pruned =
                            chain_.prune_connected_subtree(
                                candidate_id);

                        result.
                            historical_pruned_connected_shares +=
                                pruned;

                        if (pruned != 0) {
                            // A structural replay branch can temporarily win
                            // best-tip selection before it crosses historical
                            // trust. Once canonical Zano history proves that
                            // branch stale, invalidate all payout expectations
                            // derived from the previous in-memory topology.
                            expected_payout_.reset();
                            expected_payout_plan_.reset();
                            expected_payout_parent_id_.reset();

                            const P2pTipHint current_tip =
                                p2p_tip_hint_from_chain(chain_);

                            if (!is_zero_share_id(
                                    current_tip.share_id)) {
                                relay_tip =
                                    make_p2p_tip_announce_envelope(
                                        current_tip);
                            }
                        }
                    }
                }

                if (parent_blocked) {
                    retryable_historical_.erase(candidate_id);
                }

                if (parent_blocked &&
                    remember_deferred_historical(
                        candidate_share,
                        candidate_peer,
                        evidence,
                        required_capability,
                        now)) {
                    // Keep only evidence that crossed candidate binding and
                    // local anchoring and is blocked solely on sidechain
                    // ancestry. The parent may be absent or merely replayed
                    // without validated ancestry. The evidence remains
                    // untrusted and is fully rechecked after exact-parent
                    // recovery.
                    remember_historical_evidence(
                        candidate_work_key,
                        evidence,
                        now);
                    followup = make_p2p_share_request_envelope(
                        candidate_share.parent_id);
                    followup_peer = candidate_peer.node_id;
                } else if (canonical_pow_temporarily_unavailable) {
                    // The peer work was structurally checked, but the local
                    // canonical daemon cannot yet reconstruct the historical
                    // PoW authority. Preserve only the bounded untrusted
                    // evidence so a later local retry or exact candidate can
                    // rerun the full historical trust crossing without downloading
                    // work again. No trusted-work or share admission occurs
                    // here.
                    remember_historical_evidence(
                        candidate_work_key,
                        evidence,
                        now);
                    remember_retryable_historical(
                        candidate_share,
                        candidate_peer,
                        required_capability,
                        now);
                } else {
                    // Never let malformed/stale evidence poison reuse of this
                    // height/header key. A later peer may provide fresh bytes.
                    retryable_historical_.erase(candidate_id);
                    historical_evidence_.erase(candidate_work_key);
                }
                return std::nullopt;
            }

            // Successful trust consumes any outstanding local-oracle retry
            // registration for this exact share.
            retryable_historical_.erase(candidate_id);

            // Trusted evidence may be reused by another share with the same
            // Zano work key, but every candidate still reruns the complete
            // parent-bound historical trust crossing.
            remember_historical_evidence(
                candidate_work_key,
                evidence,
                now);

            result.historical_share = candidate_share;
            result.historical_share_retried = true;

            if (const ConnectedShare* replayed =
                    chain_.find(candidate_id);
                replayed != nullptr &&
                !replayed->validated_ancestry) {
                const ShareWorkContext* trusted =
                    trusted_work_.find(
                        candidate_share.zano_height,
                        candidate_share.mining_header_hash,
                        candidate_share.parent_id);

                if (trusted == nullptr) {
                    throw std::logic_error(
                        "trusted historical replay work was not registered");
                }

                const RevalidateShareResult revalidated =
                    chain_.revalidate_connected_share(
                        candidate_id,
                        *trusted,
                        now,
                        mode);

                switch (revalidated.status) {
                case RevalidateShareStatus::Validated:
                    result.share_status =
                        P2pShareReceiveStatus::Connected;
                    break;
                case RevalidateShareStatus::AlreadyValidated:
                    result.share_status =
                        P2pShareReceiveStatus::Duplicate;
                    break;
                case RevalidateShareStatus::ParentUnvalidated:
                case RevalidateShareStatus::NotConnected:
                case RevalidateShareStatus::Rejected:
                    result.share_status =
                        P2pShareReceiveStatus::Rejected;
                    break;
                }

                switch (revalidated.status) {
                case RevalidateShareStatus::Validated:
                    // Trust state changed in place. This is deliberately not
                    // appended to historical_admitted_shares because the share
                    // already exists in durable structural history.
                    result.historical_share_connected = true;
                    return candidate_id;

                case RevalidateShareStatus::AlreadyValidated:
                    return std::nullopt;

                case RevalidateShareStatus::ParentUnvalidated:
                    // Historical trust cannot be Trusted unless its explicit
                    // payout parent had validated ancestry. Reaching this state
                    // would violate the crossing invariant.
                    throw std::logic_error(
                        "historical replay parent lost validated ancestry");

                case RevalidateShareStatus::NotConnected:
                    throw std::logic_error(
                        "historical replay disappeared during revalidation");

                case RevalidateShareStatus::Rejected:
                    return std::nullopt;
                }
            }

            P2pShareReceiver receiver(chain_, trusted_work_);
            const P2pShareReceiveResult retried =
                receiver.receive_share(
                    candidate_peer,
                    candidate_share,
                    required_capability,
                    now,
                    mode);
            // Report the final historical admission result regardless of
            // whether the retry was triggered directly by completed work
            // retrieval or by a later ShareAnnounce/ShareResponse reusing
            // retained evidence.
            result.share_status = retried.status;

            if (retried.chain_result.best_tip_changed) {
                expected_payout_.reset();
                expected_payout_plan_.reset();
                expected_payout_parent_id_.reset();
            }
            if (retried.missing_parent_id.has_value()) {
                followup = make_p2p_share_request_envelope(
                    *retried.missing_parent_id);
                followup_peer = candidate_peer.node_id;
            }
            if (retried.status == P2pShareReceiveStatus::Connected ||
                retried.status == P2pShareReceiveStatus::Orphan) {
                result.historical_admitted_shares.push_back(
                    candidate_share);
            }
            if (retried.status == P2pShareReceiveStatus::Connected) {
                result.historical_share_connected = true;
                relay_share = make_p2p_share_announce_envelope(
                    candidate_share);
                relay_tip = make_p2p_tip_announce_envelope(
                    p2p_tip_hint_from_chain(chain_));
                return share_id(candidate_share);
            }
            return std::nullopt;
        };

        const auto resume_deferred_descendants =
            [&](const ShareId& connected_parent_id) {
            std::vector<ShareId> queue{connected_parent_id};
            for (std::size_t index = 0; index < queue.size(); ++index) {
                const ShareId parent_id = queue[index];
                std::vector<ShareId> ready;
                for (const auto& [candidate_id, deferred] :
                     deferred_historical_) {
                    if (deferred.share.parent_id == parent_id) {
                        ready.push_back(candidate_id);
                    }
                }

                for (const ShareId& candidate_id : ready) {
                    const auto it =
                        deferred_historical_.find(candidate_id);
                    if (it == deferred_historical_.end()) {
                        continue;
                    }
                    const DeferredHistoricalCandidate deferred =
                        it->second;
                    deferred_historical_.erase(it);

                    const auto connected = attempt_historical(
                        deferred.share,
                        deferred.candidate_peer,
                        deferred.evidence,
                        deferred.required_capability,
                        false);
                    if (connected.has_value()) {
                        queue.push_back(*connected);
                    }
                }
            }
        };

        // A structurally replayed share may already have the exact immutable
        // mining-work proposal in this node's own archive. Prefer that local
        // evidence before asking the peer for identical bytes. Archive presence
        // is not trust: attempt_historical() reruns candidate binding,
        // canonical anchoring, payout ancestry and miner-tx proofs before any
        // trusted-work authorization or share revalidation can occur.
        const auto attempt_local_historical =
            [&](const Share& candidate_share,
                const P2pHandshake& candidate_peer,
                std::uint64_t required_capability) {
                if (!work_retrieval_ ||
                    !historical_trust_sources_ready_unlocked()) {
                    return false;
                }

                const MiningWorkKey work_key{
                    candidate_share.zano_height,
                    candidate_share.mining_header_hash,
                };

                const auto local_bytes =
                    work_retrieval_->read_local(work_key);

                if (!local_bytes.has_value()) {
                    return false;
                }

                HistoricalEvidence evidence{
                    deserialize_p2p_mining_context_payload(
                        *local_bytes),
                    candidate_peer,
                    now,
                };

                const auto connected =
                    attempt_historical(
                        candidate_share,
                        candidate_peer,
                        evidence,
                        required_capability,
                        true);

                if (connected.has_value()) {
                    resume_deferred_descendants(*connected);
                }

                // Local archive presence alone must never suppress retrieval of
                // fresh peer evidence. Keep the local result only when it
                // actually crossed trust, is blocked solely on sidechain
                // ancestry (which starts the parent-first recovery walk), or
                // is waiting on temporarily unavailable canonical PoW context.
                const bool trusted =
                    result.historical_trust_status.has_value() &&
                    *result.historical_trust_status ==
                        HistoricalTrustStatus::Trusted;

                const bool parent_blocked =
                    result.historical_trust_status.has_value() &&
                    *result.historical_trust_status ==
                        HistoricalTrustStatus::PayoutRejected &&
                    result.historical_payout_status.has_value() &&
                    (*result.historical_payout_status ==
                         HistoricalPayoutStatus::ParentMissing ||
                     *result.historical_payout_status ==
                         HistoricalPayoutStatus::UnverifiedAncestry);

                const bool canonical_pow_temporarily_unavailable =
                    result.historical_trust_status.has_value() &&
                    ((*result.historical_trust_status ==
                          HistoricalTrustStatus::AnchorRejected ||
                      *result.historical_trust_status ==
                          HistoricalTrustStatus::
                              AnchorChangedBeforePromotion) &&
                     result.historical_anchor_status.has_value() &&
                     *result.historical_anchor_status ==
                         HistoricalAnchorStatus::
                             CanonicalPowContextUnavailable);

                const bool replay_parent_mismatch_pruned =
                    result.historical_pruned_connected_shares != 0;

                return trusted ||
                       parent_blocked ||
                       canonical_pow_temporarily_unavailable ||
                       replay_parent_mismatch_pruned;
            };

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
                const MiningWorkKey work_key{
                    share.zano_height,
                    share.mining_header_hash,
                };
                expire_historical_state(now);
                const auto evidence_it =
                    historical_evidence_.find(work_key);
                if (evidence_it != historical_evidence_.end() &&
                    historical_trust_sources_ready_unlocked()) {
                    const auto connected = attempt_historical(
                        share,
                        peer,
                        evidence_it->second,
                        kP2pCapabilityShareGossip,
                        true);
                    if (connected.has_value()) {
                        resume_deferred_descendants(*connected);
                    }
                } else {
                    auto work_request =
                        work_retrieval_->begin(peer, work_key, now);
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
                resume_deferred_descendants(receive.chain_result.id);
            }
            break;
        }
        case P2pMessageType::ShareRequest: {
            std::lock_guard lock(state_mutex_);
            followup = answer_p2p_share_request(peer, envelope, chain_);

            // A transport handshake is only a connection-time snapshot. If
            // that advertised share was later pruned by canonical
            // reconciliation, a reconnecting peer can legitimately request an
            // ID we no longer have. Preserve the explicit NotFound response,
            // then advertise the current application-level tip directly to
            // that same peer so synchronization can resume from fresh state.
            const P2pShareResponse response =
                parse_p2p_share_response_envelope(*followup);

            if (response.code == P2pShareResponseCode::NotFound) {
                const P2pTipHint current_tip =
                    p2p_tip_hint_from_chain(chain_);

                if (!is_zero_share_id(current_tip.share_id)) {
                    fresh_tip_followup =
                        make_p2p_tip_announce_envelope(current_tip);
                }
            }

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
                        const MiningWorkKey work_key{
                            response.share->zano_height,
                            response.share->mining_header_hash,
                        };
                        expire_historical_state(now);
                        const auto evidence_it =
                            historical_evidence_.find(work_key);
                        if (evidence_it != historical_evidence_.end() &&
                            historical_trust_sources_ready_unlocked()) {
                            const auto connected = attempt_historical(
                                *response.share,
                                peer,
                                evidence_it->second,
                                kP2pCapabilityShareSync,
                                true);
                            if (connected.has_value()) {
                                resume_deferred_descendants(*connected);
                            }
                        } else if (!attempt_local_historical(
                                       *response.share,
                                       peer,
                                       kP2pCapabilityShareSync)) {
                            auto work_request =
                                work_retrieval_->begin(
                                    peer,
                                    work_key,
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
                        resume_deferred_descendants(
                            sync.share_result->chain_result.id);
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
                followup =
                    make_p2p_share_request_envelope(
                        *decision.requested_id);
            } else if (
                decision.status ==
                    P2pTipSyncStatus::KnownConnectedTip &&
                work_retrieval_ &&
                historical_trust_sources_ready_unlocked()) {
                const ConnectedShare* connected =
                    chain_.find(hint.share_id);

                if (connected != nullptr &&
                    !connected->validated_ancestry) {
                    // Structural replay is not synchronization completion.
                    // Ask for the exact advertised share so the normal
                    // historical trust path can walk backward parent-first.
                    followup =
                        make_p2p_share_request_envelope(
                            hint.share_id);
                    followup_peer = peer.node_id;
                }
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

            std::lock_guard lock(state_mutex_);
            expire_historical_state(now);

            const auto pending_it = pending_historical_.find(
                PendingHistoricalKey{peer.node_id, work_key});
            if (!historical_trust_sources_ready_unlocked() ||
                pending_it == pending_historical_.end()) {
                // With no exact waiting candidate, leave the structurally
                // checked bytes in P2pWorkRetrieval's untrusted cache.
                break;
            }

            const auto work_bytes =
                work_retrieval_->take_untrusted(work_key);
            if (!work_bytes.has_value()) {
                throw std::runtime_error(
                    "completed historical work is missing from untrusted cache");
            }

            HistoricalEvidence evidence{
                deserialize_p2p_mining_context_payload(*work_bytes),
                peer,
                now,
            };

            const PendingHistoricalCandidate candidate =
                pending_it->second;
            pending_historical_.erase(pending_it);

            const auto connected = attempt_historical(
                candidate.share,
                peer,
                evidence,
                candidate.required_capability,
                true);
            if (connected.has_value()) {
                resume_deferred_descendants(*connected);
            }
            break;
        }
        case P2pMessageType::Handshake:
            result.status = P2pNodeMessageStatus::UnexpectedHandshake;
            break;
        }

        // Persistence replay can contain several independent unvalidated
        // frontiers. Connection-time tip recovery starts only one ancestry
        // walk, so once protocol handling has no required reply outstanding,
        // opportunistically advance other replay frontiers using this same
        // authenticated peer.
        //
        // Only a share whose exact parent already has validated ancestry is
        // eligible. That keeps recovery parent-first and prevents this
        // scheduler from creating an alternative ancestry authority.
        //
        // The scheduler does not invent another trust path: exact immutable
        // local work is tried through attempt_local_historical(), and when no
        // acceptable local evidence exists the existing MiningWorkRequest /
        // pending_historical_ path is used.
        if (!followup.has_value() &&
            work_retrieval_ &&
            (peer.capabilities & kP2pCapabilityShareSync) != 0) {
            const P2pNodeMessageResult outer_result = result;
            bool replay_frontier_connected = false;

            {
                std::lock_guard lock(state_mutex_);
                expire_historical_state(now);

                if (historical_trust_sources_ready_unlocked()) {
                    std::vector<ShareId> replay_ids =
                        chain_.connected_share_ids();

                    std::sort(
                        replay_ids.begin(),
                        replay_ids.end(),
                        [this](const ShareId& left,
                               const ShareId& right) {
                            const ConnectedShare* lhs =
                                chain_.find(left);
                            const ConnectedShare* rhs =
                                chain_.find(right);

                            if (lhs == nullptr ||
                                rhs == nullptr) {
                                throw std::logic_error(
                                    "connected replay share disappeared "
                                    "during frontier scheduling");
                            }

                            if (lhs->share.share_height !=
                                rhs->share.share_height) {
                                return lhs->share.share_height <
                                       rhs->share.share_height;
                            }

                            return left < right;
                        });

                    // Keep one inbound protocol pass bounded. Local replay
                    // validation may expose several immediately adjacent
                    // frontiers, while the first frontier requiring network
                    // evidence stops the batch and remains serialized through
                    // the existing request machinery.
                    std::size_t frontier_budget =
                        kMiningWorkMaxPending;

                    result.historical_share_connected = false;

                    for (const ShareId& candidate_id :
                         replay_ids) {
                        if (frontier_budget == 0 ||
                            followup.has_value()) {
                            break;
                        }

                        const ConnectedShare* connected =
                            chain_.find(candidate_id);

                        if (connected == nullptr ||
                            connected->validated_ancestry) {
                            continue;
                        }

                        bool parent_ready = false;

                        if (is_zero_share_id(
                                connected->share.parent_id)) {
                            parent_ready = true;
                        } else {
                            const ConnectedShare* parent =
                                chain_.find(
                                    connected->share.parent_id);

                            parent_ready =
                                parent != nullptr &&
                                parent->validated_ancestry;
                        }

                        if (!parent_ready) {
                            continue;
                        }

                        const ReplayFrontierKey attempt_key{
                            peer.node_id,
                            candidate_id,
                        };

                        if (replay_frontier_attempts_.contains(
                                attempt_key)) {
                            continue;
                        }

                        const Share candidate =
                            connected->share;

                        --frontier_budget;

                        const bool handled_locally =
                            attempt_local_historical(
                                candidate,
                                peer,
                                kP2pCapabilityShareSync);

                        if (handled_locally) {
                            replay_frontier_attempts_.
                                insert_or_assign(
                                    attempt_key,
                                    now);

                            replay_frontier_connected =
                                replay_frontier_connected ||
                                result.
                                    historical_share_connected;

                            // Parent-first local recovery may expose the next
                            // child immediately. Continue the bounded scan
                            // unless the trust crossing itself scheduled a
                            // required protocol reply.
                            continue;
                        }

                        const MiningWorkKey work_key{
                            candidate.zano_height,
                            candidate.mining_header_hash,
                        };

                        auto work_request =
                            work_retrieval_->begin(
                                peer,
                                work_key,
                                now);

                        if (work_request.has_value()) {
                            replay_frontier_attempts_.
                                insert_or_assign(
                                    attempt_key,
                                    now);

                            followup =
                                std::move(work_request);
                            followup_peer =
                                peer.node_id;

                            remember_pending_historical(
                                peer,
                                candidate,
                                kP2pCapabilityShareSync,
                                now);
                        }

                        // Network evidence remains serialized. If begin()
                        // could not start a request, leave the frontier
                        // unmarked so a later protocol event may retry.
                        break;
                    }

                    replay_frontier_connected =
                        replay_frontier_connected ||
                        result.historical_share_connected;
                }
            }

            // Frontier scheduling is ancillary to the outer protocol message.
            // Do not overwrite its share/trust diagnostics or persistence
            // semantics. Preserve only the fact that replay trust advanced so
            // the runtime requests a canonical payout/template refresh.
            result = outer_result;
            result.historical_share_connected =
                result.historical_share_connected ||
                replay_frontier_connected;
        }

        const std::uint32_t penalty = p2p_node_message_penalty(result);
        if (penalty != 0) {
            runtime.report_peer_misbehavior(peer.node_id, penalty);
        }

        if (followup.has_value()) {
            result.sent_followup =
                runtime.send_to(followup_peer, *followup);
        }

        if (fresh_tip_followup.has_value()) {
            static_cast<void>(
                runtime.send_to(peer.node_id, *fresh_tip_followup));
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

std::vector<Share> p2p_node_unique_admitted_shares(
    const P2pNodeMessageResult& result,
    const P2pEnvelope& envelope) {
    std::vector<Share> admitted;

    const auto append_unique =
        [&admitted](const Share& share) {
            const ShareId id = share_id(share);

            const bool already_present =
                std::any_of(
                    admitted.begin(),
                    admitted.end(),
                    [&id](const Share& existing) {
                        return share_id(existing) == id;
                    });

            if (!already_present) {
                admitted.push_back(share);
            }
        };

    const bool direct_share_admitted =
        (result.status ==
             P2pNodeMessageStatus::ShareProcessed ||
         result.status ==
             P2pNodeMessageStatus::ShareResponseProcessed) &&
        (result.share_status ==
             P2pShareReceiveStatus::Connected ||
         result.share_status ==
             P2pShareReceiveStatus::Orphan);

    if (direct_share_admitted) {
        if (result.status ==
            P2pNodeMessageStatus::ShareProcessed) {
            append_unique(
                parse_p2p_share_announce_envelope(
                    envelope));
        } else {
            const P2pShareResponse response =
                parse_p2p_share_response_envelope(
                    envelope);

            if (response.share.has_value()) {
                append_unique(*response.share);
            }
        }
    }

    for (const Share& historical :
         result.historical_admitted_shares) {
        append_unique(historical);
    }

    return admitted;
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
    std::function<RpcCanonicalHeader(std::uint64_t)> lookup,
    std::function<std::optional<RpcHistoricalPowContext>(
        std::uint64_t)> historical_pow_lookup) {
    if (!load_local_observations ||
        !lookup ||
        !historical_pow_lookup) {
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
    historical_pow_lookup_ =
        std::move(historical_pow_lookup);
}

void P2pNodeProtocol::remember_trusted_work(
    const ShareWorkContext& context,
    std::optional<ShareId> parent_id) {
    std::lock_guard lock(state_mutex_);

    if (!local_mining_anchor_.has_value() ||
        !local_mining_context_.has_value()) {
        throw std::runtime_error(
            "trusted local work has no installed Zano mining context");
    }

    const P2pMiningAnchor& anchor =
        *local_mining_anchor_;
    const P2pMiningContextProposal& proposal =
        *local_mining_context_;

    if (anchor.zano_height != context.zano_height ||
        proposal.zano_height != context.zano_height ||
        anchor.network_difficulty != context.network_difficulty ||
        proposal.network_difficulty != context.network_difficulty ||
        anchor.prev_hash != proposal.prev_hash ||
        validate_p2p_mining_context_structure(proposal) !=
            context.mining_header_hash) {
        throw std::runtime_error(
            "trusted local work does not match installed Zano mining context");
    }

    ShareId resolved_parent{};

    if (parent_id.has_value()) {
        resolved_parent = *parent_id;

        if (!is_zero_share_id(resolved_parent)) {
            const ConnectedShare* parent =
                chain_.find(resolved_parent);

            if (parent == nullptr) {
                throw std::runtime_error(
                    "trusted local work parent is not connected");
            }

            if (chain_.enforces_sidechain_difficulty() &&
                !parent->validated_ancestry) {
                throw std::runtime_error(
                    "trusted local work parent lacks validated ancestry");
            }
        }

        // When a payout plan is installed, work provenance must be bound to
        // that exact same sidechain snapshot.
        if (expected_payout_parent_id_.has_value() &&
            *expected_payout_parent_id_ != resolved_parent) {
            throw std::runtime_error(
                "trusted local work parent does not match payout parent");
        }
    } else {
        const ConnectedShare* parent =
            chain_.best_tip();

        if (parent != nullptr) {
            resolved_parent = parent->id;
        }
    }

    trusted_work_.remember(
        context,
        resolved_parent,
        anchor.prev_hash);
}

P2pCanonicalReconciliationResult
P2pNodeProtocol::reconcile_canonical_parent_unlocked(
    std::uint64_t zano_height,
    const Hash256& canonical_parent_hash) {
    P2pCanonicalReconciliationResult result;

    // Prune oldest affected roots first. Once an ancestor is removed, later
    // IDs from the snapshot may already have disappeared as descendants.
    std::vector<ShareId> connected_ids =
        chain_.connected_share_ids();

    std::sort(
        connected_ids.begin(),
        connected_ids.end(),
        [this](const ShareId& left, const ShareId& right) {
            const ConnectedShare* left_share = chain_.find(left);
            const ConnectedShare* right_share = chain_.find(right);

            if (left_share == nullptr || right_share == nullptr) {
                return left < right;
            }
            if (left_share->share.share_height !=
                right_share->share.share_height) {
                return left_share->share.share_height <
                       right_share->share.share_height;
            }
            return left < right;
        });

    for (const ShareId& id : connected_ids) {
        const ConnectedShare* connected = chain_.find(id);
        if (connected == nullptr ||
            connected->share.zano_height != zano_height) {
            continue;
        }

        const Hash256* recorded_parent =
            trusted_work_.find_zano_parent_hash(
                connected->share.zano_height,
                connected->share.mining_header_hash,
                connected->share.parent_id);

        // Compatibility/synthetic work carries no canonical-parent provenance
        // and is never guessed stale. Production trust crossings do carry it.
        if (recorded_parent == nullptr ||
            *recorded_parent == canonical_parent_hash) {
            continue;
        }

        result.pruned_connected_shares +=
            chain_.prune_connected_subtree(id);
    }

    // Revoke stale authorization even when it is currently off-chain, so an
    // old share cannot later regain admission merely because its work survived
    // in the registry.
    result.revoked_trusted_work =
        trusted_work_.erase_zano_parent_mismatch(
            zano_height,
            canonical_parent_hash);

    if (result.pruned_connected_shares != 0) {
        expected_payout_.reset();
        expected_payout_plan_.reset();
        expected_payout_parent_id_.reset();
    }

    return result;
}

P2pCanonicalReconciliationResult
P2pNodeProtocol::reconcile_unavailable_work_height_unlocked(
    std::uint64_t zano_height) {
    P2pCanonicalReconciliationResult result;

    std::vector<ShareId> connected_ids =
        chain_.connected_share_ids();

    std::sort(
        connected_ids.begin(),
        connected_ids.end(),
        [this](const ShareId& left, const ShareId& right) {
            const ConnectedShare* left_share = chain_.find(left);
            const ConnectedShare* right_share = chain_.find(right);

            if (left_share == nullptr || right_share == nullptr) {
                return left < right;
            }
            if (left_share->share.share_height !=
                right_share->share.share_height) {
                return left_share->share.share_height <
                       right_share->share.share_height;
            }
            return left < right;
        });

    for (const ShareId& id : connected_ids) {
        const ConnectedShare* connected = chain_.find(id);
        if (connected == nullptr ||
            connected->share.zano_height != zano_height) {
            continue;
        }

        result.pruned_connected_shares +=
            chain_.prune_connected_subtree(id);
    }

    result.revoked_trusted_work =
        trusted_work_.erase_zano_height(zano_height);

    if (result.pruned_connected_shares != 0 ||
        result.revoked_trusted_work != 0) {
        expected_payout_.reset();
        expected_payout_plan_.reset();
        expected_payout_parent_id_.reset();
    }

    return result;
}

P2pCanonicalReconciliationResult
P2pNodeProtocol::reconcile_unavailable_work_above_unlocked(
    std::uint64_t maximum_zano_height) {
    P2pCanonicalReconciliationResult result;

    std::vector<ShareId> connected_ids =
        chain_.connected_share_ids();

    std::sort(
        connected_ids.begin(),
        connected_ids.end(),
        [this](const ShareId& left, const ShareId& right) {
            const ConnectedShare* left_share = chain_.find(left);
            const ConnectedShare* right_share = chain_.find(right);

            if (left_share == nullptr || right_share == nullptr) {
                return left < right;
            }
            if (left_share->share.share_height !=
                right_share->share.share_height) {
                return left_share->share.share_height <
                       right_share->share.share_height;
            }
            return left < right;
        });

    for (const ShareId& id : connected_ids) {
        const ConnectedShare* connected = chain_.find(id);
        if (connected == nullptr ||
            connected->share.zano_height <= maximum_zano_height) {
            continue;
        }

        result.pruned_connected_shares +=
            chain_.prune_connected_subtree(id);
    }

    result.revoked_trusted_work =
        trusted_work_.erase_zano_heights_above(
            maximum_zano_height);

    if (result.pruned_connected_shares != 0 ||
        result.revoked_trusted_work != 0) {
        expected_payout_.reset();
        expected_payout_plan_.reset();
        expected_payout_parent_id_.reset();
    }

    return result;
}

std::vector<std::uint64_t>
P2pNodeProtocol::trusted_work_provenance_heights() const {
    std::lock_guard lock(state_mutex_);
    return trusted_work_.provenance_zano_heights();
}

P2pCanonicalReconciliationResult
P2pNodeProtocol::reconcile_canonical_parent(
    std::uint64_t zano_height,
    const Hash256& canonical_parent_hash) {
    std::lock_guard lock(state_mutex_);
    return reconcile_canonical_parent_unlocked(
        zano_height,
        canonical_parent_hash);
}

P2pCanonicalReconciliationResult
P2pNodeProtocol::reconcile_unavailable_work_height(
    std::uint64_t zano_height) {
    std::lock_guard lock(state_mutex_);
    return reconcile_unavailable_work_height_unlocked(
        zano_height);
}

P2pCanonicalReconciliationResult
P2pNodeProtocol::reconcile_unavailable_work_above(
    std::uint64_t maximum_zano_height) {
    std::lock_guard lock(state_mutex_);
    return reconcile_unavailable_work_above_unlocked(
        maximum_zano_height);
}

void P2pNodeProtocol::reconcile_local_parent_replacement_unlocked(
    const P2pMiningAnchor& next_anchor) {
    if (!local_mining_anchor_.has_value() ||
        local_mining_anchor_->zano_height != next_anchor.zano_height ||
        local_mining_anchor_->prev_hash == next_anchor.prev_hash) {
        return;
    }

    static_cast<void>(
        reconcile_canonical_parent_unlocked(
            next_anchor.zano_height,
            next_anchor.prev_hash));

    // A same-height canonical-parent replacement also invalidates any payout
    // expectation installed for the previous daemon template, even when no
    // connected share happened to require pruning.
    expected_payout_.reset();
    expected_payout_plan_.reset();
    expected_payout_parent_id_.reset();
}

void P2pNodeProtocol::set_local_mining_context(
    const P2pMiningAnchor& anchor,
    const P2pMiningContextProposal& proposal) {
    std::lock_guard lock(state_mutex_);
    reconcile_local_parent_replacement_unlocked(anchor);
    local_mining_anchor_ = anchor;
    local_mining_context_ = proposal;
}

void P2pNodeProtocol::set_local_mining_context(
    const P2pMiningAnchor& anchor,
    const P2pMiningContextProposal& proposal,
    const P2pPayoutAddress& payout) {
    std::lock_guard lock(state_mutex_);
    reconcile_local_parent_replacement_unlocked(anchor);
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
    const PplnsCoinbasePlan& plan,
    std::optional<ShareId> parent_id) {
    std::lock_guard lock(state_mutex_);

    reconcile_local_parent_replacement_unlocked(anchor);

    ShareId resolved_parent{};

    if (parent_id.has_value()) {
        resolved_parent = *parent_id;

        if (!is_zero_share_id(resolved_parent)) {
            const ConnectedShare* parent =
                chain_.find(resolved_parent);

            if (parent == nullptr) {
                throw std::runtime_error(
                    "local payout-plan parent is not connected");
            }

            if (chain_.enforces_sidechain_difficulty() &&
                !parent->validated_ancestry) {
                throw std::runtime_error(
                    "local payout-plan parent lacks validated ancestry");
            }
        }
    } else {
        const ConnectedShare* parent =
            chain_.best_tip();

        if (parent != nullptr) {
            resolved_parent = parent->id;
        }
    }

    local_mining_anchor_ = anchor;
    local_mining_context_ = proposal;
    expected_payout_plan_ = plan;
    expected_payout_.reset();
    expected_payout_parent_id_ = resolved_parent;
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
