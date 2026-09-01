#include "zano_p2pool/crypto_hash.hpp"
#include "zano_p2pool/share_chain.hpp"
#include "zano_p2pool/sidechain_params.hpp"
#include "test_check.hpp"

#include <algorithm>
#include <cstdint>
#include <string>

namespace {

zano_p2pool::Hash256 hash_from_hex(std::string_view hex) {
    const auto bytes = zano_p2pool::hex_to_bytes(hex);
    CHECK(bytes.size() == 32);
    zano_p2pool::Hash256 hash{};
    std::copy(bytes.begin(), bytes.end(), hash.begin());
    return hash;
}

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

zano_p2pool::Share make_v2_share(
    std::string_view share_difficulty,
    std::uint64_t nonce) {
    using namespace zano_p2pool;

    Share share = make_share(share_difficulty, nonce);
    PayoutPublicKeys payout;
    payout.spend_public_key[0] = 0x02;
    payout.spend_public_key[31] = static_cast<std::uint8_t>(nonce & 0xffU);
    payout.view_public_key[0] = 0x03;
    payout.view_public_key[31] =
        static_cast<std::uint8_t>((nonce ^ 0x5aU) & 0xffU);
    share.version = kShareVersion2;
    share.payout = payout;
    share.miner_id = miner_id_from_payout(payout);
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

zano_p2pool::Share make_v2_child(
    const zano_p2pool::Share& parent,
    std::string_view share_difficulty,
    std::uint64_t nonce) {
    auto child = make_v2_share(share_difficulty, nonce);
    child.parent_id = zano_p2pool::share_id(parent);
    child.share_height = parent.share_height + 1;
    child.timestamp = parent.timestamp + 1;
    return child;
}

zano_p2pool::ShareWorkContext context_for(const zano_p2pool::Share& share) {
    return zano_p2pool::ShareWorkContext{
        share.zano_height,
        share.mining_header_hash,
        share.network_difficulty,
    };
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

    // A consensus-configured chain requires payout-capable share v2 and the
    // exact branch-relative target. Both checks occur before ProgPoWZ.
    SidechainParameters consensus_params = canonical_sidechain_parameters(
        SidechainParentNetwork::Testnet);
    consensus_params.minimum_share_difficulty = 10;
    consensus_params.target_share_seconds = 10;
    consensus_params.difficulty_window_shares = 20;

    ShareChain consensus_chain(consensus_params);
    CHECK(consensus_chain.enforces_sidechain_difficulty());
    const Difficulty128 consensus_network = difficulty128_from_decimal("1000");
    CHECK(consensus_chain.expected_next_share_difficulty(consensus_network) ==
          difficulty128_from_decimal("10"));

    Share legacy_root = make_share("10", 89);
    legacy_root.network_difficulty = consensus_network;
    const AddShareResult legacy_root_result = consensus_chain.submit_share(
        legacy_root,
        context_for(legacy_root),
        legacy_root.timestamp);
    CHECK(legacy_root_result.disposition == ShareDisposition::Rejected);
    CHECK(legacy_root_result.reject_reason ==
          ShareRejectReason::UnexpectedShareVersion);

    Share wrong_root = make_v2_share("9", 90);
    wrong_root.network_difficulty = consensus_network;
    const AddShareResult wrong_root_result = consensus_chain.submit_share(
        wrong_root,
        context_for(wrong_root),
        wrong_root.timestamp);
    CHECK(wrong_root_result.disposition == ShareDisposition::Rejected);
    CHECK(wrong_root_result.reject_reason ==
          ShareRejectReason::UnexpectedShareDifficulty);

    // Branch-relative retargeting follows the explicit parent, not the global
    // best tip. Synthetic unchecked insertion is used only to construct the two
    // deterministic histories without requiring PoW vectors for every sample.
    ShareChain fork_difficulty_chain(consensus_params);
    Share difficulty_root = make_v2_share("10", 100);
    difficulty_root.timestamp = 10'000;
    difficulty_root.network_difficulty = consensus_network;
    CHECK(fork_difficulty_chain.add_share_unchecked(difficulty_root).disposition ==
          ShareDisposition::Connected);

    Share steady_branch = make_v2_child(difficulty_root, "10", 101);
    steady_branch.timestamp = difficulty_root.timestamp + 10;
    steady_branch.network_difficulty = consensus_network;
    CHECK(fork_difficulty_chain.add_share_unchecked(steady_branch).disposition ==
          ShareDisposition::Connected);

    Share fast_branch = make_v2_child(difficulty_root, "10", 102);
    fast_branch.timestamp = difficulty_root.timestamp + 5;
    fast_branch.network_difficulty = consensus_network;
    CHECK(fork_difficulty_chain.add_share_unchecked(fast_branch).disposition ==
          ShareDisposition::Connected);

    CHECK(fork_difficulty_chain.expected_child_share_difficulty(
              share_id(steady_branch), consensus_network) ==
          difficulty128_from_decimal("10"));
    CHECK(fork_difficulty_chain.expected_child_share_difficulty(
              share_id(fast_branch), consensus_network) ==
          difficulty128_from_decimal("20"));

    // Orphans are retained but cannot affect the active tip until their parent
    // arrives. Parent insertion deterministically promotes descendants.
    ShareChain chain;
    Share root = make_share("10", 1);
    const ShareId root_id = share_id(root);
    Share child = make_child(root, "20", 2);
    const ShareId child_id = share_id(child);

    const AddShareResult orphan_result = chain.add_share_unchecked(child);
    CHECK(orphan_result.disposition == ShareDisposition::Orphan);
    CHECK(chain.orphan_size() == 1);
    CHECK(chain.connected_size() == 0);
    CHECK(chain.is_orphan(child_id));
    CHECK(chain.best_tip() == nullptr);

    const AddShareResult duplicate_orphan = chain.add_share_unchecked(child);
    CHECK(duplicate_orphan.disposition == ShareDisposition::Duplicate);

    const AddShareResult root_result = chain.add_share_unchecked(root);
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

    const AddShareResult duplicate_connected = chain.add_share_unchecked(root);
    CHECK(duplicate_connected.disposition == ShareDisposition::Duplicate);

    // A lower-work sibling is connected and retained, but explicitly stale.
    Share sibling = make_child(root, "5", 3);
    const ShareId sibling_id = share_id(sibling);
    const AddShareResult sibling_result = chain.add_share_unchecked(sibling);
    CHECK(sibling_result.disposition == ShareDisposition::Connected);
    CHECK(!sibling_result.best_tip_changed);
    CHECK(chain.best_tip()->id == child_id);
    CHECK(chain.is_stale(sibling_id));
    CHECK(!chain.is_on_best_chain(sibling_id));

    // Extending the stale branch with enough work causes a reorg.
    Share sibling_tip = make_child(sibling, "21", 4);
    const ShareId sibling_tip_id = share_id(sibling_tip);
    const AddShareResult sibling_tip_result =
        chain.add_share_unchecked(sibling_tip);
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
    const AddShareResult bad_root_result = chain.add_share_unchecked(bad_root);
    CHECK(bad_root_result.disposition == ShareDisposition::Rejected);
    CHECK(bad_root_result.reject_reason == ShareRejectReason::InvalidRootHeight);

    Share bad_non_root = make_share("1", 11);
    bad_non_root.parent_id = root_id;
    bad_non_root.share_height = 0;
    const AddShareResult bad_non_root_result =
        chain.add_share_unchecked(bad_non_root);
    CHECK(bad_non_root_result.disposition == ShareDisposition::Rejected);
    CHECK(bad_non_root_result.reject_reason ==
          ShareRejectReason::InvalidNonRootHeight);

    Share bad_height = make_child(root, "1", 12);
    bad_height.share_height = 2;
    const AddShareResult bad_height_result =
        chain.add_share_unchecked(bad_height);
    CHECK(bad_height_result.disposition == ShareDisposition::Rejected);
    CHECK(bad_height_result.reject_reason == ShareRejectReason::ParentHeightMismatch);

    Share zero_share_difficulty = make_share("1", 13);
    zero_share_difficulty.share_difficulty = {};
    const AddShareResult zero_share_result =
        chain.add_share_unchecked(zero_share_difficulty);
    CHECK(zero_share_result.disposition == ShareDisposition::Rejected);
    CHECK(zero_share_result.reject_reason ==
          ShareRejectReason::ZeroShareDifficulty);

    Share zero_network_difficulty = make_share("1", 14);
    zero_network_difficulty.network_difficulty = {};
    const AddShareResult zero_network_result =
        chain.add_share_unchecked(zero_network_difficulty);
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
    CHECK(orphan_height_chain.add_share_unchecked(bad_orphan).disposition ==
          ShareDisposition::Orphan);
    const AddShareResult late_parent_result =
        orphan_height_chain.add_share_unchecked(late_parent);
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
    CHECK(tie_chain.add_share_unchecked(first).disposition ==
          ShareDisposition::Connected);
    CHECK(tie_chain.add_share_unchecked(second).disposition ==
          ShareDisposition::Connected);
    CHECK(tie_chain.best_tip() != nullptr);
    CHECK(tie_chain.best_tip()->id == expected_tie_tip);

    // Production admission rejects future timestamps before invoking PoW.
    Share future = make_share("1", 40);
    const std::uint64_t now = future.timestamp;
    future.timestamp = now + kShareMaxFutureSeconds + 1;
    const AddShareResult future_result =
        tie_chain.submit_share(future, context_for(future), now);
    CHECK(future_result.disposition == ShareDisposition::Rejected);
    CHECK(future_result.reject_reason == ShareRejectReason::TimestampTooFarFuture);

    // Parent-relative timestamps may step backwards by at most the explicit
    // tolerance. A larger backstep is rejected before PoW evaluation.
    ShareChain timestamp_chain;
    Share time_parent = make_share("1", 50);
    time_parent.timestamp = 2'000;
    CHECK(timestamp_chain.add_share_unchecked(time_parent).disposition ==
          ShareDisposition::Connected);
    Share old_child = make_child(time_parent, "1", 51);
    old_child.timestamp =
        time_parent.timestamp - kShareMaxParentBackstepSeconds - 1;
    const AddShareResult old_child_result = timestamp_chain.submit_share(
        old_child,
        context_for(old_child),
        time_parent.timestamp);
    CHECK(old_child_result.disposition == ShareDisposition::Rejected);
    CHECK(old_child_result.reject_reason ==
          ShareRejectReason::TimestampBeforeParentTolerance);

    // A share cannot substitute mining context supplied by a peer/miner.
    Share context_share = make_share("1", 60);
    ShareWorkContext wrong_height = context_for(context_share);
    ++wrong_height.zano_height;
    CHECK(tie_chain.submit_share(
              context_share, wrong_height, context_share.timestamp).reject_reason ==
          ShareRejectReason::ZanoHeightMismatch);

    ShareWorkContext wrong_header = context_for(context_share);
    wrong_header.mining_header_hash[0] ^= 0xff;
    CHECK(tie_chain.submit_share(
              context_share, wrong_header, context_share.timestamp).reject_reason ==
          ShareRejectReason::MiningHeaderMismatch);

    ShareWorkContext wrong_network = context_for(context_share);
    wrong_network.network_difficulty = difficulty128_from_decimal("1");
    CHECK(tie_chain.submit_share(
              context_share, wrong_network, context_share.timestamp).reject_reason ==
          ShareRejectReason::NetworkDifficultyMismatch);

#ifdef ZANO_P2POOL_HAVE_PROGPOWZ
    // Exact Zano compatibility vector: this hash meets share difficulty 3 but
    // not network difficulty 4. Only after the local ProgPoWZ check succeeds is
    // its claimed work allowed into the chain.
    Share verified;
    verified.timestamp = 1'700'100'000;
    verified.zano_height = 0;
    verified.mining_header_hash = hash_from_hex(
        "ffeeddccbbaa9988776655443322110000112233445566778899aabbccddeeff");
    verified.nonce = UINT64_C(0x123456789abcdef0);
    verified.share_difficulty = difficulty128_from_decimal("3");
    verified.network_difficulty = difficulty128_from_decimal("4");
    verified.miner_id[31] = 0x42;

    ShareChain verified_chain;
    const AddShareResult verified_result = verified_chain.submit_share(
        verified,
        context_for(verified),
        verified.timestamp,
        ProgPowZContextMode::Light);
    CHECK(verified_result.disposition == ShareDisposition::Connected);
    const ConnectedShare* connected = verified_chain.find(share_id(verified));
    CHECK(connected != nullptr);
    CHECK(connected->pow_validation.has_value());
    CHECK(connected->pow_validation->meets_share_difficulty);
    CHECK(!connected->pow_validation->meets_network_difficulty);
    CHECK(connected->pow_validation->classification ==
          CandidateClassification::Share);
    CHECK(hash_to_hex(connected->pow_validation->pow.final_hash) ==
          "4feba8deef1ac892ee334cf258d029cc8651f037215f1767b8ce5c704a4fd68b");

    Share block_candidate = verified;
    block_candidate.network_difficulty = difficulty128_from_decimal("3");
    block_candidate.miner_id[31] = 0x43;
    ShareChain block_chain;
    const AddShareResult block_result = block_chain.submit_share(
        block_candidate,
        context_for(block_candidate),
        block_candidate.timestamp,
        ProgPowZContextMode::Light);
    CHECK(block_result.disposition == ShareDisposition::Connected);
    const ConnectedShare* block_connected =
        block_chain.find(share_id(block_candidate));
    CHECK(block_connected != nullptr);
    CHECK(block_connected->pow_validation.has_value());
    CHECK(block_connected->pow_validation->meets_network_difficulty);
    CHECK(block_connected->pow_validation->classification ==
          CandidateClassification::Block);

    Share invalid_pow = verified;
    invalid_pow.share_difficulty = difficulty128_from_decimal("4");
    invalid_pow.network_difficulty = difficulty128_from_decimal("5");
    invalid_pow.miner_id[31] = 0x44;
    ShareChain invalid_chain;
    const AddShareResult invalid_result = invalid_chain.submit_share(
        invalid_pow,
        context_for(invalid_pow),
        invalid_pow.timestamp,
        ProgPowZContextMode::Light);
    CHECK(invalid_result.disposition == ShareDisposition::Rejected);
    CHECK(invalid_result.reject_reason == ShareRejectReason::InvalidPow);
    CHECK(invalid_chain.connected_size() == 0);
#else
    // Lightweight builds fail closed rather than accepting unverified work.
    Share unavailable = make_share("1", 70);
    ShareChain unavailable_chain;
    const AddShareResult unavailable_result = unavailable_chain.submit_share(
        unavailable,
        context_for(unavailable),
        unavailable.timestamp);
    CHECK(unavailable_result.disposition == ShareDisposition::Rejected);
    CHECK(unavailable_result.reject_reason ==
          ShareRejectReason::PowBackendUnavailable);
    CHECK(unavailable_chain.connected_size() == 0);
#endif

    CHECK(std::string(share_disposition_name(ShareDisposition::Connected)) ==
          "connected");
    CHECK(std::string(share_reject_reason_name(
              ShareRejectReason::ParentHeightMismatch)) ==
          "parent-height-mismatch");
    CHECK(std::string(share_reject_reason_name(
              ShareRejectReason::UnexpectedShareVersion)) ==
          "unexpected-share-version");
    CHECK(std::string(share_reject_reason_name(
              ShareRejectReason::UnexpectedShareDifficulty)) ==
          "unexpected-share-difficulty");
    CHECK(std::string(share_reject_reason_name(
              ShareRejectReason::InvalidPow)) ==
          "invalid-pow");

    return 0;
}
