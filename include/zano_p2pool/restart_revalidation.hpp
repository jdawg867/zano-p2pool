#pragma once

#include "zano_p2pool/historical_work.hpp"
#include "zano_p2pool/mining_work_archive.hpp"
#include "zano_p2pool/share_chain.hpp"
#include "zano_p2pool/sidechain_params.hpp"

#include <cstdint>
#include <functional>
#include <span>

namespace zano_p2pool {

enum class RestartRevalidationStatus : std::uint8_t {
    Revalidated,
    AlreadyValidated,
    ShareMissing,
    ParameterMismatch,
    CandidateMismatch,
    ParentUnvalidated,
    AnchorRejected,
    AnchorChangedBeforeRevalidation,
    ShareRejected,
};

struct RestartRevalidationResult {
    RestartRevalidationStatus status{
        RestartRevalidationStatus::ShareMissing};
    ShareId share_id{};
    HistoricalAnchorResult initial_anchor{};
    HistoricalAnchorResult final_anchor{};
    RevalidateShareResult share_result{};
};

// Restart-only trust crossing for one share that was structurally replayed from
// ShareStore with validated_ancestry=false.
//
// This API never trusts a persisted validation flag and never inserts work into
// P2pTrustedWorkRegistry. It reconstructs only the ShareWorkContext required to
// rerun sidechain difficulty and ProgPoWZ validation from an exact locally
// archived proposal plus fresh local-daemon anchoring.
//
// The exact proposal is read back from this node's MiningWorkArchive by
// content ID inside the crossing; callers cannot substitute arbitrary proposal
// bytes. local_observations must be the complete observation set loaded from
// that same archive so conflicting local observations remain fail-closed.
//
// The archived proposal's miner transaction/payout policy remain outside this
// authority. P2P trusted-work promotion still uses the normal historical trust
// crossing.
[[nodiscard]] RestartRevalidationResult revalidate_recovered_share(
    ShareChain& chain,
    const SidechainParameters& params,
    const ShareId& share_id,
    const MiningWorkArchive& local_archive,
    const P2pMiningContextId& proposal_id,
    std::span<const P2pMiningAnchor> local_observations,
    const std::function<RpcCanonicalHeader(std::uint64_t)>& lookup,
    std::uint64_t now,
    ProgPowZContextMode mode = ProgPowZContextMode::Light);

[[nodiscard]] const char* restart_revalidation_status_name(
    RestartRevalidationStatus status) noexcept;

}  // namespace zano_p2pool
