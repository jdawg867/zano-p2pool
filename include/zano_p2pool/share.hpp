#pragma once

#include "zano_p2pool/pow_target.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace zano_p2pool {

using ShareId = Hash256;
using MinerId = Hash256;
using Difficulty128 = std::array<std::uint8_t, 16>;

inline constexpr std::uint8_t kShareVersion1 = 1;
inline constexpr std::size_t kShareV1SerializedSize = 165;

// Canonical v1 local-sidechain share. All fixed-width integers are serialized
// big-endian. Difficulty values are unsigned 128-bit integers serialized as
// fixed 16-byte big-endian values. The wire format begins with the ASCII domain
// marker "ZP2S" followed by the version byte.
struct Share {
    std::uint8_t version{kShareVersion1};
    ShareId parent_id{};
    std::uint64_t share_height{0};
    std::uint64_t timestamp{0};
    std::uint64_t zano_height{0};
    Hash256 mining_header_hash{};
    std::uint64_t nonce{0};
    Difficulty128 share_difficulty{};
    Difficulty128 network_difficulty{};
    MinerId miner_id{};

    bool operator==(const Share&) const = default;
};

[[nodiscard]] Difficulty128 difficulty128_from_decimal(
    std::string_view decimal);
[[nodiscard]] std::string difficulty128_to_decimal(
    const Difficulty128& difficulty);
[[nodiscard]] bool difficulty128_is_zero(
    const Difficulty128& difficulty) noexcept;

[[nodiscard]] bool is_zero_share_id(const ShareId& id) noexcept;

[[nodiscard]] std::vector<std::uint8_t> serialize_share(const Share& share);
[[nodiscard]] Share deserialize_share(std::span<const std::uint8_t> bytes);
[[nodiscard]] ShareId share_id(const Share& share);

}  // namespace zano_p2pool
