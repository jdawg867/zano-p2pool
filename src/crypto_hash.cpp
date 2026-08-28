#include "zano_p2pool/crypto_hash.hpp"

#include <array>
#include <cctype>
#include <cstring>
#include <stdexcept>

namespace zano_p2pool {
namespace {

constexpr std::array<std::uint64_t, 24> kRoundConstants{
    0x0000000000000001ULL, 0x0000000000008082ULL,
    0x800000000000808aULL, 0x8000000080008000ULL,
    0x000000000000808bULL, 0x0000000080000001ULL,
    0x8000000080008081ULL, 0x8000000000008009ULL,
    0x000000000000008aULL, 0x0000000000000088ULL,
    0x0000000080008009ULL, 0x000000008000000aULL,
    0x000000008000808bULL, 0x800000000000008bULL,
    0x8000000000008089ULL, 0x8000000000008003ULL,
    0x8000000000008002ULL, 0x8000000000000080ULL,
    0x000000000000800aULL, 0x800000008000000aULL,
    0x8000000080008081ULL, 0x8000000000008080ULL,
    0x0000000080000001ULL, 0x8000000080008008ULL,
};

constexpr std::array<int, 24> kRotationConstants{
    1, 3, 6, 10, 15, 21, 28, 36, 45, 55, 2, 14,
    27, 41, 56, 8, 25, 43, 62, 18, 39, 61, 20, 44,
};

constexpr std::array<int, 24> kPiLane{
    10, 7, 11, 17, 18, 3, 5, 16, 8, 21, 24, 4,
    15, 23, 19, 13, 12, 2, 20, 14, 22, 9, 6, 1,
};

[[nodiscard]] constexpr std::uint64_t rotl64(
    std::uint64_t value,
    int shift) noexcept {
    return (value << shift) | (value >> (64 - shift));
}

[[nodiscard]] std::uint64_t load_le64(const std::uint8_t* p) noexcept {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(p[i]) << (8 * i);
    }
    return value;
}

void store_le64(std::uint8_t* p, std::uint64_t value) noexcept {
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<std::uint8_t>(value >> (8 * i));
    }
}

void keccak_f1600(std::array<std::uint64_t, 25>& state) noexcept {
    std::array<std::uint64_t, 5> bc{};

    for (std::size_t round = 0; round < kRoundConstants.size(); ++round) {
        for (std::size_t i = 0; i < 5; ++i) {
            bc[i] = state[i] ^ state[i + 5] ^ state[i + 10] ^
                    state[i + 15] ^ state[i + 20];
        }

        for (std::size_t i = 0; i < 5; ++i) {
            const std::uint64_t t =
                bc[(i + 4) % 5] ^ rotl64(bc[(i + 1) % 5], 1);
            for (std::size_t j = 0; j < 25; j += 5) {
                state[j + i] ^= t;
            }
        }

        std::uint64_t t = state[1];
        for (std::size_t i = 0; i < kPiLane.size(); ++i) {
            const int j = kPiLane[i];
            const std::uint64_t next = state[static_cast<std::size_t>(j)];
            state[static_cast<std::size_t>(j)] =
                rotl64(t, kRotationConstants[i]);
            t = next;
        }

        for (std::size_t j = 0; j < 25; j += 5) {
            for (std::size_t i = 0; i < 5; ++i) {
                bc[i] = state[j + i];
            }
            for (std::size_t i = 0; i < 5; ++i) {
                state[j + i] ^=
                    (~bc[(i + 1) % 5]) & bc[(i + 2) % 5];
            }
        }

        state[0] ^= kRoundConstants[round];
    }
}

[[nodiscard]] int hex_nibble(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    throw std::invalid_argument("invalid hexadecimal character");
}

}  // namespace

Hash256 cn_fast_hash(std::span<const std::uint8_t> data) {
    constexpr std::size_t kRate = 136;  // 200 - 2 * 32
    constexpr std::size_t kRateWords = kRate / 8;

    std::array<std::uint64_t, 25> state{};

    while (data.size() >= kRate) {
        for (std::size_t i = 0; i < kRateWords; ++i) {
            state[i] ^= load_le64(data.data() + i * 8);
        }
        keccak_f1600(state);
        data = data.subspan(kRate);
    }

    std::array<std::uint8_t, kRate> tail{};
    if (!data.empty()) {
        std::memcpy(tail.data(), data.data(), data.size());
    }
    tail[data.size()] = 0x01;
    tail[kRate - 1] |= 0x80;

    for (std::size_t i = 0; i < kRateWords; ++i) {
        state[i] ^= load_le64(tail.data() + i * 8);
    }
    keccak_f1600(state);

    Hash256 result{};
    for (std::size_t i = 0; i < result.size() / 8; ++i) {
        store_le64(result.data() + i * 8, state[i]);
    }
    return result;
}

std::vector<std::uint8_t> hex_to_bytes(std::string_view hex) {
    if (hex.starts_with("0x") || hex.starts_with("0X")) {
        hex.remove_prefix(2);
    }
    if ((hex.size() & 1U) != 0U) {
        throw std::invalid_argument("hex string must contain an even number of digits");
    }

    std::vector<std::uint8_t> bytes;
    bytes.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const int high = hex_nibble(hex[i]);
        const int low = hex_nibble(hex[i + 1]);
        bytes.push_back(static_cast<std::uint8_t>((high << 4) | low));
    }
    return bytes;
}

std::string bytes_to_hex(std::span<const std::uint8_t> bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out(bytes.size() * 2, '0');
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        out[2 * i] = kHex[bytes[i] >> 4];
        out[2 * i + 1] = kHex[bytes[i] & 0x0f];
    }
    return out;
}

std::string hash_to_hex(const Hash256& hash) {
    return bytes_to_hex(hash);
}

}  // namespace zano_p2pool
