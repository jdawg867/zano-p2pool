#include "zano_p2pool/historical_work.hpp"
#include <stdexcept>

namespace zano_p2pool {
HistoricalParentResult audit_historical_parent(
    const P2pMiningContextProposal& proposal,
    const std::function<RpcCanonicalHeader(std::uint64_t)>& lookup) {
    if (proposal.zano_height == 0)
        throw std::runtime_error("historical work has no parent height");
    HistoricalParentResult result;
    result.mining_header_hash = validate_p2p_mining_context_structure(proposal);
    const auto height = proposal.zano_height - 1;
    const auto first = lookup(height);
    const auto second = lookup(height);
    if (first.height != height || second.height != height ||
        first.hash == Hash256{} || second.hash == Hash256{})
        throw std::runtime_error("invalid canonical parent lookup");
    if (first.hash != second.hash) {
        result.status = HistoricalParentStatus::ParentChangedDuringCheck;
    } else if (first.hash == proposal.prev_hash) {
        result.status = HistoricalParentStatus::ParentMatchedUntrusted;
    }
    return result;
}
}
