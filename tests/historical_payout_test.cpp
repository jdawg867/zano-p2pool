#include "zano_p2pool/historical_payout.hpp"
#include "test_check.hpp"
#include <stdexcept>

using namespace zano_p2pool;
namespace {
Share make_share(const ShareId& parent, std::uint64_t height, std::uint8_t miner) {
    Share s;
    s.version = kShareVersion2;
    s.parent_id = parent;
    s.share_height = height;
    s.timestamp = 1000 + height * 10;
    s.zano_height = 100;
    s.mining_header_hash[0] = miner;
    s.nonce = height;
    s.share_difficulty = difficulty128_from_decimal("1");
    s.network_difficulty = difficulty128_from_decimal("1");
    PayoutPublicKeys keys;
    keys.spend_public_key.fill(miner);
    keys.view_public_key.fill(miner + 1);
    s.payout = keys;
    s.miner_id = miner_id_from_payout(keys);
    return s;
}
AddShareResult submit(ShareChain& chain, const Share& share) {
    return chain.submit_share(share, {share.zano_height, share.mining_header_hash,
        share.network_difficulty}, 2000);
}
}
int main() {
    auto params = canonical_sidechain_parameters(SidechainParentNetwork::Testnet);
    params.minimum_share_difficulty = 1;
    const auto one = difficulty128_from_decimal("1");
    ShareChain chain(params);
    const auto root = make_share({}, 0, 1);
    const auto root_id = share_id(root);
    CHECK(derive_historical_payout_plan(chain, params, {}, one, 100).status ==
        HistoricalPayoutStatus::BootstrapHistoryRequired);
    CHECK(derive_historical_payout_plan(chain, params, root_id, one, 100).status ==
        HistoricalPayoutStatus::ParentMissing);
    auto wrong_params = params;
    wrong_params.pplns_window_shares = 2;
    CHECK(derive_historical_payout_plan(chain, wrong_params, {}, one, 100).status ==
        HistoricalPayoutStatus::ParameterMismatch);
    ShareChain unchecked(params);
    CHECK(unchecked.add_share_unchecked(root).disposition == ShareDisposition::Connected);
    CHECK(!unchecked.find(root_id)->validated_ancestry);
    CHECK(derive_historical_payout_plan(unchecked, params, root_id, one, 100).status ==
        HistoricalPayoutStatus::UnverifiedAncestry);

    // The low-level PPLNS primitive must now enforce the same ancestry trust
    // boundary when the ShareChain carries consensus sidechain parameters.
    const auto child = make_share(root_id, 1, 3);
    const auto child_id = share_id(child);
    CHECK(unchecked.add_share_unchecked(child).disposition == ShareDisposition::Connected);

    bool unchecked_parent_threw = false;
    try {
        static_cast<void>(build_sidechain_pplns_window_at_parent(
            unchecked, root_id, params, one));
    } catch (const std::logic_error&) {
        unchecked_parent_threw = true;
    }
    CHECK(unchecked_parent_threw);

    bool unchecked_tip_threw = false;
    try {
        static_cast<void>(build_sidechain_pplns_window(
            unchecked, params, one));
    } catch (const std::logic_error&) {
        unchecked_tip_threw = true;
    }
    CHECK(unchecked_tip_threw);

    CHECK(build_sidechain_pplns_window_at_parent(
        unchecked, {}, params, one).included_shares == 0);

    ShareId unknown{};
    unknown[0] = 99;
    bool missing = false;
    try {
        static_cast<void>(build_sidechain_pplns_window_at_parent(
            unchecked, unknown, params, one));
    } catch (const std::invalid_argument&) {
        missing = true;
    }
    CHECK(missing);

    auto capped = params;
    capped.pplns_max_network_difficulty_multiplier = 1;

    bool unchecked_child_threw = false;
    try {
        static_cast<void>(build_sidechain_pplns_window_at_parent(
            unchecked, child_id, capped, one));
    } catch (const std::logic_error&) {
        unchecked_child_threw = true;
    }
    CHECK(unchecked_child_threw);

    if (!progpowz_available()) return 0;
    // Difficulty one makes these real PoW checks deterministic without nonce searching.
    CHECK(submit(chain, root).disposition == ShareDisposition::Connected);
    CHECK(chain.find(root_id)->validated_ancestry);
    CHECK(submit(chain, child).disposition == ShareDisposition::Connected);
    const auto plan = derive_historical_payout_plan(chain, params, root_id, one, 100);
    CHECK(plan.status == HistoricalPayoutStatus::PlanDerived);
    CHECK(plan.parent_id == root_id);
    CHECK(plan.plan.destinations.size() == 1);
    CHECK(plan.plan.destinations[0].miner_id == root.miner_id);
    CHECK(plan.plan.destinations[0].amount == 100);
    const auto child_plan = derive_historical_payout_plan(chain, params, child_id, one, 100);
    CHECK(child_plan.plan.destinations.size() == 2);
    for (const auto& d : child_plan.plan.destinations) CHECK(d.amount == 50);

    auto fork = make_share(root_id, 1, 5);
    const auto fork_id = share_id(fork);
    CHECK(submit(chain, fork).disposition == ShareDisposition::Connected);
    auto fork_child = make_share(fork_id, 2, 7);
    CHECK(submit(chain, fork_child).disposition == ShareDisposition::Connected);
    CHECK(chain.best_tip()->id == share_id(fork_child));
    CHECK(!chain.is_on_best_chain(child_id));
    CHECK(derive_historical_payout_plan(chain, params, child_id, one, 100).plan.destinations ==
        child_plan.plan.destinations);

    // Even a newly PoW-validated descendant cannot hide an unchecked root.
    ShareChain mixed(params);
    CHECK(mixed.add_share_unchecked(root).disposition == ShareDisposition::Connected);
    CHECK(submit(mixed, child).disposition == ShareDisposition::Connected);
    CHECK(mixed.find(child_id)->pow_validation.has_value());
    CHECK(!mixed.find(child_id)->validated_ancestry);
    CHECK(derive_historical_payout_plan(mixed, params, child_id, one, 100).status ==
        HistoricalPayoutStatus::UnverifiedAncestry);
    CHECK(submit(mixed, root).disposition == ShareDisposition::Duplicate);
    CHECK(!mixed.find(root_id)->validated_ancestry);

    ShareChain orphans(params);
    CHECK(submit(orphans, child).disposition == ShareDisposition::Orphan);
    CHECK(submit(orphans, root).disposition == ShareDisposition::Connected);
    CHECK(orphans.find(child_id)->validated_ancestry);
    ShareChain mixed_orphans(params);
    CHECK(submit(mixed_orphans, child).disposition == ShareDisposition::Orphan);
    CHECK(mixed_orphans.add_share_unchecked(root).disposition == ShareDisposition::Connected);
    CHECK(!mixed_orphans.find(child_id)->validated_ancestry);
}
