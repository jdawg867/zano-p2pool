#define main p2p_miner_tx_proofs_fixture_main
#include "p2p_miner_tx_proofs_test.cpp"
#undef main

#include "zano_p2pool/progpowz.hpp"
#include "zano_p2pool/rpc_client.hpp"
#include "zano_p2pool/p2p_node.hpp"
#include "zano_p2pool/restart_recovery.hpp"
#include "zano_p2pool/share_store.hpp"
#include "zano_p2pool/sidechain_params.hpp"
#include "test_check.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace zano_p2pool;

struct Temporary {
    std::filesystem::path path;

    Temporary() {
        std::string pattern =
            (std::filesystem::temp_directory_path() /
             "zano-restart-recovery-XXXXXX").string();
        if (mkdtemp(pattern.data()) == nullptr) {
            throw std::runtime_error("mkdtemp failed");
        }
        path = pattern;
    }

    ~Temporary() {
        std::filesystem::remove_all(path);
    }
};

Share make_recovery_share(
    const P2pMiningContextProposal& proposal,
    const ShareId& parent_id,
    std::uint64_t share_height,
    std::uint64_t timestamp,
    const PayoutPublicKeys& payout) {
    Share share;
    share.version = kShareVersion2;
    share.parent_id = parent_id;
    share.share_height = share_height;
    share.timestamp = timestamp;
    share.zano_height = proposal.zano_height;
    share.mining_header_hash =
        validate_p2p_mining_context_structure(proposal);
    share.nonce = 9;
    share.share_difficulty = difficulty128_from_decimal("1");
    share.network_difficulty = proposal.network_difficulty;
    share.payout = payout;
    share.miner_id = miner_id_from_payout(payout);
    return share;
}

}  // namespace

