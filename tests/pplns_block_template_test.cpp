#include "zano_p2pool/crypto_hash.hpp"
#include "zano_p2pool/pplns_block_template.hpp"
#include "test_check.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace zano_p2pool;

std::vector<std::uint8_t> make_current_coinbase_suffix(
    std::span<const Hash256> tx_hashes) {
    std::vector<std::uint8_t> suffix{0x00, 0x00, 0x02, 0x2f};
    suffix.insert(suffix.end(), 16, 0xa5);
    suffix.push_back(0x30);
    suffix.insert(suffix.end(), 96, 0x5a);
    suffix.push_back(static_cast<std::uint8_t>(tx_hashes.size()));
    for (const auto& hash : tx_hashes) {
        suffix.insert(suffix.end(), hash.begin(), hash.end());
    }
    return suffix;
}

}  // namespace

int main() {
    using namespace zano_p2pool;

    constexpr std::string_view kBlockHeaderHex =
        "030000000000000000"
        "ce93bc37eb9764713c048e34e664b6a569ef8a482a8e757147209a428f098807"
        "008793c8d40600";
    constexpr std::string_view kMinerPrefixHex =
        "04010096890a064e0016ccb22467a02ed1abc42f8ee99f644156e6fecf78b452d843e8ab3ce3f07692b913177a616e6f2d7032706f6f6c2d6865616465722d7465737415000b02ac950b0262aa023f001203b6984fdcb2419692ca95b836644a8f4748e68ac8f5fef53d774b07cb8dff1129c42b889b5554144a3fd721ff0be9bf1f67b8ea6b0401bdb3b6ea3aa6f5afb935a1f52c1ed634248baea8fcb4c845f8c2acbeb0955fb07d8b69b1d871960374c32d3eaafafc623bf483e858d42e8bf4ec7df064ada2e34934469cff6b626841d49dd386718298edaa6a988fd9e1f3003f00bebdd890b675d084cec18de16ef98991cb57efb7a788b63c2c521471a018dd5f09348bbfd543a0f6452c4ed4e13ebafdbed2158d6b15dada12bf0af8672379062ea9021d8fe336ee2dbd7482d3c7dd6c3745138eeaeb25942eebeab885881af074c32d3eaafafc623bf483e858d42e8bf4ec7df064ada2e34934469cff6b626837286cec4bbae098966a7225863663ba0006";

    const auto header = hex_to_bytes(kBlockHeaderHex);
    const auto prefix = hex_to_bytes(kMinerPrefixHex);

    Hash256 tx1{};
    Hash256 tx2{};
    for (std::size_t i = 0; i < 32; ++i) {
        tx1[i] = static_cast<std::uint8_t>(i + 1);
        tx2[i] = static_cast<std::uint8_t>(0x80U + i);
    }
    const std::array<Hash256, 2> regular{tx1, tx2};

    std::vector<std::uint8_t> base_bytes = header;
    base_bytes.insert(base_bytes.end(), prefix.begin(), prefix.end());
    const auto suffix = make_current_coinbase_suffix(regular);
    base_bytes.insert(base_bytes.end(), suffix.begin(), suffix.end());

    BlockTemplate base;
    base.blocktemplate_blob = bytes_to_hex(base_bytes);
    const MiningHeaderWork original = derive_mining_header_work(base_bytes);
    CHECK(original.tx_hashes.hashes.size() == 2);

    std::vector<std::uint8_t> replacement(
        base_bytes.begin() + static_cast<std::ptrdiff_t>(
            original.block_header.serialized_size),
        base_bytes.begin() + static_cast<std::ptrdiff_t>(
            original.tx_hashes.serialized_offset));

    // Mutate one byte in the existing extra_user_data string. Its serialized
    // length and all transaction structure remain canonical, but the miner-tx
    // prefix hash (and therefore mining header) must change.
    const std::string marker = "zano-p2pool-header-test";
    const auto marker_it = std::search(
        replacement.begin(), replacement.end(), marker.begin(), marker.end());
    CHECK(marker_it != replacement.end());
    *marker_it ^= 0x01U;

    const auto rebuilt = replace_hf6_miner_tx(
        base,
        bytes_to_hex(replacement));

    CHECK(rebuilt.block.blob_bytes() == base_bytes.size());
    CHECK(rebuilt.mining_work.block_header.serialized ==
          original.block_header.serialized);
    CHECK(rebuilt.mining_work.tx_hashes.hashes == original.tx_hashes.hashes);
    CHECK(rebuilt.mining_work.miner_tx_prefix.hash !=
          original.miner_tx_prefix.hash);
    CHECK(rebuilt.mining_work.header_hash != original.header_hash);

    const auto rebuilt_bytes = hex_to_bytes(rebuilt.block.blocktemplate_blob);
    CHECK(std::equal(
        rebuilt_bytes.begin(),
        rebuilt_bytes.begin() + static_cast<std::ptrdiff_t>(
            original.block_header.serialized_size),
        base_bytes.begin()));

    const std::size_t rebuilt_trailer =
        rebuilt.mining_work.tx_hashes.serialized_offset;
    CHECK(std::equal(
        rebuilt_bytes.begin() + static_cast<std::ptrdiff_t>(rebuilt_trailer),
        rebuilt_bytes.end(),
        base_bytes.begin() + static_cast<std::ptrdiff_t>(
            original.tx_hashes.serialized_offset),
        base_bytes.end()));

    return 0;
}
