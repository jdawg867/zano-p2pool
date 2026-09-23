#pragma once

#include "zano_p2pool/mining_work_archive.hpp"
#include "zano_p2pool/restart_revalidation.hpp"
#include "zano_p2pool/share_chain.hpp"
#include "zano_p2pool/sidechain_params.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>

namespace zano_p2pool {

struct RestartRecoveryResult {
    std::size_t archive_records{0};
    std::size_t connected_considered{0};
    std::size_t revalidated{0};
    std::size_t already_validated{0};
    std::size_t missing_local_work{0};
    std::size_t parent_unvalidated{0};
    std::size_t rejected{0};

    // Connected structural replay records removed because stable canonical
    // Zano history proved a replay root referenced a stale parent.
    std::size_t pruned_connected_shares{0};
};

// Revalidates connected persistence-replay history in explicit parent-first
// order. The archive is completely enumerated and structurally checked before
// any share state is mutated. Work matching is exact on Zano height, locally
// derived mining-header hash and network difficulty.
//
// Shares without an exact locally archived proposal remain unvalidated.
// Descendants whose parents cannot be revalidated remain unvalidated. Expected
// trust-crossing failures are counted and do not authorize any fallback to a
// best tip, a different work template, or a persisted validation flag.
//
// Archive/format/RPC exceptions are deliberately propagated. A caller may
// continue running after reporting the failure; every share that did not
// individually complete revalidate_recovered_share() remains fail-closed.
[[nodiscard]] RestartRecoveryResult recover_replayed_history(
    ShareChain& chain,
    const SidechainParameters& params,
    const MiningWorkArchive& local_archive,
    const std::function<RpcCanonicalHeader(std::uint64_t)>& lookup,
    std::uint64_t now,
    ProgPowZContextMode mode = ProgPowZContextMode::Light,
    std::size_t max_archive_records = 10000);

}  // namespace zano_p2pool
