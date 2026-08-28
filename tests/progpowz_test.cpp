#include "zano_p2pool/progpowz.hpp"
#include "zano_p2pool/share_validation.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

zano_p2pool::Hash256 from_hex(std::string_view hex) {
    assert(hex.size() == 64);

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
        assert(false && "invalid hex digit");
        return 0;
    };

    zano_p2pool::Hash256 bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::uint8_t>(
            (nibble(hex[i * 2]) << 4) | nibble(hex[i * 2 + 1]));
    }
    return bytes;
}

std::string to_hex(const zano_p2pool::Hash256& bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result(bytes.size() * 2, '0');
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        result[i * 2] = digits[bytes[i] >> 4];
        result[i * 2 + 1] = digits[bytes[i] & 0x0f];
    }
    return result;
}

}  // namespace

int main() {
    using zano_p2pool::CandidateClassification;
    using zano_p2pool::ProgPowZContextMode;
    using zano_p2pool::progpowz_available;
    using zano_p2pool::progpowz_epoch;
    using zano_p2pool::progpowz_hash;
    using zano_p2pool::progpowz_revision;
    using zano_p2pool::validate_candidate;

    assert(progpowz_epoch(0) == 0);
    assert(progpowz_epoch(29999) == 0);
    assert(progpowz_epoch(30000) == 1);
    assert(progpowz_epoch(164895) == 5);

#ifdef ZANO_P2POOL_HAVE_PROGPOWZ
    assert(progpowz_available());
    assert(std::string(progpowz_revision()) == "0.9.2");

    // Official ProgPoW 0.9.2 vector, block 0. Light and full contexts are
    // algorithmically equivalent; light mode keeps this compatibility test
    // practical on CI and developer machines.
    const auto header = from_hex(
        "ffeeddccbbaa9988776655443322110000112233445566778899aabbccddeeff");
    constexpr auto nonce = UINT64_C(0x123456789abcdef0);
    const auto result = progpowz_hash(
        0,
        header,
        nonce,
        ProgPowZContextMode::Light);

    assert(to_hex(result.mix_hash) ==
           "c2e883b6876ec4cc514b9cea269f343095619faf9f2edcafb3fcf6928fa58141");
    assert(to_hex(result.final_hash) ==
           "fa70fbf9979f80ec3db2c3f118a5e683fcf5f54ea7edc41b0b5d336508694cb8");

    // Difficulty 1 accepts every 256-bit hash. This vector is above the
    // difficulty-2 target, which makes it a convenient deterministic example
    // of a P2Pool share that is not a full network solution.
    const auto share = validate_candidate(
        0,
        header,
        nonce,
        "1",
        "2",
        ProgPowZContextMode::Light);
    assert(share.pow.final_hash == result.final_hash);
    assert(share.meets_share_difficulty);
    assert(!share.meets_network_difficulty);
    assert(share.classification == CandidateClassification::Share);

    const auto invalid = validate_candidate(
        0,
        header,
        nonce,
        "2",
        "3",
        ProgPowZContextMode::Light);
    assert(!invalid.meets_share_difficulty);
    assert(!invalid.meets_network_difficulty);
    assert(invalid.classification == CandidateClassification::Invalid);

    const auto block = validate_candidate(
        0,
        header,
        nonce,
        "1",
        "1",
        ProgPowZContextMode::Light);
    assert(block.meets_share_difficulty);
    assert(block.meets_network_difficulty);
    assert(block.classification == CandidateClassification::Block);
#else
    assert(!progpowz_available());
    assert(std::string(progpowz_revision()) == "unavailable");

    bool threw = false;
    try {
        (void)progpowz_hash(
            0,
            {},
            0,
            ProgPowZContextMode::Light);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);
#endif

    return 0;
}
