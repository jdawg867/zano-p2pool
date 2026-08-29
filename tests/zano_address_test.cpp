#include "zano_p2pool/crypto_hash.hpp"
#include "zano_p2pool/zano_address.hpp"
#include "test_check.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using namespace zano_p2pool;

constexpr std::string_view kAlphabet =
    "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
constexpr std::array<std::size_t, 9> kEncodedSizes{
    0, 2, 3, 5, 6, 7, 9, 10, 11};

std::string encode_block(const std::uint8_t* data, std::size_t size) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < size; ++i) {
        value = (value << 8) | data[i];
    }

    std::string out(kEncodedSizes[size], '1');
    for (std::size_t i = out.size(); i-- > 0 && value != 0;) {
        const std::uint64_t digit = value % 58;
        value /= 58;
        out[i] = kAlphabet[static_cast<std::size_t>(digit)];
    }
    return out;
}

std::string encode_base58(const std::vector<std::uint8_t>& bytes) {
    std::string out;
    for (std::size_t offset = 0; offset < bytes.size(); offset += 8) {
        const std::size_t size = std::min<std::size_t>(8, bytes.size() - offset);
        out += encode_block(bytes.data() + offset, size);
    }
    return out;
}

std::string make_address(
    std::uint64_t prefix,
    const P2pPayoutAddress& payout,
    bool extra_payload_byte = false) {
    std::vector<std::uint8_t> raw;
    do {
        std::uint8_t byte = static_cast<std::uint8_t>(prefix & 0x7fU);
        prefix >>= 7;
        if (prefix != 0) {
            byte |= 0x80U;
        }
        raw.push_back(byte);
    } while (prefix != 0);

    raw.insert(
        raw.end(),
        payout.spend_public_key.begin(),
        payout.spend_public_key.end());
    raw.insert(
        raw.end(),
        payout.view_public_key.begin(),
        payout.view_public_key.end());
    if (extra_payload_byte) {
        raw.push_back(0x42);
    }

    const Hash256 checksum = cn_fast_hash(raw);
    raw.insert(raw.end(), checksum.begin(), checksum.begin() + 4);
    return encode_base58(raw);
}

}  // namespace

int main() {
    P2pPayoutAddress expected;
    for (std::size_t i = 0; i < 32; ++i) {
        expected.spend_public_key[i] = static_cast<std::uint8_t>(0x10U + i);
        expected.view_public_key[i] = static_cast<std::uint8_t>(0x80U + i);
    }

    const std::string address = make_address(
        kZanoStandardAddressBase58Prefix,
        expected);
    CHECK(address.rfind("Zx", 0) == 0);

    const ZanoAddressDecodeResult decoded =
        decode_zano_standard_address(address);
    CHECK(decoded.status == ZanoAddressDecodeStatus::Valid);
    CHECK(decoded.payout == expected);
    CHECK(std::string(zano_address_decode_status_name(decoded.status)) == "valid");

    std::string bad_checksum = address;
    bad_checksum.back() = bad_checksum.back() == '1' ? '2' : '1';
    CHECK(decode_zano_standard_address(bad_checksum).status ==
          ZanoAddressDecodeStatus::InvalidChecksum);

    std::string bad_base58 = address;
    bad_base58[3] = '0';  // not in the CryptoNote Base58 alphabet
    CHECK(decode_zano_standard_address(bad_base58).status ==
          ZanoAddressDecodeStatus::InvalidBase58);

    const std::string wrong_prefix = make_address(0xc6, expected);
    CHECK(decode_zano_standard_address(wrong_prefix).status ==
          ZanoAddressDecodeStatus::InvalidPrefix);

    const std::string unsupported = make_address(
        kZanoStandardAddressBase58Prefix,
        expected,
        true);
    CHECK(decode_zano_standard_address(unsupported).status ==
          ZanoAddressDecodeStatus::UnsupportedPayload);

    return 0;
}
