#include "currency_core/currency_format_utils.h"
#include "currency_core/currency_format_utils_blocks.h"
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

    std::cout << "hashing_blob_bytes=" << hashing_blob.size() << '\n';
    std::cout << "header_hash="
              << epee::string_tools::pod_to_hex(header_hash) << '\n';
    return 0;
}
