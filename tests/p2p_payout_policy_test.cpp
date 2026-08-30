#include "test_check.hpp"

#include "zano_p2pool/crypto_hash.hpp"
#include "zano_p2pool/p2p_payout_policy.hpp"
#include "zano_p2pool/zano_curve.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using namespace zano_p2pool;

constexpr const char* kScalarOneHex =
    "0100000000000000000000000000000000000000000000000000000000000000";
constexpr const char* kScalarTwoHex =
    "0200000000000000000000000000000000000000000000000000000000000000";
constexpr const char* kZeroScalarHex =
    "0000000000000000000000000000000000000000000000000000000000000000";
constexpr const char* kEd25519BasepointHex =
    "5866666666666666666666666666666666666666666666666666666666666666";
constexpr const char* kNativeCoinAssetId1Div8Hex =
    "74c32d3eaafafc623bf483e858d42e8bf4ec7df064ada2e34934469cff6b6268";

[[nodiscard]] ZanoCurveKey key_from_hex(const std::string& hex) {
    const auto bytes = hex_to_bytes(hex);
    CHECK(bytes.size() == 32);
    ZanoCurveKey key{};
    std::copy(bytes.begin(), bytes.end(), key.begin());
    return key;
}

void append_key(std::vector<std::uint8_t>& out, const ZanoCurveKey& key) {
    out.insert(out.end(), key.begin(), key.end());
}

[[nodiscard]] std::vector<std::uint8_t> make_prefix(
    const ZanoCurveKey& tx_public_key,
    const std::vector<ZanoCurveKey>& stealth_addresses,
    const ZanoCurveKey& amount_commitment,
    const ZanoCurveKey& blinded_asset_id) {
    std::vector<std::uint8_t> prefix;
    prefix.reserve(64 + stealth_addresses.size() * 147);

    prefix.push_back(0x04);  // transaction version 4
    prefix.push_back(0x01);  // one input
    prefix.push_back(0x00);  // txin_gen
    prefix.push_back(0x01);  // height 1
    prefix.push_back(0x01);  // one extra
    prefix.push_back(0x16);  // transaction public key
    append_key(prefix, tx_public_key);

    CHECK(stealth_addresses.size() <= 0x7f);
    prefix.push_back(static_cast<std::uint8_t>(stealth_addresses.size()));
    for (const auto& stealth : stealth_addresses) {
        prefix.push_back(0x3f);  // tx_out_zarcanum
        prefix.push_back(0x00);  // output serialization version 0
        append_key(prefix, stealth);
        append_key(prefix, tx_public_key);  // structurally valid placeholder concealing point
        append_key(prefix, amount_commitment);
        append_key(prefix, blinded_asset_id);
        prefix.insert(prefix.end(), 8 + 8 + 1, 0);  // encrypted fields + mix_attr
    }
    prefix.push_back(0x06);  // hardfork_id 6
    return prefix;
}

[[nodiscard]] std::string repeated_scalar_list(
    const char* scalar_hex,
    std::size_t count) {
    std::string result;
    for (std::size_t i = 0; i < count; ++i) {
        if (!result.empty()) {
            result += ", ";
        }
        result += scalar_hex;
    }
    return result;
}

[[nodiscard]] std::string make_tgc(
    const char* secret_key_hex,
    std::size_t output_count,
    const char* amount_hex = kScalarOneHex,
    const char* amount_mask_hex = kZeroScalarHex,
    const char* asset_mask_hex = kZeroScalarHex) {
    return std::string("{\"tx_key\":\"") +
           kEd25519BasepointHex + secret_key_hex +
           "\",\"amounts\":\"" +
           repeated_scalar_list(amount_hex, output_count) +
           "\",\"amount_blinding_masks\":\"" +
           repeated_scalar_list(amount_mask_hex, output_count) +
           "\",\"asset_id_blinding_masks\":\"" +
           repeated_scalar_list(asset_mask_hex, output_count) +
           "\"}";
}

}  // namespace

