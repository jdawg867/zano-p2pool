#include "zano_p2pool/mining_header.hpp"

#include "zano_p2pool/crypto_hash.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

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
        if (pos_ > data_.size() || count > data_.size() - pos_) {
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

[[nodiscard]] std::size_t encoded_varint_size(std::uint64_t value) noexcept {
    std::size_t size = 1;
    while (value >= 0x80U) {
        value >>= 7;
        ++size;
    }
    return size;
}

void append_varint(std::vector<std::uint8_t>& out, std::uint64_t value) {
    do {
        std::uint8_t byte = static_cast<std::uint8_t>(value & 0x7fU);
        value >>= 7;
        if (value != 0) {
            byte |= 0x80U;
        }
        out.push_back(byte);
    } while (value != 0);
}

[[nodiscard]] bool decode_varint_at(
    std::span<const std::uint8_t> data,
    std::size_t offset,
    std::uint64_t& value,
    std::size_t& consumed) {
    if (offset >= data.size()) {
        return false;
    }

    value = 0;
    consumed = 0;
    unsigned shift = 0;
    for (unsigned i = 0; i < 10 && offset + i < data.size(); ++i) {
        const std::uint8_t byte = data[offset + i];
        if (shift == 63 && (byte & 0x7eU) != 0U) {
            return false;
        }
        value |= static_cast<std::uint64_t>(byte & 0x7fU) << shift;
        ++consumed;
        if ((byte & 0x80U) == 0U) {
            return consumed == encoded_varint_size(value);
        }
        shift += 7;
    }
    return false;
}

[[nodiscard]] bool has_current_hf6_coinbase_suffix_shape(
    std::span<const std::uint8_t> block_blob,
    std::size_t miner_suffix_offset,
    std::size_t tx_hashes_offset) {
    // Current HF6 PoW coinbase transaction suffix:
    //   attachment vector count = 0
    //   signature vector count  = 0
    //   proof vector count      = 2
    //   proof[0] tag            = 47 (zc_outs_range_proof)
    //   proof[1] tag            = 48 (zc_balance_proof)
    // The balance proof is a fixed 96-byte generic double-Schnorr payload,
    // therefore its serialized variant occupies 97 bytes including tag 48.
    constexpr std::size_t kBalanceProofSerializedSize = 97;

    if (miner_suffix_offset > block_blob.size() ||
        tx_hashes_offset > block_blob.size() ||
        tx_hashes_offset < miner_suffix_offset + 4 + kBalanceProofSerializedSize) {
        return false;
    }

    if (block_blob[miner_suffix_offset] != 0 ||
        block_blob[miner_suffix_offset + 1] != 0 ||
        block_blob[miner_suffix_offset + 2] != 2 ||
        block_blob[miner_suffix_offset + 3] != 47) {
        return false;
    }

    const std::size_t balance_proof_offset =
        tx_hashes_offset - kBalanceProofSerializedSize;
    return balance_proof_offset > miner_suffix_offset + 3 &&
           block_blob[balance_proof_offset] == 48;
}

[[nodiscard]] Hash256 hash_pair(const Hash256& left, const Hash256& right) {
    std::array<std::uint8_t, 64> pair{};
    std::copy(left.begin(), left.end(), pair.begin());
    std::copy(right.begin(), right.end(), pair.begin() + 32);
    return cn_fast_hash(pair);
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

ParsedTxHashTrailer parse_hf6_tx_hash_trailer(
    std::span<const std::uint8_t> block_blob,
    std::size_t miner_suffix_offset) {
    if (miner_suffix_offset >= block_blob.size()) {
        throw std::runtime_error("miner suffix offset is outside block blob");
    }

    std::vector<ParsedTxHashTrailer> candidates;
    const std::size_t max_count =
        (block_blob.size() - miner_suffix_offset) / 32;

    for (std::size_t count = 0; count <= max_count; ++count) {
        if (count > std::numeric_limits<std::uint64_t>::max()) {
            break;
        }
        const auto count64 = static_cast<std::uint64_t>(count);
        const std::size_t count_bytes = encoded_varint_size(count64);

        if (count > (std::numeric_limits<std::size_t>::max() - count_bytes) / 32) {
            break;
        }
        const std::size_t trailer_size = count_bytes + count * 32;
        if (trailer_size > block_blob.size() - miner_suffix_offset) {
            continue;
        }

        const std::size_t offset = block_blob.size() - trailer_size;
        if (!has_current_hf6_coinbase_suffix_shape(
                block_blob, miner_suffix_offset, offset)) {
            continue;
        }

        std::uint64_t decoded_count = 0;
        std::size_t consumed = 0;
        if (!decode_varint_at(block_blob, offset, decoded_count, consumed) ||
            decoded_count != count64 || consumed != count_bytes) {
            continue;
        }

        ParsedTxHashTrailer candidate;
        candidate.serialized_offset = offset;
        candidate.serialized_size = trailer_size;
        candidate.hashes.reserve(count);

        std::size_t pos = offset + count_bytes;
        for (std::size_t i = 0; i < count; ++i) {
            Hash256 hash{};
            std::copy_n(block_blob.begin() + static_cast<std::ptrdiff_t>(pos),
                        32,
                        hash.begin());
            candidate.hashes.push_back(hash);
            pos += 32;
        }
        candidates.push_back(std::move(candidate));
    }

    if (candidates.empty()) {
        throw std::runtime_error(
            "could not locate current HF6 block transaction-hash trailer");
    }
    if (candidates.size() != 1) {
        throw std::runtime_error(
            "ambiguous current HF6 block transaction-hash trailer");
    }
    return std::move(candidates.front());
}

Hash256 transaction_tree_hash(
    const Hash256& miner_tx_hash,
    std::span<const Hash256> regular_tx_hashes) {
    std::vector<Hash256> hashes;
    hashes.reserve(regular_tx_hashes.size() + 1);
    hashes.push_back(miner_tx_hash);
    hashes.insert(hashes.end(), regular_tx_hashes.begin(), regular_tx_hashes.end());

    const std::size_t count = hashes.size();
    if (count == 1) {
        return hashes[0];
    }
    if (count == 2) {
        return hash_pair(hashes[0], hashes[1]);
    }

    // Exact algorithm from Zano src/crypto/tree-hash.c.
    std::size_t cnt = count - 1;
    for (std::size_t shift = 1; shift < 8 * sizeof(std::size_t); shift <<= 1) {
        cnt |= cnt >> shift;
    }
    cnt &= ~(cnt >> 1);

    std::vector<Hash256> intermediate(cnt);
    const std::size_t direct = 2 * cnt - count;
    std::copy_n(hashes.begin(), direct, intermediate.begin());

    std::size_t source = direct;
    std::size_t dest = direct;
    for (; dest < cnt; source += 2, ++dest) {
        intermediate[dest] = hash_pair(hashes[source], hashes[source + 1]);
    }

    while (cnt > 2) {
        cnt >>= 1;
        for (std::size_t i = 0, j = 0; j < cnt; i += 2, ++j) {
            intermediate[j] = hash_pair(intermediate[i], intermediate[i + 1]);
        }
    }

    return hash_pair(intermediate[0], intermediate[1]);
}

std::vector<std::uint8_t> make_hashing_blob(
    std::span<const std::uint8_t> zero_nonce_block_header,
    const Hash256& tx_tree_root,
    std::size_t total_transaction_count) {
    if (total_transaction_count == 0 ||
        total_transaction_count > std::numeric_limits<std::uint64_t>::max()) {
        throw std::runtime_error("invalid total transaction count");
    }

    std::vector<std::uint8_t> blob;
    blob.reserve(zero_nonce_block_header.size() + tx_tree_root.size() + 10);
    blob.insert(
        blob.end(), zero_nonce_block_header.begin(), zero_nonce_block_header.end());
    blob.insert(blob.end(), tx_tree_root.begin(), tx_tree_root.end());
    append_varint(blob, static_cast<std::uint64_t>(total_transaction_count));
    return blob;
}

MiningHeaderWork derive_mining_header_work(
    std::span<const std::uint8_t> block_blob) {
    MiningHeaderWork result;
    result.block_header = parse_pow_block_header(block_blob);

    const auto miner_and_tail = block_blob.subspan(result.block_header.serialized_size);
    result.miner_tx_prefix = parse_hf6_miner_tx_prefix(miner_and_tail);

    const std::size_t miner_suffix_offset =
        result.block_header.serialized_size +
        result.miner_tx_prefix.serialized_size;
    result.tx_hashes = parse_hf6_tx_hash_trailer(block_blob, miner_suffix_offset);

    result.tx_tree_root = transaction_tree_hash(
        result.miner_tx_prefix.hash,
        std::span<const Hash256>(result.tx_hashes.hashes));
    result.hashing_blob = make_hashing_blob(
        result.block_header.serialized,
        result.tx_tree_root,
        result.tx_hashes.hashes.size() + 1);
    result.header_hash = cn_fast_hash(result.hashing_blob);
    return result;
}

std::vector<std::uint8_t> make_single_tx_hashing_blob(
    std::span<const std::uint8_t> zero_nonce_block_header,
    const Hash256& miner_tx_hash) {
    return make_hashing_blob(zero_nonce_block_header, miner_tx_hash, 1);
}

}  // namespace zano_p2pool
