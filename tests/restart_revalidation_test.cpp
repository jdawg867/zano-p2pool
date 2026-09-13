#define main p2p_miner_tx_proofs_fixture_main
#include "p2p_miner_tx_proofs_test.cpp"
#undef main

#include "zano_p2pool/progpowz.hpp"
#include "zano_p2pool/restart_revalidation.hpp"
#include "zano_p2pool/sidechain_params.hpp"

#include <cstdlib>
#include <filesystem>
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
             "zano-restart-revalidation-XXXXXX").string();
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
    using namespace zano_p2pool;

    const ZanoCurveKey scalar_one = key_from_hex(kScalarOneHex);
    const ZanoCurveKey basepoint = key_from_hex(kEd25519BasepointHex);
    const ZanoCurveKey native_asset = key_from_hex(kNativeCoinAssetId1Div8Hex);
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

    const P2pMiningAnchor observation = anchor_for(proposal);
    const std::vector<P2pMiningAnchor> observations{observation};

    auto params =
        canonical_sidechain_parameters(SidechainParentNetwork::Testnet);
    params.minimum_share_difficulty = 1;

    Temporary temporary;
    MiningWorkArchive archive(
        temporary.path / "work",
        sidechain_id(params));
    const auto proposal_payload =
        serialize_p2p_mining_context_payload(proposal);
    const P2pMiningContextId proposal_id =
        archive.put(proposal_payload);
    const std::vector<P2pMiningAnchor> archived_observations =
        load_local_mining_anchors(archive);
    CHECK(archived_observations.size() == 1);
    CHECK(archived_observations == observations);

    ShareId zero_parent{};
    const Share root = make_recovery_share(
        proposal, zero_parent, 0, 100, payout_keys);
    const ShareId root_id = share_id(root);

    const Share child = make_recovery_share(
        proposal, root_id, 1, 101, payout_keys);
    const ShareId child_id = share_id(child);

    // A caller cannot substitute proposal bytes that were never published into
    // the local mining-work archive. Missing content IDs fail before daemon
    // lookup and before any share state mutation.
    ShareChain archive_gate_chain(params);
    CHECK(archive_gate_chain.add_share_unchecked(root).disposition ==
          ShareDisposition::Connected);
    P2pMiningContextId missing_proposal_id = proposal_id;
    missing_proposal_id[0] ^= 0x01U;
    int archive_gate_lookups = 0;
    bool missing_archive_rejected = false;
    try {
        static_cast<void>(revalidate_recovered_share(
            archive_gate_chain,
            params,
            root_id,
            archive,
            missing_proposal_id,
            archived_observations,
            [&](std::uint64_t height) {
                ++archive_gate_lookups;
                return RpcCanonicalHeader{height, proposal.prev_hash};
            },
            200));
    } catch (const std::exception&) {
        missing_archive_rejected = true;
    }
    CHECK(missing_archive_rejected);
    CHECK(archive_gate_lookups == 0);
    CHECK(!archive_gate_chain.find(root_id)->validated_ancestry);

    if (!progpowz_available()) {
        ShareChain unavailable_chain(params);
        CHECK(unavailable_chain.add_share_unchecked(root).disposition ==
              ShareDisposition::Connected);
        const auto unavailable = revalidate_recovered_share(
            unavailable_chain,
            params,
            root_id,
            archive,
            proposal_id,
            archived_observations,
            [&](std::uint64_t height) {
                return RpcCanonicalHeader{height, proposal.prev_hash};
            },
            200);
        CHECK(unavailable.status == RestartRevalidationStatus::ShareRejected);
        CHECK(unavailable.share_result.reject_reason ==
              ShareRejectReason::PowBackendUnavailable);
        return 0;
    }

    Share wrong_root = root;
    wrong_root.mining_header_hash[0] ^= 0x01U;
    ShareChain mismatch_chain(params);
    CHECK(mismatch_chain.add_share_unchecked(wrong_root).disposition ==
          ShareDisposition::Connected);
    int lookup_calls = 0;
    const auto mismatch = revalidate_recovered_share(
        mismatch_chain,
        params,
        share_id(wrong_root),
        archive,
        proposal_id,
        archived_observations,
        [&](std::uint64_t height) {
            ++lookup_calls;
            return RpcCanonicalHeader{height, proposal.prev_hash};
        },
        200);
    CHECK(mismatch.status == RestartRevalidationStatus::CandidateMismatch);
    CHECK(lookup_calls == 0);
    CHECK(!mismatch_chain.find(share_id(wrong_root))->validated_ancestry);

    ShareChain anchor_chain(params);
    CHECK(anchor_chain.add_share_unchecked(root).disposition ==
          ShareDisposition::Connected);
    Hash256 other_parent = proposal.prev_hash;
    other_parent[0] ^= 0x01U;
    const auto bad_anchor = revalidate_recovered_share(
        anchor_chain,
        params,
        root_id,
        archive,
        proposal_id,
        archived_observations,
        [&](std::uint64_t height) {
            return RpcCanonicalHeader{height, other_parent};
        },
        200);
    CHECK(bad_anchor.status == RestartRevalidationStatus::AnchorRejected);
    CHECK(!anchor_chain.find(root_id)->validated_ancestry);

    ShareChain reorg_chain(params);
    CHECK(reorg_chain.add_share_unchecked(root).disposition ==
          ShareDisposition::Connected);
    int reorg_calls = 0;
    const auto reorg = revalidate_recovered_share(
        reorg_chain,
        params,
        root_id,
        archive,
        proposal_id,
        archived_observations,
        [&](std::uint64_t height) {
            ++reorg_calls;
            const Hash256 hash =
                reorg_calls <= 2 ? proposal.prev_hash : other_parent;
            return RpcCanonicalHeader{height, hash};
        },
        200);
    CHECK(reorg.status ==
          RestartRevalidationStatus::AnchorChangedBeforeRevalidation);
    CHECK(reorg_calls == 4);
    CHECK(!reorg_chain.find(root_id)->validated_ancestry);

    ShareChain parent_chain(params);
    CHECK(parent_chain.add_share_unchecked(root).disposition ==
          ShareDisposition::Connected);
    CHECK(parent_chain.add_share_unchecked(child).disposition ==
          ShareDisposition::Connected);

    lookup_calls = 0;
    const auto child_first = revalidate_recovered_share(
        parent_chain,
        params,
        child_id,
        archive,
        proposal_id,
        archived_observations,
        [&](std::uint64_t height) {
            ++lookup_calls;
            return RpcCanonicalHeader{height, proposal.prev_hash};
        },
        200);
    CHECK(child_first.status ==
          RestartRevalidationStatus::ParentUnvalidated);
    CHECK(lookup_calls == 0);
    CHECK(!parent_chain.find(child_id)->validated_ancestry);

    lookup_calls = 0;
    const auto root_revalidated = revalidate_recovered_share(
        parent_chain,
        params,
        root_id,
        archive,
        proposal_id,
        archived_observations,
        [&](std::uint64_t height) {
            ++lookup_calls;
            return RpcCanonicalHeader{height, proposal.prev_hash};
        },
        200);
    CHECK(root_revalidated.status ==
          RestartRevalidationStatus::Revalidated);
    CHECK(lookup_calls == 4);
    CHECK(parent_chain.find(root_id)->validated_ancestry);
    CHECK(parent_chain.find(root_id)->pow_validation.has_value());

    lookup_calls = 0;
    const auto child_revalidated = revalidate_recovered_share(
        parent_chain,
        params,
        child_id,
        archive,
        proposal_id,
        archived_observations,
        [&](std::uint64_t height) {
            ++lookup_calls;
            return RpcCanonicalHeader{height, proposal.prev_hash};
        },
        200);
    CHECK(child_revalidated.status ==
          RestartRevalidationStatus::Revalidated);
    CHECK(lookup_calls == 4);
    CHECK(parent_chain.find(child_id)->validated_ancestry);
    CHECK(parent_chain.find(child_id)->pow_validation.has_value());

    lookup_calls = 0;
    const auto again = revalidate_recovered_share(
        parent_chain,
        params,
        root_id,
        archive,
        proposal_id,
        archived_observations,
        [&](std::uint64_t height) {
            ++lookup_calls;
            return RpcCanonicalHeader{height, proposal.prev_hash};
        },
        200);
    CHECK(again.status == RestartRevalidationStatus::AlreadyValidated);
    CHECK(lookup_calls == 0);

    CHECK(std::string(restart_revalidation_status_name(
              RestartRevalidationStatus::Revalidated)) ==
          "revalidated");
    CHECK(std::string(restart_revalidation_status_name(
              RestartRevalidationStatus::ParentUnvalidated)) ==
          "parent-unvalidated");

    return 0;
}
