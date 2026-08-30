#pragma once

#include "zano_p2pool/p2p_mining_context_trust.hpp"
#include "zano_p2pool/p2p_runtime.hpp"
#include "zano_p2pool/p2p_share.hpp"
#include "zano_p2pool/p2p_sync.hpp"
#include "zano_p2pool/p2p_tip.hpp"

#include <cstdint>
#include <mutex>
#include <optional>

namespace zano_p2pool {

enum class P2pNodeMessageStatus : std::uint8_t {
    ShareProcessed,
    ShareRequestAnswered,
    ShareResponseProcessed,
    TipProcessed,
    MiningContextProcessed,
    MiningContextDeferred,
    UnexpectedHandshake,
};

struct P2pNodeMessageResult {
    P2pNodeMessageStatus status{P2pNodeMessageStatus::UnexpectedHandshake};
    P2pShareReceiveStatus share_status{P2pShareReceiveStatus::Rejected};
    P2pShareSyncReceiveStatus sync_status{P2pShareSyncReceiveStatus::NotFound};
    P2pTipSyncStatus tip_status{P2pTipSyncStatus::NoRemoteTip};
    P2pMiningContextTrustStatus mining_context_status{
        P2pMiningContextTrustStatus::ProofsRejected};
    bool mining_context_registry_inserted{false};
    bool sent_followup{false};
    bool relayed_share{false};
    bool relayed_tip{false};
};

// Returns the deterministic score penalty for a fully parsed node-protocol
// result. Benign synchronization states such as duplicates, unknown work,
// missing parents and mining-anchor races remain neutral. Malformed payloads
// throw before a result exists and are scored by P2pNodeProtocol::handle().
[[nodiscard]] std::uint32_t p2p_node_message_penalty(
    const P2pNodeMessageResult& result) noexcept;

// Runtime protocol dispatcher for share-chain synchronization. ShareChain,
// P2pTrustedWorkRegistry and the current local mining context are protected by
// the same node-wide mutex so Stratum, P2P gossip, sync and checkpoint-6B.4
// trust promotion share one serialized consensus boundary.
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

    void remember_trusted_work(const ShareWorkContext& context);
    void set_local_mining_context(
        const P2pMiningAnchor& anchor,
        const P2pMiningContextProposal& proposal);
    void set_expected_payout(const P2pPayoutAddress& payout);
    void clear_expected_payout() noexcept;

    [[nodiscard]] std::size_t trusted_work_count() const noexcept;
    [[nodiscard]] std::size_t connected_share_count() const noexcept;
    [[nodiscard]] P2pTipHint local_tip() const noexcept;
    [[nodiscard]] bool mining_context_trust_ready() const noexcept;
    [[nodiscard]] std::optional<P2pEnvelope>
    local_mining_context_envelope() const;

private:
    ShareChain& chain_;
    P2pTrustedWorkRegistry& trusted_work_;
    std::mutex& state_mutex_;
    std::optional<P2pMiningAnchor> local_mining_anchor_;
    std::optional<P2pMiningContextProposal> local_mining_context_;
    std::optional<P2pPayoutAddress> expected_payout_;
};

[[nodiscard]] const char* p2p_node_message_status_name(
    P2pNodeMessageStatus status) noexcept;

}  // namespace zano_p2pool
