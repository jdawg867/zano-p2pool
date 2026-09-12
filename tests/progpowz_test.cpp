#include "zano_p2pool/progpowz.hpp"
#include "zano_p2pool/share_validation.hpp"
#include "test_check.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <cstring>
#ifdef ZANO_P2POOL_HAVE_PROGPOWZ
#include <ethash/ethash.hpp>
#endif
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
    using zano_p2pool::progpowz_seed;
    CHECK(progpowz_seed(0) == zano_p2pool::Hash256{});
    CHECK(progpowz_seed(29999) == zano_p2pool::Hash256{});
    CHECK(progpowz_seed(30000) == from_hex(
        "290decd9548b62a8d60345a988386fc84ba6bc95484008f6362f93160ef3e563"));
    CHECK(progpowz_seed(180000) == from_hex(
        "582b06447f087674bcc0a32a19961e77dafb9e17955792f79ec8936e3d9742fc"));
    CHECK(progpowz_seed(209999) == progpowz_seed(180000));
    CHECK(progpowz_seed(210000) != progpowz_seed(209999));
    bool seed_overflow = false;
    try { static_cast<void>(progpowz_seed(std::numeric_limits<std::uint64_t>::max())); }
    catch (const std::out_of_range&) { seed_overflow = true; }
    CHECK(seed_overflow);
#ifdef ZANO_P2POOL_HAVE_PROGPOWZ
    for (const int epoch : {0, 1, 5, 6, 7}) {
        const auto upstream_seed = ethash::calculate_epoch_seed(epoch);
        const auto derived_seed = progpowz_seed(static_cast<std::uint64_t>(epoch) * 30000);
        CHECK(std::memcmp(upstream_seed.bytes, derived_seed.data(), derived_seed.size()) == 0);
    }
#endif

    using zano_p2pool::CandidateClassification;
    using zano_p2pool::ProgPowZContextMode;
    using zano_p2pool::progpowz_available;
    using zano_p2pool::progpowz_epoch;
    using zano_p2pool::progpowz_hash;
    using zano_p2pool::progpowz_revision;
    using zano_p2pool::validate_candidate;

    CHECK(progpowz_epoch(0) == 0);
    CHECK(progpowz_epoch(29999) == 0);
    CHECK(progpowz_epoch(30000) == 1);
    CHECK(progpowz_epoch(164895) == 5);

#ifdef ZANO_P2POOL_HAVE_PROGPOWZ
    CHECK(progpowz_available());
    CHECK(std::string(progpowz_revision()) == "0.9.2");

    // Deterministic compatibility vector produced by the audited Zano
    // libethash source pinned in CI at commit 1508cf6a. This intentionally
    // tests Zano's exact ProgPoWZ implementation rather than assuming an
    // upstream/generic ProgPoW vector is interchangeable.
    const auto header = from_hex(
        "ffeeddccbbaa9988776655443322110000112233445566778899aabbccddeeff");
    constexpr auto nonce = UINT64_C(0x123456789abcdef0);
    const auto result = progpowz_hash(
        0,
        header,
        nonce,
        ProgPowZContextMode::Light);

    CHECK(to_hex(result.mix_hash) ==
          "1476a46ba81f00a5acd854e603c79a219fcb128db00b1809718855128471eb71");
    CHECK(to_hex(result.final_hash) ==
          "4feba8deef1ac892ee334cf258d029cc8651f037215f1767b8ce5c704a4fd68b");

    // This exact final hash meets difficulty 3 but not difficulty 4.
    const auto share = validate_candidate(
        0,
        header,
        nonce,
        "3",
        "4",
        ProgPowZContextMode::Light);
    CHECK(share.pow.final_hash == result.final_hash);
    CHECK(share.meets_share_difficulty);
    CHECK(!share.meets_network_difficulty);
    CHECK(share.classification == CandidateClassification::Share);

    const auto invalid = validate_candidate(
        0,
        header,
        nonce,
        "4",
        "5",
        ProgPowZContextMode::Light);
    CHECK(!invalid.meets_share_difficulty);
    CHECK(!invalid.meets_network_difficulty);
    CHECK(invalid.classification == CandidateClassification::Invalid);

    const auto block = validate_candidate(
        0,
        header,
        nonce,
        "3",
        "3",
        ProgPowZContextMode::Light);
    CHECK(block.meets_share_difficulty);
    CHECK(block.meets_network_difficulty);
    CHECK(block.classification == CandidateClassification::Block);
#else
    CHECK(!progpowz_available());
    CHECK(std::string(progpowz_revision()) == "unavailable");

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
    CHECK(threw);
#endif

    return 0;
}
