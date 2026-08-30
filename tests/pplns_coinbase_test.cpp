#include "zano_p2pool/pplns_coinbase.hpp"
#include "test_check.hpp"

#include <cstdint>
#include <string>

namespace {

using namespace zano_p2pool;

ChainWork work(std::string_view decimal) {
    return share_work(difficulty128_from_decimal(decimal));
}

PayoutPublicKeys payout(std::uint8_t seed) {
    PayoutPublicKeys result;
    for (std::size_t i = 0; i < 32; ++i) {
        result.spend_public_key[i] = static_cast<std::uint8_t>(seed + i);
        result.view_public_key[i] = static_cast<std::uint8_t>(seed + 0x40U + i);
    }
    return result;
}

PplnsMinerWork row(std::uint8_t seed, std::string_view credited_work) {
    const PayoutPublicKeys keys = payout(seed);
    return PplnsMinerWork{
        miner_id_from_payout(keys),
        work(credited_work),
        keys,
    };
}

}  // namespace

int main() {
    using namespace zano_p2pool;

    PplnsWindow window;
    window.requested_work = work("4");
    window.covered_work = work("4");
    window.complete = true;
    window.miners = {
        row(0x10, "1"),
        row(0x20, "3"),
    };

    const auto ready = make_pplns_coinbase_plan(window, 1000);
    CHECK(ready.status == PplnsCoinbasePlanStatus::Ready);
    CHECK(ready.reward_atomic == 1000);
    CHECK(ready.destinations.size() == 2);
    CHECK(ready.destinations[0].amount + ready.destinations[1].amount == 1000);
    CHECK(ready.destinations[0].miner_id ==
          miner_id_from_payout(ready.destinations[0].payout));
    CHECK(ready.destinations[1].miner_id ==
          miner_id_from_payout(ready.destinations[1].payout));

    PplnsWindow incomplete = window;
    incomplete.complete = false;
    CHECK(make_pplns_coinbase_plan(incomplete, 1000).status ==
          PplnsCoinbasePlanStatus::IncompleteWindow);

    PplnsWindow legacy = window;
    legacy.miners[0].payout.reset();
    CHECK(make_pplns_coinbase_plan(legacy, 1000).status ==
          PplnsCoinbasePlanStatus::MissingPayoutIdentity);

    PplnsWindow too_many;
    too_many.requested_work = work("33");
    too_many.covered_work = work("33");
    too_many.complete = true;
    for (std::size_t i = 0; i < 33; ++i) {
        too_many.miners.push_back(row(
            static_cast<std::uint8_t>(i + 1),
            "1"));
    }
    CHECK(make_pplns_coinbase_plan(too_many, 3300).status ==
          PplnsCoinbasePlanStatus::TooManyRecipients);

    PplnsWindow zero_amount;
    zero_amount.requested_work = work("2");
    zero_amount.covered_work = work("2");
    zero_amount.complete = true;
    zero_amount.miners = {row(0x60, "1"), row(0x70, "1")};
    CHECK(make_pplns_coinbase_plan(zero_amount, 1).status ==
          PplnsCoinbasePlanStatus::ZeroPayout);

    PplnsWindow empty;
    empty.requested_work = work("1");
    empty.covered_work = ChainWork{};
    empty.complete = false;
    CHECK(make_pplns_coinbase_plan(empty, 1000).status ==
          PplnsCoinbasePlanStatus::EmptyWindow);

    CHECK(std::string(pplns_coinbase_plan_status_name(
              PplnsCoinbasePlanStatus::TooManyRecipients)) ==
          "too-many-recipients");

    return 0;
}
