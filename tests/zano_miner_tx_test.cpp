#include "zano_p2pool/crypto_hash.hpp"
#include "zano_p2pool/mining_header.hpp"
#include "zano_p2pool/zano_address.hpp"
#include "zano_p2pool/zano_miner_tx.hpp"
#include "test_check.hpp"

#include <stdexcept>
#include <string>

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

    const auto tx_bytes = hex_to_bytes(result.tx_blob_hex);
    const ParsedMinerTxPrefix parsed = parse_hf6_miner_tx_prefix(tx_bytes);
    CHECK(parsed.version == 4);
    CHECK(parsed.hardfork_id == 6);
    CHECK(parsed.vin_count == 1);
    CHECK(parsed.vout_count == 2);

    return 0;
#endif
}
