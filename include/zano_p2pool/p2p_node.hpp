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
    std::optional<Share> historical_share;
    std::vector<Share> historical_admitted_shares;
    bool historical_share_retried{false};
    bool historical_share_connected{false};
    bool sent_followup{false};
    bool relayed_share{false};
    bool relayed_tip{false};
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

    // The authenticated transport handshake already carries the peer's best
    // share id/height. Use it as a synchronization hint immediately on
    // connection instead of waiting for an incidental TipAnnounce.
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
};

[[nodiscard]] const char* p2p_node_message_status_name(
    P2pNodeMessageStatus status) noexcept;

}  // namespace zano_p2pool
