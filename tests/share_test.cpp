#include "zano_p2pool/crypto_hash.hpp"
#include "zano_p2pool/share.hpp"
#include "zano_p2pool/share_chain.hpp"
#include "zano_p2pool/share_store.hpp"
#include "zano_p2pool/sidechain_params.hpp"
#include "test_check.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

template <typename Exception, typename Fn>
void expect_throw(Fn&& fn) {
    bool threw = false;
    try {
        fn();
    } catch (const Exception&) {
        threw = true;
    }
    CHECK(threw);
}

zano_p2pool::Share make_vector_share() {
    using namespace zano_p2pool;

    Share share;
    for (std::size_t i = 0; i < share.parent_id.size(); ++i) {
        share.parent_id[i] = static_cast<std::uint8_t>(i);
        share.miner_id[i] = static_cast<std::uint8_t>(0xa0U + i);
    }
    share.share_height = 1;
    share.timestamp = UINT64_C(0x0102030405060708);
    share.zano_height = 165014;
    const auto mining_header = hex_to_bytes(
        "43147bd3560a1385c7359475e8974bbfc7aeac85c328e779e037b2d8eeec604e");
    CHECK(mining_header.size() == share.mining_header_hash.size());
    std::copy(
        mining_header.begin(), mining_header.end(), share.mining_header_hash.begin());
    share.nonce = UINT64_C(0x1122334455667788);
    share.share_difficulty = difficulty128_from_decimal("100000000");
    share.network_difficulty = difficulty128_from_decimal("1229990");
    return share;
}

zano_p2pool::Share make_store_share(
    const zano_p2pool::ShareId& parent_id,
    std::uint64_t share_height,
    std::uint64_t nonce,
    std::uint8_t payout_seed) {
    using namespace zano_p2pool;

    Share share;
    share.version = kShareVersion2;
    share.parent_id = parent_id;
    share.share_height = share_height;
    share.timestamp = 1'800'000'000 + share_height * 10;
    share.zano_height = 170000;
    for (std::size_t i = 0; i < share.mining_header_hash.size(); ++i) {
        share.mining_header_hash[i] = static_cast<std::uint8_t>(0x40U + i);
    }
    share.nonce = nonce;
    share.share_difficulty = difficulty128_from_decimal("100000000");
    share.network_difficulty = difficulty128_from_decimal("100000000");

    PayoutPublicKeys payout;
    for (std::size_t i = 0; i < payout.spend_public_key.size(); ++i) {
        payout.spend_public_key[i] =
            static_cast<std::uint8_t>(payout_seed + i);
        payout.view_public_key[i] =
            static_cast<std::uint8_t>(payout_seed + 0x40U + i);
    }
    share.payout = payout;
    share.miner_id = miner_id_from_payout(payout);
    return share;
}

std::filesystem::path temporary_store_path(std::string_view suffix) {
    const auto stamp = std::chrono::steady_clock::now()
                           .time_since_epoch()
                           .count();
    return std::filesystem::temp_directory_path() /
           ("zano_p2pool_share_store_" + std::to_string(stamp) + "_" +
            std::string(suffix) + ".dat");
}

void remove_store(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path.string() + ".tmp", ec);
}

}  // namespace

