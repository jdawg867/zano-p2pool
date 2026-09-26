#pragma once

#include "zano_p2pool/share.hpp"
#include "zano_p2pool/share_validation.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace zano_p2pool {

struct SidechainParameters;

using ChainWork = std::array<std::uint8_t, 32>;

inline constexpr std::uint64_t kShareMaxFutureSeconds = 60;
inline constexpr std::uint64_t kShareMaxParentBackstepSeconds = 60;

[[nodiscard]] ChainWork share_work(const Difficulty128& difficulty);
[[nodiscard]] std::string chain_work_hex(const ChainWork& work);
[[nodiscard]] bool add_chain_work_checked(
    const ChainWork& left,
    const ChainWork& right,
    ChainWork& result) noexcept;

struct ShareWorkContext {
    std::uint64_t zano_height{0};
    Hash256 mining_header_hash{};
    Difficulty128 network_difficulty{};
};

enum class ShareDisposition {
    Connected,
    Orphan,
    Duplicate,
    Rejected,
};

enum class ShareRejectReason {
    None,
    UnexpectedShareVersion,
    ZeroShareDifficulty,
    ZeroNetworkDifficulty,
    ShareDifficultyAboveNetwork,
    UnexpectedShareDifficulty,
    InvalidRootHeight,
    InvalidNonRootHeight,
    ParentHeightMismatch,
    TimestampTooFarFuture,
    TimestampBeforeParentTolerance,
    ZanoHeightMismatch,
    MiningHeaderMismatch,
    NetworkDifficultyMismatch,
    PowBackendUnavailable,
    InvalidPow,
    CumulativeWorkOverflow,
};

struct AddShareResult {
    ShareDisposition disposition{ShareDisposition::Rejected};
    ShareRejectReason reject_reason{ShareRejectReason::None};
    ShareId id{};
    std::size_t promoted_orphans{0};
    bool best_tip_changed{false};
};

enum class RevalidateShareStatus : std::uint8_t {
    Validated,
    AlreadyValidated,
    NotConnected,
    ParentUnvalidated,
    Rejected,
};

struct RevalidateShareResult {
    RevalidateShareStatus status{RevalidateShareStatus::NotConnected};
    ShareRejectReason reject_reason{ShareRejectReason::None};
    ShareId id{};
};

// Exact validation state that may be exported to or restored from a separately
// authenticated/authorized replay-validation snapshot. This structure carries
// no trust by itself; callers must establish snapshot authority before restore.
struct ReplayValidationRecord {
    ShareId share_id{};
    CandidateValidation validation{};
};

struct ConnectedShare {
    Share share{};
    ShareId id{};
    ChainWork cumulative_work{};
    std::optional<CandidateValidation> pow_validation;
    // True only when this share and every ancestor entered through submit_share
    // with configured difficulty enforcement and caller-supplied trusted work.
    // Unchecked/replayed records never acquire this flag merely by connecting.
    bool validated_ancestry{false};
};

class ShareChain {
public:
    // The default constructor preserves the existing synthetic/testing behavior
    // where callers may build arbitrary work branches with add_share_unchecked().
    // A production sidechain passes its canonical SidechainParameters explicitly;
    // submit_share() then enforces the configured share-version range and the
    // branch-relative expected difficulty.
    ShareChain() = default;
    explicit ShareChain(const SidechainParameters& sidechain_parameters);

    [[nodiscard]] AddShareResult submit_share(
        const Share& share,
        const ShareWorkContext& trusted_context,
        std::uint64_t now,
        ProgPowZContextMode mode = ProgPowZContextMode::Light);

    [[nodiscard]] AddShareResult add_share_unchecked(const Share& share);

    // Re-run production admission checks for an already-connected record that
    // entered through unchecked persistence replay. This API does not establish
    // mining-work trust by itself: trusted_context must already have crossed an
    // independent trust boundary. A non-root share can only be upgraded after
    // its exact parent has validated ancestry.
    [[nodiscard]] RevalidateShareResult revalidate_connected_share(
        const ShareId& id,
        const ShareWorkContext& trusted_context,
        std::uint64_t now,
        ProgPowZContextMode mode = ProgPowZContextMode::Light);

    // Export exact cached validation state for every connected share whose
    // complete ancestry is already locally validated. The returned records are
    // deterministic parent-first order: share_height, then ShareId.
    //
    // This exports cache state only. Canonical Zano checkpoint authority is
    // intentionally owned by the separate replay-validation store/runtime
    // boundary.
    [[nodiscard]] std::vector<ReplayValidationRecord>
    replay_validation_snapshot() const;

    // Restore validation state only after the caller has independently
    // authorized the containing durable snapshot. Input order is irrelevant:
    // records are resolved to exact connected ShareIds, fully preflighted, then
    // applied parent-first without recomputing ProgPoWZ.
    //
    // The operation is fail-closed and mutation-atomic: an unknown ShareId,
    // duplicate record, inconsistent CandidateValidation, missing/unvalidated
    // parent, current sidechain-policy mismatch, or conflicting already-
    // validated state throws before any record is changed.
    //
    // Returns the number of previously-unvalidated connected shares upgraded.
    // Reapplying an identical snapshot is idempotent and returns zero.
    [[nodiscard]] std::size_t restore_replay_validation(
        std::span<const ReplayValidationRecord> records);

