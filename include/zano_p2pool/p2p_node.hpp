#pragma once

#include "zano_p2pool/p2p_runtime.hpp"
#include "zano_p2pool/p2p_share.hpp"
#include "zano_p2pool/p2p_sync.hpp"
#include "zano_p2pool/p2p_tip.hpp"

#include <cstdint>
#include <mutex>

namespace zano_p2pool {

enum class P2pNodeMessageStatus : std::uint8_t {
    ShareProcessed,
    ShareRequestAnswered,
    ShareResponseProcessed,
    TipProcessed,
    MiningContextDeferred,
    UnexpectedHandshake,
};

struct P2pNodeMessageResult {
    P2pNodeMessageStatus status{P2pNodeMessageStatus::UnexpectedHandshake};
    P2pShareReceiveStatus share_status{P2pShareReceiveStatus::Rejected};
    P2pShareSyncReceiveStatus sync_status{P2pShareSyncReceiveStatus::NotFound};
    P2pTipSyncStatus tip_status{P2pTipSyncStatus::NoRemoteTip};
    bool sent_followup{false};
};

// Runtime protocol dispatcher for share-chain synchronization. ShareChain and
// P2pTrustedWorkRegistry are protected by the same node-wide mutex so Stratum,
// P2P gossip and sync cannot mutate/read consensus-facing state concurrently.
// MiningContextAnnounce is deliberately deferred to the separate 6B.4 trust
// gate because it additionally requires a current local daemon anchor and
// expected payout identity.
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

    [[nodiscard]] std::size_t trusted_work_count() const noexcept;
    [[nodiscard]] std::size_t connected_share_count() const noexcept;
    [[nodiscard]] P2pTipHint local_tip() const noexcept;

private:
    ShareChain& chain_;
    P2pTrustedWorkRegistry& trusted_work_;
    std::mutex& state_mutex_;
};

[[nodiscard]] const char* p2p_node_message_status_name(
    P2pNodeMessageStatus status) noexcept;

}  // namespace zano_p2pool
