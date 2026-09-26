#pragma once

#include "zano_p2pool/mining_work_archive.hpp"
#include "zano_p2pool/replay_validation_store.hpp"
#include "zano_p2pool/restart_revalidation.hpp"
#include "zano_p2pool/share_chain.hpp"
#include "zano_p2pool/sidechain_params.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>

namespace zano_p2pool {

enum class RestartReplayValidationStatus : std::uint8_t {
    Missing,
    Restored,
    Stale,
};

struct RestartReplayValidationResult {
    RestartReplayValidationStatus status{
        RestartReplayValidationStatus::Missing};

    std::optional<ReplayValidationCheckpoint> checkpoint;

    std::size_t records_loaded{0};
    std::size_t records_restored{0};
};

// Attempt to reuse a durable validation snapshot before expensive historical
// replay recovery.
//
// The store itself is never authority. If present, its canonical Zano
// checkpoint must first match two stable reads from the caller-supplied LOCAL
// canonical-header source. Only then may ShareChain restore the exact cached
// validation records.
//
// Missing cache returns Missing. A stable canonical hash mismatch returns Stale
// without mutation. Malformed storage, unstable/wrong-height canonical
// evidence, parameter mismatch, or invalid ShareChain restore state throws.
// ShareChain restoration itself is preflighted and mutation-atomic.
[[nodiscard]] RestartReplayValidationResult
restore_replayed_validation_cache(
    ShareChain& chain,
    const SidechainParameters& params,
    const ReplayValidationStore& store,
    const std::function<RpcCanonicalHeader(std::uint64_t)>& lookup);

enum class RestartReplayValidationPersistStatus : std::uint8_t {
    Saved,
    CanonicalChanged,
    CheckpointTooOld,
};

struct RestartReplayValidationPersistResult {
    RestartReplayValidationPersistStatus status{
        RestartReplayValidationPersistStatus::Saved};

    std::optional<ReplayValidationCheckpoint> checkpoint;

    std::size_t records_exported{0};
    std::size_t records_saved{0};
};

// Persist currently validated replay state behind stable LOCAL canonical Zano
// authority.
//
// checkpoint_work_height is the mining/template height whose parent block is
// expected_parent_hash. The parent is independently read twice from the local
// canonical-header source before any durable write.
//
// Stable disagreement with expected_parent_hash returns CanonicalChanged and
// leaves the existing cache untouched.
//
// Every exported validated share must have a Zano work height no greater than
// checkpoint_work_height. Otherwise CheckpointTooOld is returned without
// replacing the cache. This prevents a checkpoint from claiming authority over
// validation state that depends on later work.
//
// The caller must serialize ShareChain access when used concurrently. Startup
// recovery is single-threaded and requires no external mutex.
//
// Store I/O failures and malformed/inconsistent validated state throw. The
// caller may treat this cache as an optimization and continue fail-closed
// without it.
[[nodiscard]] RestartReplayValidationPersistResult
persist_replayed_validation_cache(
    const ShareChain& chain,
    const SidechainParameters& params,
    ReplayValidationStore& store,
    std::uint64_t checkpoint_work_height,
    const Hash256& expected_parent_hash,
    const std::function<RpcCanonicalHeader(std::uint64_t)>& lookup);

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
