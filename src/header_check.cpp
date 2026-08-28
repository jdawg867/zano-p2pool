#include "zano_p2pool/crypto_hash.hpp"
#include "zano_p2pool/mining_header.hpp"

#include <exception>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: zano-p2pool-header BLOCK_TEMPLATE_HEX\n";
        return 2;
    }

    try {
        const auto block_blob = zano_p2pool::hex_to_bytes(argv[1]);
        const auto work = zano_p2pool::derive_mining_header_work(block_blob);

        std::cout << "block_blob_bytes=" << block_blob.size() << '\n';
        std::cout << "block_header_bytes="
                  << work.block_header.serialized_size << '\n';
        std::cout << "miner_tx_prefix_bytes="
                  << work.miner_tx_prefix.serialized_size << '\n';
        std::cout << "miner_tx_hash="
                  << zano_p2pool::hash_to_hex(work.miner_tx_prefix.hash) << '\n';
        std::cout << "tx_hashes_count=" << work.tx_hashes.hashes.size() << '\n';
        std::cout << "tx_tree_root="
                  << zano_p2pool::hash_to_hex(work.tx_tree_root) << '\n';
        std::cout << "hashing_blob_bytes=" << work.hashing_blob.size() << '\n';
        std::cout << "hashing_blob_hex="
                  << zano_p2pool::bytes_to_hex(work.hashing_blob) << '\n';
        std::cout << "header_hash="
                  << zano_p2pool::hash_to_hex(work.header_hash) << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}
