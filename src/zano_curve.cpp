#include "zano_p2pool/zano_curve.hpp"

#include <cstring>

#ifdef ZANO_P2POOL_HAVE_ZANO_CURVE
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

}  // namespace zano_p2pool
