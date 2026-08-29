#include "zano_p2pool/p2p_miner_tx_binding.hpp"

#include "zano_p2pool/crypto_hash.hpp"
#include "zano_p2pool/mining_header.hpp"

#include <json-c/json.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

namespace zano_p2pool {
namespace {

using JsonPtr = std::unique_ptr<json_object, decltype(&json_object_put)>;

class Cursor {
public:
    explicit Cursor(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

    [[nodiscard]] std::uint8_t read_u8() {
        require(1);
        return bytes_[offset_++];
    }

    [[nodiscard]] std::uint64_t read_varint() {
        std::uint64_t value = 0;
        unsigned shift = 0;
        const std::size_t start = offset_;

        for (unsigned i = 0; i < 10; ++i) {
            const std::uint8_t byte = read_u8();
            if (shift == 63 && (byte & 0x7eU) != 0U) {
                throw std::runtime_error("miner-tx varint overflows uint64");
            }
            value |= static_cast<std::uint64_t>(byte & 0x7fU) << shift;
            if ((byte & 0x80U) == 0U) {
                std::size_t expected = 1;
                std::uint64_t tmp = value;
                while (tmp >= 0x80U) {
                    tmp >>= 7;
                    ++expected;
                }
                if (offset_ - start != expected) {
                    throw std::runtime_error("non-canonical miner-tx varint");
                }
                return value;
            }
            shift += 7;
        }
        throw std::runtime_error("miner-tx varint is too long");
    }

    void skip(std::size_t count) {
        require(count);
        offset_ += count;
    }

    void skip_blob() {
        const std::uint64_t count = read_varint();
        if (count > std::numeric_limits<std::size_t>::max()) {
            throw std::runtime_error("miner-tx blob length does not fit size_t");
        }
        skip(static_cast<std::size_t>(count));
    }

    [[nodiscard]] ZanoCurveKey read_key() {
        require(32);
        ZanoCurveKey result{};
        std::copy_n(
            bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
            result.size(),
            result.begin());
        offset_ += result.size();
        return result;
    }

private:
    void require(std::size_t count) const {
        if (offset_ > bytes_.size() || count > bytes_.size() - offset_) {
            throw std::runtime_error("truncated miner transaction prefix");
        }
    }

    std::span<const std::uint8_t> bytes_;
    std::size_t offset_{0};
};

struct TgcKeyPair {
    ZanoCurveKey public_key{};
    ZanoCurveKey secret_key{};
};

[[nodiscard]] TgcKeyPair parse_tgc_keypair(std::string_view json_text) {
    if (json_text.empty()) {
        throw std::runtime_error("empty miner_tx_tgc JSON");
    }

    json_tokener* tokener = json_tokener_new();
    if (tokener == nullptr) {
        throw std::runtime_error("json_tokener_new failed");
    }
    json_object* parsed = json_tokener_parse_ex(
        tokener,
        json_text.data(),
        static_cast<int>(json_text.size()));
    const auto error = json_tokener_get_error(tokener);
    json_tokener_free(tokener);

    JsonPtr root(parsed, &json_object_put);
    if (error != json_tokener_success || !root ||
        json_object_get_type(root.get()) != json_type_object) {
        throw std::runtime_error("invalid miner_tx_tgc JSON");
    }

    json_object* tx_key = nullptr;
    if (!json_object_object_get_ex(root.get(), "tx_key", &tx_key) ||
        tx_key == nullptr || json_object_get_type(tx_key) != json_type_string) {
        throw std::runtime_error("miner_tx_tgc has no string tx_key");
    }

    const std::string tx_key_hex = json_object_get_string(tx_key);
    const auto bytes = hex_to_bytes(tx_key_hex);
    if (bytes.size() != 64) {
        throw std::runtime_error("miner_tx_tgc tx_key must be exactly 64 bytes");
    }

    TgcKeyPair result;
    std::copy_n(bytes.begin(), 32, result.public_key.begin());
    std::copy_n(bytes.begin() + 32, 32, result.secret_key.begin());
    return result;
}

[[nodiscard]] ZanoCurveKey parse_prefix_tx_public_key(
    std::span<const std::uint8_t> miner_tx_prefix) {
    Cursor cursor(miner_tx_prefix);

    if (cursor.read_varint() != 4) {
        throw std::runtime_error("unsupported miner transaction version");
    }

    const std::uint64_t vin_count = cursor.read_varint();
    if (vin_count != 1) {
        throw std::runtime_error("current PoW miner transaction must have one input");
    }
    if (cursor.read_u8() != 0) {
        throw std::runtime_error("current PoW miner transaction input is not txin_gen");
    }
    static_cast<void>(cursor.read_varint());  // txin_gen.height

    const std::uint64_t extra_count = cursor.read_varint();
    if (extra_count > 1024) {
        throw std::runtime_error("miner transaction has unreasonable extra count");
    }

    bool found_public_key = false;
    ZanoCurveKey tx_public_key{};

    for (std::uint64_t i = 0; i < extra_count; ++i) {
        const std::uint8_t tag = cursor.read_u8();
        switch (tag) {
        case 11:  // tx_derivation_hint: string
        case 19:  // extra_user_data: string
        case 21:  // extra_padding: vector<uint8_t>
            cursor.skip_blob();
            break;
        case 14:  // etc_tx_details_unlock_time
        case 15:  // etc_tx_details_expiration_time
        case 16:  // etc_tx_details_flags
        case 78:  // etc_coinbase_block_cumulative_size
            static_cast<void>(cursor.read_varint());
            break;
        case 18:  // extra_attachment_info
            static_cast<void>(cursor.read_varint());
            cursor.skip(32);
            static_cast<void>(cursor.read_varint());
            break;
        case 22:  // crypto::public_key
            if (found_public_key) {
                throw std::runtime_error("miner transaction has multiple tx public keys");
            }
            tx_public_key = cursor.read_key();
            found_public_key = true;
            break;
        default:
            throw std::runtime_error(
                "unsupported miner transaction extra tag: " +
                std::to_string(tag));
        }
    }

    if (!found_public_key) {
        throw std::runtime_error("miner transaction has no tx public key");
    }
    return tx_public_key;
}

}  // namespace

P2pMinerTxBindingResult verify_miner_tx_tgc_key_binding(
    std::span<const std::uint8_t> miner_tx_prefix,
    std::string_view miner_tx_tgc_json) noexcept {
    P2pMinerTxBindingResult result;

    TgcKeyPair tgc_keypair{};
    try {
        tgc_keypair = parse_tgc_keypair(miner_tx_tgc_json);
    } catch (...) {
        result.status = P2pMinerTxBindingStatus::MalformedTgc;
        return result;
    }

    try {
        result.tx_public_key = parse_prefix_tx_public_key(miner_tx_prefix);
    } catch (...) {
        result.status = P2pMinerTxBindingStatus::MalformedMinerTxPrefix;
        return result;
    }

    if (!zano_curve_backend_available()) {
        result.status = P2pMinerTxBindingStatus::BackendUnavailable;
        return result;
    }

    if (!zano_secret_key_matches_public(
            tgc_keypair.secret_key,
            tgc_keypair.public_key)) {
        result.status = P2pMinerTxBindingStatus::TgcKeyPairMismatch;
        return result;
    }

    if (tgc_keypair.public_key != result.tx_public_key) {
        result.status = P2pMinerTxBindingStatus::PrefixPublicKeyMismatch;
        return result;
    }

    result.status = P2pMinerTxBindingStatus::Verified;
    return result;
}

P2pMinerTxBindingResult verify_p2p_mining_context_key_binding(
    const P2pMiningContextProposal& proposal,
    const P2pMiningContextCheckResult& anchored_check) noexcept {
    P2pMinerTxBindingResult result;
    if (anchored_check.status !=
            P2pMiningContextCheckStatus::AnchoredUnverifiedMinerTx ||
        anchored_check.proposal_id != p2p_mining_context_id(proposal)) {
        result.status = P2pMinerTxBindingStatus::NotAnchored;
        return result;
    }

    try {
        const auto work = derive_mining_header_work(proposal.block_template_blob);
        if (work.header_hash != anchored_check.mining_header_hash) {
            result.status = P2pMinerTxBindingStatus::NotAnchored;
            return result;
        }
        return verify_miner_tx_tgc_key_binding(
            work.miner_tx_prefix.serialized,
            proposal.miner_tx_tgc_json);
    } catch (...) {
        result.status = P2pMinerTxBindingStatus::MalformedMinerTxPrefix;
        return result;
    }
}

const char* p2p_miner_tx_binding_status_name(
    P2pMinerTxBindingStatus status) noexcept {
    switch (status) {
    case P2pMinerTxBindingStatus::Verified:
        return "verified";
    case P2pMinerTxBindingStatus::NotAnchored:
        return "not-anchored";
    case P2pMinerTxBindingStatus::BackendUnavailable:
        return "backend-unavailable";
    case P2pMinerTxBindingStatus::MalformedTgc:
        return "malformed-tgc";
    case P2pMinerTxBindingStatus::MalformedMinerTxPrefix:
        return "malformed-miner-tx-prefix";
    case P2pMinerTxBindingStatus::TgcKeyPairMismatch:
        return "tgc-keypair-mismatch";
    case P2pMinerTxBindingStatus::PrefixPublicKeyMismatch:
        return "prefix-public-key-mismatch";
    }
    return "unknown";
}

}  // namespace zano_p2pool
