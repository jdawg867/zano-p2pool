#include "zano_p2pool/zano_miner_tx.hpp"

#include "zano_p2pool/crypto_hash.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef ZANO_P2POOL_HAVE_ZANO_MINER_TX
#include "currency_core/currency_format_utils.h"
#include "serialization/binary_utils.h"
#include "serialization/keyvalue_serialization.h"
#endif

namespace zano_p2pool {

bool zano_miner_tx_builder_available() noexcept {
#ifdef ZANO_P2POOL_HAVE_ZANO_MINER_TX
    return true;
#else
    return false;
#endif
}

std::uint64_t extract_zano_hf6_coinbase_cumulative_size(
    std::span<const std::uint8_t> tx_blob) {
#ifndef ZANO_P2POOL_HAVE_ZANO_MINER_TX
    (void)tx_blob;
    throw std::runtime_error(
        "exact Zano miner-tx parser is not enabled; configure with "
        "ZANO_P2POOL_ZANO_SOURCE_DIR");
#else
    if (tx_blob.empty()) {
        throw std::invalid_argument("daemon miner transaction is empty");
    }

    const std::string blob(tx_blob.begin(), tx_blob.end());
    currency::transaction tx{};
    if (!currency::parse_and_validate_tx_from_blob(blob, tx)) {
        throw std::runtime_error("failed to parse daemon Zano miner transaction");
    }

    currency::etc_coinbase_block_cumulative_size cumulative_size{};
    if (!currency::get_type_in_variant_container(tx.extra, cumulative_size)) {
        throw std::runtime_error(
            "daemon HF6 miner transaction is missing cumulative block size");
    }
    return cumulative_size.v;
#endif
}

ZanoMinerTxResult build_zano_hf6_pplns_miner_tx(
    std::uint64_t height,
    std::uint64_t fee,
    std::uint64_t expected_block_reward,
    const PplnsCoinbasePlan& plan,
    std::string_view extra_text,
    std::uint64_t coinbase_block_cumulative_size) {
#ifndef ZANO_P2POOL_HAVE_ZANO_MINER_TX
    (void)height;
    (void)fee;
    (void)expected_block_reward;
    (void)plan;
    (void)extra_text;
    (void)coinbase_block_cumulative_size;
    throw std::runtime_error(
        "exact Zano miner-tx builder is not enabled; configure with "
        "ZANO_P2POOL_ZANO_SOURCE_DIR");
#else
    if (plan.status != PplnsCoinbasePlanStatus::Ready) {
        throw std::invalid_argument("PPLNS coinbase plan is not ready");
    }
    if (plan.destinations.empty()) {
        throw std::invalid_argument("PPLNS coinbase plan has no destinations");
    }
    if (plan.destinations.size() > kZanoHf6MaxCoinbaseOutputs) {
        throw std::invalid_argument("PPLNS coinbase plan exceeds Zano output limit");
    }
    if (plan.reward_atomic != expected_block_reward) {
        throw std::invalid_argument(
            "PPLNS coinbase reward does not match daemon template reward");
    }

    static_assert(
        CURRENCY_TX_MIN_ALLOWED_OUTS == 2,
        "re-audit PPLNS physical-output expansion for this Zano version");

    std::vector<currency::tx_destination_entry> destinations;
    destinations.reserve(std::max<std::size_t>(
        plan.destinations.size(), CURRENCY_TX_MIN_ALLOWED_OUTS));

    const auto append_destination = [&](const PayoutPublicKeys& payout,
                                        std::uint64_t amount) {
        currency::account_public_address address{};
        static_assert(sizeof(address.spend_public_key) == 32);
        static_assert(sizeof(address.view_public_key) == 32);
        std::copy(
            payout.spend_public_key.begin(),
            payout.spend_public_key.end(),
            reinterpret_cast<std::uint8_t*>(&address.spend_public_key));
        std::copy(
            payout.view_public_key.begin(),
            payout.view_public_key.end(),
            reinterpret_cast<std::uint8_t*>(&address.view_public_key));
        address.flags = 0;

        currency::tx_destination_entry de{};
        de.addr.push_back(address);
        de.amount = amount;
        de.asset_id = currency::native_coin_asset_id;
        de.flags |= currency::tx_destination_entry_flags::
            tdef_explicit_native_asset_id;
        destinations.push_back(std::move(de));
    };

    for (const auto& destination : plan.destinations) {
        append_destination(destination.payout, destination.amount);
    }

    // Zano enforces CURRENCY_TX_MIN_ALLOWED_OUTS for PoW coinbase
    // transactions after HF4. A valid PPLNS window can contain only one miner,
    // so preserve that one logical payout while expanding it into two physical
    // outputs to the same payout keys. Payout verification aggregates physical
    // outputs per recipient and therefore still binds to the unchanged plan.
    if (destinations.size() == 1) {
        if (plan.destinations.front().amount < CURRENCY_TX_MIN_ALLOWED_OUTS) {
            throw std::invalid_argument(
                "single-recipient PPLNS payout is too small for Zano minimum outputs");
        }

        const std::uint64_t total = plan.destinations.front().amount;
        const std::uint64_t first_amount = total / 2;
        const std::uint64_t second_amount = total - first_amount;
        destinations.front().amount = first_amount;
        append_destination(plan.destinations.front().payout, second_amount);
    }

    if (destinations.size() > kZanoHf6MaxCoinbaseOutputs) {
        throw std::invalid_argument(
            "expanded PPLNS coinbase exceeds Zano output limit");
    }

    const currency::account_public_address miner_address =
        boost::get<currency::account_public_address>(
            destinations.front().addr.front());
    const currency::account_public_address stakeholder_address = miner_address;

    // Zano HF6 consensus requires etc_coinbase_block_cumulative_size to be
    // present in coinbase extra. construct_miner_tx() intentionally preserves
    // pre-seeded extra fields, which is also how Zano's own template builder
    // supplies this value before constructing the rest of the transaction.
    currency::transaction tx{};
    currency::etc_coinbase_block_cumulative_size cumulative_size_entry{};
    cumulative_size_entry.v = coinbase_block_cumulative_size;
    tx.extra.push_back(cumulative_size_entry);

    std::uint64_t block_reward_without_fee = 0;
    std::uint64_t block_reward = 0;
    currency::tx_generation_context generation_context{};
    const std::string extra_nonce(extra_text);

    // Current pinned Zano uses transaction version 4 / hardfork id 6 for HF6
    // PoW miner transactions. median=current full-reward-zone and block size 0
    // deliberately force the no-penalty reward branch. The caller must reject
    // the assembled block if it later exceeds kZanoFullRewardZoneBytes.
    const bool constructed = currency::construct_miner_tx(
        static_cast<std::size_t>(height),
        kZanoFullRewardZoneBytes,
        boost::multiprecision::uint128_t(0),
        0,
        fee,
        miner_address,
        stakeholder_address,
        tx,
        block_reward_without_fee,
        block_reward,
        4,
        6,
        extra_nonce,
        kZanoHf6MaxCoinbaseOutputs,
        false,
        currency::pos_entry{},
        &generation_context,
        nullptr,
        destinations);
    if (!constructed) {
        throw std::runtime_error("Zano construct_miner_tx rejected PPLNS destinations");
    }
    if (block_reward != expected_block_reward ||
        block_reward_without_fee != expected_block_reward) {
        throw std::runtime_error(
            "Zano canonical miner-tx reward disagrees with daemon template");
    }

    std::string blob;
    if (!serialization::dump_binary(tx, blob)) {
        throw std::runtime_error("failed to serialize canonical Zano miner tx");
    }

    const std::string miner_tx_tgc_json =
        epee::serialization::store_t_to_json(generation_context);
    if (miner_tx_tgc_json.empty()) {
        throw std::runtime_error(
            "failed to serialize canonical Zano miner tx generation context");
    }

    std::vector<std::uint8_t> bytes(blob.begin(), blob.end());
    return ZanoMinerTxResult{
        bytes_to_hex(bytes),
        miner_tx_tgc_json,
        block_reward_without_fee,
        block_reward,
    };
#endif
}

}  // namespace zano_p2pool
