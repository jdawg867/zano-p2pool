#include "zano_p2pool/pplns_block_template.hpp"

#include "zano_p2pool/crypto_hash.hpp"

#include <cstddef>
#include <stdexcept>
#include <vector>

namespace zano_p2pool {

PplnsBlockTemplate replace_hf6_miner_tx(
    const BlockTemplate& base,
    std::string_view miner_tx_blob_hex) {
    if (base.blocktemplate_blob.empty()) {
        throw std::invalid_argument("base block template blob is empty");
    }
    if (miner_tx_blob_hex.empty()) {
        throw std::invalid_argument("replacement miner transaction is empty");
    }

    const auto base_bytes = hex_to_bytes(base.blocktemplate_blob);
    const auto replacement = hex_to_bytes(miner_tx_blob_hex);
    const MiningHeaderWork base_work = derive_mining_header_work(base_bytes);

    const std::size_t header_end = base_work.block_header.serialized_size;
    const std::size_t trailer_begin = base_work.tx_hashes.serialized_offset;
    if (header_end > trailer_begin || trailer_begin > base_bytes.size()) {
        throw std::logic_error("invalid parsed block-template boundaries");
    }

    // Validate the replacement as a current HF6 miner transaction before it is
    // embedded. Full proof verification is performed by the exact constructor
    // path / P2P trust layer; this structural parse prevents arbitrary bytes
    // from becoming a candidate template.
    static_cast<void>(parse_hf6_miner_tx_prefix(replacement));

    std::vector<std::uint8_t> rebuilt;
    rebuilt.reserve(
        header_end + replacement.size() + (base_bytes.size() - trailer_begin));
    rebuilt.insert(
        rebuilt.end(),
        base_bytes.begin(),
        base_bytes.begin() + static_cast<std::ptrdiff_t>(header_end));
    rebuilt.insert(rebuilt.end(), replacement.begin(), replacement.end());
    rebuilt.insert(
        rebuilt.end(),
        base_bytes.begin() + static_cast<std::ptrdiff_t>(trailer_begin),
        base_bytes.end());

    PplnsBlockTemplate result;
    result.block = base;
    result.block.blocktemplate_blob = bytes_to_hex(rebuilt);
    result.mining_work = derive_mining_header_work(rebuilt);

    if (result.mining_work.tx_hashes.hashes != base_work.tx_hashes.hashes) {
        throw std::logic_error(
            "replacement miner transaction changed regular transaction hashes");
    }
    if (result.mining_work.block_header.serialized !=
        base_work.block_header.serialized) {
        throw std::logic_error(
            "replacement miner transaction changed serialized block header");
    }

    return result;
}

}  // namespace zano_p2pool
