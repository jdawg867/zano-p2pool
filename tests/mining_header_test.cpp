#include "zano_p2pool/crypto_hash.hpp"
#include "zano_p2pool/mining_header.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

bool check(bool condition, const char* expression, int line) {
    if (condition) {
        return true;
    }
    std::cerr << "CHECK failed at line " << line << ": " << expression << '\n';
    return false;
}

#define CHECK(expr) do { if (!check((expr), #expr, __LINE__)) return 1; } while (false)

zano_p2pool::Hash256 pair_hash(
    const zano_p2pool::Hash256& left,
    const zano_p2pool::Hash256& right) {
    std::array<std::uint8_t, 64> bytes{};
    std::copy(left.begin(), left.end(), bytes.begin());
    std::copy(right.begin(), right.end(), bytes.begin() + 32);
    return zano_p2pool::cn_fast_hash(bytes);
}

std::vector<std::uint8_t> make_current_coinbase_suffix(
    std::span<const zano_p2pool::Hash256> tx_hashes) {
    // The proof payload contents do not participate in the miner transaction
    // prefix hash. We only model the current serialized suffix shape needed to
    // locate the final block.tx_hashes vector.
    std::vector<std::uint8_t> suffix{0x00, 0x00, 0x02, 0x2f};
    suffix.insert(suffix.end(), 16, 0xa5);  // dummy range-proof payload
    suffix.push_back(0x30);                // zc_balance_proof tag 48
    suffix.insert(suffix.end(), 96, 0x5a); // fixed balance-proof payload

    if (tx_hashes.size() >= 0x80) {
        throw std::runtime_error("test helper only supports short tx vectors");
    }
    suffix.push_back(static_cast<std::uint8_t>(tx_hashes.size()));
    for (const auto& hash : tx_hashes) {
        suffix.insert(suffix.end(), hash.begin(), hash.end());
    }
    return suffix;
}

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

    // Exercise the complete blocktemplate_blob path. The proof bytes are dummy
    // by design: Zano's miner tx ID hashes only transaction_prefix, while the
    // final tx_hashes field is serialized after the full miner transaction.
    std::vector<std::uint8_t> synthetic_block = header_bytes;
    synthetic_block.insert(
        synthetic_block.end(), miner_prefix_bytes.begin(), miner_prefix_bytes.end());
    const auto empty_suffix =
        make_current_coinbase_suffix(std::span<const Hash256>{});
    synthetic_block.insert(
        synthetic_block.end(), empty_suffix.begin(), empty_suffix.end());

    const auto work = derive_mining_header_work(synthetic_block);
    CHECK(work.block_header.serialized_size == 48);
    CHECK(work.miner_tx_prefix.serialized_size == 373);
    CHECK(work.tx_hashes.hashes.empty());
    CHECK(hash_to_hex(work.miner_tx_prefix.hash) == kMinerHash);
    CHECK(hash_to_hex(work.tx_tree_root) == kMinerHash);
    CHECK(bytes_to_hex(work.hashing_blob) == kHashingBlobHex);
    CHECK(hash_to_hex(work.header_hash) == kHeaderHash);

    // One regular transaction: CryptoNote tree_hash hashes the two 32-byte
    // entries directly.
    Hash256 tx1{};
    for (std::size_t i = 0; i < tx1.size(); ++i) {
        tx1[i] = static_cast<std::uint8_t>(i + 1);
    }
    const std::array<Hash256, 1> one_tx{tx1};
    CHECK(transaction_tree_hash(miner.hash, one_tx) == pair_hash(miner.hash, tx1));

    // Three total transactions exercise Zano's non-power-of-two tree rule:
    // H(miner || H(tx1 || tx2)).
    Hash256 tx2{};
    for (std::size_t i = 0; i < tx2.size(); ++i) {
        tx2[i] = static_cast<std::uint8_t>(0x80U + i);
    }
    const std::array<Hash256, 2> two_txs{tx1, tx2};
    const auto expected_three = pair_hash(miner.hash, pair_hash(tx1, tx2));
    CHECK(transaction_tree_hash(miner.hash, two_txs) == expected_three);

    // The reverse trailer locator must also work when regular tx hashes are
    // present, without parsing the 870-byte range proof.
    std::vector<std::uint8_t> block_with_txs = header_bytes;
    block_with_txs.insert(
        block_with_txs.end(), miner_prefix_bytes.begin(), miner_prefix_bytes.end());
    const auto tx_suffix = make_current_coinbase_suffix(two_txs);
    block_with_txs.insert(block_with_txs.end(), tx_suffix.begin(), tx_suffix.end());
    const auto work_with_txs = derive_mining_header_work(block_with_txs);
    CHECK(work_with_txs.tx_hashes.hashes.size() == 2);
    CHECK(work_with_txs.tx_hashes.hashes[0] == tx1);
    CHECK(work_with_txs.tx_hashes.hashes[1] == tx2);
    CHECK(work_with_txs.tx_tree_root == expected_three);

    return 0;
}
