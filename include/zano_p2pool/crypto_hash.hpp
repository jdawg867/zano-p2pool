#pragma once

#include "zano_p2pool/pow_target.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace zano_p2pool {

// Zano/CryptoNote fast hash: Keccak-256 with the original Keccak padding
// (0x01 ... 0x80), not NIST SHA3-256 padding.
[[nodiscard]] Hash256 cn_fast_hash(std::span<const std::uint8_t> data);

[[nodiscard]] std::vector<std::uint8_t> hex_to_bytes(std::string_view hex);
[[nodiscard]] std::string bytes_to_hex(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::string hash_to_hex(const Hash256& hash);

}  // namespace zano_p2pool
