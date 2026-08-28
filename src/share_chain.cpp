#include "zano_p2pool/share_chain.hpp"

#include <algorithm>
#include <stdexcept>

namespace zano_p2pool {

ChainWork share_work(const Difficulty128& difficulty) {
    if (difficulty128_is_zero(difficulty)) {
        throw std::invalid_argument("share difficulty must be greater than zero");
    }

    ChainWork work{};
    std::copy(
        difficulty.begin(),
        difficulty.end(),
        work.begin() + static_cast<std::ptrdiff_t>(work.size() - difficulty.size()));
    return work;
}

std::string chain_work_hex(const ChainWork& work) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result(work.size() * 2, '0');
    for (std::size_t i = 0; i < work.size(); ++i) {
        result[i * 2] = digits[work[i] >> 4];
        result[i * 2 + 1] = digits[work[i] & 0x0f];
    }
    return result;
}

bool add_chain_work_checked(
    const ChainWork& left,
    const ChainWork& right,
    ChainWork& result) noexcept {
    unsigned carry = 0;
    for (std::size_t i = left.size(); i-- > 0;) {
        const unsigned sum =
            static_cast<unsigned>(left[i]) +
            static_cast<unsigned>(right[i]) + carry;
        result[i] = static_cast<std::uint8_t>(sum & 0xffU);
        carry = sum >> 8;
    }
    return carry == 0;
}

ShareRejectReason ShareChain::structural_reject_reason(
    const Share& share) const noexcept {
    if (difficulty128_is_zero(share.share_difficulty)) {
        return ShareRejectReason::ZeroShareDifficulty;
    }
    if (difficulty128_is_zero(share.network_difficulty)) {
        return ShareRejectReason::ZeroNetworkDifficulty;
    }

    if (is_zero_share_id(share.parent_id)) {
        if (share.share_height != 0) {
            return ShareRejectReason::InvalidRootHeight;
        }
    } else if (share.share_height == 0) {
        return ShareRejectReason::InvalidNonRootHeight;
    }

    return ShareRejectReason::None;
}

bool ShareChain::better_tip(
    const ConnectedShare& candidate,
    const ConnectedShare& current) const noexcept {
    if (candidate.cumulative_work != current.cumulative_work) {
        return candidate.cumulative_work > current.cumulative_work;
    }
    if (candidate.share.share_height != current.share.share_height) {
        return candidate.share.share_height > current.share.share_height;
    }
    return candidate.id < current.id;
}

ShareRejectReason ShareChain::connect_share(
    const Share& share,
    const ShareId& id,
    bool& best_tip_changed) {
    ChainWork cumulative = share_work(share.share_difficulty);

    if (!is_zero_share_id(share.parent_id)) {
        const auto parent_it = connected_.find(share.parent_id);
        if (parent_it == connected_.end()) {
            throw std::logic_error("connect_share called without a connected parent");
        }

        const ConnectedShare& parent = parent_it->second;
        if (parent.share.share_height == UINT64_MAX ||
            share.share_height != parent.share.share_height + 1) {
            return ShareRejectReason::ParentHeightMismatch;
        }

        ChainWork summed{};
        if (!add_chain_work_checked(
                parent.cumulative_work,
                cumulative,
                summed)) {
            return ShareRejectReason::CumulativeWorkOverflow;
        }
        cumulative = summed;
    }

    const auto [it, inserted] = connected_.emplace(
        id,
        ConnectedShare{share, id, cumulative});
    if (!inserted) {
        throw std::logic_error("duplicate share reached connect_share");
    }

    if (!best_tip_id_) {
        best_tip_id_ = id;
        best_tip_changed = true;
    } else {
        const auto best_it = connected_.find(*best_tip_id_);
        if (best_it == connected_.end()) {
            throw std::logic_error("best-tip invariant failed");
        }
        if (better_tip(it->second, best_it->second)) {
            best_tip_id_ = id;
            best_tip_changed = true;
        }
    }

    return ShareRejectReason::None;
}

