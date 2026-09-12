#include "zano_p2pool/historical_work.hpp"
#include "zano_p2pool/mining_header.hpp"
#include "hf6_test_suffix.hpp"
#include "zano_p2pool/progpowz.hpp"
#include <filesystem>
#include <unistd.h>
#include "test_check.hpp"
#include <algorithm>
#include <functional>

namespace {

using namespace zano_p2pool;

[[nodiscard]] bool throws_runtime_error(const std::function<void()>& fn) {
    try {
        fn();
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

[[nodiscard]] Hash256 hash_from_hex(std::string_view hex) {
    const auto bytes = hex_to_bytes(hex);
    CHECK(bytes.size() == 32);
    Hash256 hash{};
    std::copy(bytes.begin(), bytes.end(), hash.begin());
    return hash;
}

[[nodiscard]] std::vector<std::uint8_t> make_current_coinbase_suffix() {
    // Same current-HF6 suffix shape used by mining_header_test. Proof bytes are
    // dummy because this checkpoint validates transport/structural anchoring,
    // not miner-transaction cryptographic proofs.
    std::vector<std::uint8_t> suffix{0x00, 0x00, 0x02, 0x2f};
    const auto range = make_structural_range_proof();
    suffix.insert(suffix.end(), range.begin(), range.end());
    suffix.push_back(0x30);
    suffix.insert(suffix.end(), 96, 0x5a);
    suffix.push_back(0x00);  // zero regular transaction hashes
    return suffix;
}

[[nodiscard]] P2pMiningContextProposal make_proposal() {
    constexpr std::string_view kBlockHeaderHex =
        "030000000000000000"
        "ce93bc37eb9764713c048e34e664b6a569ef8a482a8e757147209a428f098807"
        "008793c8d40600";

    constexpr std::string_view kMinerPrefixHex =
        "04010096890a064e0016ccb22467a02ed1abc42f8ee99f644156e6fecf78b452d843e8ab3ce3f07692b913177a616e6f2d7032706f6f6c2d6865616465722d7465737415000b02ac950b0262aa023f001203b6984fdcb2419692ca95b836644a8f4748e68ac8f5fef53d774b07cb8dff1129c42b889b5554144a3fd721ff0be9bf1f67b8ea6b0401bdb3b6ea3aa6f5afb935a1f52c1ed634248baea8fcb4c845f8c2acbeb0955fb07d8b69b1d871960374c32d3eaafafc623bf483e858d42e8bf4ec7df064ada2e34934469cff6b626841d49dd386718298edaa6a988fd9e1f3003f00bebdd890b675d084cec18de16ef98991cb57efb7a788b63c2c521471a018dd5f09348bbfd543a0f6452c4ed4e13ebafdbed2158d6b15dada12bf0af8672379062ea9021d8fe336ee2dbd7482d3c7dd6c3745138eeaeb25942eebeab885881af074c32d3eaafafc623bf483e858d42e8bf4ec7df064ada2e34934469cff6b626837286cec4bbae098966a7225863663ba0006";

    P2pMiningContextProposal proposal;
    proposal.zano_height = 165014;
    proposal.prev_hash = hash_from_hex(
        "ce93bc37eb9764713c048e34e664b6a569ef8a482a8e757147209a428f098807");
    proposal.network_difficulty = difficulty128_from_decimal("1229990");
    proposal.seed = hash_from_hex(
        "f2e59013a0a379837166b59f871b20a8a0d101d1c355ea85d35329360e69c000");
    proposal.block_reward_without_fee = 1'000'000'000'000ULL;
    proposal.block_reward = 1'000'000'000'000ULL;
    proposal.txs_fee = 0;
    proposal.block_template_blob = hex_to_bytes(kBlockHeaderHex);
    const auto miner_prefix = hex_to_bytes(kMinerPrefixHex);
    proposal.block_template_blob.insert(
        proposal.block_template_blob.end(), miner_prefix.begin(), miner_prefix.end());
    const auto suffix = make_current_coinbase_suffix();
    proposal.block_template_blob.insert(
        proposal.block_template_blob.end(), suffix.begin(), suffix.end());
    proposal.miner_tx_tgc_json =
        R"json({"tx_key":"00112233445566778899aabbccddeeff","tx_pub_key_p":"1122","tx_outs_attr":0})json";
    return proposal;
}

} // namespace
int main() {
    auto proposal = make_proposal();
    int calls = 0;
    const auto lookup = [&](std::uint64_t h) {
        CHECK(h == proposal.zano_height - 1);
        ++calls;
        return RpcCanonicalHeader{h, proposal.prev_hash};
    };
    CHECK(audit_historical_parent(proposal, lookup).status ==
          HistoricalParentStatus::ParentMatchedUntrusted);
    CHECK(calls == 2);
    auto other = proposal.prev_hash;
    other[0] ^= 1;
    CHECK(audit_historical_parent(proposal, [&](std::uint64_t h) {
        return RpcCanonicalHeader{h, other};
    }).status == HistoricalParentStatus::ParentMismatch);
    calls = 0;
    CHECK(audit_historical_parent(proposal, [&](std::uint64_t h) {
        return RpcCanonicalHeader{h, ++calls == 1 ? proposal.prev_hash : other};
    }).status == HistoricalParentStatus::ParentChangedDuringCheck);
    CHECK(throws_runtime_error([&] {
        static_cast<void>(audit_historical_parent(proposal, [&](std::uint64_t h) {
            return RpcCanonicalHeader{h + 1, proposal.prev_hash};
        }));
    }));
    CHECK(throws_runtime_error([&] {
        static_cast<void>(audit_historical_parent(proposal, [](std::uint64_t) -> RpcCanonicalHeader {
            throw std::runtime_error("daemon unavailable");
        }));
    }));
    calls = 0;
    // A metadata claim cannot override the actual blob's parent/height.
    auto malformed = proposal;
    malformed.prev_hash = other;
    CHECK(throws_runtime_error([&] { static_cast<void>(audit_historical_parent(malformed, lookup)); }));
    malformed = proposal;
    ++malformed.zano_height;
    CHECK(throws_runtime_error([&] { static_cast<void>(audit_historical_parent(malformed, lookup)); }));
    malformed = proposal;
    malformed.zano_height = 0;
    CHECK(throws_runtime_error([&] { static_cast<void>(audit_historical_parent(malformed, lookup)); }));
    CHECK(calls == 0);
    // Compare the peer's anchor with independently supplied local observations.
    proposal.seed = progpowz_seed(proposal.zano_height);
    P2pMiningAnchor local{proposal.zano_height, proposal.prev_hash,
        proposal.network_difficulty, proposal.seed, proposal.block_reward_without_fee};
    std::vector<P2pMiningAnchor> observations{local};
    CHECK(audit_historical_local_anchor(proposal, observations, lookup).status ==
        HistoricalAnchorStatus::AnchorMatchedUntrusted);
    CHECK(audit_historical_local_anchor(proposal, {}, lookup).status ==
        HistoricalAnchorStatus::LocalObservationMissing);
    auto wrong_parent = local;
    wrong_parent.prev_hash = other;
    observations = {wrong_parent};
    CHECK(audit_historical_local_anchor(proposal, observations, lookup).status ==
        HistoricalAnchorStatus::LocalObservationMissing);
    observations = {local};
    auto changed = proposal;
    changed.network_difficulty = difficulty128_from_decimal("9");
    CHECK(audit_historical_local_anchor(changed, observations, lookup).status ==
        HistoricalAnchorStatus::LocalObservationMismatch);
    changed = proposal;
    ++changed.block_reward_without_fee;
    CHECK(audit_historical_local_anchor(changed, observations, lookup).status ==
        HistoricalAnchorStatus::LocalObservationMismatch);
    changed = proposal;
    changed.seed[0] ^= 1;
    CHECK(audit_historical_local_anchor(changed, observations, lookup).status ==
        HistoricalAnchorStatus::SeedMismatch);
    auto conflict = local;
    ++conflict.block_reward_without_fee;
    observations = {local, conflict};
    CHECK(audit_historical_local_anchor(proposal, observations, lookup).status ==
        HistoricalAnchorStatus::LocalObservationConflict);
    std::reverse(observations.begin(), observations.end());
    CHECK(audit_historical_local_anchor(proposal, observations, lookup).status ==
        HistoricalAnchorStatus::LocalObservationConflict);
    observations = {local, local};
    CHECK(audit_historical_local_anchor(proposal, observations, lookup).matching_observations == 2);
    CHECK(audit_historical_local_anchor(proposal, observations, [&](std::uint64_t h) {
        return RpcCanonicalHeader{h, other};
    }).status == HistoricalAnchorStatus::ParentMismatch);
    calls = 0;
    CHECK(audit_historical_local_anchor(proposal, observations, [&](std::uint64_t h) {
        return RpcCanonicalHeader{h, ++calls == 1 ? local.prev_hash : other};
    }).status == HistoricalAnchorStatus::ParentChangedDuringCheck);

    std::string pattern = (std::filesystem::temp_directory_path()/"zano-anchor-XXXXXX").string();
    CHECK(mkdtemp(pattern.data()) != nullptr);
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::filesystem::remove_all(path); } } cleanup{pattern};
    Hash256 sidechain{}; sidechain[0] = 1;
    MiningWorkArchive archive(pattern, sidechain);
    CHECK(load_local_mining_anchors(archive).empty());
    static_cast<void>(archive.put(serialize_p2p_mining_context_payload(proposal)));
    CHECK(load_local_mining_anchors(archive) == std::vector<P2pMiningAnchor>{local});
    CHECK(throws_runtime_error([&] { static_cast<void>(load_local_mining_anchors(archive, 0)); }));
    MiningWorkArchive other_chain(pattern, other);
    CHECK(throws_runtime_error([&] { static_cast<void>(load_local_mining_anchors(other_chain)); }));
    // No ShareChain or trusted-work registry is available to this API.
}
