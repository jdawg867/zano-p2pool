#pragma once

#include "zano_p2pool/historical_trust.hpp"
#include "zano_p2pool/p2p_mining_context_trust.hpp"
#include "zano_p2pool/p2p_runtime.hpp"
#include "zano_p2pool/p2p_share.hpp"
#include "zano_p2pool/p2p_sync.hpp"
#include "zano_p2pool/p2p_tip.hpp"
#include "zano_p2pool/p2p_work_retrieval.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace zano_p2pool {

enum class P2pNodeMessageStatus : std::uint8_t {
    ShareProcessed,
    ShareRequestAnswered,
    ShareResponseProcessed,
    TipProcessed,
    MiningContextProcessed,
    MiningContextDeferred,
    UnexpectedHandshake,
    MiningWorkRequestAnswered,
    MiningWorkResponseProcessed,
};

struct P2pCanonicalReconciliationResult {
    std::size_t pruned_connected_shares{0};
    std::size_t revoked_trusted_work{0};
};

struct P2pNodeMessageResult {
    P2pNodeMessageStatus status{P2pNodeMessageStatus::UnexpectedHandshake};
    P2pShareReceiveStatus share_status{P2pShareReceiveStatus::Rejected};
    P2pShareSyncReceiveStatus sync_status{P2pShareSyncReceiveStatus::NotFound};
    P2pTipSyncStatus tip_status{P2pTipSyncStatus::NoRemoteTip};
    P2pMiningContextTrustStatus mining_context_status{
        P2pMiningContextTrustStatus::ProofsRejected};
    bool mining_context_registry_inserted{false};
    std::optional<Hash256> untrusted_work_received;
    std::optional<HistoricalTrustStatus> historical_trust_status;
    std::optional<HistoricalAnchorStatus> historical_anchor_status;
    std::optional<HistoricalPayoutStatus> historical_payout_status;
    std::optional<P2pMiningContextTrustStatus>
        historical_promotion_status;
    std::optional<P2pMinerTxProofStatus>
        historical_proof_status;
    std::optional<P2pPayoutPolicyStatus>
        historical_payout_policy_status;
    std::optional<Share> historical_share;
    std::vector<Share> historical_admitted_shares;
    bool historical_share_retried{false};
    bool historical_share_connected{false};
    bool sent_followup{false};
    bool relayed_share{false};
    bool relayed_tip{false};
};

struct P2pHistoricalRetrySummary {
    std::size_t attempted{};
    std::size_t trusted{};
    std::size_t connected{};
    std::size_t remaining{};
};

[[nodiscard]] std::uint32_t p2p_node_message_penalty(
    const P2pNodeMessageResult& result) noexcept;

class P2pNodeProtocol {
public:
    P2pNodeProtocol(
        ShareChain& chain,
        P2pTrustedWorkRegistry& trusted_work,
        std::mutex& state_mutex) noexcept;

    [[nodiscard]] P2pNodeMessageResult handle(
        P2pRuntime& runtime,
        const P2pHandshake& peer,
        const P2pEnvelope& envelope,
        std::uint64_t now,
        ProgPowZContextMode mode = ProgPowZContextMode::Light);

    // Retry exact historical candidates that previously failed only because
    // the local canonical daemon could not yet reconstruct historical PoW
    // authority. Each retry re-enters the normal gossip/sync handling path and
    // therefore reruns the complete historical trust crossing.
    [[nodiscard]] P2pHistoricalRetrySummary
    retry_historical_pow_unavailable(
        P2pRuntime& runtime,
        std::uint64_t now,
        ProgPowZContextMode mode = ProgPowZContextMode::Light);

    // Use the authenticated handshake tip as an initial synchronization hint.
    // Because that transport snapshot can become stale on a long-lived node,
    // advertise the current local application-level tip when the handshake
    // itself gives us nothing that needs to be requested.
    [[nodiscard]] std::optional<P2pEnvelope>
    initial_sync_request(const P2pHandshake& peer);

    // Configure before runtime threads start; retrieval and callbacks must
    // outlive runtime. Retrieved peer work remains untrusted until the
    // historical trust crossing succeeds for the exact waiting share.
    void set_work_retrieval(P2pWorkRetrieval* retrieval) noexcept { work_retrieval_ = retrieval; }
    void set_historical_trust_sources(
        const SidechainParameters& params,
        std::function<std::vector<P2pMiningAnchor>()> load_local_observations,
        std::function<RpcCanonicalHeader(std::uint64_t)> lookup,
        std::function<std::optional<RpcHistoricalPowContext>(
            std::uint64_t)> historical_pow_lookup);
    void remember_trusted_work(const ShareWorkContext& context);
    void set_local_mining_context(
        const P2pMiningAnchor& anchor,
        const P2pMiningContextProposal& proposal);
    void set_local_mining_context(
        const P2pMiningAnchor& anchor,
        const P2pMiningContextProposal& proposal,
        const P2pPayoutAddress& payout);
    void set_local_mining_context(
        const P2pMiningAnchor& anchor,
        const P2pMiningContextProposal& proposal,
        const PplnsCoinbasePlan& plan);

    // Exactly one payout expectation is active at a time. Bootstrap/legacy
    // daemon templates use the single public payout identity; canonical PPLNS
    // templates install the complete deterministic destination plan instead.
    void set_expected_payout(const P2pPayoutAddress& payout);
    void set_expected_payout_plan(const PplnsCoinbasePlan& plan);
    void clear_expected_payout() noexcept;

    [[nodiscard]] std::size_t trusted_work_count() const noexcept;