    [[nodiscard]] const ConnectedShare* find(const ShareId& id) const noexcept;
    [[nodiscard]] const Share* find_orphan_share(const ShareId& id) const noexcept;
    [[nodiscard]] const ConnectedShare* best_tip() const noexcept;

    [[nodiscard]] bool contains(const ShareId& id) const noexcept;
    [[nodiscard]] bool is_orphan(const ShareId& id) const noexcept;
    [[nodiscard]] bool is_on_best_chain(const ShareId& id) const noexcept;
    [[nodiscard]] bool is_stale(const ShareId& id) const noexcept;

    [[nodiscard]] std::size_t connected_size() const noexcept;
    [[nodiscard]] std::size_t orphan_size() const noexcept;
    // Snapshot of currently connected IDs. Callers that need ancestry order
    // must explicitly sort by share_height; map key order has no consensus
    // meaning.
    [[nodiscard]] std::vector<ShareId> connected_share_ids() const;

    // Deterministic snapshot of every record that should survive durable
    // persistence: currently connected shares plus unresolved orphans.
    //
    // Records are ordered by share height and then ShareId. Replay does not
    // require parent-first order, but deterministic ordering makes rewrites
    // stable and normally places connected parents before descendants.
    [[nodiscard]] std::vector<Share> persistence_snapshot() const;

    // Remove one connected share and every connected descendant from the
    // active in-memory chain, then deterministically select the best remaining
    // tip. Returns the exact removed ShareIds in deterministic ShareId order;
    // an unknown root returns an empty vector.
    [[nodiscard]] std::vector<ShareId> prune_connected_subtree_ids(
        const ShareId& root_id);

    // Count-only compatibility wrapper for callers that do not need the exact
    // removed identities.
    [[nodiscard]] std::size_t prune_connected_subtree(
        const ShareId& root_id);

    [[nodiscard]] bool enforces_sidechain_difficulty() const noexcept {
        return difficulty_policy_.has_value();
    }

    [[nodiscard]] bool matches_sidechain_parameters(const SidechainParameters& params) const;

    // Expected difficulty for a new share extending the currently selected tip.
    // On an empty chain this returns the configured minimum, capped by the
    // current Zano network difficulty.
    [[nodiscard]] Difficulty128 expected_next_share_difficulty(
        const Difficulty128& network_difficulty) const;

    // Expected difficulty for a child of an explicit connected parent. A zero
    // parent id denotes a root share and therefore an empty history.
    [[nodiscard]] Difficulty128 expected_child_share_difficulty(
        const ShareId& parent_id,
        const Difficulty128& network_difficulty) const;

private:
    struct DifficultyPolicy {
        std::uint8_t minimum_share_version{0};
        std::uint8_t maximum_share_version{0};
        std::uint64_t target_share_seconds{0};
        std::uint64_t minimum_share_difficulty{0};
        std::uint64_t difficulty_window_shares{0};
    };

    struct OrphanShare {
        Share share{};
        ShareId id{};
        std::optional<CandidateValidation> pow_validation;
        bool enforce_sidechain_difficulty{false};
    };

    [[nodiscard]] bool better_tip(
        const ConnectedShare& candidate,
        const ConnectedShare& current) const noexcept;
    [[nodiscard]] ShareRejectReason structural_reject_reason(
        const Share& share) const noexcept;
    [[nodiscard]] ShareRejectReason trusted_context_reject_reason(
        const Share& share,
        const ShareWorkContext& trusted_context) const noexcept;
    [[nodiscard]] ShareRejectReason absolute_timestamp_reject_reason(
        const Share& share,
        std::uint64_t now) const noexcept;
    [[nodiscard]] ShareRejectReason parent_timestamp_reject_reason(
        const Share& share,
        const Share& parent) const noexcept;
    [[nodiscard]] ShareRejectReason expected_difficulty_reject_reason(
        const Share& share) const;
    [[nodiscard]] ShareRejectReason connect_share(
        const Share& share,
        const ShareId& id,
        std::optional<CandidateValidation> pow_validation,
        bool enforce_sidechain_difficulty,
        bool& best_tip_changed);
    void promote_children(
        const ShareId& parent_id,
        std::size_t& promoted_orphans,
        bool& best_tip_changed);
    [[nodiscard]] AddShareResult add_prevalidated_share(
        const Share& share,
        std::optional<CandidateValidation> pow_validation,
        bool enforce_sidechain_difficulty);

    std::map<ShareId, ConnectedShare> connected_;
    std::map<ShareId, OrphanShare> orphans_;
    std::map<ShareId, std::vector<ShareId>> orphans_by_parent_;
    std::optional<ShareId> best_tip_id_;
    std::optional<DifficultyPolicy> difficulty_policy_;
    std::optional<Hash256> parameter_id_;
};

[[nodiscard]] const char* share_disposition_name(
    ShareDisposition disposition) noexcept;
[[nodiscard]] const char* revalidate_share_status_name(
    RevalidateShareStatus status) noexcept;
[[nodiscard]] const char* share_reject_reason_name(
    ShareRejectReason reason) noexcept;

}  // namespace zano_p2pool
