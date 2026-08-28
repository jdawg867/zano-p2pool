#include "currency_core/currency_format_utils.h"
#include "currency_core/currency_format_utils_blocks.h"
#include "currency_core/currency_format_utils_transactions.h"
#include "crypto/hash.h"
#include "string_tools.h"

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: zano_header_oracle BLOCK_TEMPLATE_HEX\n";
        return 2;
    }

    const std::string hex = argv[1];
    std::string block_blob;
    if (!epee::string_tools::parse_hexstr_to_binbuff(hex, block_blob)) {
        std::cerr << "error: invalid block-template hex\n";
        return 1;
    }

    currency::block block{};
    if (!currency::parse_and_validate_block_from_blob(block_blob, block)) {
        std::cerr << "error: Zano could not deserialize/validate block template\n";
        return 1;
    }

    const auto header_blob =
        t_serializable_object_to_blob(static_cast<currency::block_header>(block));
    const auto miner_prefix_blob =
        t_serializable_object_to_blob(
            static_cast<currency::transaction_prefix>(block.miner_tx));
    const crypto::hash miner_tx_hash =
        currency::get_transaction_hash(block.miner_tx);
    const crypto::hash tree_root = currency::get_tx_tree_hash(block);

    auto hashing_blob = currency::get_block_hashing_blob(block);
    if (hashing_blob.size() < 9) {
        std::cerr << "error: hashing blob is unexpectedly short\n";
        return 1;
    }

    // Zano Stratum requires nonce zero in the hashing blob before cn_fast_hash.
    // Current Zano stores the 64-bit PoW nonce at bytes [1, 9).
    for (std::size_t i = 1; i < 9; ++i) {
        hashing_blob[i] = 0;
    }

    const crypto::hash header_hash =
        crypto::cn_fast_hash(hashing_blob.data(), hashing_blob.size());

    std::cout << "block_blob_bytes=" << block_blob.size() << '\n';
    std::cout << "block_header_bytes=" << header_blob.size() << '\n';
    std::cout << "block_header_hex="
              << epee::string_tools::buff_to_hex_nodelimer(header_blob) << '\n';
    std::cout << "miner_tx_version=" << block.miner_tx.version << '\n';
    std::cout << "miner_tx_hardfork_id="
              << static_cast<unsigned>(block.miner_tx.hardfork_id) << '\n';
    std::cout << "miner_tx_vin_count=" << block.miner_tx.vin.size() << '\n';
    std::cout << "miner_tx_extra_count=" << block.miner_tx.extra.size() << '\n';
    std::cout << "miner_tx_vout_count=" << block.miner_tx.vout.size() << '\n';
    std::cout << "miner_tx_prefix_bytes=" << miner_prefix_blob.size() << '\n';
    std::cout << "miner_tx_prefix_hex="
              << epee::string_tools::buff_to_hex_nodelimer(miner_prefix_blob)
              << '\n';
    std::cout << "miner_tx_hash="
              << epee::string_tools::pod_to_hex(miner_tx_hash) << '\n';
    std::cout << "tx_hashes_count=" << block.tx_hashes.size() << '\n';
    std::cout << "tx_tree_root="
              << epee::string_tools::pod_to_hex(tree_root) << '\n';
    std::cout << "hashing_blob_bytes=" << hashing_blob.size() << '\n';
    std::cout << "hashing_blob_hex="
              << epee::string_tools::buff_to_hex_nodelimer(hashing_blob) << '\n';
    std::cout << "header_hash="
              << epee::string_tools::pod_to_hex(header_hash) << '\n';
    return 0;
}
