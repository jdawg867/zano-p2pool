#include "zano_p2pool/mining_header.hpp"

#include "zano_p2pool/crypto_hash.hpp"

#include <limits>
#include <stdexcept>
#include <string>

namespace zano_p2pool {
namespace {

class Cursor {
public:
    explicit Cursor(std::span<const std::uint8_t> data) : data_(data) {}

    [[nodiscard]] std::size_t position() const noexcept { return pos_; }

    [[nodiscard]] std::uint8_t read_u8() {
        require(1);
        return data_[pos_++];
    }

    [[nodiscard]] std::uint64_t read_varint() {
        std::uint64_t value = 0;
        unsigned shift = 0;
        for (unsigned i = 0; i < 10; ++i) {
            const std::uint8_t byte = read_u8();
            if (shift == 63 && (byte & 0x7eU) != 0U) {
                throw std::runtime_error("varint overflows uint64");
            }
            value |= static_cast<std::uint64_t>(byte & 0x7fU) << shift;
            if ((byte & 0x80U) == 0U) {
                return value;
            }
            shift += 7;
        }
        throw std::runtime_error("varint is too long");
    }

    void skip(std::size_t count) {
        require(count);
        pos_ += count;
    }

    void skip_blob() {
        const std::uint64_t size = read_varint();
        if (size > std::numeric_limits<std::size_t>::max()) {
            throw std::runtime_error("serialized blob length does not fit size_t");
        }
        skip(static_cast<std::size_t>(size));
    }

private:
    void require(std::size_t count) const {
        if (count > data_.size() - pos_) {
            throw std::runtime_error("truncated Zano serialization");
        }
    }

    std::span<const std::uint8_t> data_;
    std::size_t pos_{0};
};

void skip_current_coinbase_extra(Cursor& cursor, std::uint8_t tag) {
    switch (tag) {
    case 11:  // tx_derivation_hint: string
    case 19:  // extra_user_data: string
    case 21:  // extra_padding: vector<uint8_t>
        cursor.skip_blob();
        return;
    case 14:  // etc_tx_details_unlock_time
    case 15:  // etc_tx_details_expiration_time
    case 16:  // etc_tx_details_flags
    case 78:  // etc_coinbase_block_cumulative_size
        static_cast<void>(cursor.read_varint());
        return;
    case 18:  // extra_attachment_info: varint size + hash + varint count
        static_cast<void>(cursor.read_varint());
        cursor.skip(32);
        static_cast<void>(cursor.read_varint());
        return;
    case 22:  // crypto::public_key
        cursor.skip(32);
        return;
    default:
        throw std::runtime_error(
            "unsupported current coinbase extra tag: " + std::to_string(tag));
    }
}

void skip_current_coinbase_output(Cursor& cursor, std::uint8_t tag) {
    // Current HF6 PoW miner transactions use tx_out_zarcanum (tag 63).
    if (tag != 63) {
        throw std::runtime_error(
            "unsupported current coinbase output tag: " + std::to_string(tag));
    }

    // BEGIN_VERSIONED_SERIALIZE writes the structure version first. Zano's
    // current TX_OUT_ZARCANUM_CURRENT_VERSION is 0.
    const std::uint8_t version = cursor.read_u8();
    if (version != 0) {
        throw std::runtime_error("unsupported tx_out_zarcanum version");
    }

    // stealth_address, concealing_point, amount_commitment, blinded_asset_id
    cursor.skip(32 * 4);
    // encrypted_amount, encrypted_payment_id
    cursor.skip(8 + 8);
    // mix_attr
    cursor.skip(1);
}

}  // namespace

ParsedBlockHeader parse_pow_block_header(
    std::span<const std::uint8_t> block_blob) {
    Cursor cursor(block_blob);

    // Zano block_header serialization order:
    // major_version, nonce, prev_id, minor_version(varint), timestamp(varint), flags.
    static_cast<void>(cursor.read_u8());  // major_version
    cursor.skip(8);                       // nonce
    cursor.skip(32);                      // prev_id
    static_cast<void>(cursor.read_varint());  // minor_version
    static_cast<void>(cursor.read_varint());  // timestamp
    static_cast<void>(cursor.read_u8());      // flags

    ParsedBlockHeader result;
    result.serialized_size = cursor.position();
    result.serialized.assign(
        block_blob.begin(),
        block_blob.begin() + static_cast<std::ptrdiff_t>(result.serialized_size));

    if (result.serialized.size() < 9) {
        throw std::runtime_error("serialized block header is unexpectedly short");
    }
    for (std::size_t i = 1; i < 9; ++i) {
        result.serialized[i] = 0;
    }
    return result;
}

ParsedMinerTxPrefix parse_hf6_miner_tx_prefix(
    std::span<const std::uint8_t> miner_tx_blob) {
    Cursor cursor(miner_tx_blob);
    ParsedMinerTxPrefix result;

    result.version = cursor.read_varint();
    if (result.version != 4) {
        throw std::runtime_error(
            "unsupported current miner transaction version: " +
            std::to_string(result.version));
    }

    const std::uint64_t vin_count = cursor.read_varint();
    result.vin_count = static_cast<std::size_t>(vin_count);
    for (std::uint64_t i = 0; i < vin_count; ++i) {
        const std::uint8_t tag = cursor.read_u8();
        if (tag != 0) {
            throw std::runtime_error(
                "unsupported current miner input tag: " + std::to_string(tag));
        }
        static_cast<void>(cursor.read_varint());  // txin_gen.height
    }

    const std::uint64_t extra_count = cursor.read_varint();
    result.extra_count = static_cast<std::size_t>(extra_count);
    for (std::uint64_t i = 0; i < extra_count; ++i) {
        skip_current_coinbase_extra(cursor, cursor.read_u8());
    }

    const std::uint64_t vout_count = cursor.read_varint();
    result.vout_count = static_cast<std::size_t>(vout_count);
    for (std::uint64_t i = 0; i < vout_count; ++i) {
        skip_current_coinbase_output(cursor, cursor.read_u8());
    }

    // Current transaction version 4 is POST_HF5 and serializes hardfork_id.
    result.hardfork_id = cursor.read_u8();

    result.serialized_size = cursor.position();
    result.serialized.assign(
        miner_tx_blob.begin(),
        miner_tx_blob.begin() + static_cast<std::ptrdiff_t>(result.serialized_size));
    result.hash = cn_fast_hash(result.serialized);
    return result;
}

std::vector<std::uint8_t> make_single_tx_hashing_blob(
    std::span<const std::uint8_t> zero_nonce_block_header,
    const Hash256& miner_tx_hash) {
    std::vector<std::uint8_t> blob;
    blob.reserve(zero_nonce_block_header.size() + miner_tx_hash.size() + 1);
    blob.insert(
        blob.end(), zero_nonce_block_header.begin(), zero_nonce_block_header.end());
    blob.insert(blob.end(), miner_tx_hash.begin(), miner_tx_hash.end());
    blob.push_back(0x01);  // varint(tx_hashes.size() + 1), with zero regular txs
    return blob;
}

}  // namespace zano_p2pool
