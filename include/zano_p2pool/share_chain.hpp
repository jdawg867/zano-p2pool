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

private:
    struct OrphanShare {
        Share share{};
        ShareId id{};
        std::optional<CandidateValidation> pow_validation;
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
    [[nodiscard]] ShareRejectReason connect_share(
        const Share& share,
        const ShareId& id,
        std::optional<CandidateValidation> pow_validation,
        bool& best_tip_changed);
    void promote_children(
        const ShareId& parent_id,
        std::size_t& promoted_orphans,
        bool& best_tip_changed);
    [[nodiscard]] AddShareResult add_prevalidated_share(
        const Share& share,
        std::optional<CandidateValidation> pow_validation);

    std::map<ShareId, ConnectedShare> connected_;
    std::map<ShareId, OrphanShare> orphans_;
    std::map<ShareId, std::vector<ShareId>> orphans_by_parent_;
    std::optional<ShareId> best_tip_id_;
};

[[nodiscard]] const char* share_disposition_name(
    ShareDisposition disposition) noexcept;
[[nodiscard]] const char* share_reject_reason_name(
    ShareRejectReason reason) noexcept;

}  // namespace zano_p2pool
