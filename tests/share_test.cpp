#include "zano_p2pool/crypto_hash.hpp"
#include "zano_p2pool/share.hpp"
#include "test_check.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

template <typename Exception, typename Fn>
void expect_throw(Fn&& fn) {
    bool threw = false;
    try {
        fn();
    } catch (const Exception&) {
        threw = true;
    }
    CHECK(threw);
}

zano_p2pool::Share make_vector_share() {
    using namespace zano_p2pool;

    Share share;
    for (std::size_t i = 0; i < share.parent_id.size(); ++i) {
        share.parent_id[i] = static_cast<std::uint8_t>(i);
        share.miner_id[i] = static_cast<std::uint8_t>(0xa0U + i);
    }
    share.share_height = 1;
    share.timestamp = UINT64_C(0x0102030405060708);
    share.zano_height = 165014;
    const auto mining_header = hex_to_bytes(
        "43147bd3560a1385c7359475e8974bbfc7aeac85c328e779e037b2d8eeec604e");
    CHECK(mining_header.size() == share.mining_header_hash.size());
    std::copy(
        mining_header.begin(), mining_header.end(), share.mining_header_hash.begin());
    share.nonce = UINT64_C(0x1122334455667788);
    share.share_difficulty = difficulty128_from_decimal("100000000");
    share.network_difficulty = difficulty128_from_decimal("1229990");
    return share;
}

}  // namespace

int main() {
    using namespace zano_p2pool;

    constexpr std::string_view kExpectedHex =
        "5a50325301"
        "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
        "0000000000000001"
        "0102030405060708"
        "0000000000028496"
        "43147bd3560a1385c7359475e8974bbfc7aeac85c328e779e037b2d8eeec604e"
        "1122334455667788"
        "00000000000000000000000005f5e100"
        "0000000000000000000000000012c4a6"
        "a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebf";

    const Share share = make_vector_share();
    const auto encoded = serialize_share(share);
    CHECK(encoded.size() == kShareV1SerializedSize);
    CHECK(bytes_to_hex(encoded) == kExpectedHex);

    const Share decoded = deserialize_share(encoded);
    CHECK(decoded == share);
    CHECK(serialize_share(decoded) == encoded);
    CHECK(share_id(decoded) == share_id(share));

    Share mutated = share;
    ++mutated.nonce;
    CHECK(share_id(mutated) != share_id(share));

    Share root = share;
    root.parent_id = {};
    CHECK(is_zero_share_id(root.parent_id));
    CHECK(!is_zero_share_id(share.parent_id));

    CHECK(difficulty128_to_decimal(share.share_difficulty) == "100000000");
    CHECK(difficulty128_to_decimal(share.network_difficulty) == "1229990");
    CHECK(!difficulty128_is_zero(share.share_difficulty));
    CHECK(difficulty128_is_zero(Difficulty128{}));

    const auto max128 = difficulty128_from_decimal(
        "340282366920938463463374607431768211455");
    CHECK(std::all_of(max128.begin(), max128.end(), [](std::uint8_t byte) {
        return byte == 0xff;
    }));
    CHECK(difficulty128_to_decimal(max128) ==
          "340282366920938463463374607431768211455");

    expect_throw<std::invalid_argument>([] {
        (void)difficulty128_from_decimal("0");
    });
    expect_throw<std::invalid_argument>([] {
        (void)difficulty128_from_decimal("12x34");
    });
    expect_throw<std::out_of_range>([] {
        (void)difficulty128_from_decimal(
            "340282366920938463463374607431768211456");
    });

    auto bad_magic = encoded;
    bad_magic[0] ^= 0xff;
    expect_throw<std::runtime_error>([&] {
        (void)deserialize_share(bad_magic);
    });

    auto bad_version = encoded;
    bad_version[4] = 2;
    expect_throw<std::runtime_error>([&] {
        (void)deserialize_share(bad_version);
    });

    auto truncated = encoded;
    truncated.pop_back();
    expect_throw<std::runtime_error>([&] {
        (void)deserialize_share(truncated);
    });

    auto trailing = encoded;
    trailing.push_back(0);
    expect_throw<std::runtime_error>([&] {
        (void)deserialize_share(trailing);
    });

    Share unsupported = share;
    unsupported.version = 2;
    expect_throw<std::invalid_argument>([&] {
        (void)serialize_share(unsupported);
    });

    return 0;
}