void ShareChain::promote_children(
    const ShareId& parent_id,
    std::size_t& promoted_orphans,
    bool& best_tip_changed) {
    auto children_it = orphans_by_parent_.find(parent_id);
    if (children_it == orphans_by_parent_.end()) {
        return;
    }

    std::vector<ShareId> child_ids = std::move(children_it->second);
    orphans_by_parent_.erase(children_it);
    std::sort(child_ids.begin(), child_ids.end());

    for (const ShareId& child_id : child_ids) {
        auto orphan_it = orphans_.find(child_id);
        if (orphan_it == orphans_.end()) {
            continue;
        }

        const Share child = orphan_it->second.share;
        const ShareRejectReason reason =
            connect_share(child, child_id, best_tip_changed);
        orphans_.erase(orphan_it);

        if (reason == ShareRejectReason::None) {
            ++promoted_orphans;
            promote_children(child_id, promoted_orphans, best_tip_changed);
        }
    }
}

AddShareResult ShareChain::add_share(const Share& share) {
    AddShareResult result;
    result.id = share_id(share);

    if (connected_.contains(result.id) || orphans_.contains(result.id)) {
        result.disposition = ShareDisposition::Duplicate;
        return result;
    }

    result.reject_reason = structural_reject_reason(share);
    if (result.reject_reason != ShareRejectReason::None) {
        result.disposition = ShareDisposition::Rejected;
        return result;
    }

    if (!is_zero_share_id(share.parent_id) &&
        !connected_.contains(share.parent_id)) {
        orphans_.emplace(result.id, OrphanShare{share, result.id});
        orphans_by_parent_[share.parent_id].push_back(result.id);
        result.disposition = ShareDisposition::Orphan;
        return result;
    }

    const std::optional<ShareId> old_best_tip = best_tip_id_;
    result.reject_reason =
        connect_share(share, result.id, result.best_tip_changed);
    if (result.reject_reason != ShareRejectReason::None) {
        result.disposition = ShareDisposition::Rejected;
        return result;
    }

    result.disposition = ShareDisposition::Connected;
    promote_children(
        result.id,
        result.promoted_orphans,
        result.best_tip_changed);
    result.best_tip_changed = old_best_tip != best_tip_id_;
    return result;
}

const ConnectedShare* ShareChain::find(const ShareId& id) const noexcept {
    const auto it = connected_.find(id);
    return it == connected_.end() ? nullptr : &it->second;
}

const ConnectedShare* ShareChain::best_tip() const noexcept {
    if (!best_tip_id_) {
        return nullptr;
    }
    return find(*best_tip_id_);
}

bool ShareChain::contains(const ShareId& id) const noexcept {
    return connected_.contains(id);
}

bool ShareChain::is_orphan(const ShareId& id) const noexcept {
    return orphans_.contains(id);
}

bool ShareChain::is_on_best_chain(const ShareId& id) const noexcept {
    const ConnectedShare* current = best_tip();
    while (current != nullptr) {
        if (current->id == id) {
            return true;
        }
        if (is_zero_share_id(current->share.parent_id)) {
            break;
        }
        current = find(current->share.parent_id);
    }
    return false;
}

bool ShareChain::is_stale(const ShareId& id) const noexcept {
    return contains(id) && !is_on_best_chain(id);
}

std::size_t ShareChain::connected_size() const noexcept {
    return connected_.size();
}

std::size_t ShareChain::orphan_size() const noexcept {
    return orphans_.size();
}

const char* share_disposition_name(ShareDisposition disposition) noexcept {
    switch (disposition) {
    case ShareDisposition::Connected:
        return "connected";
    case ShareDisposition::Orphan:
        return "orphan";
    case ShareDisposition::Duplicate:
        return "duplicate";
    case ShareDisposition::Rejected:
        return "rejected";
    }
    return "unknown";
}

const char* share_reject_reason_name(ShareRejectReason reason) noexcept {
    switch (reason) {
    case ShareRejectReason::None:
        return "none";
    case ShareRejectReason::ZeroShareDifficulty:
        return "zero-share-difficulty";
    case ShareRejectReason::ZeroNetworkDifficulty:
        return "zero-network-difficulty";
    case ShareRejectReason::InvalidRootHeight:
        return "invalid-root-height";
    case ShareRejectReason::InvalidNonRootHeight:
        return "invalid-non-root-height";
    case ShareRejectReason::ParentHeightMismatch:
        return "parent-height-mismatch";
    case ShareRejectReason::CumulativeWorkOverflow:
        return "cumulative-work-overflow";
    }
    return "unknown";
}

}  // namespace zano_p2pool
