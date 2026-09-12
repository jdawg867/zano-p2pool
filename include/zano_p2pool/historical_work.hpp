#pragma once

#include "zano_p2pool/p2p_mining_context.hpp"
#include "zano_p2pool/rpc_client.hpp"
#include <functional>

namespace zano_p2pool {
enum class HistoricalParentStatus {
    ParentMatchedUntrusted,
    ParentMismatch,
    ParentChangedDuringCheck,
};
struct HistoricalParentResult {
    HistoricalParentStatus status{HistoricalParentStatus::ParentMismatch};
    Hash256 mining_header_hash{};
};
// The lookup must query the operator's local daemon by height, never by a
// peer-supplied hash. Two samples detect a change during this check; they do
// not establish permanent finality or authorize trusted-work insertion.
[[nodiscard]] HistoricalParentResult audit_historical_parent(
    const P2pMiningContextProposal& proposal,
    const std::function<RpcCanonicalHeader(std::uint64_t)>& lookup);
}
