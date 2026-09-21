#define main p2p_miner_tx_proofs_fixture_main
#include "p2p_miner_tx_proofs_test.cpp"
#undef main

#include "zano_p2pool/progpowz.hpp"
#include "zano_p2pool/rpc_client.hpp"
#include "zano_p2pool/p2p_node.hpp"
#include "zano_p2pool/restart_recovery.hpp"
#include "zano_p2pool/sidechain_params.hpp"
#include "test_check.hpp"

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
    CHECK(lookup_calls == 8);

    CHECK(chain.find(root_id)->validated_ancestry);
    CHECK(chain.find(child_id)->validated_ancestry);
    CHECK(!chain.find(missing_work_id)->validated_ancestry);

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
    CHECK(lookup_calls == 2);

    // A stable canonical-parent mismatch in this recovery snapshot is not
    // merely an unvalidated replay record. Keeping that rejected branch
    // connected leaves its descendants eligible to remain the structural best
    // tip indefinitely, while a fresh node can never validate the same ancestry.
    // Restart recovery must
    // therefore remove the rejected share and its descendant subtree from the
    // active in-memory chain. Durable ShareStore evidence remains untouched.
    CHECK(bad_chain.find(root_id) == nullptr);
    CHECK(bad_chain.find(child_id) == nullptr);
    CHECK(bad_chain.connected_size() == 0);
    CHECK(bad_chain.best_tip() == nullptr);

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
