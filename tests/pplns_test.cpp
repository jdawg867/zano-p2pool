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

    // A window larger than chain history is explicitly incomplete, and repeated
    // shares from the same miner aggregate deterministically.
    const PplnsWindow bootstrap = build_pplns_window(chain, work("2000"));
    CHECK(!bootstrap.complete);
    CHECK(bootstrap.covered_work == work("1000"));
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

    ShareChain empty;
    const PplnsWindow empty_window = build_pplns_window(empty, work("100"));
    CHECK(!empty_window.complete);
    CHECK(empty_window.covered_work == ChainWork{});
    CHECK(empty_window.miners.empty());
    CHECK(allocate_pplns_reward(empty_window, 1000).empty());

    bool zero_threw = false;
    try {
        static_cast<void>(build_pplns_window(chain, ChainWork{}));
    } catch (const std::invalid_argument&) {
        zero_threw = true;
    }
    CHECK(zero_threw);

    return 0;
}
