#include "zano_p2pool/zano_miner_tx.hpp"

#include "zano_p2pool/crypto_hash.hpp"

#include <stdexcept>
#include <string>
#include <vector>

#ifdef ZANO_P2POOL_HAVE_ZANO_MINER_TX
#include "currency_core/currency_format_utils.h"
#include "serialization/binary_utils.h"
#endif

namespace zano_p2pool {

bool zano_miner_tx_builder_available() noexcept {
#ifdef ZANO_P2POOL_HAVE_ZANO_MINER_TX
    return true;
#else
    return false;
#endif
}

ZanoMinerTxResult build_zano_hf6_pplns_miner_tx(
    std::uint64_t height,
    std::uint64_t fee,
    std::uint64_t expected_block_reward,
    const PplnsCoinbasePlan& plan,
    std::string_view extra_text) {
#ifndef ZANO_P2POOL_HAVE_ZANO_MINER_TX
    (void)height;
    (void)fee;
    (void)expected_block_reward;
    (void)plan;
    (void)extra_text;
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

    std::vector<currency::tx_destination_entry> destinations;
    destinations.reserve(plan.destinations.size());
    for (const auto& destination : plan.destinations) {
        currency::account_public_address address{};
        static_assert(sizeof(address.spend_public_key) == 32);
        static_assert(sizeof(address.view_public_key) == 32);
        std::copy(
            destination.payout.spend_public_key.begin(),
            destination.payout.spend_public_key.end(),
            reinterpret_cast<std::uint8_t*>(&address.spend_public_key));
        std::copy(
            destination.payout.view_public_key.begin(),
            destination.payout.view_public_key.end(),
            reinterpret_cast<std::uint8_t*>(&address.view_public_key));
        address.flags = 0;

        currency::tx_destination_entry de{};
        de.addr.push_back(address);
        de.amount = destination.amount;
        de.asset_id = currency::native_coin_asset_id;
        de.flags |= currency::tx_destination_entry_flags::
            tdef_explicit_native_asset_id;
        destinations.push_back(std::move(de));
    }

    const currency::account_public_address miner_address =
        boost::get<currency::account_public_address>(
            destinations.front().addr.front());
    const currency::account_public_address stakeholder_address = miner_address;

    currency::transaction tx{};
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

    std::vector<std::uint8_t> bytes(blob.begin(), blob.end());
    return ZanoMinerTxResult{
        bytes_to_hex(bytes),
        block_reward_without_fee,
        block_reward,
    };
#endif
}

}  // namespace zano_p2pool
