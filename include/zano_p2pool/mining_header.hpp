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

// Constructs the canonical Zano mining hashing blob for the special case where
// the block contains only the miner transaction. This helper is intentionally
// narrow until the full miner-tx suffix / regular tx-hash list parser lands.
[[nodiscard]] std::vector<std::uint8_t> make_single_tx_hashing_blob(
    std::span<const std::uint8_t> zero_nonce_block_header,
    const Hash256& miner_tx_hash);

}  // namespace zano_p2pool
