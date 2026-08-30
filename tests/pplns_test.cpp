#include "zano_p2pool/pplns.hpp"
#include "test_check.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>

namespace {

using namespace zano_p2pool;

MinerId miner(std::uint8_t value) {
    MinerId id{};
    id.fill(value);
    return id;
}

PayoutPublicKeys payout(std::uint8_t seed) {
    PayoutPublicKeys result;
    for (std::size_t i = 0; i < 32; ++i) {
        result.spend_public_key[i] = static_cast<std::uint8_t>(seed + i);
        result.view_public_key[i] = static_cast<std::uint8_t>(seed + 0x40U + i);
    }
    return result;
}

ChainWork work(std::string_view decimal) {
    return share_work(difficulty128_from_decimal(decimal));
}

ShareId append_share(
    ShareChain& chain,
    const ShareId& parent,
    std::uint64_t height,
    std::string_view difficulty,
    const MinerId& miner_id) {
    Share share;
    share.parent_id = parent;
    share.share_height = height;
    share.timestamp = 1000 + height;
    share.zano_height = 200000 + height;
    share.mining_header_hash.fill(static_cast<std::uint8_t>(height + 1));
    share.nonce = height + 100;
    share.share_difficulty = difficulty128_from_decimal(difficulty);
    share.network_difficulty = difficulty128_from_decimal("1000000");
    share.miner_id = miner_id;

    const AddShareResult result = chain.add_share_unchecked(share);
    CHECK(result.disposition == ShareDisposition::Connected);
    return result.id;
}

ShareId append_share_v2(
    ShareChain& chain,
    const ShareId& parent,
    std::uint64_t height,
    std::string_view difficulty,
    const PayoutPublicKeys& payout_keys) {
    Share share;
    share.version = kShareVersion2;
    share.parent_id = parent;
    share.share_height = height;
    share.timestamp = 2000 + height;
    share.zano_height = 210000 + height;
    share.mining_header_hash.fill(static_cast<std::uint8_t>(height + 0x20U));
    share.nonce = height + 200;
    share.share_difficulty = difficulty128_from_decimal(difficulty);
    share.network_difficulty = difficulty128_from_decimal("1000000");
    share.miner_id = miner_id_from_payout(payout_keys);
    share.payout = payout_keys;

    const AddShareResult result = chain.add_share_unchecked(share);
    CHECK(result.disposition == ShareDisposition::Connected);
    return result.id;
}

const PplnsMinerWork* find_work(
    const PplnsWindow& window,
    const MinerId& miner_id) {
    for (const auto& row : window.miners) {
        if (row.miner_id == miner_id) {
            return &row;
        }
    }
    return nullptr;
}

}  // namespace

