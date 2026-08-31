#include "zano_p2pool/crypto_hash.hpp"
#include "zano_p2pool/mining_header.hpp"
#include "zano_p2pool/p2p_miner_tx_binding.hpp"
#include "zano_p2pool/p2p_payout_policy.hpp"
#include "zano_p2pool/pplns_template.hpp"
#include "zano_p2pool/zano_address.hpp"
#include "zano_p2pool/zano_miner_tx.hpp"
#include "test_check.hpp"

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace zano_p2pool;

PayoutPublicKeys decode_payout(const char* address) {
    const auto decoded = decode_zano_standard_address(address);
    CHECK(decoded.status == ZanoAddressDecodeStatus::Valid);
    return PayoutPublicKeys{
        decoded.payout.spend_public_key,
        decoded.payout.view_public_key,
    };
}

std::vector<std::uint8_t> make_block_blob(
    const std::vector<std::uint8_t>& miner_tx) {
    std::vector<std::uint8_t> blob;
    blob.push_back(0x03);              // block major version
    blob.insert(blob.end(), 8, 0);     // zero template nonce
    blob.insert(blob.end(), 32, 0x11); // previous block hash
    blob.push_back(0x00);              // minor version
    blob.push_back(0x01);              // timestamp
    blob.push_back(0x00);              // PoW flags
    blob.insert(blob.end(), miner_tx.begin(), miner_tx.end());
    blob.push_back(0x00);              // zero regular transaction hashes
    return blob;
}

Share make_payout_share(
    const PayoutPublicKeys& payout,
    std::uint64_t share_height,
    std::uint64_t timestamp,
    std::uint64_t difficulty,
    const ShareId& parent = {}) {
    Share share;
    share.version = kShareVersion2;
    share.parent_id = parent;
    share.share_height = share_height;
    share.timestamp = timestamp;
    share.zano_height = 166331;
    share.mining_header_hash.fill(0x22);
    share.share_difficulty = difficulty128_from_decimal(
        std::to_string(difficulty));
    share.network_difficulty = difficulty128_from_decimal("1000000");
    share.payout = payout;
    share.miner_id = miner_id_from_payout(payout);
    return share;
}

}  // namespace

