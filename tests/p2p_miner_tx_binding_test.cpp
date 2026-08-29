#include "test_check.hpp"

#include "zano_p2pool/crypto_hash.hpp"
#include "zano_p2pool/p2p_miner_tx_binding.hpp"
#include "zano_p2pool/zano_curve.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using namespace zano_p2pool;

constexpr const char* kScalarOneHex =
    "0100000000000000000000000000000000000000000000000000000000000000";
constexpr const char* kScalarTwoHex =
    "0200000000000000000000000000000000000000000000000000000000000000";
constexpr const char* kEd25519BasepointHex =
    "5866666666666666666666666666666666666666666666666666666666666666";

[[nodiscard]] std::vector<std::uint8_t> make_prefix(
    const std::string& tx_public_key_hex) {
    const auto public_key = hex_to_bytes(tx_public_key_hex);
    CHECK(public_key.size() == 32);

    // Current HF6 PoW transaction prefix, truncated immediately after extra.
    // The binding parser intentionally needs only version/input/extra fields.
    std::vector<std::uint8_t> prefix;
    prefix.reserve(6 + public_key.size());
    prefix.push_back(0x04);  // transaction version 4
    prefix.push_back(0x01);  // one input
    prefix.push_back(0x00);  // txin_gen variant tag
    prefix.push_back(0x01);  // txin_gen.height = 1
    prefix.push_back(0x01);  // one extra field
    prefix.push_back(0x16);  // crypto::public_key variant tag 22
    prefix.insert(prefix.end(), public_key.begin(), public_key.end());
    return prefix;
}

[[nodiscard]] std::string make_tgc(
    const std::string& public_key_hex,
    const std::string& secret_key_hex) {
    return std::string("{\"tx_key\":\"") +
           public_key_hex + secret_key_hex +
           "\",\"tx_pub_key_p\":\"00\",\"tx_outs_attr\":0}";
}

}  // namespace

int main() {
    const auto prefix = make_prefix(kEd25519BasepointHex);
    const auto good_tgc = make_tgc(kEd25519BasepointHex, kScalarOneHex);

    const auto malformed_tgc = verify_miner_tx_tgc_key_binding(prefix, "{}");
    CHECK(malformed_tgc.status == P2pMinerTxBindingStatus::MalformedTgc);

    auto malformed_prefix = prefix;
    malformed_prefix[0] = 0x03;
    CHECK(verify_miner_tx_tgc_key_binding(malformed_prefix, good_tgc).status ==
          P2pMinerTxBindingStatus::MalformedMinerTxPrefix);

    malformed_prefix = prefix;
    malformed_prefix.resize(10);
    CHECK(verify_miner_tx_tgc_key_binding(malformed_prefix, good_tgc).status ==
          P2pMinerTxBindingStatus::MalformedMinerTxPrefix);

    const auto good = verify_miner_tx_tgc_key_binding(prefix, good_tgc);
    if (!zano_curve_backend_available()) {
        CHECK(good.status == P2pMinerTxBindingStatus::BackendUnavailable);
        CHECK(std::string(p2p_miner_tx_binding_status_name(good.status)) ==
              "backend-unavailable");
        return 0;
    }

    CHECK(good.status == P2pMinerTxBindingStatus::Verified);
    CHECK(bytes_to_hex(good.tx_public_key) == kEd25519BasepointHex);
    CHECK(std::string(p2p_miner_tx_binding_status_name(good.status)) == "verified");

    const auto bad_keypair_tgc = make_tgc(kEd25519BasepointHex, kScalarTwoHex);
    CHECK(verify_miner_tx_tgc_key_binding(prefix, bad_keypair_tgc).status ==
          P2pMinerTxBindingStatus::TgcKeyPairMismatch);

    std::string other_prefix_key = kEd25519BasepointHex;
    other_prefix_key[0] = other_prefix_key[0] == '5' ? '6' : '5';
    const auto mismatched_prefix = make_prefix(other_prefix_key);
    CHECK(verify_miner_tx_tgc_key_binding(mismatched_prefix, good_tgc).status ==
          P2pMinerTxBindingStatus::PrefixPublicKeyMismatch);

    ZanoCurveKey scalar_one{};
    ZanoCurveKey basepoint{};
    const auto scalar_one_bytes = hex_to_bytes(kScalarOneHex);
    const auto basepoint_bytes = hex_to_bytes(kEd25519BasepointHex);
    std::copy(scalar_one_bytes.begin(), scalar_one_bytes.end(), scalar_one.begin());
    std::copy(basepoint_bytes.begin(), basepoint_bytes.end(), basepoint.begin());
    CHECK(zano_secret_key_matches_public(scalar_one, basepoint));

    return 0;
}
