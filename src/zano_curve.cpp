#include "zano_p2pool/zano_curve.hpp"

#include <cstring>

#ifdef ZANO_P2POOL_HAVE_ZANO_CURVE
#include "crypto/crypto-sugar.h"
#include "crypto/crypto.h"
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

}  // namespace zano_p2pool