int main() {
    const ZanoCurveKey scalar_one = key_from_hex(kScalarOneHex);
    const ZanoCurveKey basepoint = key_from_hex(kEd25519BasepointHex);
    const ZanoCurveKey native_asset = key_from_hex(kNativeCoinAssetId1Div8Hex);

    P2pPayoutAddress payout{basepoint, basepoint};

    std::vector<ZanoCurveKey> stealths(2);
    if (zano_curve_backend_available()) {
        CHECK(zano_derive_output_public_key(
            scalar_one, payout.spend_public_key, payout.view_public_key, 0, stealths[0]));
        CHECK(zano_derive_output_public_key(
            scalar_one, payout.spend_public_key, payout.view_public_key, 1, stealths[1]));
    } else {
        stealths[0] = basepoint;
        stealths[1] = basepoint;
    }

    const auto prefix = make_prefix(basepoint, stealths, native_asset, native_asset);
    const auto tgc = make_tgc(kScalarOneHex, 2);

    const auto good = verify_miner_tx_payout_policy(
        prefix, tgc, 2, 2, 12345, payout);
    if (!zano_curve_backend_available()) {
        CHECK(good.status == P2pPayoutPolicyStatus::BackendUnavailable);
        CHECK(std::string(p2p_payout_policy_status_name(good.status)) ==
              "backend-unavailable");
        return 0;
    }

    CHECK(good.status == P2pPayoutPolicyStatus::Verified);
    CHECK(good.output_count == 2);
    CHECK(good.verified_reward == 2);
    CHECK(std::string(p2p_payout_policy_status_name(good.status)) == "verified");

    // HF6 burns transaction fees; template reward must equal base reward.
    CHECK(verify_miner_tx_payout_policy(prefix, tgc, 1, 2, 1, payout).status ==
          P2pPayoutPolicyStatus::RewardMetadataMismatch);

    // If metadata agrees on a larger reward but commitments sum to two, reject.
    CHECK(verify_miner_tx_payout_policy(prefix, tgc, 3, 3, 0, payout).status ==
          P2pPayoutPolicyStatus::RewardSumMismatch);

    P2pPayoutAddress wrong_payout = payout;
    wrong_payout.spend_public_key = {};
    CHECK(verify_miner_tx_payout_policy(prefix, tgc, 2, 2, 0, wrong_payout).status ==
          P2pPayoutPolicyStatus::DestinationMismatch);

    auto bad_stealths = stealths;
    bad_stealths[0][0] ^= 0x01U;
    const auto bad_destination_prefix =
        make_prefix(basepoint, bad_stealths, native_asset, native_asset);
    CHECK(verify_miner_tx_payout_policy(
              bad_destination_prefix, tgc, 2, 2, 0, payout).status ==
          P2pPayoutPolicyStatus::DestinationMismatch);

    ZanoCurveKey bad_commitment = native_asset;
    bad_commitment[0] ^= 0x01U;
    const auto bad_commitment_prefix =
        make_prefix(basepoint, stealths, bad_commitment, native_asset);
    CHECK(verify_miner_tx_payout_policy(
              bad_commitment_prefix, tgc, 2, 2, 0, payout).status ==
          P2pPayoutPolicyStatus::AmountCommitmentMismatch);

    ZanoCurveKey non_native = native_asset;
    non_native[0] ^= 0x01U;
    const auto non_native_prefix =
        make_prefix(basepoint, stealths, native_asset, non_native);
    CHECK(verify_miner_tx_payout_policy(
              non_native_prefix, tgc, 2, 2, 0, payout).status ==
          P2pPayoutPolicyStatus::NonNativeAsset);

    const auto nonzero_asset_mask_tgc =
        make_tgc(kScalarOneHex, 2, kScalarOneHex, kZeroScalarHex, kScalarOneHex);
    CHECK(verify_miner_tx_payout_policy(
              prefix, nonzero_asset_mask_tgc, 2, 2, 0, payout).status ==
          P2pPayoutPolicyStatus::NonNativeAsset);

    const auto one_output_prefix =
        make_prefix(basepoint, std::vector<ZanoCurveKey>{stealths[0]}, native_asset, native_asset);
    const auto one_output_tgc = make_tgc(kScalarOneHex, 1);
    CHECK(verify_miner_tx_payout_policy(
              one_output_prefix, one_output_tgc, 1, 1, 0, payout).status ==
          P2pPayoutPolicyStatus::InvalidOutputCount);

    const std::string malformed_counts_tgc =
        std::string("{\"tx_key\":\"") + kEd25519BasepointHex + kScalarOneHex +
        "\",\"amounts\":\"" + repeated_scalar_list(kScalarOneHex, 1) +
        "\",\"amount_blinding_masks\":\"" + repeated_scalar_list(kZeroScalarHex, 2) +
        "\",\"asset_id_blinding_masks\":\"" + repeated_scalar_list(kZeroScalarHex, 2) +
        "\"}";
    CHECK(verify_miner_tx_payout_policy(
              prefix, malformed_counts_tgc, 2, 2, 0, payout).status ==
          P2pPayoutPolicyStatus::TgcOutputCountMismatch);

    const auto bad_keypair_tgc = make_tgc(kScalarTwoHex, 2);
    CHECK(verify_miner_tx_payout_policy(
              prefix, bad_keypair_tgc, 2, 2, 0, payout).status ==
          P2pPayoutPolicyStatus::KeyBindingFailed);

    return 0;
}
