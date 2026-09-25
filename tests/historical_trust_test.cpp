#define main p2p_miner_tx_proofs_fixture_main
#include "p2p_miner_tx_proofs_test.cpp"
#undef main

#include "zano_p2pool/historical_trust.hpp"
#include "zano_p2pool/progpowz.hpp"
#include "zano_p2pool/sidechain_params.hpp"

#include <string>
#include <vector>

namespace {

using namespace zano_p2pool;

Share make_parent_share(const PayoutPublicKeys& payout) {
    Share share;
    share.version = kShareVersion2;
    share.share_height = 0;
    share.timestamp = 100;
    share.zano_height = 1;
    share.mining_header_hash.fill(0x44);
    share.nonce = 7;
    share.share_difficulty = difficulty128_from_decimal("1");
    share.network_difficulty = difficulty128_from_decimal("1");
    share.payout = payout;
    share.miner_id = miner_id_from_payout(payout);
    return share;
}

Share make_candidate_share(
    const P2pMiningContextProposal& proposal,
    const ShareId& parent_id,
    const PayoutPublicKeys& payout) {
    Share share;
    share.version = kShareVersion2;
    share.parent_id = parent_id;
    share.share_height = 1;
    share.timestamp = 101;
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

    // Historical anchoring derives the seed independently from height.
    proposal.seed = progpowz_seed(proposal.zano_height);

    const P2pMiningAnchor local_observation = anchor_for(proposal);
    const std::vector<P2pMiningAnchor> observations{local_observation};
    const P2pHandshake peer = make_peer();

    const auto canonical_lookup = [&](std::uint64_t height) {
        CHECK(height == proposal.zano_height - 1);
        return RpcCanonicalHeader{height, proposal.prev_hash};
    };

    auto params =
        canonical_sidechain_parameters(SidechainParentNetwork::Testnet);
    params.minimum_share_difficulty = 1;

    const Share parent = make_parent_share(payout_keys);
    const ShareId parent_id = share_id(parent);
    const Share candidate =
        make_candidate_share(proposal, parent_id, payout_keys);

    // Candidate/proposal mismatches fail before daemon lookup or registry
    // insertion.
    Share wrong_candidate = candidate;
    wrong_candidate.mining_header_hash[0] ^= 0x01U;
    int lookup_calls = 0;
    P2pTrustedWorkRegistry mismatch_registry;
    ShareChain empty_chain(params);
    const auto mismatch = promote_historical_mining_context(
        mismatch_registry,
        empty_chain,
        params,
        wrong_candidate,
        peer,
        proposal,
        observations,
        [&](std::uint64_t height) {
            ++lookup_calls;
            return canonical_lookup(height);
        });
    CHECK(mismatch.status == HistoricalTrustStatus::CandidateMismatch);
    CHECK(lookup_calls == 0);
    CHECK(mismatch_registry.size() == 0);

    // A daemon parent mismatch cannot reach payout reconstruction or trusted
    // work.
    Hash256 other_parent_hash = proposal.prev_hash;
    other_parent_hash[0] ^= 0x01U;
    P2pTrustedWorkRegistry anchor_registry;
    const auto bad_anchor = promote_historical_mining_context(
        anchor_registry,
        empty_chain,
        params,
        candidate,
        peer,
        proposal,
        observations,
        [&](std::uint64_t height) {
            return RpcCanonicalHeader{height, other_parent_hash};
        });
    CHECK(bad_anchor.status == HistoricalTrustStatus::AnchorRejected);
    CHECK(bad_anchor.initial_anchor.status ==
          HistoricalAnchorStatus::ParentMismatch);
    CHECK(anchor_registry.size() == 0);

    // Structurally connected persistence/replay history is not enough for a
    // historical payout plan.
    ShareChain unchecked_chain(params);
    CHECK(unchecked_chain.add_share_unchecked(parent).disposition ==
          ShareDisposition::Connected);
    P2pTrustedWorkRegistry unchecked_registry;
    const auto unchecked = promote_historical_mining_context(
        unchecked_registry,
        unchecked_chain,
        params,
        candidate,
        peer,
        proposal,
        observations,
        canonical_lookup);
    CHECK(unchecked.status == HistoricalTrustStatus::PayoutRejected);
    CHECK(unchecked.payout.status ==
          HistoricalPayoutStatus::UnverifiedAncestry);
    CHECK(unchecked_registry.size() == 0);

    // A zero-parent historical candidate is bootstrap work only when it is the
    // canonical v2 root shape. Malformed root-like shares fail before anchoring
    // or trusted-work insertion.
    Share root_candidate = candidate;
    root_candidate.parent_id = ShareId{};
    root_candidate.share_height = 0;

    Share bad_root_height = root_candidate;
    bad_root_height.share_height = 1;

    int bad_root_lookup_calls = 0;
    P2pTrustedWorkRegistry bad_root_height_registry;

    const auto bad_root_height_result =
        promote_historical_mining_context(
            bad_root_height_registry,
            empty_chain,
            params,
            bad_root_height,
            peer,
            proposal,
            observations,
            [&](std::uint64_t height) {
                ++bad_root_lookup_calls;
                return canonical_lookup(height);
            });

    CHECK(bad_root_height_result.status ==
          HistoricalTrustStatus::CandidateMismatch);
    CHECK(bad_root_lookup_calls == 0);
    CHECK(bad_root_height_registry.size() == 0);

    Share bad_root_v1 = root_candidate;
    bad_root_v1.version = kShareVersion1;
    bad_root_v1.payout.reset();

    P2pTrustedWorkRegistry bad_root_v1_registry;

    const auto bad_root_v1_result =
        promote_historical_mining_context(
            bad_root_v1_registry,
            empty_chain,
            params,
            bad_root_v1,
            peer,
            proposal,
            observations,
            canonical_lookup);

    CHECK(bad_root_v1_result.status ==
          HistoricalTrustStatus::CandidateMismatch);
    CHECK(bad_root_v1_registry.size() == 0);

    Share bad_root_binding = root_candidate;
    CHECK(bad_root_binding.payout.has_value());
    bad_root_binding.miner_id[0] ^= 0x01U;

    P2pTrustedWorkRegistry bad_root_binding_registry;

    const auto bad_root_binding_result =
        promote_historical_mining_context(
            bad_root_binding_registry,
            empty_chain,
            params,
            bad_root_binding,
            peer,
            proposal,
            observations,
            canonical_lookup);

    CHECK(bad_root_binding_result.status ==
          HistoricalTrustStatus::CandidateMismatch);
    CHECK(bad_root_binding_registry.size() == 0);

    // The success/reorg tests need real share PoW validation.
    if (!progpowz_available()) {
        return 0;
    }

    // A genuine historical root has no earlier sidechain PPLNS plan. Its exact
    // work must still pass independent local anchoring plus the complete
    // bootstrap miner-tx accounting/balance/range proof crossing.
    P2pTrustedWorkRegistry root_registry;

    const auto trusted_root =
        promote_historical_mining_context(
            root_registry,
            empty_chain,
            params,
            root_candidate,
            peer,
            proposal,
            observations,
            canonical_lookup);

    if (!zano_curve_backend_available()) {
        CHECK(trusted_root.status ==
              HistoricalTrustStatus::PromotionRejected);
        CHECK(trusted_root.promotion.status ==
              P2pMiningContextTrustStatus::ProofsRejected);
        CHECK(trusted_root.promotion.proof_status ==
              P2pMinerTxProofStatus::BackendUnavailable);
        CHECK(root_registry.size() == 0);
    } else {
        CHECK(trusted_root.status ==
              HistoricalTrustStatus::Trusted);
        CHECK(trusted_root.payout.status ==
              HistoricalPayoutStatus::BootstrapRoot);
        CHECK(is_zero_share_id(trusted_root.payout.parent_id));
        CHECK(std::string(historical_payout_status_name(
                  trusted_root.payout.status)) == "bootstrap-root");
        CHECK(trusted_root.initial_anchor.status ==
              HistoricalAnchorStatus::AnchorMatchedUntrusted);
        CHECK(trusted_root.final_anchor.status ==
              HistoricalAnchorStatus::AnchorMatchedUntrusted);
        CHECK(trusted_root.promotion.status ==
              P2pMiningContextTrustStatus::Trusted);
        CHECK(trusted_root.promotion.proof_status ==
              P2pMinerTxProofStatus::ProofsVerified);
        CHECK(trusted_root.promotion.payout_status ==
              P2pPayoutPolicyStatus::Verified);
        CHECK(trusted_root.promotion.registry_inserted);
        CHECK(root_registry.size() == 1);

        CHECK(root_registry.find(
                  root_candidate.zano_height,
                  root_candidate.mining_header_hash,
                  ShareId{}) != nullptr);

        ShareId wrong_root_parent{};
        wrong_root_parent.back() = 0x01U;

        CHECK(root_registry.find(
                  root_candidate.zano_height,
                  root_candidate.mining_header_hash,
                  wrong_root_parent) == nullptr);

        // The validated root work must be sufficient for ordinary share
        // admission, which then establishes the first validated payout-history
        // entry used by subsequent PPLNS templates.
        ShareChain root_chain(params);
        P2pShareReceiver root_receiver(
            root_chain,
            root_registry);

        const auto admitted_root =
            root_receiver.receive_share(
                peer,
                root_candidate,
                kP2pCapabilityShareGossip,
                200,
                ProgPowZContextMode::Light);

        CHECK(admitted_root.status ==
              P2pShareReceiveStatus::Connected);
        CHECK(admitted_root.chain_result.disposition ==
              ShareDisposition::Connected);
        CHECK(root_chain.contains(
              share_id(root_candidate)));
        CHECK(root_chain.find(
              share_id(root_candidate))->validated_ancestry);
    }

    ShareChain chain(params);
    const ShareWorkContext parent_context{
        parent.zano_height,
        parent.mining_header_hash,
        parent.network_difficulty,
    };
    CHECK(chain.submit_share(
              parent,
              parent_context,
              200,
              ProgPowZContextMode::Light).disposition ==
          ShareDisposition::Connected);
    CHECK(chain.find(parent_id) != nullptr);
    CHECK(chain.find(parent_id)->validated_ancestry);

    // The daemon is sampled twice by the first anchor audit and twice again
    // immediately before promotion. A reorg between those phases must leave
    // the registry untouched.
    int reorg_calls = 0;
    P2pTrustedWorkRegistry reorg_registry;
    const auto reorg = promote_historical_mining_context(
        reorg_registry,
        chain,
        params,
        candidate,
        peer,
        proposal,
        observations,
        [&](std::uint64_t height) {
            ++reorg_calls;
            return RpcCanonicalHeader{
                height,
                reorg_calls <= 2
                    ? proposal.prev_hash
                    : other_parent_hash};
        });
    CHECK(reorg_calls == 4);
    CHECK(reorg.status ==
          HistoricalTrustStatus::AnchorChangedBeforePromotion);
    CHECK(reorg.initial_anchor.status ==
          HistoricalAnchorStatus::AnchorMatchedUntrusted);
    CHECK(reorg.payout.status == HistoricalPayoutStatus::PlanDerived);
    CHECK(reorg.final_anchor.status ==
          HistoricalAnchorStatus::ParentMismatch);
    CHECK(reorg_registry.size() == 0);

    P2pTrustedWorkRegistry trusted_work;
    const auto trusted = promote_historical_mining_context(
        trusted_work,
        chain,
        params,
        candidate,
        peer,
        proposal,
        observations,
        canonical_lookup);

    // Lightweight builds can prove the fail-closed orchestration but cannot
    // cross the cryptographic miner-tx boundary.
    if (!zano_curve_backend_available()) {
        CHECK(trusted.status == HistoricalTrustStatus::PromotionRejected);
        CHECK(trusted.promotion.status ==
              P2pMiningContextTrustStatus::ProofsRejected);
        CHECK(trusted.promotion.proof_status ==
              P2pMinerTxProofStatus::BackendUnavailable);
        CHECK(trusted_work.size() == 0);
        return 0;
    }

    CHECK(trusted.status == HistoricalTrustStatus::Trusted);
    CHECK(trusted.initial_anchor.status ==
          HistoricalAnchorStatus::AnchorMatchedUntrusted);
    CHECK(trusted.final_anchor.status ==
          HistoricalAnchorStatus::AnchorMatchedUntrusted);
    CHECK(trusted.payout.status == HistoricalPayoutStatus::PlanDerived);
    CHECK(trusted.payout.parent_id == parent_id);
    CHECK(trusted.payout.plan.reward_atomic ==
          proposal.block_reward_without_fee);
    CHECK(trusted.promotion.status == P2pMiningContextTrustStatus::Trusted);
    CHECK(trusted.promotion.proof_status ==
          P2pMinerTxProofStatus::ProofsVerified);
    CHECK(trusted.promotion.payout_status ==
          P2pPayoutPolicyStatus::Verified);
    CHECK(trusted.promotion.registry_inserted);
    CHECK(trusted_work.size() == 1);
    CHECK(trusted_work.find(
              candidate.zano_height,
              candidate.mining_header_hash,
              candidate.parent_id) != nullptr);

    ShareId wrong_sidechain_parent = candidate.parent_id;
    wrong_sidechain_parent[0] ^= 0x01U;
    CHECK(trusted_work.find(
              candidate.zano_height,
              candidate.mining_header_hash,
              wrong_sidechain_parent) == nullptr);

    // Once the historical work has crossed every gate, normal share admission
    // consumes the parent-bound trusted context and performs local ProgPoWZ.
    P2pShareReceiver receiver(chain, trusted_work);
    const auto admitted = receiver.receive_share(
        peer,
        candidate,
        kP2pCapabilityShareGossip,
        200,
        ProgPowZContextMode::Light);
    CHECK(admitted.status == P2pShareReceiveStatus::Connected);
    CHECK(admitted.chain_result.disposition == ShareDisposition::Connected);
    CHECK(chain.contains(share_id(candidate)));
    CHECK(chain.find(share_id(candidate))->validated_ancestry);

    CHECK(std::string(historical_trust_status_name(
              HistoricalTrustStatus::Trusted)) == "trusted");
    CHECK(std::string(historical_trust_status_name(
              HistoricalTrustStatus::AnchorChangedBeforePromotion)) ==
          "anchor-changed-before-promotion");

    return 0;
}
