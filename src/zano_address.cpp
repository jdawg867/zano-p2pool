#include "zano_p2pool/zano_address.hpp"

#include "zano_p2pool/crypto_hash.hpp"

#include <boost/multiprecision/cpp_int.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace zano_p2pool {
namespace {

constexpr std::string_view kBase58Alphabet =
    "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
constexpr std::array<std::size_t, 9> kEncodedBlockSizes{
    0, 2, 3, 5, 6, 7, 9, 10, 11};
constexpr std::size_t kDecodedFullBlockSize = 8;
constexpr std::size_t kEncodedFullBlockSize = 11;
constexpr std::size_t kAddressChecksumSize = 4;
constexpr std::size_t kClassicAddressPayloadSize = 64;

[[nodiscard]] int decoded_block_size(std::size_t encoded_size) noexcept {
    for (std::size_t i = 0; i < kEncodedBlockSizes.size(); ++i) {
        if (kEncodedBlockSizes[i] == encoded_size) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

[[nodiscard]] int base58_digit(char c) noexcept {
    const std::size_t position = kBase58Alphabet.find(c);
    return position == std::string_view::npos
        ? -1
        : static_cast<int>(position);
}

bool decode_block(
    std::string_view encoded,
    std::span<std::uint8_t> output) noexcept {
    if (encoded.empty() || output.empty() || output.size() > 8) {
        return false;
    }

    boost::multiprecision::uint128_t value = 0;
    for (const char c : encoded) {
        const int digit = base58_digit(c);
        if (digit < 0) {
            return false;
        }
        value *= 58;
        value += static_cast<unsigned int>(digit);
    }

    boost::multiprecision::uint128_t limit = 1;
    limit <<= (output.size() * 8);
    if (value >= limit) {
        return false;
    }

    for (std::size_t i = output.size(); i-- > 0;) {
        output[i] = static_cast<std::uint8_t>(
            (value & 0xff).convert_to<unsigned int>());
        value >>= 8;
    }
    return true;
}

bool decode_base58(
    std::string_view encoded,
    std::vector<std::uint8_t>& decoded) noexcept {
    if (encoded.empty()) {
        decoded.clear();
        return true;
    }

    const std::size_t full_blocks = encoded.size() / kEncodedFullBlockSize;
    const std::size_t remainder = encoded.size() % kEncodedFullBlockSize;
    const int tail_size = decoded_block_size(remainder);
    if (tail_size < 0) {
        return false;
    }

    decoded.assign(
        full_blocks * kDecodedFullBlockSize +
            static_cast<std::size_t>(tail_size),
        0);

    for (std::size_t i = 0; i < full_blocks; ++i) {
        const std::string_view block = encoded.substr(
            i * kEncodedFullBlockSize,
            kEncodedFullBlockSize);
        std::span<std::uint8_t> out(
            decoded.data() + i * kDecodedFullBlockSize,
            kDecodedFullBlockSize);
        if (!decode_block(block, out)) {
            return false;
        }
    }

    if (remainder != 0) {
        const std::string_view block = encoded.substr(
            full_blocks * kEncodedFullBlockSize,
            remainder);
        std::span<std::uint8_t> out(
            decoded.data() + full_blocks * kDecodedFullBlockSize,
            static_cast<std::size_t>(tail_size));
        if (!decode_block(block, out)) {
            return false;
        }
    }
    return true;
}

bool read_canonical_varint(
    std::span<const std::uint8_t> bytes,
    std::uint64_t& value,
    std::size_t& consumed) noexcept {
    value = 0;
    consumed = 0;
    unsigned int shift = 0;

    for (std::size_t i = 0; i < bytes.size() && i < 10; ++i) {
        const std::uint8_t byte = bytes[i];
        const std::uint64_t part = byte & 0x7fU;
        if (shift >= 64 ||
            (shift == 63 && part > 1)) {
            return false;
        }
        value |= part << shift;
        if ((byte & 0x80U) == 0) {
            consumed = i + 1;
            // Reject non-minimal encodings such as 0x81 0x00.
            if (consumed > 1 && part == 0) {
                return false;
            }
            return true;
        }
        shift += 7;
    }
    return false;
}

}  // namespace

ZanoAddressDecodeResult decode_zano_standard_address(
    std::string_view address) noexcept {
    ZanoAddressDecodeResult result;
    try {
        std::vector<std::uint8_t> decoded;
        if (!decode_base58(address, decoded) ||
            decoded.size() <= kAddressChecksumSize) {
            result.status = ZanoAddressDecodeStatus::InvalidBase58;
            return result;
        }

        const std::size_t content_size = decoded.size() - kAddressChecksumSize;
        const std::span<const std::uint8_t> content(
            decoded.data(), content_size);
        const Hash256 checksum_hash = cn_fast_hash(content);
        if (!std::equal(
                decoded.begin() + static_cast<std::ptrdiff_t>(content_size),
                decoded.end(),
                checksum_hash.begin())) {
            result.status = ZanoAddressDecodeStatus::InvalidChecksum;
            return result;
        }

        std::uint64_t prefix = 0;
        std::size_t prefix_size = 0;
        if (!read_canonical_varint(content, prefix, prefix_size) ||
            prefix != kZanoStandardAddressBase58Prefix) {
            result.status = ZanoAddressDecodeStatus::InvalidPrefix;
            return result;
        }

        if (content.size() - prefix_size != kClassicAddressPayloadSize) {
            result.status = ZanoAddressDecodeStatus::UnsupportedPayload;
            return result;
        }

        const auto payload = content.subspan(prefix_size);
        std::copy_n(
            payload.begin(),
            result.payout.spend_public_key.size(),
            result.payout.spend_public_key.begin());
        std::copy_n(
            payload.begin() + static_cast<std::ptrdiff_t>(
                result.payout.spend_public_key.size()),
            result.payout.view_public_key.size(),
            result.payout.view_public_key.begin());
        result.status = ZanoAddressDecodeStatus::Valid;
        return result;
    } catch (...) {
        result.status = ZanoAddressDecodeStatus::InvalidBase58;
        return result;
    }
}

const char* zano_address_decode_status_name(
    ZanoAddressDecodeStatus status) noexcept {
    switch (status) {
    case ZanoAddressDecodeStatus::Valid:
        return "valid";
    case ZanoAddressDecodeStatus::InvalidBase58:
        return "invalid-base58";
    case ZanoAddressDecodeStatus::InvalidChecksum:
        return "invalid-checksum";
    case ZanoAddressDecodeStatus::InvalidPrefix:
        return "invalid-prefix";
    case ZanoAddressDecodeStatus::UnsupportedPayload:
        return "unsupported-payload";
    }
    return "unknown";
}

}  // namespace zano_p2pool
