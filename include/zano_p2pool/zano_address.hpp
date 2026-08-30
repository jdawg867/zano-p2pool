#pragma once

#include "zano_p2pool/p2p_payout_policy.hpp"

#include <cstdint>
#include <string_view>

namespace zano_p2pool {

inline constexpr std::uint64_t kZanoStandardAddressBase58Prefix = 0xc5;

enum class ZanoAddressDecodeStatus : std::uint8_t {
    Valid,
    InvalidBase58,
    InvalidChecksum,
    InvalidPrefix,
    UnsupportedPayload,
};

struct ZanoAddressDecodeResult {
    ZanoAddressDecodeStatus status{ZanoAddressDecodeStatus::InvalidBase58};
    P2pPayoutAddress payout{};
};

// Decode the classic non-auditable Zano address format used by ordinary `Zx`
// payout addresses. The payload is exactly account_public_address_old:
// spend_public_key (32 bytes) || view_public_key (32 bytes). The outer address
// uses CryptoNote block-base58, varint prefix 0xc5 and a four-byte cn_fast_hash
// checksum. No wallet secret material is involved.
[[nodiscard]] ZanoAddressDecodeResult decode_zano_standard_address(
    std::string_view address) noexcept;

[[nodiscard]] const char* zano_address_decode_status_name(
    ZanoAddressDecodeStatus status) noexcept;

}  // namespace zano_p2pool
