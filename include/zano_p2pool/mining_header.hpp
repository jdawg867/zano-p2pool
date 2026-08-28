#pragma once

#include "zano_p2pool/pow_target.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace zano_p2pool {

struct ParsedBlockHeader {
    std::size_t serialized_size{};
    std::vector<std::uint8_t> serialized;
};

struct ParsedMinerTxPrefix {
    std::uint64_t version{};
    std::uint8_t hardfork_id{};
    std::size_t vin_count{};
    std::size_t extra_count{};
    std::size_t vout_count{};
    std::size_t serialized_size{};
    std::vector<std::uint8_t> serialized;
    Hash256 hash{};
};

struct ParsedTxHashTrailer {
    std::size_t serialized_offset{};
    std::size_t serialized_size{};
    std::vector<Hash256> hashes;
};

struct MiningHeaderWork {
    ParsedBlockHeader block_header;
    ParsedMinerTxPrefix miner_tx_prefix;
    ParsedTxHashTrailer tx_hashes;
    Hash256 tx_tree_root{};
    std::vector<std::uint8_t> hashing_blob;
    Hash256 header_hash{};
};

// Parses the current Zano PoW block-header serialization from the beginning of
// a full block blob. The returned serialized bytes have the nonce zeroed, as
// required before computing the ProgPoWZ mining header hash.
[[nodiscard]] ParsedBlockHeader parse_pow_block_header(
    std::span<const std::uint8_t> block_blob);

// Parses the current HF6 PoW coinbase transaction prefix. The input must begin
// at the miner transaction (immediately after the serialized block header).
// This deliberately stops at transaction_prefix; signatures/proofs are outside
// the miner transaction hash used by get_tx_tree_hash().
[[nodiscard]] ParsedMinerTxPrefix parse_hf6_miner_tx_prefix(
    std::span<const std::uint8_t> miner_tx_blob);

// Locates and parses block.tx_hashes, which is the final serialized field in a
// current HF6 block. The current PoW coinbase suffix is structurally validated
// so a random byte inside a hash cannot be mistaken for the vector count.
[[nodiscard]] ParsedTxHashTrailer parse_hf6_tx_hash_trailer(
    std::span<const std::uint8_t> block_blob,
    std::size_t miner_suffix_offset);

// Exact CryptoNote tree_hash used by current Zano. The miner transaction hash
// must be the first hash, followed by the block's regular transaction hashes.
[[nodiscard]] Hash256 transaction_tree_hash(
    const Hash256& miner_tx_hash,
    std::span<const Hash256> regular_tx_hashes);

[[nodiscard]] std::vector<std::uint8_t> make_hashing_blob(
    std::span<const std::uint8_t> zero_nonce_block_header,
    const Hash256& tx_tree_root,
    std::size_t total_transaction_count);

// Derives Zano's canonical ProgPoWZ mining-header input directly from the full
// RPC blocktemplate_blob, without linking currency_core.
[[nodiscard]] MiningHeaderWork derive_mining_header_work(
    std::span<const std::uint8_t> block_blob);

// Compatibility helper retained for existing callers/tests.
[[nodiscard]] std::vector<std::uint8_t> make_single_tx_hashing_blob(
    std::span<const std::uint8_t> zero_nonce_block_header,
    const Hash256& miner_tx_hash);

}  // namespace zano_p2pool
