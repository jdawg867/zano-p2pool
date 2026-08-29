#include "zano_p2pool/zano_curve.hpp"

#include <cstring>

#ifdef ZANO_P2POOL_HAVE_ZANO_CURVE
#include "crypto/crypto-sugar.h"
#include "crypto/crypto.h"
#include "crypto/zarcanum.h"
#endif

namespace zano_p2pool {

bool zano_curve_backend_available() noexcept {
#ifdef ZANO_P2POOL_HAVE_ZANO_CURVE
    return true;
#else
    return false;
#endif
}

bool zano_secret_key_matches_public(
    const ZanoCurveKey& secret_key,
    const ZanoCurveKey& public_key) noexcept {
#ifdef ZANO_P2POOL_HAVE_ZANO_CURVE
    try {
        crypto::secret_key secret{};
        crypto::public_key expected{};
        crypto::public_key actual{};

        static_assert(sizeof(secret) == ZanoCurveKey{}.size());
        static_assert(sizeof(expected) == ZanoCurveKey{}.size());

        std::memcpy(&secret, secret_key.data(), secret_key.size());
        std::memcpy(&expected, public_key.data(), public_key.size());

        if (!crypto::secret_key_to_public_key(secret, actual)) {
            return false;
        }
        return std::memcmp(&actual, &expected, sizeof(actual)) == 0;
    } catch (...) {
        return false;
    }
#else
    static_cast<void>(secret_key);
    static_cast<void>(public_key);
    return false;
#endif
}

bool zano_derive_output_public_key(
    const ZanoCurveKey& tx_secret_key,
    const ZanoCurveKey& spend_public_key,
    const ZanoCurveKey& view_public_key,
    std::size_t output_index,
    ZanoCurveKey& output_public_key) noexcept {
#ifdef ZANO_P2POOL_HAVE_ZANO_CURVE
    try {
        crypto::secret_key tx_secret{};
        crypto::public_key spend_public{};
        crypto::public_key view_public{};
        crypto::key_derivation derivation{};
        crypto::public_key derived{};

        std::memcpy(&tx_secret, tx_secret_key.data(), tx_secret_key.size());
        std::memcpy(&spend_public, spend_public_key.data(), spend_public_key.size());
        std::memcpy(&view_public, view_public_key.data(), view_public_key.size());

        if (!crypto::generate_key_derivation(
                view_public, tx_secret, derivation)) {
            return false;
        }
        if (!crypto::derive_public_key(
                derivation, output_index, spend_public, derived)) {
            return false;
        }

        std::memcpy(output_public_key.data(), &derived, output_public_key.size());
        return true;
    } catch (...) {
        return false;
    }
#else
    static_cast<void>(tx_secret_key);
    static_cast<void>(spend_public_key);
    static_cast<void>(view_public_key);
    static_cast<void>(output_index);
    output_public_key = {};
    return false;
#endif
}

bool zano_amount_commitment_matches(
    std::uint64_t amount,
    const ZanoCurveKey& amount_blinding_mask,
    const ZanoCurveKey& blinded_asset_id,
    const ZanoCurveKey& amount_commitment) noexcept {
#ifdef ZANO_P2POOL_HAVE_ZANO_CURVE
    try {
        crypto::secret_key mask_secret{};
        crypto::public_key asset_public{};
        crypto::public_key commitment_public{};

        std::memcpy(
            &mask_secret,
            amount_blinding_mask.data(),
            amount_blinding_mask.size());
        std::memcpy(
            &asset_public,
            blinded_asset_id.data(),
            blinded_asset_id.size());
        std::memcpy(
            &commitment_public,
            amount_commitment.data(),
            amount_commitment.size());

        crypto::scalar_t mask(mask_secret);
        if (!mask.is_reduced()) {
            return false;
        }

        crypto::point_t asset_point(asset_public);
        crypto::point_t committed_point(commitment_public);
        asset_point.modify_mul8();
        committed_point.modify_mul8();

        const crypto::point_t expected =
            crypto::scalar_t(amount) * asset_point +
            mask * crypto::c_point_G;
        return committed_point == expected;
    } catch (...) {
        return false;
    }
#else
    static_cast<void>(amount);
    static_cast<void>(amount_blinding_mask);
    static_cast<void>(blinded_asset_id);
    static_cast<void>(amount_commitment);
    return false;
#endif
}

bool zano_verify_hf6_miner_balance_proof(
    const ZanoCurveKey& tx_id,
    std::uint64_t block_reward,
    const ZanoCurveKey& tx_public_key,
    std::span<const ZanoCurveKey> amount_commitments,
    std::span<const std::uint8_t> serialized_proof) noexcept {
#ifdef ZANO_P2POOL_HAVE_ZANO_CURVE
    try {
        static_assert(sizeof(crypto::hash) == ZanoCurveKey{}.size());
        static_assert(sizeof(crypto::public_key) == ZanoCurveKey{}.size());
        static_assert(sizeof(crypto::generic_double_schnorr_sig) == 96);

        if (block_reward == 0 || amount_commitments.empty() ||
            serialized_proof.size() != sizeof(crypto::generic_double_schnorr_sig)) {
            return false;
        }

        crypto::hash message_hash{};
        crypto::public_key tx_public{};
        crypto::generic_double_schnorr_sig proof{};
        std::memcpy(&message_hash, tx_id.data(), tx_id.size());
        std::memcpy(&tx_public, tx_public_key.data(), tx_public_key.size());
        std::memcpy(&proof, serialized_proof.data(), serialized_proof.size());

        crypto::point_t outputs_sum = crypto::c_point_0;
        for (const auto& serialized_commitment : amount_commitments) {
            crypto::public_key commitment_public{};
            std::memcpy(
                &commitment_public,
                serialized_commitment.data(),
                serialized_commitment.size());
            outputs_sum += crypto::point_t(commitment_public);
        }
        outputs_sum.modify_mul8();

        // Exact current-HF6 PoW coinbase balance equation from Zano:
        // generated native reward * H - sum(output amount commitments) = lin(G).
        const crypto::point_t commitment_to_zero =
            crypto::scalar_t(block_reward) * crypto::c_point_H - outputs_sum;

        return crypto::verify_double_schnorr_sig<
            crypto::gt_G,
            crypto::gt_G>(
                message_hash,
                commitment_to_zero,
                tx_public,
                proof);
    } catch (...) {
        return false;
    }
#else
    static_cast<void>(tx_id);
    static_cast<void>(block_reward);
    static_cast<void>(tx_public_key);
    static_cast<void>(amount_commitments);
    static_cast<void>(serialized_proof);
    return false;
#endif
}

}  // namespace zano_p2pool
