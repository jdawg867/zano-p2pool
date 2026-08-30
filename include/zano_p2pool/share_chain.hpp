#pragma once

#include "zano_p2pool/share.hpp"
#include "zano_p2pool/share_validation.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
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

struct ConnectedShare {
    Share share{};
    ShareId id{};
    ChainWork cumulative_work{};
    std::optional<CandidateValidation> pow_validation;
};

class ShareChain {
public:
    // The default constructor preserves the existing synthetic/testing behavior
    // where callers may build arbitrary work branches with add_share_unchecked().
    // A production sidechain passes its canonical SidechainParameters explicitly;
    // submit_share() then enforces the branch-relative expected difficulty.
    ShareChain() = default;
    explicit ShareChain(const SidechainParameters& sidechain_parameters);

    [[nodiscard]] AddShareResult submit_share(
        const Share& share,
        const ShareWorkContext& trusted_context,
        std::uint64_t now,
        ProgPowZContextMode mode = ProgPowZContextMode::Light);

    [[nodiscard]] AddShareResult add_share_unchecked(const Share& share);

    [[nodiscard]] const ConnectedShare* find(const ShareId& id) const noexcept;
    [[nodiscard]] const Share* find_orphan_share(const ShareId& id) const noexcept;
    [[nodiscard]] const ConnectedShare* best_tip() const noexcept;

    [[nodiscard]] bool contains(const ShareId& id) const noexcept;
    [[nodiscard]] bool is_orphan(const ShareId& id) const noexcept;
    [[nodiscard]] bool is_on_best_chain(const ShareId& id) const noexcept;
    [[nodiscard]] bool is_stale(const ShareId& id) const noexcept;

    [[nodiscard]] std::size_t connected_size() const noexcept;
    [[nodiscard]] std::size_t orphan_size() const noexcept;

    [[nodiscard]] bool enforces_sidechain_difficulty() const noexcept {
        return difficulty_policy_.has_value();
    }

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
};

[[nodiscard]] const char* share_disposition_name(
    ShareDisposition disposition) noexcept;
[[nodiscard]] const char* share_reject_reason_name(
    ShareRejectReason reason) noexcept;

}  // namespace zano_p2pool