int main() {
    using namespace zano_p2pool;

    constexpr std::string_view kExpectedHex =
        "5a50325301"
        "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
        "0000000000000001"
        "0102030405060708"
        "0000000000028496"
        "43147bd3560a1385c7359475e8974bbfc7aeac85c328e779e037b2d8eeec604e"
        "1122334455667788"
        "00000000000000000000000005f5e100"
        "0000000000000000000000000012c4a6"
        "a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebf";

    const Share share = make_vector_share();
    const auto encoded = serialize_share(share);
    CHECK(encoded.size() == kShareV1SerializedSize);
    CHECK(bytes_to_hex(encoded) == kExpectedHex);

    const Share decoded = deserialize_share(encoded);
    CHECK(decoded == share);
    CHECK(serialize_share(decoded) == encoded);
    CHECK(share_id(decoded) == share_id(share));

    // V2 preserves all v1 fields, appends the two public payout keys and binds
    // miner_id to those keys. No secret wallet material is serialized.
    Share v2 = share;
    v2.version = kShareVersion2;
    PayoutPublicKeys payout;
    for (std::size_t i = 0; i < 32; ++i) {
        payout.spend_public_key[i] = static_cast<std::uint8_t>(0x10U + i);
        payout.view_public_key[i] = static_cast<std::uint8_t>(0x80U + i);
    }
    v2.payout = payout;
    v2.miner_id = miner_id_from_payout(payout);

    const auto encoded_v2 = serialize_share(v2);
    CHECK(encoded_v2.size() == kShareV2SerializedSize);
    CHECK(encoded_v2.size() == encoded.size() + 64);
    const Share decoded_v2 = deserialize_share(encoded_v2);
    CHECK(decoded_v2 == v2);
    CHECK(decoded_v2.payout.has_value());
    CHECK(decoded_v2.miner_id == miner_id_from_payout(*decoded_v2.payout));
    CHECK(share_id(v2) != share_id(share));

    Share bad_binding = v2;
    bad_binding.miner_id[0] ^= 1;
    expect_throw<std::invalid_argument>([&] {
        (void)serialize_share(bad_binding);
    });

    auto tampered_payout = encoded_v2;
    tampered_payout.back() ^= 1;
    expect_throw<std::runtime_error>([&] {
        (void)deserialize_share(tampered_payout);
    });

    Share mutated = share;
    ++mutated.nonce;
    CHECK(share_id(mutated) != share_id(share));

    Share root = share;
    root.parent_id = {};
    CHECK(is_zero_share_id(root.parent_id));
    CHECK(!is_zero_share_id(share.parent_id));

    CHECK(difficulty128_to_decimal(share.share_difficulty) == "100000000");
    CHECK(difficulty128_to_decimal(share.network_difficulty) == "1229990");
    CHECK(!difficulty128_is_zero(share.share_difficulty));
    CHECK(difficulty128_is_zero(Difficulty128{}));

    const auto max128 = difficulty128_from_decimal(
        "340282366920938463463374607431768211455");
    CHECK(std::all_of(max128.begin(), max128.end(), [](std::uint8_t byte) {
        return byte == 0xff;
    }));
    CHECK(difficulty128_to_decimal(max128) ==
          "340282366920938463463374607431768211455");

    expect_throw<std::invalid_argument>([] {
        (void)difficulty128_from_decimal("0");
    });
    expect_throw<std::invalid_argument>([] {
        (void)difficulty128_from_decimal("12x34");
    });
    expect_throw<std::out_of_range>([] {
        (void)difficulty128_from_decimal(
            "340282366920938463463374607431768211456");
    });

    auto bad_magic = encoded;
    bad_magic[0] ^= 0xff;
    expect_throw<std::runtime_error>([&] {
        (void)deserialize_share(bad_magic);
    });

    auto bad_version = encoded;
    bad_version[4] = 3;
    expect_throw<std::runtime_error>([&] {
        (void)deserialize_share(bad_version);
    });

    auto truncated = encoded;
    truncated.pop_back();
    expect_throw<std::runtime_error>([&] {
        (void)deserialize_share(truncated);
    });

    auto trailing = encoded;
    trailing.push_back(0);
    expect_throw<std::runtime_error>([&] {
        (void)deserialize_share(trailing);
    });

    Share missing_payout = share;
    missing_payout.version = kShareVersion2;
    expect_throw<std::invalid_argument>([&] {
        (void)serialize_share(missing_payout);
    });

    Share v1_with_payout = share;
    v1_with_payout.payout = payout;
    expect_throw<std::invalid_argument>([&] {
        (void)serialize_share(v1_with_payout);
    });

    // Persistence: canonical records replay into the same connected chain and
    // reproduce the deterministic best tip.
    const SidechainParameters testnet_params = canonical_sidechain_parameters(
        SidechainParentNetwork::Testnet);
    const SidechainId testnet_id = sidechain_id(testnet_params);
    const Share stored_root = make_store_share({}, 0, 1, 0x10);
    const Share stored_child = make_store_share(
        share_id(stored_root), 1, 2, 0x20);

    const std::filesystem::path replay_path = temporary_store_path("replay");
    remove_store(replay_path);
    {
        ShareStore store(replay_path, testnet_id);
        store.append(stored_root);
        store.append(stored_child);

        ShareChain restored(testnet_params);
        const ShareStoreLoadResult load = store.load_into(restored);
        CHECK(load.records_loaded == 2);
        CHECK(load.connected_shares == 2);
        CHECK(load.orphan_shares == 0);
        CHECK(!load.truncated_tail_repaired);
        CHECK(load.best_tip_id.has_value());
        CHECK(*load.best_tip_id == share_id(stored_child));
        CHECK(restored.best_tip() != nullptr);
        CHECK(restored.best_tip()->share == stored_child);
    }
    remove_store(replay_path);

    // Recovery remains deterministic even if an orphan was persisted before its
    // parent and only became connected later in the original runtime.
    const std::filesystem::path orphan_path = temporary_store_path("orphan");
    remove_store(orphan_path);
    {
        ShareStore store(orphan_path, testnet_id);
        store.append(stored_child);
        store.append(stored_root);

        ShareChain restored(testnet_params);
        const ShareStoreLoadResult load = store.load_into(restored);
        CHECK(load.records_loaded == 2);
        CHECK(load.connected_shares == 2);
        CHECK(load.orphan_shares == 0);
        CHECK(load.best_tip_id == std::optional<ShareId>{share_id(stored_child)});
    }
    remove_store(orphan_path);

    // A store is consensus-profile specific and must never be replayed under a
    // different parent network / SidechainId.
    const std::filesystem::path network_path = temporary_store_path("network");
    remove_store(network_path);
    {
        ShareStore testnet_store(network_path, testnet_id);
        testnet_store.append(stored_root);

        const SidechainParameters mainnet_params = canonical_sidechain_parameters(
            SidechainParentNetwork::Mainnet);
        ShareStore wrong_network(network_path, sidechain_id(mainnet_params));
        ShareChain chain(mainnet_params);
        expect_throw<std::runtime_error>([&] {
            (void)wrong_network.load_into(chain);
        });
    }
    remove_store(network_path);

    // A complete record whose checksum no longer matches is corruption, not a
    // crash tail, and therefore fails closed.
    const std::filesystem::path corrupt_path = temporary_store_path("corrupt");
    remove_store(corrupt_path);
    {
        ShareStore store(corrupt_path, testnet_id);
        store.append(stored_root);
        {
            std::fstream file(
                corrupt_path,
                std::ios::binary | std::ios::in | std::ios::out);
            CHECK(static_cast<bool>(file));
            file.seekg(-1, std::ios::end);
            char byte = 0;
            file.read(&byte, 1);
            CHECK(static_cast<bool>(file));
            byte ^= 1;
            file.seekp(-1, std::ios::end);
            file.write(&byte, 1);
            file.flush();
            CHECK(static_cast<bool>(file));
        }

        ShareChain restored(testnet_params);
        expect_throw<std::runtime_error>([&] {
            (void)store.load_into(restored);
        });
    }
    remove_store(corrupt_path);

    // A short final record is the expected failure shape of an interrupted
    // append. Recovery truncates only that incomplete tail and keeps every prior
    // complete record.
    const std::filesystem::path tail_path = temporary_store_path("tail");
    remove_store(tail_path);
    {
        ShareStore store(tail_path, testnet_id);
        store.append(stored_root);
        const std::uintmax_t one_record_size = std::filesystem::file_size(tail_path);
        store.append(stored_child);
        const std::uintmax_t two_record_size = std::filesystem::file_size(tail_path);
        CHECK(two_record_size > one_record_size + 10);
        std::filesystem::resize_file(tail_path, two_record_size - 10);

        ShareChain restored(testnet_params);
        const ShareStoreLoadResult load = store.load_into(restored);
        CHECK(load.records_loaded == 1);
        CHECK(load.connected_shares == 1);
        CHECK(load.orphan_shares == 0);
        CHECK(load.truncated_tail_repaired);
        CHECK(std::filesystem::file_size(tail_path) == one_record_size);
        CHECK(restored.best_tip() != nullptr);
        CHECK(restored.best_tip()->share == stored_root);

        ShareChain restored_again(testnet_params);
        const ShareStoreLoadResult clean = store.load_into(restored_again);
        CHECK(clean.records_loaded == 1);
        CHECK(!clean.truncated_tail_repaired);
    }
    remove_store(tail_path);

    // Durable compaction regression. A subtree proven stale in memory must not
    // reappear after ShareStore rewrite + restart replay. At the same time,
    // unrelated connected history and unresolved orphan evidence must survive.
    const std::filesystem::path rewrite_path =
        temporary_store_path("rewrite");
    remove_store(rewrite_path);
    {
        const Share rewrite_root =
            make_store_share({}, 0, 1, 0x51);
        const Share rewrite_child =
            make_store_share(
                share_id(rewrite_root),
                1,
                2,
                0x52);
        const Share survivor_root =
            make_store_share({}, 0, 3, 0x61);

        ShareId missing_parent{};
        missing_parent[0] = 0xee;

        const Share surviving_orphan =
            make_store_share(
                missing_parent,
                1,
                1,
                0x71);

        const ShareId rewrite_root_id =
            share_id(rewrite_root);
        const ShareId rewrite_child_id =
            share_id(rewrite_child);
        const ShareId survivor_root_id =
            share_id(survivor_root);
        const ShareId surviving_orphan_id =
            share_id(surviving_orphan);

        ShareStore store(rewrite_path, testnet_id);

        store.append(rewrite_root);
        store.append(rewrite_child);
        store.append(survivor_root);
        store.append(surviving_orphan);

        const std::uintmax_t before_rewrite_size =
            std::filesystem::file_size(rewrite_path);

        ShareChain active(testnet_params);
        const ShareStoreLoadResult initial =
            store.load_into(active);

        CHECK(initial.records_loaded == 4);
        CHECK(initial.connected_shares == 3);
        CHECK(initial.orphan_shares == 1);

        CHECK(active.find(rewrite_root_id) != nullptr);
        CHECK(active.find(rewrite_child_id) != nullptr);
        CHECK(active.find(survivor_root_id) != nullptr);
        CHECK(active.is_orphan(surviving_orphan_id));

        CHECK(
            active.prune_connected_subtree(
                rewrite_root_id) == 2);

        CHECK(active.find(rewrite_root_id) == nullptr);
        CHECK(active.find(rewrite_child_id) == nullptr);
        CHECK(active.find(survivor_root_id) != nullptr);
        CHECK(active.is_orphan(surviving_orphan_id));

        const std::vector<Share> snapshot =
            active.persistence_snapshot();

        CHECK(snapshot.size() == 2);

        bool saw_survivor = false;
        bool saw_orphan = false;
        for (const Share& share : snapshot) {
            const ShareId id = share_id(share);
            saw_survivor |= id == survivor_root_id;
            saw_orphan |= id == surviving_orphan_id;
        }

        CHECK(saw_survivor);
        CHECK(saw_orphan);

        store.rewrite(snapshot);

        const std::uintmax_t after_rewrite_size =
            std::filesystem::file_size(rewrite_path);

        CHECK(after_rewrite_size < before_rewrite_size);

        // First restart after compaction: stale records are physically gone.
        ShareChain restored_after_rewrite(testnet_params);
        const ShareStoreLoadResult first_restart =
            store.load_into(restored_after_rewrite);

        CHECK(first_restart.records_loaded == 2);
        CHECK(first_restart.connected_shares == 1);
        CHECK(first_restart.orphan_shares == 1);

        CHECK(
            restored_after_rewrite.find(
                rewrite_root_id) == nullptr);
        CHECK(
            restored_after_rewrite.find(
                rewrite_child_id) == nullptr);
        CHECK(
            restored_after_rewrite.find(
                survivor_root_id) != nullptr);
        CHECK(
            restored_after_rewrite.is_orphan(
                surviving_orphan_id));

        CHECK(restored_after_rewrite.best_tip() != nullptr);
        CHECK(
            restored_after_rewrite.best_tip()->id ==
            survivor_root_id);

        // Second restart proves the removed subtree cannot resurrect on a later
        // replay either.
        ShareChain second_restart_chain(testnet_params);
        const ShareStoreLoadResult second_restart =
            store.load_into(second_restart_chain);

        CHECK(second_restart.records_loaded == 2);
        CHECK(second_restart.connected_shares == 1);
        CHECK(second_restart.orphan_shares == 1);

        CHECK(
            second_restart_chain.find(
                rewrite_root_id) == nullptr);
        CHECK(
            second_restart_chain.find(
                rewrite_child_id) == nullptr);
        CHECK(
            second_restart_chain.find(
                survivor_root_id) != nullptr);
        CHECK(
            second_restart_chain.is_orphan(
                surviving_orphan_id));
    }
    remove_store(rewrite_path);


    // Exact-ID durable erase regression. Only explicitly named records are
    // removed from the current durable log. Unrelated connected history,
    // unresolved orphan evidence, and records appended on either side of the
    // erase operation must survive.
    const std::filesystem::path erase_path =
        temporary_store_path("erase-records");
    remove_store(erase_path);
    {
        const Share keep_root =
            make_store_share({}, 0, 10, 0x81);

        const Share stale_root =
            make_store_share({}, 0, 11, 0x82);

        const Share stale_child =
            make_store_share(
                share_id(stale_root),
                1,
                12,
                0x83);

        ShareId missing_parent{};
        missing_parent[0] = 0xed;

        const Share surviving_orphan =
            make_store_share(
                missing_parent,
                1,
                13,
                0x84);

        const Share appended_before_erase =
            make_store_share(
                share_id(keep_root),
                1,
                14,
                0x85);

        const Share appended_after_erase =
            make_store_share(
                share_id(appended_before_erase),
                2,
                15,
                0x86);

        const ShareId keep_root_id =
            share_id(keep_root);

        const ShareId stale_root_id =
            share_id(stale_root);

        const ShareId stale_child_id =
            share_id(stale_child);

        const ShareId surviving_orphan_id =
            share_id(surviving_orphan);

        const ShareId appended_before_id =
            share_id(appended_before_erase);

        const ShareId appended_after_id =
            share_id(appended_after_erase);

        ShareStore store(erase_path, testnet_id);

        store.append(keep_root);
        store.append(stale_root);
        store.append(stale_child);
        store.append(surviving_orphan);
        store.append(appended_before_erase);

        const std::uintmax_t before_erase_size =
            std::filesystem::file_size(erase_path);

        const std::vector<ShareId> stale_ids{
            stale_root_id,
            stale_child_id,
        };

        CHECK(store.erase_records(stale_ids) == 2);

        const std::uintmax_t after_erase_size =
            std::filesystem::file_size(erase_path);

        CHECK(after_erase_size < before_erase_size);

        ShareChain after_erase(testnet_params);
        const ShareStoreLoadResult erased_load =
            store.load_into(after_erase);

        CHECK(erased_load.records_loaded == 3);
        CHECK(erased_load.connected_shares == 2);
        CHECK(erased_load.orphan_shares == 1);

        CHECK(after_erase.find(keep_root_id) != nullptr);
        CHECK(after_erase.find(appended_before_id) != nullptr);

        CHECK(after_erase.find(stale_root_id) == nullptr);
        CHECK(after_erase.find(stale_child_id) == nullptr);

        CHECK(
            after_erase.is_orphan(
                surviving_orphan_id));

        // Repeating the exact erase is idempotent. Because nothing is removed,
        // the durable file is not rewritten.
        const std::uintmax_t before_noop_size =
            std::filesystem::file_size(erase_path);

        CHECK(store.erase_records(stale_ids) == 0);

        CHECK(
            std::filesystem::file_size(erase_path) ==
            before_noop_size);

        // A later normal append must extend the filtered durable history.
        store.append(appended_after_erase);

        ShareChain final_chain(testnet_params);
        const ShareStoreLoadResult final_load =
            store.load_into(final_chain);

        CHECK(final_load.records_loaded == 4);
        CHECK(final_load.connected_shares == 3);
        CHECK(final_load.orphan_shares == 1);

        CHECK(final_chain.find(keep_root_id) != nullptr);
        CHECK(final_chain.find(appended_before_id) != nullptr);
        CHECK(final_chain.find(appended_after_id) != nullptr);

        CHECK(final_chain.find(stale_root_id) == nullptr);
        CHECK(final_chain.find(stale_child_id) == nullptr);

        CHECK(
            final_chain.is_orphan(
                surviving_orphan_id));

        // Empty input is also a no-op.
        const std::vector<ShareId> no_ids;
        CHECK(store.erase_records(no_ids) == 0);
    }
    remove_store(erase_path);

    return 0;
}
