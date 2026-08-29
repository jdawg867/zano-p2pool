#pragma once

#include <array>
#include <cstdint>

namespace zano_p2pool {

using ZanoCurveKey = std::array<std::uint8_t, 32>;

[[nodiscard]] bool zano_curve_backend_available() noexcept;

// Uses Zano's exact Ed25519/CryptoNote secret_key_to_public_key operation when
// the pinned Zano crypto backend is compiled in. Lightweight builds fail closed
// by returning false and must check zano_curve_backend_available() separately.
[[nodiscard]] bool zano_secret_key_matches_public(
    const ZanoCurveKey& secret_key,
    const ZanoCurveKey& public_key) noexcept;

}  // namespace zano_p2pool
