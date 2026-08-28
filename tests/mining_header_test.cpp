#include "zano_p2pool/crypto_hash.hpp"
#include "zano_p2pool/mining_header.hpp"

#include <iostream>
#include <span>
#include <string>

namespace {

bool check(bool condition, const char* expression, int line) {
    if (condition) {
        return true;
    }
    std::cerr << "CHECK failed at line " << line << ": " << expression << '\n';
    return false;
}

#define CHECK(expr) do { if (!check((expr), #expr, __LINE__)) return 1; } while (false)

}  // namespace

int main() {
    using namespace zano_p2pool;

    // Zano testnet RPC template captured at height 165014.
    constexpr std::string_view kBlockHeaderHex =
        "030000000000000000"
        "ce93bc37eb9764713c048e34e664b6a569ef8a482a8e757147209a428f098807"
        "008793c8d40600";

    constexpr std::string_view kMinerPrefixHex =
        "04010096890a064e0016ccb22467a02ed1abc42f8ee99f644156e6fecf78b452d843e8ab3ce3f07692b913177a616e6f2d7032706f6f6c2d6865616465722d7465737415000b02ac950b0262aa023f001203b6984fdcb2419692ca95b836644a8f4748e68ac8f5fef53d774b07cb8dff1129c42b889b5554144a3fd721ff0be9bf1f67b8ea6b0401bdb3b6ea3aa6f5afb935a1f52c1ed634248baea8fcb4c845f8c2acbeb0955fb07d8b69b1d871960374c32d3eaafafc623bf483e858d42e8bf4ec7df064ada2e34934469cff6b626841d49dd386718298edaa6a988fd9e1f3003f00bebdd890b675d084cec18de16ef98991cb57efb7a788b63c2c521471a018dd5f09348bbfd543a0f6452c4ed4e13ebafdbed2158d6b15dada12bf0af8672379062ea9021d8fe336ee2dbd7482d3c7dd6c3745138eeaeb25942eebeab885881af074c32d3eaafafc623bf483e858d42e8bf4ec7df064ada2e34934469cff6b626837286cec4bbae098966a7225863663ba0006";

    constexpr std::string_view kMinerHash =
        "86f2642370461817117caff3f4bec6ae89c96ea1f3f0c213e98a31bfd8718e01";
    constexpr std::string_view kHashingBlobHex =
        "030000000000000000ce93bc37eb9764713c048e34e664b6a569ef8a482a8e757147209a428f098807008793c8d40600"
        "86f2642370461817117caff3f4bec6ae89c96ea1f3f0c213e98a31bfd8718e01"
        "01";
    constexpr std::string_view kHeaderHash =
        "43147bd3560a1385c7359475e8974bbfc7aeac85c328e779e037b2d8eeec604e";

    const auto header_bytes = hex_to_bytes(kBlockHeaderHex);
    CHECK(header_bytes.size() == 48);
    const auto header = parse_pow_block_header(header_bytes);
    CHECK(header.serialized_size == 48);
    CHECK(bytes_to_hex(header.serialized) == kBlockHeaderHex);

    const auto miner_prefix_bytes = hex_to_bytes(kMinerPrefixHex);
    CHECK(miner_prefix_bytes.size() == 373);
    const auto miner = parse_hf6_miner_tx_prefix(miner_prefix_bytes);
    CHECK(miner.version == 4);
    CHECK(miner.hardfork_id == 6);
    CHECK(miner.vin_count == 1);
    CHECK(miner.extra_count == 6);
    CHECK(miner.vout_count == 2);
    CHECK(miner.serialized_size == 373);
    CHECK(hash_to_hex(miner.hash) == kMinerHash);

    const auto hashing_blob = make_single_tx_hashing_blob(
        std::span<const std::uint8_t>(header.serialized), miner.hash);
    CHECK(hashing_blob.size() == 81);
    CHECK(bytes_to_hex(hashing_blob) == kHashingBlobHex);
    CHECK(hash_to_hex(cn_fast_hash(hashing_blob)) == kHeaderHash);

    return 0;
}
