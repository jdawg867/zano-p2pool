#include "zano_p2pool/share_chain.hpp"
#include "test_check.hpp"

#include <algorithm>
#include <cstdint>
#include <string>

namespace {

zano_p2pool::Share make_share(
    std::string_view share_difficulty,
    std::uint64_t nonce) {
    using namespace zano_p2pool;

    Share share;
    share.timestamp = 1'700'000'000 + nonce;
    share.zano_height = 165014;
    share.nonce = nonce;
    share.share_difficulty = difficulty128_from_decimal(share_difficulty);
    share.network_difficulty = difficulty128_from_decimal("1229990");
    share.miner_id[31] = static_cast<std::uint8_t>(nonce & 0xffU);
    return share;
}

zano_p2pool::Share make_child(
    const zano_p2pool::Share& parent,
    std::string_view share_difficulty,
    std::uint64_t nonce) {
    auto child = make_share(share_difficulty, nonce);
    child.parent_id = zano_p2pool::share_id(parent);
    child.share_height = parent.share_height + 1;
    child.timestamp = parent.timestamp + 1;
    return child;
}

}  // namespace

int main() {
    using namespace zano_p2pool;

    // Per-share work is exactly the integer share difficulty, represented as
    // an unsigned 256-bit big-endian value.
    const ChainWork work100m =
        share_work(difficulty128_from_decimal("100000000"));
    CHECK(chain_work_hex(work100m) ==
          "0000000000000000000000000000000000000000000000000000000005f5e100");

    const ChainWork work10 = share_work(difficulty128_from_decimal("10"));
    const ChainWork work20 = share_work(difficulty128_from_decimal("20"));
    ChainWork work30{};
    CHECK(add_chain_work_checked(work10, work20, work30));
    CHECK(chain_work_hex(work30) ==
          "000000000000000000000000000000000000000000000000000000000000001e");

    ChainWork max_work{};
    max_work.fill(0xff);
    ChainWork overflow_result{};
    const ChainWork work1 = share_work(difficulty128_from_decimal("1"));
    CHECK(!add_chain_work_checked(max_work, work1, overflow_result));

    // Orphans are retained but cannot affect the active tip until their parent
    // arrives. Parent insertion deterministically promotes descendants.
    ShareChain chain;
    Share root = make_share("10", 1);
    const ShareId root_id = share_id(root);
    Share child = make_child(root, "20", 2);
    const ShareId child_id = share_id(child);

    const AddShareResult orphan_result = chain.add_share(child);
    CHECK(orphan_result.disposition == ShareDisposition::Orphan);
    CHECK(chain.orphan_size() == 1);
    CHECK(chain.connected_size() == 0);
    CHECK(chain.is_orphan(child_id));
    CHECK(chain.best_tip() == nullptr);

    const AddShareResult duplicate_orphan = chain.add_share(child);
    CHECK(duplicate_orphan.disposition == ShareDisposition::Duplicate);

    const AddShareResult root_result = chain.add_share(root);
    CHECK(root_result.disposition == ShareDisposition::Connected);
    CHECK(root_result.promoted_orphans == 1);
    CHECK(root_result.best_tip_changed);
    CHECK(chain.connected_size() == 2);
    CHECK(chain.orphan_size() == 0);
    CHECK(chain.contains(root_id));
    CHECK(chain.contains(child_id));
    CHECK(chain.best_tip() != nullptr);
    CHECK(chain.best_tip()->id == child_id);
    CHECK(chain_work_hex(chain.best_tip()->cumulative_work) ==
          "000000000000000000000000000000000000000000000000000000000000001e");
    CHECK(chain.is_on_best_chain(root_id));
    CHECK(chain.is_on_best_chain(child_id));
    CHECK(!chain.is_stale(root_id));

    const AddShareResult duplicate_connected = chain.add_share(root);
    CHECK(duplicate_connected.disposition == ShareDisposition::Duplicate);

    // A lower-work sibling is connected and retained, but explicitly stale.
    Share sibling = make_child(root, "5", 3);
    const ShareId sibling_id = share_id(sibling);
    const AddShareResult sibling_result = chain.add_share(sibling);
    CHECK(sibling_result.disposition == ShareDisposition::Connected);
    CHECK(!sibling_result.best_tip_changed);
    CHECK(chain.best_tip()->id == child_id);
    CHECK(chain.is_stale(sibling_id));
    CHECK(!chain.is_on_best_chain(sibling_id));

    // Extending the stale branch with enough verified work causes a reorg.
    Share sibling_tip = make_child(sibling, "21", 4);
    const ShareId sibling_tip_id = share_id(sibling_tip);
    const AddShareResult sibling_tip_result = chain.add_share(sibling_tip);
    CHECK(sibling_tip_result.disposition == ShareDisposition::Connected);
    CHECK(sibling_tip_result.best_tip_changed);
    CHECK(chain.best_tip()->id == sibling_tip_id);
    CHECK(chain_work_hex(chain.best_tip()->cumulative_work) ==
          "0000000000000000000000000000000000000000000000000000000000000024");
    CHECK(chain.is_stale(child_id));
    CHECK(chain.is_on_best_chain(sibling_id));

    // Root/non-root height rules fail closed.
    Share bad_root = make_share("1", 10);
    bad_root.share_height = 1;
    const AddShareResult bad_root_result = chain.add_share(bad_root);
    CHECK(bad_root_result.disposition == ShareDisposition::Rejected);
    CHECK(bad_root_result.reject_reason == ShareRejectReason::InvalidRootHeight);

    Share bad_non_root = make_share("1", 11);
    bad_non_root.parent_id = root_id;
    bad_non_root.share_height = 0;
    const AddShareResult bad_non_root_result = chain.add_share(bad_non_root);
    CHECK(bad_non_root_result.disposition == ShareDisposition::Rejected);
    CHECK(bad_non_root_result.reject_reason ==
          ShareRejectReason::InvalidNonRootHeight);

    Share bad_height = make_child(root, "1", 12);
    bad_height.share_height = 2;
    const AddShareResult bad_height_result = chain.add_share(bad_height);
    CHECK(bad_height_result.disposition == ShareDisposition::Rejected);
    CHECK(bad_height_result.reject_reason == ShareRejectReason::ParentHeightMismatch);

    Share zero_share_difficulty = make_share("1", 13);
    zero_share_difficulty.share_difficulty = {};
    const AddShareResult zero_share_result = chain.add_share(zero_share_difficulty);
    CHECK(zero_share_result.disposition == ShareDisposition::Rejected);
    CHECK(zero_share_result.reject_reason ==
          ShareRejectReason::ZeroShareDifficulty);

    Share zero_network_difficulty = make_share("1", 14);
    zero_network_difficulty.network_difficulty = {};
    const AddShareResult zero_network_result = chain.add_share(zero_network_difficulty);
    CHECK(zero_network_result.disposition == ShareDisposition::Rejected);
    CHECK(zero_network_result.reject_reason ==
          ShareRejectReason::ZeroNetworkDifficulty);

    // If an orphan's claimed height is inconsistent, it is discarded when the
    // missing parent arrives rather than becoming connected.
    ShareChain orphan_height_chain;
    Share late_parent = make_share("10", 20);
    Share bad_orphan = make_child(late_parent, "10", 21);
    bad_orphan.share_height = 2;
    const ShareId bad_orphan_id = share_id(bad_orphan);
    CHECK(orphan_height_chain.add_share(bad_orphan).disposition ==
          ShareDisposition::Orphan);
    const AddShareResult late_parent_result =
        orphan_height_chain.add_share(late_parent);
    CHECK(late_parent_result.disposition == ShareDisposition::Connected);
    CHECK(late_parent_result.promoted_orphans == 0);
    CHECK(!orphan_height_chain.is_orphan(bad_orphan_id));
    CHECK(!orphan_height_chain.contains(bad_orphan_id));

    // Equal cumulative work and equal height are broken by the smallest share
    // ID, independent of arrival order.
    Share tie_a = make_share("50", 30);
    Share tie_b = make_share("50", 31);
    const ShareId tie_a_id = share_id(tie_a);
    const ShareId tie_b_id = share_id(tie_b);
    const ShareId expected_tie_tip = std::min(tie_a_id, tie_b_id);

    ShareChain tie_chain;
    const Share& first = tie_a_id > tie_b_id ? tie_a : tie_b;
    const Share& second = tie_a_id > tie_b_id ? tie_b : tie_a;
    CHECK(tie_chain.add_share(first).disposition == ShareDisposition::Connected);
    CHECK(tie_chain.add_share(second).disposition == ShareDisposition::Connected);
    CHECK(tie_chain.best_tip() != nullptr);
    CHECK(tie_chain.best_tip()->id == expected_tie_tip);

    CHECK(std::string(share_disposition_name(ShareDisposition::Connected)) ==
          "connected");
    CHECK(std::string(share_reject_reason_name(
              ShareRejectReason::ParentHeightMismatch)) ==
          "parent-height-mismatch");

    return 0;
}