    // Snapshot of Zano heights for trusted work carrying independently
    // established canonical-parent provenance.
    [[nodiscard]] std::vector<std::uint64_t>
    trusted_work_provenance_heights() const;

    // Reconcile one historical mining height against a stable canonical Zano
    // parent. Only provenance-bearing work that conflicts with canonical
    // history is revoked. Connected shares using that stale work are removed
    // with their descendants, preserving any valid older prefix.
    [[nodiscard]] P2pCanonicalReconciliationResult
    reconcile_canonical_parent(
        std::uint64_t zano_height,
        const Hash256& canonical_parent_hash);

    // Revoke a trusted-work height that is ahead of the daemon's current
    // canonical template after a rollback. No canonical parent exists against
    // which this work can remain authorized.
    [[nodiscard]] P2pCanonicalReconciliationResult
    reconcile_unavailable_work_height(
        std::uint64_t zano_height);

    [[nodiscard]] P2pCanonicalReconciliationResult
    reconcile_unavailable_work_above(
        std::uint64_t maximum_zano_height);

    [[nodiscard]] std::size_t connected_share_count() const noexcept;
    [[nodiscard]] P2pTipHint local_tip() const noexcept;
    [[nodiscard]] bool mining_context_trust_ready() const noexcept;
    [[nodiscard]] std::optional<P2pEnvelope>
    local_mining_context_envelope() const;

private:
    struct PendingHistoricalCandidate {
        Share share;
        std::uint64_t required_capability{};
        std::uint64_t started{};
    };

    struct HistoricalEvidence {
        P2pMiningContextProposal proposal;
        P2pHandshake source_peer;
        std::uint64_t received{};
    };

    struct DeferredHistoricalCandidate {
        Share share;
        P2pHandshake candidate_peer;
        HistoricalEvidence evidence;
        std::uint64_t required_capability{};

        // Identifies one backward ancestry-recovery walk. Descendants in the
        // same walk share this root so valid parent progress refreshes only
        // that recovery session rather than unrelated peer state.
        ShareId recovery_root{};

        // Inactivity timestamp. Active ancestry recovery may legitimately
        // exceed the mining-work request lifetime in total, but a stalled
        // recovery session still expires after that lifetime without progress.
        std::uint64_t last_progress{};
    };

    struct RetryableHistoricalCandidate {
        Share share;
        P2pHandshake candidate_peer;
        std::uint64_t required_capability{};
        std::uint64_t started{};
    };

    using PendingHistoricalKey = std::pair<NodeId, MiningWorkKey>;

    [[nodiscard]] bool historical_trust_sources_ready_unlocked() const noexcept;
    void expire_historical_state(std::uint64_t now);
    void remember_pending_historical(
        const P2pHandshake& peer,
        const Share& share,
        std::uint64_t required_capability,
        std::uint64_t now);
    void remember_historical_evidence(
        const MiningWorkKey& key,
        HistoricalEvidence evidence,
        std::uint64_t now);
    [[nodiscard]] bool remember_deferred_historical(
        const Share& share,
        const P2pHandshake& candidate_peer,
        const HistoricalEvidence& evidence,
        std::uint64_t required_capability,
        std::uint64_t now);
    void remember_retryable_historical(
        const Share& share,
        const P2pHandshake& candidate_peer,
        std::uint64_t required_capability,
        std::uint64_t now);

    // Called with state_mutex_ held. A same-height canonical-parent
    // replacement invalidates every work authorization anchored to the
    // displaced Zano parent and any active sidechain subtree using that
    // mining height.
    [[nodiscard]] P2pCanonicalReconciliationResult
    reconcile_canonical_parent_unlocked(
        std::uint64_t zano_height,
        const Hash256& canonical_parent_hash);

    [[nodiscard]] P2pCanonicalReconciliationResult
    reconcile_unavailable_work_height_unlocked(
        std::uint64_t zano_height);

    [[nodiscard]] P2pCanonicalReconciliationResult
    reconcile_unavailable_work_above_unlocked(
        std::uint64_t maximum_zano_height);

    void reconcile_local_parent_replacement_unlocked(
        const P2pMiningAnchor& next_anchor);

    P2pWorkRetrieval* work_retrieval_{nullptr};
    ShareChain& chain_;
    P2pTrustedWorkRegistry& trusted_work_;
    std::mutex& state_mutex_;
    std::optional<P2pMiningAnchor> local_mining_anchor_;
    std::optional<P2pMiningContextProposal> local_mining_context_;
    std::optional<P2pPayoutAddress> expected_payout_;
    std::optional<PplnsCoinbasePlan> expected_payout_plan_;
    std::optional<ShareId> expected_payout_parent_id_;

    std::optional<SidechainParameters> historical_params_;
    std::function<std::vector<P2pMiningAnchor>()>
        load_historical_observations_;
    std::function<RpcCanonicalHeader(std::uint64_t)>
        historical_parent_lookup_;
    std::function<std::optional<RpcHistoricalPowContext>(
        std::uint64_t)>
        historical_pow_lookup_;
    std::map<PendingHistoricalKey, PendingHistoricalCandidate>
        pending_historical_;
    std::map<MiningWorkKey, HistoricalEvidence>
        historical_evidence_;
    std::map<ShareId, DeferredHistoricalCandidate>
        deferred_historical_;
    std::map<ShareId, RetryableHistoricalCandidate>
        retryable_historical_;
};

[[nodiscard]] const char* p2p_node_message_status_name(
    P2pNodeMessageStatus status) noexcept;

}  // namespace zano_p2pool
