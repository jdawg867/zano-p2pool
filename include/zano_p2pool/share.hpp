#pragma once

#include "zano_p2pool/pow_target.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace zano_p2pool {

using ShareId = Hash256;
using MinerId = Hash256;
using Difficulty128 = std::array<std::uint8_t, 16>;

struct PayoutPublicKeys {
    Hash256 spend_public_key{};
    Hash256 view_public_key{};

    bool operator==(const PayoutPublicKeys&) const = default;
};

inline constexpr std::uint8_t kShareVersion1 = 1;
inline constexpr std::uint8_t kShareVersion2 = 2;
inline constexpr std::size_t kShareV1SerializedSize = 165;
inline constexpr std::size_t kShareV2SerializedSize =
    kShareV1SerializedSize + 64;

// Canonical local-sidechain share. V1 ends at miner_id and remains accepted for
// development/backward compatibility. V2 appends the miner's public Zano spend
// and view keys. V2 therefore carries enough information for every peer to
// reproduce payout accounting without a node-local MinerId -> address database.
// No wallet secret material is present in either version.
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
    std::optional<PayoutPublicKeys> payout;

    bool operator==(const Share&) const = default;
};

[[nodiscard]] Difficulty128 difficulty128_from_decimal(
    std::string_view decimal);
[[nodiscard]] std::string difficulty128_to_decimal(
    const Difficulty128& difficulty);
[[nodiscard]] bool difficulty128_is_zero(
    const Difficulty128& difficulty) noexcept;

[[nodiscard]] bool is_zero_share_id(const ShareId& id) noexcept;

// Domain-separated public payout identity. A v2 share is canonical only when
// miner_id equals this hash of its spend/view public keys.
[[nodiscard]] MinerId miner_id_from_payout(
    const PayoutPublicKeys& payout);

[[nodiscard]] std::vector<std::uint8_t> serialize_share(const Share& share);
[[nodiscard]] Share deserialize_share(std::span<const std::uint8_t> bytes);
[[nodiscard]] ShareId share_id(const Share& share);

}  // namespace zano_p2pool
