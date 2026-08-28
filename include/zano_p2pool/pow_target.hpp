#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace zano_p2pool {

using Hash256 = std::array<std::uint8_t, 32>;

// Canonical target representation used by zano-p2pool.
// Bytes are stored most-significant first so hex() matches the numeric target.
struct DifficultyTarget {
    Hash256 big_endian{};

    [[nodiscard]] std::string hex() const;
};

// Zano consensus computes floor((2^256 - 1) / difficulty).
// Difficulty is a positive decimal uint128 value, matching Zano's
// currency::wide_difficulty_type.
[[nodiscard]] DifficultyTarget difficulty_to_target(
    std::string_view difficulty_decimal);

// Zano's check_hash() treats the incoming 32 hash bytes as a big-endian
// 256-bit integer for the consensus comparison.
[[nodiscard]] bool hash_meets_target(
    const Hash256& hash,
    const DifficultyTarget& target);

[[nodiscard]] bool hash_meets_difficulty(
    const Hash256& hash,
    std::string_view difficulty_decimal);

}  // namespace zano_p2pool
