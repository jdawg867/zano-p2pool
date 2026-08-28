#pragma once

#include "zano_p2pool/share.hpp"

#include <array>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace zano_p2pool {

using ChainWork = std::array<std::uint8_t, 32>;

[[nodiscard]] ChainWork share_work(const Difficulty128& difficulty);
[[nodiscard]] std::string chain_work_hex(const ChainWork& work);
[[nodiscard]] bool add_chain_work_checked(
    const ChainWork& left,
    const ChainWork& right,
    ChainWork& result) noexcept;

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
    InvalidRootHeight,
    InvalidNonRootHeight,
    ParentHeightMismatch,
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
};

// In-memory structural share-chain core. At this checkpoint add_share() enforces
// canonical identity, parent/height rules, duplicate/orphan handling and checked
// cumulative work. The next Milestone 0.3 checkpoint adds mandatory ProgPoWZ
// admission so unverified claimed difficulty cannot contribute chain work.
class ShareChain {
public:
    [[nodiscard]] AddShareResult add_share(const Share& share);

    [[nodiscard]] const ConnectedShare* find(const ShareId& id) const noexcept;
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
    };

    [[nodiscard]] bool better_tip(
        const ConnectedShare& candidate,
        const ConnectedShare& current) const noexcept;
    [[nodiscard]] ShareRejectReason structural_reject_reason(
        const Share& share) const noexcept;
    [[nodiscard]] ShareRejectReason connect_share(
        const Share& share,
        const ShareId& id,
        bool& best_tip_changed);
    void promote_children(
        const ShareId& parent_id,
        std::size_t& promoted_orphans,
        bool& best_tip_changed);

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
