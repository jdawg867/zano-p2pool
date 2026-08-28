#include "zano_p2pool/pow_target.hpp"

#include <boost/multiprecision/cpp_int.hpp>

#include <cstddef>
#include <limits>
#include <stdexcept>

namespace zano_p2pool {
namespace {

using boost::multiprecision::uint128_t;
using boost::multiprecision::uint256_t;

uint128_t parse_difficulty(std::string_view text) {
    if (text.empty()) {
        throw std::invalid_argument("difficulty is empty");
    }

    uint256_t value = 0;
    const uint256_t max128 = std::numeric_limits<uint128_t>::max();

    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            throw std::invalid_argument("difficulty must contain decimal digits only");
        }

        value *= 10;
        value += static_cast<unsigned>(ch - '0');

        if (value > max128) {
            throw std::out_of_range("difficulty exceeds Zano uint128 range");
        }
    }

    if (value == 0) {
        throw std::invalid_argument("difficulty must be greater than zero");
    }

    return value.convert_to<uint128_t>();
}

uint256_t bytes_to_uint256(const Hash256& bytes) {
    uint256_t value = 0;

    for (const std::uint8_t byte : bytes) {
        value <<= 8;
        value |= byte;
    }

    return value;
}

Hash256 uint256_to_big_endian_bytes(uint256_t value) {
    Hash256 bytes{};

    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[bytes.size() - 1 - i] =
            static_cast<std::uint8_t>((value & 0xff).convert_to<unsigned>());
        value >>= 8;
    }

    return bytes;
}

}  // namespace

std::string DifficultyTarget::hex() const {
    static constexpr char digits[] = "0123456789abcdef";

    std::string result(big_endian.size() * 2, '0');
    for (std::size_t i = 0; i < big_endian.size(); ++i) {
        result[i * 2] = digits[big_endian[i] >> 4];
        result[i * 2 + 1] = digits[big_endian[i] & 0x0f];
    }

    return result;
}

DifficultyTarget difficulty_to_target(std::string_view difficulty_decimal) {
    const uint128_t difficulty = parse_difficulty(difficulty_decimal);
    const uint256_t max256 = std::numeric_limits<uint256_t>::max();
    const uint256_t target = max256 / difficulty;

    return DifficultyTarget{uint256_to_big_endian_bytes(target)};
}

bool hash_meets_target(const Hash256& hash, const DifficultyTarget& target) {
    return bytes_to_uint256(hash) <= bytes_to_uint256(target.big_endian);
}

bool hash_meets_difficulty(
    const Hash256& hash,
    std::string_view difficulty_decimal) {
    return hash_meets_target(hash, difficulty_to_target(difficulty_decimal));
}

}  // namespace zano_p2pool
