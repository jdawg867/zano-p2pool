#include "zano_p2pool/share.hpp"

#include "zano_p2pool/crypto_hash.hpp"

#include <boost/multiprecision/cpp_int.hpp>

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace zano_p2pool {
namespace {

using boost::multiprecision::uint128_t;
using boost::multiprecision::uint256_t;

constexpr std::array<std::uint8_t, 4> kShareMagic{'Z', 'P', '2', 'S'};

uint128_t parse_uint128_decimal(std::string_view text) {
    if (text.empty()) {
        throw std::invalid_argument("difficulty is empty");
    }

    uint256_t value = 0;
    const uint256_t max128 = std::numeric_limits<uint128_t>::max();
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            throw std::invalid_argument(
                "difficulty must contain decimal digits only");
        }
        value *= 10;
        value += static_cast<unsigned>(ch - '0');
        if (value > max128) {
            throw std::out_of_range("difficulty exceeds uint128 range");
        }
    }

    if (value == 0) {
        throw std::invalid_argument("difficulty must be greater than zero");
    }
    return value.convert_to<uint128_t>();
}

void append_u64_be(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

std::uint64_t read_u64_be(
    std::span<const std::uint8_t> bytes,
    std::size_t& offset) {
    if (offset > bytes.size() || bytes.size() - offset < 8) {
        throw std::runtime_error("truncated share uint64 field");
    }

    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value = (value << 8) | bytes[offset++];
    }
    return value;
}

template <std::size_t N>
void append_array(
    std::vector<std::uint8_t>& out,
    const std::array<std::uint8_t, N>& value) {
    out.insert(out.end(), value.begin(), value.end());
}

template <std::size_t N>
std::array<std::uint8_t, N> read_array(
    std::span<const std::uint8_t> bytes,
    std::size_t& offset) {
    if (offset > bytes.size() || bytes.size() - offset < N) {
        throw std::runtime_error("truncated share byte-array field");
    }

    std::array<std::uint8_t, N> value{};
    std::copy_n(
        bytes.begin() + static_cast<std::ptrdiff_t>(offset),
        N,
        value.begin());
    offset += N;
    return value;
}

}  // namespace

Difficulty128 difficulty128_from_decimal(std::string_view decimal) {
    uint128_t value = parse_uint128_decimal(decimal);
    Difficulty128 bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[bytes.size() - 1 - i] = static_cast<std::uint8_t>(
            (value & 0xff).convert_to<unsigned>());
        value >>= 8;
    }
    return bytes;
}

std::string difficulty128_to_decimal(const Difficulty128& difficulty) {
    uint128_t value = 0;
    for (const std::uint8_t byte : difficulty) {
        value <<= 8;
        value |= byte;
    }
    return value.convert_to<std::string>();
}

bool difficulty128_is_zero(const Difficulty128& difficulty) noexcept {
    return std::all_of(
        difficulty.begin(), difficulty.end(), [](std::uint8_t byte) {
            return byte == 0;
        });
}

bool is_zero_share_id(const ShareId& id) noexcept {
    return std::all_of(id.begin(), id.end(), [](std::uint8_t byte) {
        return byte == 0;
    });
}

std::vector<std::uint8_t> serialize_share(const Share& share) {
    if (share.version != kShareVersion1) {
        throw std::invalid_argument("unsupported share version");
    }

    std::vector<std::uint8_t> bytes;
    bytes.reserve(kShareV1SerializedSize);
    append_array(bytes, kShareMagic);
    bytes.push_back(share.version);
    append_array(bytes, share.parent_id);
    append_u64_be(bytes, share.share_height);
    append_u64_be(bytes, share.timestamp);
    append_u64_be(bytes, share.zano_height);
    append_array(bytes, share.mining_header_hash);
    append_u64_be(bytes, share.nonce);
    append_array(bytes, share.share_difficulty);
    append_array(bytes, share.network_difficulty);
    append_array(bytes, share.miner_id);

    if (bytes.size() != kShareV1SerializedSize) {
        throw std::logic_error("share v1 serialized-size invariant failed");
    }
    return bytes;
}

Share deserialize_share(std::span<const std::uint8_t> bytes) {
    if (bytes.size() != kShareV1SerializedSize) {
        throw std::runtime_error("share v1 has invalid serialized size");
    }

    std::size_t offset = 0;
    const auto magic = read_array<4>(bytes, offset);
    if (magic != kShareMagic) {
        throw std::runtime_error("invalid share domain marker");
    }

    Share share;
    share.version = bytes[offset++];
    if (share.version != kShareVersion1) {
        throw std::runtime_error("unsupported share version");
    }

    share.parent_id = read_array<32>(bytes, offset);
    share.share_height = read_u64_be(bytes, offset);
    share.timestamp = read_u64_be(bytes, offset);
    share.zano_height = read_u64_be(bytes, offset);
    share.mining_header_hash = read_array<32>(bytes, offset);
    share.nonce = read_u64_be(bytes, offset);
    share.share_difficulty = read_array<16>(bytes, offset);
    share.network_difficulty = read_array<16>(bytes, offset);
    share.miner_id = read_array<32>(bytes, offset);

    if (offset != bytes.size()) {
        throw std::logic_error("share parser did not consume canonical encoding");
    }
    return share;
}

ShareId share_id(const Share& share) {
    return cn_fast_hash(serialize_share(share));
}

}  // namespace zano_p2pool