int main() {
    using namespace zano_p2pool;

#ifndef ZANO_P2POOL_HAVE_ZANO_MINER_TX
    CHECK(!zano_miner_tx_builder_available());

    bool threw = false;
    try {
        static_cast<void>(build_zano_hf6_pplns_miner_tx(
            166331,
            0,
            1000000000000ULL,
            PplnsCoinbasePlan{},
            "p2pool-test"));
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
    return 0;
#else
    CHECK(zano_miner_tx_builder_available());

    // Public sample addresses from Zano's own RPC documentation. They provide
    // real curve points without involving any wallet secret material.
    const PayoutPublicKeys payout_a = decode_payout(
        "ZxDNaMeZjwCjnHuU5gUNyrP1pM3U5vckbakzzV6dEHyDYeCpW8XGLBFTshcaY8LkG9RQn7FsQx8w2JeJzJwPwuDm2NfixPAXf");
    const PayoutPublicKeys payout_b = decode_payout(
        "ZxBvJDuQjMG9R2j4WnYUhBYNrwZPwuyXrC7FHdVmWqaESgowDvgfWtiXeNGu8Px9B24pkmjsA39fzSSiEQG1ekB225ZnrMTBp");

    PplnsCoinbasePlan plan;
    plan.status = PplnsCoinbasePlanStatus::Ready;
    plan.reward_atomic = 1000000000000ULL;
    plan.destinations = {
        PplnsCoinbaseDestination{
            miner_id_from_payout(payout_a),
            payout_a,
            400000000000ULL,
        },
        PplnsCoinbaseDestination{
            miner_id_from_payout(payout_b),
            payout_b,
            600000000000ULL,
        },
    };

    const ZanoMinerTxResult result = build_zano_hf6_pplns_miner_tx(
        166331,
        0,
        plan.reward_atomic,
        plan,
        "p2pool-test");

    CHECK(result.block_reward == plan.reward_atomic);
    CHECK(result.block_reward_without_fee == plan.reward_atomic);
    CHECK(!result.tx_blob_hex.empty());
    CHECK(!result.miner_tx_tgc_json.empty());

    const auto tx_bytes = hex_to_bytes(result.tx_blob_hex);
    const ParsedMinerTxPrefix parsed = parse_hf6_miner_tx_prefix(tx_bytes);
    CHECK(parsed.version == 4);
    CHECK(parsed.hardfork_id == 6);
    CHECK(parsed.vin_count == 1);
    CHECK(parsed.vout_count == 2);

    const P2pMinerTxBindingResult binding =
        verify_miner_tx_tgc_key_binding(tx_bytes, result.miner_tx_tgc_json);
    CHECK(binding.status == P2pMinerTxBindingStatus::Verified);

    const P2pPayoutPolicyResult payout_policy = verify_miner_tx_payout_policy(
        parsed.serialized,
        result.miner_tx_tgc_json,
        result.block_reward_without_fee,
        result.block_reward,
        0,
        plan);
    CHECK(payout_policy.status == P2pPayoutPolicyStatus::Verified);
    CHECK(payout_policy.output_count == 2);
    CHECK(payout_policy.verified_reward == plan.reward_atomic);

    PplnsCoinbasePlan reordered_plan = plan;
    std::swap(reordered_plan.destinations[0], reordered_plan.destinations[1]);
    CHECK(verify_miner_tx_payout_policy(
              parsed.serialized,
              result.miner_tx_tgc_json,
              result.block_reward_without_fee,
              result.block_reward,
              0,
              reordered_plan).status == P2pPayoutPolicyStatus::Verified);

    PplnsCoinbasePlan wrong_split = plan;
    wrong_split.destinations[0].amount += 1;
    wrong_split.destinations[1].amount -= 1;
    CHECK(verify_miner_tx_payout_policy(
              parsed.serialized,
              result.miner_tx_tgc_json,
              result.block_reward_without_fee,
              result.block_reward,
              0,
              wrong_split).status == P2pPayoutPolicyStatus::PayoutPlanMismatch);
    CHECK(std::string(p2p_payout_policy_status_name(
              P2pPayoutPolicyStatus::PayoutPlanMismatch)) ==
          "payout-plan-mismatch");

    // Zano requires at least two outputs for a post-HF4 PoW coinbase. A PPLNS
    // window with one logical recipient therefore expands to two physical
    // outputs to the same payout identity, while verification still binds the
    // aggregate amount to the unchanged one-recipient plan.
    PplnsCoinbasePlan single_plan;
    single_plan.status = PplnsCoinbasePlanStatus::Ready;
    single_plan.reward_atomic = 1000000000000ULL;
    single_plan.destinations = {
        PplnsCoinbaseDestination{
            miner_id_from_payout(payout_a),
            payout_a,
            single_plan.reward_atomic,
        },
    };

    const ZanoMinerTxResult single_result = build_zano_hf6_pplns_miner_tx(
        166331,
        0,
        single_plan.reward_atomic,
        single_plan,
        "single-recipient-test");
    const auto single_tx_bytes = hex_to_bytes(single_result.tx_blob_hex);
    const ParsedMinerTxPrefix single_parsed =
        parse_hf6_miner_tx_prefix(single_tx_bytes);
    CHECK(single_parsed.version == 4);
    CHECK(single_parsed.hardfork_id == 6);
    CHECK(single_parsed.vin_count == 1);
    CHECK(single_parsed.vout_count == 2);
    CHECK(verify_miner_tx_tgc_key_binding(
              single_tx_bytes,
              single_result.miner_tx_tgc_json).status ==
          P2pMinerTxBindingStatus::Verified);

    const P2pPayoutPolicyResult single_policy = verify_miner_tx_payout_policy(
        single_parsed.serialized,
        single_result.miner_tx_tgc_json,
        single_result.block_reward_without_fee,
        single_result.block_reward,
        0,
        single_plan);
    CHECK(single_policy.status == P2pPayoutPolicyStatus::Verified);
    CHECK(single_policy.output_count == 2);
    CHECK(single_policy.verified_reward == single_plan.reward_atomic);

    // Build a daemon-style full block with a different valid miner transaction,
    // then replace that entire transaction from verified sidechain history.
    PplnsCoinbasePlan daemon_plan = plan;
    daemon_plan.destinations[0].amount = 500000000000ULL;
    daemon_plan.destinations[1].amount = 500000000000ULL;
    const ZanoMinerTxResult daemon_miner_tx = build_zano_hf6_pplns_miner_tx(
        166331,
        0,
        daemon_plan.reward_atomic,
        daemon_plan,
        "daemon-template");

    BlockTemplate daemon_template;
    daemon_template.block_reward = plan.reward_atomic;
    daemon_template.block_reward_without_fee = plan.reward_atomic;
    daemon_template.blocktemplate_blob = bytes_to_hex(
        make_block_blob(hex_to_bytes(daemon_miner_tx.tx_blob_hex)));
    daemon_template.difficulty = "1000000";
    daemon_template.height = 166331;
    daemon_template.miner_tx_tgc_json = daemon_miner_tx.miner_tx_tgc_json;
    daemon_template.status = "OK";
    daemon_template.txs_fee = 0;

    ShareChain chain;
    const Share first = make_payout_share(payout_a, 0, 1'000, 40);
    const AddShareResult first_added = chain.add_share_unchecked(first);
    CHECK(first_added.disposition == ShareDisposition::Connected);
    const Share second = make_payout_share(
        payout_b, 1, 1'010, 60, first_added.id);
    const AddShareResult second_added = chain.add_share_unchecked(second);
    CHECK(second_added.disposition == ShareDisposition::Connected);

    const MiningHeaderWork daemon_work = derive_mining_header_work(
        hex_to_bytes(daemon_template.blocktemplate_blob));
    const PplnsTemplateResult rebuilt = build_canonical_pplns_template(
        daemon_template,
        chain,
        canonical_sidechain_parameters(SidechainParentNetwork::Testnet),
        "p2pool-test");
    CHECK(rebuilt.status == PplnsTemplateStatus::Ready);
    CHECK(rebuilt.plan.status == PplnsCoinbasePlanStatus::Ready);
    CHECK(rebuilt.plan.destinations.size() == 2);
    CHECK(rebuilt.block.blocktemplate_blob != daemon_template.blocktemplate_blob);
    CHECK(!rebuilt.block.miner_tx_tgc_json.empty());
    CHECK(rebuilt.mining_work.block_header.serialized ==
          daemon_work.block_header.serialized);
    CHECK(rebuilt.mining_work.tx_hashes.hashes == daemon_work.tx_hashes.hashes);

    std::uint64_t payout_a_amount = 0;
    std::uint64_t payout_b_amount = 0;
    for (const auto& destination : rebuilt.plan.destinations) {
        if (destination.payout == payout_a) {
            payout_a_amount += destination.amount;
        }
        if (destination.payout == payout_b) {
            payout_b_amount += destination.amount;
        }
    }
    CHECK(payout_a_amount == 400000000000ULL);
    CHECK(payout_b_amount == 600000000000ULL);

    const auto rebuilt_policy = verify_miner_tx_payout_policy(
        rebuilt.mining_work.miner_tx_prefix.serialized,
        rebuilt.block.miner_tx_tgc_json,
        rebuilt.block.block_reward_without_fee,
        rebuilt.block.block_reward,
        rebuilt.block.txs_fee,
        rebuilt.plan);
    CHECK(rebuilt_policy.status == P2pPayoutPolicyStatus::Verified);
    CHECK(rebuilt_policy.verified_reward == rebuilt.block.block_reward);

    ShareChain empty_chain;
    const auto no_history = build_canonical_pplns_template(
        daemon_template,
        empty_chain,
        canonical_sidechain_parameters(SidechainParentNetwork::Testnet));
    CHECK(no_history.status == PplnsTemplateStatus::PayoutPlanUnavailable);

    return 0;
#endif
}