int main() {
    using namespace zano_p2pool;

    const MinerId miner_a = miner(0x01);
    const MinerId miner_b = miner(0x02);
    const MinerId miner_c = miner(0x03);

    ShareChain chain;
    ShareId parent{};
    parent = append_share(chain, parent, 0, "100", miner_a);
    parent = append_share(chain, parent, 1, "200", miner_b);
    parent = append_share(chain, parent, 2, "300", miner_a);
    parent = append_share(chain, parent, 3, "400", miner_c);

    // Newest-to-oldest work for a 750-work PPLNS window:
    // C=400, A=300, then only 50 of B's 200-work share is needed.
    const PplnsWindow window = build_pplns_window(chain, work("750"));
    CHECK(window.complete);
    CHECK(window.requested_work == work("750"));
    CHECK(window.covered_work == work("750"));
    CHECK(window.included_shares == 3);
    CHECK(window.miners.size() == 3);
    CHECK(window.miners[0].miner_id == miner_a);
    CHECK(window.miners[1].miner_id == miner_b);
    CHECK(window.miners[2].miner_id == miner_c);
    CHECK(find_work(window, miner_a)->work == work("300"));
    CHECK(find_work(window, miner_b)->work == work("50"));
    CHECK(find_work(window, miner_c)->work == work("400"));

    // 1000 atomic units over 300:50:400 work gives floors 400,66,533.
    // The one remaining atomic unit goes to B because B has the largest exact
    // fractional remainder, yielding an exact total of 1000.
    const auto payouts = allocate_pplns_reward(window, 1000);
    CHECK(payouts.size() == 3);
    CHECK(payouts[0].miner_id == miner_a);
    CHECK(payouts[0].amount == 400);
    CHECK(payouts[1].miner_id == miner_b);
    CHECK(payouts[1].amount == 67);
    CHECK(payouts[2].miner_id == miner_c);
    CHECK(payouts[2].amount == 533);
    CHECK(payouts[0].amount + payouts[1].amount + payouts[2].amount == 1000);

    // A raw work window larger than chain history remains explicitly incomplete.
    const PplnsWindow bootstrap = build_pplns_window(chain, work("2000"));
    CHECK(!bootstrap.complete);
    CHECK(bootstrap.covered_work == work("1000"));
    CHECK(bootstrap.included_shares == 4);
    CHECK(find_work(bootstrap, miner_a)->work == work("400"));
    CHECK(find_work(bootstrap, miner_b)->work == work("200"));
    CHECK(find_work(bootstrap, miner_c)->work == work("400"));

    const auto bootstrap_payouts = allocate_pplns_reward(bootstrap, 100);
    CHECK(bootstrap_payouts[0].amount == 40);
    CHECK(bootstrap_payouts[1].amount == 20);
    CHECK(bootstrap_payouts[2].amount == 40);

    // Equal fractional remainders use MinerId ordering, never container order.
    PplnsWindow tie;
    tie.requested_work = work("3");
    tie.covered_work = work("3");
    tie.complete = true;
    tie.miners = {
        PplnsMinerWork{miner_c, work("1")},
        PplnsMinerWork{miner_a, work("1")},
        PplnsMinerWork{miner_b, work("1")},
    };
    const auto tie_payouts = allocate_pplns_reward(tie, 2);
    CHECK(tie_payouts.size() == 3);
    CHECK(tie_payouts[0].miner_id == miner_a);
    CHECK(tie_payouts[0].amount == 1);
    CHECK(tie_payouts[1].miner_id == miner_b);
    CHECK(tie_payouts[1].amount == 1);
    CHECK(tie_payouts[2].miner_id == miner_c);
    CHECK(tie_payouts[2].amount == 0);

    // PPLNS always walks the current verified best-chain ancestry. Work on a
    // branch that becomes stale after a reorg must never remain in the window.
    ShareChain reorg_chain;
    ShareId reorg_root{};
    reorg_root = append_share(reorg_chain, reorg_root, 0, "10", miner_a);
    const ShareId losing_tip =
        append_share(reorg_chain, reorg_root, 1, "50", miner_b);
    const ShareId winning_parent =
        append_share(reorg_chain, reorg_root, 1, "20", miner_c);
    CHECK(reorg_chain.best_tip() != nullptr);
    CHECK(reorg_chain.best_tip()->id == losing_tip);

    const ShareId winning_tip =
        append_share(reorg_chain, winning_parent, 2, "40", miner_c);
    CHECK(reorg_chain.best_tip() != nullptr);
    CHECK(reorg_chain.best_tip()->id == winning_tip);
    CHECK(reorg_chain.is_stale(losing_tip));

    const PplnsWindow reorg_window = build_pplns_window(reorg_chain, work("65"));
    CHECK(reorg_window.complete);
    CHECK(reorg_window.covered_work == work("65"));
    CHECK(find_work(reorg_window, miner_a)->work == work("5"));
    CHECK(find_work(reorg_window, miner_b) == nullptr);
    CHECK(find_work(reorg_window, miner_c)->work == work("60"));

    // Public payout identity from v2 shares is retained while repeated work for
    // the same MinerId is aggregated, so direct coinbase construction does not
    // depend on a node-local wallet/address database.
    ShareChain payout_chain;
    const PayoutPublicKeys payout_keys = payout(0x31);
    ShareId payout_parent{};
    payout_parent = append_share_v2(
        payout_chain, payout_parent, 0, "25", payout_keys);
    payout_parent = append_share_v2(
        payout_chain, payout_parent, 1, "35", payout_keys);

    const MinerId payout_miner = miner_id_from_payout(payout_keys);
    const PplnsWindow payout_window = build_pplns_window(
        payout_chain, work("60"));
    CHECK(payout_window.complete);
    CHECK(payout_window.miners.size() == 1);
    const PplnsMinerWork* payout_row = find_work(payout_window, payout_miner);
    CHECK(payout_row != nullptr);
    CHECK(payout_row->work == work("60"));
    CHECK(payout_row->payout.has_value());
    CHECK(*payout_row->payout == payout_keys);

    // Canonical sidechain payout policy uses whichever limit is smaller: work
    // in the newest capped share history, or 2x current parent-network work.
    SidechainParameters policy = canonical_sidechain_parameters(
        SidechainParentNetwork::Testnet);
    policy.pplns_window_shares = 3;
    const PplnsWindow share_capped = build_sidechain_pplns_window(
        chain, policy, difficulty128_from_decimal("500"));
    CHECK(share_capped.complete);
    CHECK(share_capped.requested_work == work("900"));
    CHECK(share_capped.covered_work == work("900"));
    CHECK(share_capped.included_shares == 3);
    CHECK(find_work(share_capped, miner_a)->work == work("300"));
    CHECK(find_work(share_capped, miner_b)->work == work("200"));
    CHECK(find_work(share_capped, miner_c)->work == work("400"));

    policy.pplns_window_shares = 4;
    const PplnsWindow work_capped = build_sidechain_pplns_window(
        chain, policy, difficulty128_from_decimal("300"));
    CHECK(work_capped.complete);
    CHECK(work_capped.requested_work == work("600"));
    CHECK(work_capped.covered_work == work("600"));
    CHECK(work_capped.included_shares == 2);
    CHECK(find_work(work_capped, miner_a)->work == work("200"));
    CHECK(find_work(work_capped, miner_b) == nullptr);
    CHECK(find_work(work_capped, miner_c)->work == work("400"));

    // A long history with one distinct miner per share is still bounded by the
    // HF6-safe 32-share policy, so it can never require more than 32 recipients.
    ShareChain many_miners;
    ShareId many_parent{};
    for (std::uint64_t height = 0; height < 40; ++height) {
        many_parent = append_share(
            many_miners,
            many_parent,
            height,
            "1",
            miner(static_cast<std::uint8_t>(height + 1)));
    }
    policy.pplns_window_shares = 32;
    const PplnsWindow bounded = build_sidechain_pplns_window(
        many_miners, policy, difficulty128_from_decimal("1000"));
    CHECK(bounded.complete);
    CHECK(bounded.requested_work == work("32"));
    CHECK(bounded.covered_work == work("32"));
    CHECK(bounded.included_shares == 32);
    CHECK(bounded.miners.size() == 32);
    CHECK(find_work(bounded, miner(0x08)) == nullptr);
    CHECK(find_work(bounded, miner(0x09)) != nullptr);
    CHECK(find_work(bounded, miner(0x28)) != nullptr);

    ShareChain empty;
    const PplnsWindow empty_window = build_pplns_window(empty, work("100"));
    CHECK(!empty_window.complete);
    CHECK(empty_window.covered_work == ChainWork{});
    CHECK(empty_window.included_shares == 0);
    CHECK(empty_window.miners.empty());
    CHECK(allocate_pplns_reward(empty_window, 1000).empty());

    const PplnsWindow empty_policy = build_sidechain_pplns_window(
        empty, policy, difficulty128_from_decimal("1000"));
    CHECK(!empty_policy.complete);
    CHECK(empty_policy.requested_work == ChainWork{});
    CHECK(empty_policy.included_shares == 0);

    bool zero_threw = false;
    try {
        static_cast<void>(build_pplns_window(chain, ChainWork{}));
    } catch (const std::invalid_argument&) {
        zero_threw = true;
    }
    CHECK(zero_threw);

    bool zero_network_threw = false;
    try {
        static_cast<void>(build_sidechain_pplns_window(
            chain, policy, Difficulty128{}));
    } catch (const std::invalid_argument&) {
        zero_network_threw = true;
    }
    CHECK(zero_network_threw);

    return 0;
}
