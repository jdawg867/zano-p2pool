#pragma once

#include "zano_p2pool/pow_target.hpp"

#include <cstdint>

namespace zano_p2pool {

struct ProgPowZResult {
    Hash256 final_hash{};
    Hash256 mix_hash{};
};

enum class ProgPowZContextMode {
    Light,
    Full,
};

inline constexpr std::uint64_t kProgPowZEpochLength = 30000;

[[nodiscard]] std::uint64_t progpowz_epoch(std::uint64_t height) noexcept;
// Independently derive the Ethash epoch seed (Keccak-256, not SHA3-256).
// Reject heights outside the block-number range supported by our PoW backend.
[[nodiscard]] Hash256 progpowz_seed(std::uint64_t height);
[[nodiscard]] bool progpowz_available() noexcept;
[[nodiscard]] const char* progpowz_revision() noexcept;

// Hash one Zano ProgPoWZ candidate. Light mode uses the light cache and is
// suitable for compatibility tests; Full mode uses the full epoch context and
// is intended for high-throughput share verification.
[[nodiscard]] ProgPowZResult progpowz_hash(
    std::uint64_t height,
    const Hash256& header_hash,
    std::uint64_t nonce,
    ProgPowZContextMode mode);

}  // namespace zano_p2pool
