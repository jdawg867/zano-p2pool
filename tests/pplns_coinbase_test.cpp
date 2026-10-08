#include "zano_p2pool/pplns_coinbase.hpp"
#include "test_check.hpp"

#include <cstdint>
#include <string>

namespace {

using namespace zano_p2pool;

ChainWork work(std::string_view decimal) {
    return share_work(
        difficulty128_from_decimal(decimal));
}

PayoutPublicKeys payout(std::uint8_t seed) {
    PayoutPublicKeys result;

    for (std::size_t i = 0; i < 32; ++i) {
        result.spend_public_key[i] =
            static_cast<std::uint8_t>(
                seed + i);

        result.view_public_key[i] =
            static_cast<std::uint8_t>(
                seed + 0x40U + i);
    }

    return result;
}

PplnsMinerWork row(
    const PayoutPublicKeys& keys,
    std::string_view credited_work) {
    return PplnsMinerWork{
        miner_id_from_payout(keys),
        work(credited_work),
        keys,
    };
}

PplnsMinerWork row(
    std::uint8_t seed,
    std::string_view credited_work) {
    return row(
        payout(seed),
        credited_work);
}

std::uint64_t amount_for(
    const PplnsCoinbasePlan& plan,
    const PayoutPublicKeys& keys) {
    std::uint64_t amount = 0;

    for (const auto& destination :
         plan.destinations) {
        if (destination.payout == keys) {
            amount += destination.amount;
        }
    }

    return amount;
}

std::uint64_t sum_plan(
    const PplnsCoinbasePlan& plan) {
    std::uint64_t total = 0;

    for (const auto& destination :
         plan.destinations) {
        total += destination.amount;
    }

    return total;
}

}  // namespace

int main() {
    using namespace zano_p2pool;

    const SidechainParameters params =
        canonical_sidechain_parameters(
            SidechainParentNetwork::Testnet);

    PplnsWindow window;
    window.requested_work = work("4");
    window.covered_work = work("4");
    window.complete = true;
    window.miners = {
        row(0x10, "1"),
        row(0x20, "3"),
    };

    const auto ready =
        make_pplns_coinbase_plan(
            window,
            1000,
            params);

    CHECK(
        ready.status ==
        PplnsCoinbasePlanStatus::Ready);

    CHECK(ready.reward_atomic == 1000);
    CHECK(ready.destinations.size() == 3);
    CHECK(sum_plan(ready) == 1000);

    CHECK(
        amount_for(
            ready,
            params.operator_fee_payout) ==
        10);

    CHECK(
        amount_for(
            ready,
            *window.miners[0].payout) +
        amount_for(
            ready,
            *window.miners[1].payout) ==
        990);

    for (const auto& destination :
         ready.destinations) {
        CHECK(destination.amount != 0);

        CHECK(
            destination.miner_id ==
            miner_id_from_payout(
                destination.payout));
    }

    // Operator also mining: one logical destination.
    PplnsWindow operator_mines;

    operator_mines.requested_work =
        work("1");

    operator_mines.covered_work =
        work("1");

    operator_mines.complete = true;

    operator_mines.miners = {
        row(
            params.operator_fee_payout,
            "1"),
    };

    const auto merged_operator =
        make_pplns_coinbase_plan(
            operator_mines,
            1000,
            params);

    CHECK(
        merged_operator.status ==
        PplnsCoinbasePlanStatus::Ready);

    CHECK(
        merged_operator.destinations.size() ==
        1);

    CHECK(
        merged_operator.destinations[0].payout ==
        params.operator_fee_payout);

    CHECK(
        merged_operator.destinations[0].amount ==
        1000);

    PplnsWindow incomplete = window;
    incomplete.complete = false;

    CHECK(
        make_pplns_coinbase_plan(
            incomplete,
            1000,
            params).status ==
        PplnsCoinbasePlanStatus::
            IncompleteWindow);

    PplnsWindow legacy = window;
    legacy.miners[0].payout.reset();

    CHECK(
        make_pplns_coinbase_plan(
            legacy,
            1000,
            params).status ==
        PplnsCoinbasePlanStatus::
            MissingPayoutIdentity);

    // 31 miners + operator = exactly 32 destinations.
    PplnsWindow max_miners;

    max_miners.requested_work =
        work("31");

    max_miners.covered_work =
        work("31");

    max_miners.complete = true;

    for (std::size_t i = 0; i < 31; ++i) {
        max_miners.miners.push_back(
            row(
                static_cast<std::uint8_t>(
                    i + 1),
                "1"));
    }

    const auto max_ready =
        make_pplns_coinbase_plan(
            max_miners,
            3100,
            params);

    CHECK(
        max_ready.status ==
        PplnsCoinbasePlanStatus::Ready);

    CHECK(max_ready.destinations.size() == 32);
    CHECK(sum_plan(max_ready) == 3100);

    CHECK(
        amount_for(
            max_ready,
            params.operator_fee_payout) ==
        31);

    // 32 distinct miners is outside v4's reserved-output bound.
    PplnsWindow too_many;

    too_many.requested_work =
        work("32");

    too_many.covered_work =
        work("32");

    too_many.complete = true;

    for (std::size_t i = 0; i < 32; ++i) {
        too_many.miners.push_back(
            row(
                static_cast<std::uint8_t>(
                    i + 1),
                "1"));
    }

    CHECK(
        make_pplns_coinbase_plan(
            too_many,
            3200,
            params).status ==
        PplnsCoinbasePlanStatus::
            TooManyRecipients);

    // Basis-point arithmetic operates on atomic units. When the exact 1%
    // result is below one atomic unit, floor rounding produces no operator
    // destination and the entire representable reward remains in PPLNS.
    const auto rounded_zero_fee =
        make_pplns_coinbase_plan(
            window,
            99,
            params);

    CHECK(
        rounded_zero_fee.status ==
        PplnsCoinbasePlanStatus::Ready);

    CHECK(sum_plan(rounded_zero_fee) == 99);

    CHECK(
        amount_for(
            rounded_zero_fee,
            params.operator_fee_payout) ==
        0);

    CHECK(rounded_zero_fee.destinations.size() == 2);

    // 100 atomic units is the first reward for which a 1% fee can be
    // represented exactly as one atomic unit.
    const auto first_nonzero_fee =
        make_pplns_coinbase_plan(
            window,
            100,
            params);

    CHECK(
        first_nonzero_fee.status ==
        PplnsCoinbasePlanStatus::Ready);

    CHECK(sum_plan(first_nonzero_fee) == 100);

    CHECK(
        amount_for(
            first_nonzero_fee,
            params.operator_fee_payout) ==
        1);

    PplnsWindow zero_miner;

    zero_miner.requested_work =
        work("1000001");

    zero_miner.covered_work =
        work("1000001");

    zero_miner.complete = true;

    zero_miner.miners = {
        row(0x60, "1"),
        row(0x70, "1000000"),
    };

    CHECK(
        make_pplns_coinbase_plan(
            zero_miner,
            100,
            params).status ==
        PplnsCoinbasePlanStatus::ZeroPayout);

    PplnsWindow empty;
    empty.requested_work = work("1");
    empty.covered_work = ChainWork{};
    empty.complete = false;

    CHECK(
        make_pplns_coinbase_plan(
            empty,
            1000,
            params).status ==
        PplnsCoinbasePlanStatus::EmptyWindow);

    return 0;
}