int main() {
    {
        zano_p2pool::Hash256 canonical_parent{};
        canonical_parent.front() = 0x91;

        std::size_t stable_calls = 0;
        const zano_p2pool::Hash256 observed =
            zano_p2pool::stable_canonical_parent_for_work_height(
                500,
                [&](std::uint64_t height) {
                    CHECK(height == 499);
                    ++stable_calls;
                    return zano_p2pool::RpcCanonicalHeader{
                        height,
                        canonical_parent,
                    };
                });

        CHECK(stable_calls == 2);
        CHECK(observed == canonical_parent);

        zano_p2pool::Hash256 changed_parent =
            canonical_parent;
        changed_parent.front() ^= 0x01U;

        std::size_t unstable_calls = 0;
        bool unstable_threw = false;
        try {
            static_cast<void>(
                zano_p2pool::stable_canonical_parent_for_work_height(
                    500,
                    [&](std::uint64_t height) {
                        CHECK(height == 499);
                        ++unstable_calls;
                        return zano_p2pool::RpcCanonicalHeader{
                            height,
                            unstable_calls == 1
                                ? canonical_parent
                                : changed_parent,
                        };
                    }));
        } catch (const std::runtime_error&) {
            unstable_threw = true;
        }

        CHECK(unstable_calls == 2);
        CHECK(unstable_threw);

        // Mining height zero has no previous canonical block from which a
        // parent can be established, and must fail before invoking the RPC
        // lookup callback.
        std::size_t zero_height_calls = 0;
        bool zero_height_threw = false;
        try {
            static_cast<void>(
                zano_p2pool::stable_canonical_parent_for_work_height(
                    0,
                    [&](std::uint64_t height) {
                        ++zero_height_calls;
                        return zano_p2pool::RpcCanonicalHeader{
                            height,
                            canonical_parent,
                        };
                    }));
        } catch (const std::invalid_argument&) {
            zero_height_threw = true;
        }

        CHECK(zero_height_threw);
        CHECK(zero_height_calls == 0);

        // A daemon response for any height other than the exact requested
        // canonical parent height cannot authorize historical work.
        std::size_t wrong_height_calls = 0;
        bool wrong_height_threw = false;
        try {
            static_cast<void>(
                zano_p2pool::stable_canonical_parent_for_work_height(
                    500,
                    [&](std::uint64_t height) {
                        ++wrong_height_calls;
                        return zano_p2pool::RpcCanonicalHeader{
                            height + 1,
                            canonical_parent,
                        };
                    }));
        } catch (const std::runtime_error&) {
            wrong_height_threw = true;
        }

        CHECK(wrong_height_threw);
        CHECK(wrong_height_calls == 2);
    }

    using namespace zano_p2pool;

    const ZanoCurveKey scalar_one = key_from_hex(kScalarOneHex);
    const ZanoCurveKey basepoint = key_from_hex(kEd25519BasepointHex);
    const ZanoCurveKey native_asset =
        key_from_hex(kNativeCoinAssetId1Div8Hex);
    const PayoutPublicKeys payout_keys{basepoint, basepoint};

    std::vector<ZanoCurveKey> stealths(2, basepoint);
    if (zano_curve_backend_available()) {
        CHECK(zano_derive_output_public_key(
            scalar_one,
            payout_keys.spend_public_key,
            payout_keys.view_public_key,
            0,
            stealths[0]));
        CHECK(zano_derive_output_public_key(
            scalar_one,
            payout_keys.spend_public_key,
            payout_keys.view_public_key,
            1,
            stealths[1]));
    }

    const auto prefix = make_prefix(basepoint, stealths, native_asset);
    const auto balance_proof = make_balance_proof(prefix, basepoint);
    const auto range_proof = make_valid_range_proof(prefix);

    P2pMiningContextProposal proposal =
        make_proposal(prefix, balance_proof, range_proof);
    proposal.seed = progpowz_seed(proposal.zano_height);

    auto params =
        canonical_sidechain_parameters(SidechainParentNetwork::Testnet);
    params.minimum_share_difficulty = 1;

    Temporary temporary;
    MiningWorkArchive archive(
        temporary.path / "work",
        sidechain_id(params));
    const P2pMiningContextId proposal_id = archive.put(
        serialize_p2p_mining_context_payload(proposal));
    CHECK(archive.list_ids() ==
          std::vector<Hash256>{proposal_id});

    ShareId zero_parent{};
    const Share root = make_recovery_share(
        proposal, zero_parent, 0, 100, payout_keys);
    const ShareId root_id = share_id(root);
    const Share child = make_recovery_share(
        proposal, root_id, 1, 101, payout_keys);
    const ShareId child_id = share_id(child);

    Share missing_work = make_recovery_share(
        proposal, child_id, 2, 102, payout_keys);
    missing_work.mining_header_hash[0] ^= 0x01U;
    const ShareId missing_work_id = share_id(missing_work);

    // Replay deliberately arrives child-before-parent. Once the parent record
    // arrives ShareChain promotes the child, but both connected records still
    // have validated_ancestry=false. Recovery must use share_height, not map or
    // arrival order, to cross root before child.
    ShareChain chain(params);
    CHECK(chain.add_share_unchecked(child).disposition ==
          ShareDisposition::Orphan);
    const AddShareResult root_added =
        chain.add_share_unchecked(root);
    CHECK(root_added.disposition == ShareDisposition::Connected);
    CHECK(root_added.promoted_orphans == 1);
    CHECK(chain.add_share_unchecked(missing_work).disposition ==
          ShareDisposition::Connected);
    CHECK(chain.connected_size() == 3);
    CHECK(!chain.find(root_id)->validated_ancestry);
    CHECK(!chain.find(child_id)->validated_ancestry);
    CHECK(!chain.find(missing_work_id)->validated_ancestry);

    if (!progpowz_available()) {
        int lookups = 0;
        const auto unavailable = recover_replayed_history(
            chain,
            params,
            archive,
            [&](std::uint64_t height) {
                ++lookups;
                return RpcCanonicalHeader{height, proposal.prev_hash};
            },
            200);
        CHECK(unavailable.archive_records == 1);
        CHECK(unavailable.connected_considered == 3);
        CHECK(unavailable.revalidated == 0);
        CHECK(unavailable.rejected == 1);
        CHECK(unavailable.parent_unvalidated == 1);
        CHECK(unavailable.missing_local_work == 1);
        return 0;
    }

    int lookup_calls = 0;
    const RestartRecoveryResult recovered =
        recover_replayed_history(
            chain,
            params,
            archive,
            [&](std::uint64_t height) {
                ++lookup_calls;
                CHECK(height == proposal.zano_height - 1);
                return RpcCanonicalHeader{height, proposal.prev_hash};
            },
            200,
            ProgPowZContextMode::Light);

    CHECK(recovered.archive_records == 1);
    CHECK(recovered.connected_considered == 3);
    CHECK(recovered.revalidated == 2);
    CHECK(recovered.already_validated == 0);
    CHECK(recovered.missing_local_work == 1);
    CHECK(recovered.parent_unvalidated == 0);
    CHECK(recovered.rejected == 0);
    CHECK(recovered.pruned_connected_shares == 0);
    CHECK(lookup_calls == 8);

    CHECK(chain.find(root_id)->validated_ancestry);
    CHECK(chain.find(child_id)->validated_ancestry);
    CHECK(!chain.find(missing_work_id)->validated_ancestry);

    // Persist the exact validation state established above only after a fresh
    // template parent re-crosses stable LOCAL canonical authority.
    ReplayValidationStore validation_store(
        temporary.path / "shares.dat.validation",
        sidechain_id(params));

    const std::uint64_t cache_work_height =
        proposal.zano_height + 10;

    Hash256 cache_parent_hash{};
    cache_parent_hash[0] = 0xc1;
    cache_parent_hash[31] = 0x7c;

    std::size_t persist_lookup_calls = 0;

    const RestartReplayValidationPersistResult persisted =
        persist_replayed_validation_cache(
            chain,
            params,
            validation_store,
            cache_work_height,
            cache_parent_hash,
            [&](std::uint64_t height) {
                ++persist_lookup_calls;
                CHECK(height == cache_work_height - 1);
                return RpcCanonicalHeader{
                    height,
                    cache_parent_hash,
                };
            });

    CHECK(
        persisted.status ==
        RestartReplayValidationPersistStatus::Saved);
    CHECK(persisted.checkpoint.has_value());
    CHECK(
        persisted.checkpoint->zano_height ==
        cache_work_height - 1);
    CHECK(
        persisted.checkpoint->block_hash ==
        cache_parent_hash);
    CHECK(persisted.records_exported == 2);
    CHECK(persisted.records_saved == 2);
    CHECK(persist_lookup_calls == 2);

    const ReplayValidationCheckpoint cache_checkpoint =
        *persisted.checkpoint;

    const auto saved_snapshot =
        validation_store.load();

    CHECK(saved_snapshot.has_value());
    CHECK(
        saved_snapshot->checkpoint.zano_height ==
        cache_checkpoint.zano_height);
    CHECK(
        saved_snapshot->checkpoint.block_hash ==
        cache_checkpoint.block_hash);
    CHECK(saved_snapshot->records.size() == 2);

    // A stable canonical mismatch is not a write authority. Preserve the
    // previously valid sidecar byte-for-byte logically.
    Hash256 replacement_parent =
        cache_parent_hash;
    replacement_parent[0] ^= 0xffU;

    std::size_t changed_persist_calls = 0;

    const RestartReplayValidationPersistResult
        canonical_changed =
            persist_replayed_validation_cache(
                chain,
                params,
                validation_store,
                cache_work_height,
                cache_parent_hash,
                [&](std::uint64_t height) {
                    ++changed_persist_calls;
                    return RpcCanonicalHeader{
                        height,
                        replacement_parent,
                    };
                });

    CHECK(
        canonical_changed.status ==
        RestartReplayValidationPersistStatus::
            CanonicalChanged);
    CHECK(canonical_changed.checkpoint.has_value());
    CHECK(canonical_changed.records_exported == 0);
    CHECK(canonical_changed.records_saved == 0);
    CHECK(changed_persist_calls == 2);

    const auto after_changed =
        validation_store.load();

    CHECK(after_changed.has_value());
    CHECK(
        after_changed->checkpoint.zano_height ==
        cache_checkpoint.zano_height);
    CHECK(
        after_changed->checkpoint.block_hash ==
        cache_checkpoint.block_hash);
    CHECK(after_changed->records.size() == 2);

    // Even stable canonical evidence is insufficient when its height does not
    // reach all validated share work. The shared proof fixture intentionally
    // uses Zano height 1, so build an independent later-height validated root
    // rather than trying to manufacture a nonzero checkpoint below height 1.
    Share horizon_root = root;
    horizon_root.zano_height =
        cache_work_height + 1;

    const ShareId horizon_root_id =
        share_id(horizon_root);

    ShareChain horizon_chain(params);

    CHECK(
        horizon_chain.add_share_unchecked(
            horizon_root).disposition ==
        ShareDisposition::Connected);

    CHECK(chain.find(root_id) != nullptr);
    CHECK(
        chain.find(root_id)->
            pow_validation.has_value());

    const std::vector<ReplayValidationRecord>
        horizon_records{
            ReplayValidationRecord{
                horizon_root_id,
                *chain.find(root_id)->pow_validation,
            },
        };

    CHECK(
        horizon_chain.restore_replay_validation(
            horizon_records) == 1);

    CHECK(
        horizon_chain.find(horizon_root_id) != nullptr);
    CHECK(
        horizon_chain.find(horizon_root_id)->
            validated_ancestry);

    const std::uint64_t old_work_height =
        cache_work_height;

    CHECK(
        horizon_root.zano_height >
        old_work_height);

    Hash256 old_parent{};
    old_parent[0] = 0x33;
    old_parent[31] = 0x44;

    std::size_t old_checkpoint_calls = 0;

    const RestartReplayValidationPersistResult
        checkpoint_too_old =
            persist_replayed_validation_cache(
                horizon_chain,
                params,
                validation_store,
                old_work_height,
                old_parent,
                [&](std::uint64_t height) {
                    ++old_checkpoint_calls;
                    CHECK(height == old_work_height - 1);
                    return RpcCanonicalHeader{
                        height,
                        old_parent,
                    };
                });

    CHECK(
        checkpoint_too_old.status ==
        RestartReplayValidationPersistStatus::
            CheckpointTooOld);
    CHECK(checkpoint_too_old.checkpoint.has_value());
    CHECK(checkpoint_too_old.records_exported == 1);
    CHECK(checkpoint_too_old.records_saved == 0);
    CHECK(old_checkpoint_calls == 2);

    const auto after_old_checkpoint =
        validation_store.load();

    CHECK(after_old_checkpoint.has_value());
    CHECK(
        after_old_checkpoint->checkpoint.zano_height ==
        cache_checkpoint.zano_height);
    CHECK(
        after_old_checkpoint->checkpoint.block_hash ==
        cache_checkpoint.block_hash);
    CHECK(after_old_checkpoint->records.size() == 2);

    ShareChain cached_chain(params);
    CHECK(cached_chain.add_share_unchecked(root).disposition ==
          ShareDisposition::Connected);
    CHECK(cached_chain.add_share_unchecked(child).disposition ==
          ShareDisposition::Connected);
    CHECK(cached_chain.add_share_unchecked(missing_work).disposition ==
          ShareDisposition::Connected);

    CHECK(!cached_chain.find(root_id)->validated_ancestry);
    CHECK(!cached_chain.find(child_id)->validated_ancestry);
    CHECK(!cached_chain.find(missing_work_id)->validated_ancestry);

    std::size_t cache_lookup_calls = 0;
    const RestartReplayValidationResult cached_restore =
        restore_replayed_validation_cache(
            cached_chain,
            params,
            validation_store,
            [&](std::uint64_t height) {
                ++cache_lookup_calls;
                CHECK(height == cache_checkpoint.zano_height);
                return RpcCanonicalHeader{
                    height,
                    cache_checkpoint.block_hash,
                };
            });

    CHECK(
        cached_restore.status ==
        RestartReplayValidationStatus::Restored);
    CHECK(cached_restore.checkpoint.has_value());
    CHECK(
        cached_restore.checkpoint->zano_height ==
        cache_checkpoint.zano_height);
    CHECK(
        cached_restore.checkpoint->block_hash ==
        cache_checkpoint.block_hash);
    CHECK(cached_restore.records_loaded == 2);
    CHECK(cached_restore.records_restored == 2);
    CHECK(cache_lookup_calls == 2);

    CHECK(cached_chain.find(root_id)->validated_ancestry);
    CHECK(cached_chain.find(child_id)->validated_ancestry);
    CHECK(!cached_chain.find(missing_work_id)->validated_ancestry);
    CHECK(
        cached_chain.find(root_id)->
            pow_validation.has_value());
    CHECK(
        cached_chain.find(child_id)->
            pow_validation.has_value());

    // Existing restart recovery must now recognize the restored prefix and
    // avoid repeating canonical/ProgPoWZ work for it.
    std::size_t cached_recovery_lookup_calls = 0;
    const RestartRecoveryResult cached_recovery =
        recover_replayed_history(
            cached_chain,
            params,
            archive,
            [&](std::uint64_t height) {
                ++cached_recovery_lookup_calls;
                return RpcCanonicalHeader{
                    height,
                    proposal.prev_hash,
                };
            },
            200,
            ProgPowZContextMode::Light);

    CHECK(cached_recovery.connected_considered == 3);
    CHECK(cached_recovery.revalidated == 0);
    CHECK(cached_recovery.already_validated == 2);
    CHECK(cached_recovery.missing_local_work == 1);
    CHECK(cached_recovery.parent_unvalidated == 0);
    CHECK(cached_recovery.rejected == 0);
    CHECK(cached_recovery.pruned_connected_shares == 0);
    CHECK(cached_recovery_lookup_calls == 0);

    // A stable canonical replacement makes the whole cache stale. No record
    // may be restored from the superseded checkpoint.
    ShareChain stale_cache_chain(params);
    CHECK(stale_cache_chain.add_share_unchecked(root).disposition ==
          ShareDisposition::Connected);
    CHECK(stale_cache_chain.add_share_unchecked(child).disposition ==
          ShareDisposition::Connected);
    CHECK(stale_cache_chain.add_share_unchecked(missing_work).disposition ==
          ShareDisposition::Connected);

    Hash256 stale_hash = cache_checkpoint.block_hash;
    stale_hash[0] ^= 0xffU;

    std::size_t stale_lookup_calls = 0;
    const RestartReplayValidationResult stale_restore =
        restore_replayed_validation_cache(
            stale_cache_chain,
            params,
            validation_store,
            [&](std::uint64_t height) {
                ++stale_lookup_calls;
                return RpcCanonicalHeader{
                    height,
                    stale_hash,
                };
            });

    CHECK(
        stale_restore.status ==
        RestartReplayValidationStatus::Stale);
    CHECK(stale_restore.records_loaded == 2);
    CHECK(stale_restore.records_restored == 0);
    CHECK(stale_lookup_calls == 2);
    CHECK(!stale_cache_chain.find(root_id)->validated_ancestry);
    CHECK(!stale_cache_chain.find(child_id)->validated_ancestry);

    // Canonical evidence changing during verification is ambiguous. Throw
    // before ShareChain mutation so the caller can fall back to full recovery.
    ShareChain unstable_cache_chain(params);
    CHECK(unstable_cache_chain.add_share_unchecked(root).disposition ==
          ShareDisposition::Connected);
    CHECK(unstable_cache_chain.add_share_unchecked(child).disposition ==
          ShareDisposition::Connected);

    Hash256 changed_checkpoint_hash =
        cache_checkpoint.block_hash;
    changed_checkpoint_hash[1] ^= 0x55U;

    std::size_t unstable_cache_calls = 0;
    bool unstable_cache_threw = false;

    try {
        static_cast<void>(
            restore_replayed_validation_cache(
                unstable_cache_chain,
                params,
                validation_store,
                [&](std::uint64_t height) {
                    ++unstable_cache_calls;
                    return RpcCanonicalHeader{
                        height,
                        unstable_cache_calls == 1
                            ? cache_checkpoint.block_hash
                            : changed_checkpoint_hash,
                    };
                }));
    } catch (const std::runtime_error&) {
        unstable_cache_threw = true;
    }

    CHECK(unstable_cache_threw);
    CHECK(unstable_cache_calls == 2);
    CHECK(
        !unstable_cache_chain.find(root_id)->
            validated_ancestry);
    CHECK(
        !unstable_cache_chain.find(child_id)->
            validated_ancestry);

    // Missing sidecar performs no RPC work and no mutation.
    ReplayValidationStore missing_validation_store(
        temporary.path / "missing.validation",
        sidechain_id(params));

    ShareChain missing_cache_chain(params);
    CHECK(missing_cache_chain.add_share_unchecked(root).disposition ==
          ShareDisposition::Connected);

    std::size_t missing_cache_calls = 0;
    const RestartReplayValidationResult missing_cache =
        restore_replayed_validation_cache(
            missing_cache_chain,
            params,
            missing_validation_store,
            [&](std::uint64_t height) {
                ++missing_cache_calls;
                return RpcCanonicalHeader{
                    height,
                    cache_checkpoint.block_hash,
                };
            });

    CHECK(
        missing_cache.status ==
        RestartReplayValidationStatus::Missing);
    CHECK(!missing_cache.checkpoint.has_value());
    CHECK(missing_cache.records_loaded == 0);
    CHECK(missing_cache.records_restored == 0);
    CHECK(missing_cache_calls == 0);
    CHECK(
        !missing_cache_chain.find(root_id)->
            validated_ancestry);

    lookup_calls = 0;
    const RestartRecoveryResult second =
        recover_replayed_history(
            chain,
            params,
            archive,
            [&](std::uint64_t height) {
                ++lookup_calls;
                return RpcCanonicalHeader{height, proposal.prev_hash};
            },
            200);
    CHECK(second.revalidated == 0);
    CHECK(second.already_validated == 2);
    CHECK(second.missing_local_work == 1);
    CHECK(second.rejected == 0);
    CHECK(second.pruned_connected_shares == 0);
    CHECK(lookup_calls == 0);

    // Live-running parent-replacement regression. This chain has already
    // crossed ancestry validation while the first Zano parent is current.
    // Replacing that parent at the same mining height must remove ancestry
    // authorized by the displaced anchor and revoke the old trusted-work
    // authorization so the same share cannot simply be admitted again.
    P2pTrustedWorkRegistry live_work;
    const ShareWorkContext live_context{
        proposal.zano_height,
        validate_p2p_mining_context_structure(proposal),
        proposal.network_difficulty,
    };
    live_work.remember(
        live_context,
        zero_parent,
        proposal.prev_hash);

    std::mutex live_mutex;
    P2pNodeProtocol live_protocol(
        chain,
        live_work,
        live_mutex);

    const P2pMiningAnchor original_live_anchor =
        anchor_for(proposal);
    live_protocol.set_local_mining_context(
        original_live_anchor,
        proposal);

    // Model a genuine replacement daemon template: the proposal metadata and
    // the previous-block field embedded in the block blob change together.
    P2pMiningContextProposal replacement_proposal = proposal;
    replacement_proposal.prev_hash[0] ^= 0x01U;

    constexpr std::size_t kFixturePrevHashOffset = 1 + 8;
    CHECK(replacement_proposal.block_template_blob.size() >
          kFixturePrevHashOffset);
    replacement_proposal
        .block_template_blob[kFixturePrevHashOffset] ^= 0x01U;

    const P2pMiningAnchor replacement_live_anchor =
        anchor_for(replacement_proposal);

    CHECK(validate_p2p_mining_context_structure(
              replacement_proposal) !=
          live_context.mining_header_hash);

    live_protocol.set_local_mining_context(
        replacement_live_anchor,
        replacement_proposal);

    CHECK(chain.find(root_id) == nullptr);
    CHECK(chain.find(child_id) == nullptr);
    CHECK(chain.find(missing_work_id) == nullptr);
    CHECK(chain.connected_size() == 0);
    CHECK(chain.best_tip() == nullptr);
    CHECK(live_work.find(
              live_context.zano_height,
              live_context.mining_header_hash,
              zero_parent) == nullptr);

    ShareChain bad_chain(params);
    CHECK(bad_chain.add_share_unchecked(root).disposition ==
          ShareDisposition::Connected);
    CHECK(bad_chain.add_share_unchecked(child).disposition ==
          ShareDisposition::Connected);

    Hash256 wrong_parent = proposal.prev_hash;
    wrong_parent[0] ^= 0x01U;
    lookup_calls = 0;
    const RestartRecoveryResult failed =
        recover_replayed_history(
            bad_chain,
            params,
            archive,
            [&](std::uint64_t height) {
                ++lookup_calls;
                return RpcCanonicalHeader{height, wrong_parent};
            },
            200);
    CHECK(failed.connected_considered == 2);
    CHECK(failed.revalidated == 0);
    CHECK(failed.rejected == 1);
    CHECK(failed.parent_unvalidated == 1);
    CHECK(failed.missing_local_work == 0);

    // root + child are removed as one stale-parent subtree.
    CHECK(failed.pruned_connected_shares == 2);

    CHECK(lookup_calls == 2);

    // A stable canonical-parent mismatch in this recovery snapshot is not
    // merely an unvalidated replay record. Keeping that rejected branch
    // connected leaves its descendants eligible to remain the structural best
    // tip indefinitely, while a fresh node can never validate the same ancestry.
    // Restart recovery must
    // therefore remove the rejected share and its descendant subtree from the
    // active in-memory chain. recover_replayed_history() itself mutates only
    // memory; the startup caller may compact ShareStore after this result.
    CHECK(bad_chain.find(root_id) == nullptr);
    CHECK(bad_chain.find(child_id) == nullptr);
    CHECK(bad_chain.connected_size() == 0);
    CHECK(bad_chain.best_tip() == nullptr);

    // Durable restart-prune regression. The persisted file still contains the
    // stale branch until startup compaction rewrites it from the already-pruned
    // in-memory snapshot. After that rewrite, neither the first nor a later
    // restart may resurrect the stale subtree.
    const auto durable_stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();

    const std::filesystem::path durable_path =
        std::filesystem::temp_directory_path() /
        ("zano_p2pool_restart_prune_" +
         std::to_string(durable_stamp) +
         ".dat");

    std::filesystem::remove(durable_path);

    {
        ShareStore durable_store(
            durable_path,
            sidechain_id(params));

        durable_store.append(root);
        durable_store.append(child);

        ShareChain before_compaction(params);
        const ShareStoreLoadResult before_load =
            durable_store.load_into(before_compaction);

        CHECK(before_load.records_loaded == 2);
        CHECK(before_load.connected_shares == 2);
        CHECK(before_compaction.find(root_id) != nullptr);
        CHECK(before_compaction.find(child_id) != nullptr);

        const std::vector<Share> surviving =
            bad_chain.persistence_snapshot();

        CHECK(surviving.empty());

        durable_store.rewrite(surviving);

        // First restart: stale durable records are physically gone.
        ShareChain first_restart(params);
        const ShareStoreLoadResult first_load =
            durable_store.load_into(first_restart);

        CHECK(first_load.records_loaded == 0);
        CHECK(first_load.connected_shares == 0);
        CHECK(first_load.orphan_shares == 0);
        CHECK(first_restart.find(root_id) == nullptr);
        CHECK(first_restart.find(child_id) == nullptr);
        CHECK(first_restart.best_tip() == nullptr);

        // Second restart proves they cannot resurrect on later replay.
        ShareChain second_restart(params);
        const ShareStoreLoadResult second_load =
            durable_store.load_into(second_restart);

        CHECK(second_load.records_loaded == 0);
        CHECK(second_load.connected_shares == 0);
        CHECK(second_load.orphan_shares == 0);
        CHECK(second_restart.find(root_id) == nullptr);
        CHECK(second_restart.find(child_id) == nullptr);
        CHECK(second_restart.best_tip() == nullptr);
    }

    std::filesystem::remove(durable_path);

    ShareChain bounded_chain(params);
    CHECK(bounded_chain.add_share_unchecked(root).disposition ==
          ShareDisposition::Connected);
    bool limit_rejected = false;
    try {
        static_cast<void>(recover_replayed_history(
            bounded_chain,
            params,
            archive,
            [&](std::uint64_t height) {
                return RpcCanonicalHeader{height, proposal.prev_hash};
            },
            200,
            ProgPowZContextMode::Light,
            0));
    } catch (const std::exception&) {
        limit_rejected = true;
    }
    CHECK(limit_rejected);
    CHECK(!bounded_chain.find(root_id)->validated_ancestry);

    return 0;
}
