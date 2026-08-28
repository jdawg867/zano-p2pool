#include "zano_p2pool/progpowz.hpp"

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
    using zano_p2pool::ProgPowZContextMode;
    using zano_p2pool::progpowz_available;
    using zano_p2pool::progpowz_epoch;
    using zano_p2pool::progpowz_hash;
    using zano_p2pool::progpowz_revision;

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
    const auto result = progpowz_hash(
        0,
        header,
        UINT64_C(0x123456789abcdef0),
        ProgPowZContextMode::Light);

    assert(to_hex(result.mix_hash) ==
           "c2e883b6876ec4cc514b9cea269f343095619faf9f2edcafb3fcf6928fa58141");
    assert(to_hex(result.final_hash) ==
           "fa70fbf9979f80ec3db2c3f118a5e683fcf5f54ea7edc41b0b5d336508694cb8");
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
