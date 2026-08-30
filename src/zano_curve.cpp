#include "zano_p2pool/zano_curve.hpp"

#include <cstring>
#include <vector>

#ifdef ZANO_P2POOL_HAVE_ZANO_CURVE
#include "crypto/crypto-sugar.h"
#include "crypto/crypto.h"
#include "crypto/range_proofs.h"
#include "crypto/zarcanum.h"
#endif

namespace zano_p2pool {
namespace {

#ifdef ZANO_P2POOL_HAVE_ZANO_CURVE
[[nodiscard]] crypto::public_key public_key_from_wire(const ZanoCurveKey& wire) {
    crypto::public_key key{};
    static_assert(sizeof(key) == ZanoCurveKey{}.size());
    std::memcpy(&key, wire.data(), wire.size());
    return key;
}

[[nodiscard]] crypto::scalar_t scalar_from_wire(const ZanoCurveKey& wire) {
    crypto::secret_key key{};
    static_assert(sizeof(key) == ZanoCurveKey{}.size());
    std::memcpy(&key, wire.data(), wire.size());
    return crypto::scalar_t(key);
}

[[nodiscard]] std::size_t ceil_log2(std::size_t value) noexcept {
    if (value <= 1) {
        return 0;
    }
    --value;
    std::size_t result = 0;
    while (value != 0) {
        value >>= 1;
        ++result;
    }
    return result;
}

[[nodiscard]] bool verify_ug_aggregation_proof(
    const crypto::hash& message,
    std::span<const ZanoCurveKey> amount_commitments_1div8,
    std::span<const ZanoCurveKey> blinded_asset_ids_1div8,
    const crypto::vector_UG_aggregation_proof& proof) {
    const std::size_t n = amount_commitments_1div8.size();
    if (n == 0 || blinded_asset_ids_1div8.size() != n ||
        proof.amount_commitments_for_rp_aggregation.size() != n ||
        proof.y0s.size() != n || proof.y1s.size() != n ||
        !proof.c.is_reduced()) {
        return false;
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (!proof.y0s[i].is_reduced() || !proof.y1s[i].is_reduced()) {
            return false;
        }
    }

    // Exact transcript/equation from Zano's
    // verify_vector_UG_aggregation_proof() at the pinned source revision.
    crypto::hash_helper_t::hs_t hash_calculator(1 + 3 * n);
    hash_calculator.add_hash(message);

    std::vector<crypto::point_t> amount_commitments;
    amount_commitments.reserve(n);
    for (const auto& wire : amount_commitments_1div8) {
        crypto::point_t point(public_key_from_wire(wire));
        point.modify_mul8();
        hash_calculator.add_point(point);
        amount_commitments.push_back(point);
    }

    std::vector<crypto::point_t> rp_commitments;
    rp_commitments.reserve(n);
    for (const auto& public_key : proof.amount_commitments_for_rp_aggregation) {
        crypto::point_t point(public_key);
        point.modify_mul8();
        hash_calculator.add_point(point);
        rp_commitments.push_back(point);
    }

    const crypto::scalar_t w = hash_calculator.calc_hash(false);

    std::vector<crypto::point_t> asset_tag_plus_u;
    asset_tag_plus_u.reserve(n);
    for (const auto& wire : blinded_asset_ids_1div8) {
        crypto::point_t asset_tag(public_key_from_wire(wire));
        asset_tag.modify_mul8();
        asset_tag_plus_u.push_back(asset_tag + w * crypto::c_point_U);
    }

    for (std::size_t i = 0; i < n; ++i) {
        const crypto::point_t response =
            proof.y0s[i] * asset_tag_plus_u[i] +
            proof.y1s[i] * crypto::c_point_G +
            proof.c * (amount_commitments[i] + w * rp_commitments[i]);
        hash_calculator.add_pub_key(response.to_public_key());
    }

    const crypto::scalar_t challenge = hash_calculator.calc_hash();
    return proof.c == challenge;
}
#endif

}  // namespace

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

bool zano_verify_hf6_miner_range_proof(
    const ZanoCurveKey& tx_id,
    std::span<const ZanoCurveKey> amount_commitments,
    std::span<const ZanoCurveKey> blinded_asset_ids,
    const ZanoBppSignature& bpp,
    const ZanoUgAggregationProof& aggregation) noexcept {
#ifdef ZANO_P2POOL_HAVE_ZANO_CURVE
    try {
        const std::size_t n = amount_commitments.size();
        if (n < 2 || n > crypto::bpp_crypto_trait_ZC_out::c_bpp_values_max ||
            blinded_asset_ids.size() != n ||
            aggregation.amount_commitments_for_rp_aggregation.size() != n ||
            aggregation.y0s.size() != n || aggregation.y1s.size() != n ||
            bpp.left.size() != bpp.right.size() ||
            bpp.left.size() !=
                crypto::bpp_crypto_trait_ZC_out::c_bpp_log2_n + ceil_log2(n)) {
            return false;
        }

        crypto::bpp_signature signature{};
        signature.L.reserve(bpp.left.size());
        signature.R.reserve(bpp.right.size());
        for (const auto& wire : bpp.left) {
            signature.L.push_back(public_key_from_wire(wire));
        }
        for (const auto& wire : bpp.right) {
            signature.R.push_back(public_key_from_wire(wire));
        }
        signature.A0 = public_key_from_wire(bpp.a0);
        signature.A = public_key_from_wire(bpp.a);
        signature.B = public_key_from_wire(bpp.b);
        signature.r = scalar_from_wire(bpp.r);
        signature.s = scalar_from_wire(bpp.s);
        signature.delta = scalar_from_wire(bpp.delta);

        crypto::vector_UG_aggregation_proof aggregation_proof{};
        aggregation_proof.amount_commitments_for_rp_aggregation.reserve(n);
        for (const auto& wire :
             aggregation.amount_commitments_for_rp_aggregation) {
            aggregation_proof.amount_commitments_for_rp_aggregation.push_back(
                public_key_from_wire(wire));
        }
        for (const auto& wire : aggregation.y0s) {
            aggregation_proof.y0s.push_back(scalar_from_wire(wire));
        }
        for (const auto& wire : aggregation.y1s) {
            aggregation_proof.y1s.push_back(scalar_from_wire(wire));
        }
        aggregation_proof.c = scalar_from_wire(aggregation.c);

        // BPP commitments are the E' points serialized premultiplied by 1/8.
        // bpp_verify() intentionally consumes those 1/8 points directly.
        std::vector<crypto::point_t> bpp_commitments;
        bpp_commitments.reserve(n);
        for (const auto& public_key :
             aggregation_proof.amount_commitments_for_rp_aggregation) {
            bpp_commitments.emplace_back(public_key);
        }
        const std::vector<crypto::bpp_sig_commit_ref_t> bpp_inputs{
            crypto::bpp_sig_commit_ref_t(signature, bpp_commitments)};
        if (!crypto::bpp_verify<crypto::bpp_crypto_trait_ZC_out>(bpp_inputs)) {
            return false;
        }

        crypto::hash message_hash{};
        std::memcpy(&message_hash, tx_id.data(), tx_id.size());
        return verify_ug_aggregation_proof(
            message_hash,
            amount_commitments,
            blinded_asset_ids,
            aggregation_proof);
    } catch (...) {
        return false;
    }
#else
    static_cast<void>(tx_id);
    static_cast<void>(amount_commitments);
    static_cast<void>(blinded_asset_ids);
    static_cast<void>(bpp);
    static_cast<void>(aggregation);
    return false;
#endif
}

}  // namespace zano_p2pool
