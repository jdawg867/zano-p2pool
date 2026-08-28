#include "zano_p2pool/pow_target.hpp"
#include "test_check.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

zano_p2pool::Hash256 from_hex(std::string_view hex) {
    CHECK(hex.size() == 64);

    auto nibble = [](char ch) -> std::uint8_t {
        if (ch >= '0' && ch <= '9') {
            return static_cast<std::uint8_t>(ch - '0');
        }
        if (ch >= 'a' && ch <= 'f') {
            return static_cast<std::uint8_t>(10 + ch - 'a');
        }
        if (ch >= 'A' && ch <= 'F') {
            return static_cast<std::uint8_t>(10 + ch - 'A');
        }
        CHECK(false && "invalid hex digit");
        return 0;
    };

    zano_p2pool::Hash256 bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::uint8_t>(
            (nibble(hex[i * 2]) << 4) | nibble(hex[i * 2 + 1]));
    }
    return bytes;
}

zano_p2pool::Hash256 increment(zano_p2pool::Hash256 bytes) {
    for (std::size_t i = bytes.size(); i-- > 0;) {
        ++bytes[i];
        if (bytes[i] != 0) {
            break;
        }
    }
    return bytes;
}

void expect_invalid_difficulty(std::string_view difficulty) {
    bool threw = false;
    try {
        (void)zano_p2pool::difficulty_to_target(difficulty);
    } catch (const std::invalid_argument&) {
        threw = true;
    } catch (const std::out_of_range&) {
        threw = true;
    }
    CHECK(threw);
}

}  // namespace

int main() {
    using zano_p2pool::DifficultyTarget;
    using zano_p2pool::Hash256;
    using zano_p2pool::difficulty_to_target;
    using zano_p2pool::hash_meets_difficulty;
    using zano_p2pool::hash_meets_target;

    CHECK(difficulty_to_target("1").hex() == std::string(64, 'f'));
    CHECK(difficulty_to_target("2").hex() ==
          "7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");

    const DifficultyTarget live_target = difficulty_to_target("1179735");
    CHECK(live_target.hex() ==
          "00000e389ed1e3ed15e9a44f9eb80f149aaaa2aad1549a754e573e3de163222f");

    CHECK(hash_meets_target(live_target.big_endian, live_target));
    CHECK(!hash_meets_target(increment(live_target.big_endian), live_target));

    Hash256 zero_hash{};
    Hash256 max_hash{};
    max_hash.fill(0xff);

    CHECK(hash_meets_difficulty(zero_hash, "1179735"));
    CHECK(hash_meets_difficulty(max_hash, "1"));
    CHECK(!hash_meets_difficulty(max_hash, "2"));

    const auto diff_255_target = difficulty_to_target("255");
    CHECK(hash_meets_target(diff_255_target.big_endian, diff_255_target));
    CHECK(!hash_meets_target(increment(diff_255_target.big_endian), diff_255_target));

    const auto half = from_hex(
        "7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
    const auto half_plus_one = from_hex(
        "8000000000000000000000000000000000000000000000000000000000000000");
    CHECK(hash_meets_difficulty(half, "2"));
    CHECK(!hash_meets_difficulty(half_plus_one, "2"));

    expect_invalid_difficulty("");
    expect_invalid_difficulty("0");
    expect_invalid_difficulty("12x34");
    expect_invalid_difficulty("-1");
    expect_invalid_difficulty("340282366920938463463374607431768211456");

    return 0;
}
