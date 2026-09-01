#define main p2p_miner_tx_proofs_fixture_main
#include "p2p_miner_tx_proofs_test.cpp"
#undef main

#include "zano_p2pool/p2p_mining_context_trust.hpp"

int main() {
    using namespace zano_p2pool;

    const ZanoCurveKey scalar_one = key_from_hex(kScalarOneHex);
    const ZanoCurveKey basepoint = key_from_hex(kEd25519BasepointHex);
    const ZanoCurveKey native_asset = key_from_hex(kNativeCoinAssetId1Div8Hex);
    const P2pPayoutAddress payout{basepoint, basepoint};
    const PayoutPublicKeys payout_keys{basepoint, basepoint};

    PplnsCoinbasePlan plan;
    plan.status = PplnsCoinbasePlanStatus::Ready;
    plan.reward_atomic = 2;
    plan.destinations = {
        PplnsCoinbaseDestination{
            miner_id_from_payout(payout_keys),
            payout_keys,
            2,
        },
    };

    std::vector<ZanoCurveKey> stealths(2, basepoint);
    if (zano_curve_backend_available()) {
        CHECK(zano_derive_output_public_key(
            scalar_one,
            payout.spend_public_key,
            payout.view_public_key,
            0,
            stealths[0]));
        CHECK(zano_derive_output_public_key(
            scalar_one,
            payout.spend_public_key,
            payout.view_public_key,
            1,
            stealths[1]));
    }

    const auto prefix = make_prefix(basepoint, stealths, native_asset);
    const auto balance_proof = make_balance_proof(prefix, basepoint);
    const auto range_proof = make_valid_range_proof(prefix);
    const P2pMiningContextProposal proposal =
        make_proposal(prefix, balance_proof, range_proof);
    const P2pEnvelope envelope = make_p2p_mining_context_envelope(proposal);
    const P2pMiningAnchor local_anchor = anchor_for(proposal);
    const P2pHandshake peer = make_peer();

    // 6B.4 is the only API that is allowed to cross from peer-provided mining
    // context into locally trusted work. Lightweight builds must fail closed.
    P2pTrustedWorkRegistry trusted_work;
    CHECK(trusted_work.size() == 0);

    const P2pMiningContextTrustResult promoted = promote_p2p_mining_context(
        trusted_work,
        peer,
        envelope,
        local_anchor,
        payout);

    // The PPLNS-plan crossing exercises the same balance/range proof pipeline,
    // but replaces the old one-wallet payout assumption with the complete
    // locally derived coinbase plan.
    P2pTrustedWorkRegistry plan_registry;
    const P2pMiningContextTrustResult plan_promoted = promote_p2p_mining_context(
        plan_registry,
        peer,
        envelope,
        local_anchor,
        plan);

    if (!zano_curve_backend_available()) {
        CHECK(promoted.status == P2pMiningContextTrustStatus::ProofsRejected);
        CHECK(promoted.proof_status == P2pMinerTxProofStatus::BackendUnavailable);
        CHECK(!promoted.registry_inserted);
        CHECK(trusted_work.size() == 0);
        CHECK(plan_promoted.status == P2pMiningContextTrustStatus::ProofsRejected);
        CHECK(plan_promoted.proof_status ==
              P2pMinerTxProofStatus::BackendUnavailable);
        CHECK(!plan_promoted.registry_inserted);
        CHECK(plan_registry.size() == 0);
        return 0;
    }

    CHECK(promoted.status == P2pMiningContextTrustStatus::Trusted);
    CHECK(promoted.check_status ==
          P2pMiningContextCheckStatus::AnchoredUnverifiedMinerTx);
    CHECK(promoted.proof_status == P2pMinerTxProofStatus::ProofsVerified);
    CHECK(promoted.payout_status == P2pPayoutPolicyStatus::Verified);
    CHECK(promoted.proposal_id == p2p_mining_context_id(proposal));
    CHECK(promoted.trusted_context.zano_height == local_anchor.zano_height);
    CHECK(promoted.trusted_context.network_difficulty ==
          local_anchor.network_difficulty);
    CHECK(promoted.registry_inserted);
    CHECK(trusted_work.size() == 1);
    CHECK(trusted_work.find(
              promoted.trusted_context.zano_height,
              promoted.trusted_context.mining_header_hash) != nullptr);
    CHECK(std::string(p2p_mining_context_trust_status_name(promoted.status)) ==
          "trusted");

    CHECK(plan_promoted.status == P2pMiningContextTrustStatus::Trusted);
    CHECK(plan_promoted.check_status ==
          P2pMiningContextCheckStatus::AnchoredUnverifiedMinerTx);
    CHECK(plan_promoted.proof_status == P2pMinerTxProofStatus::ProofsVerified);
    CHECK(plan_promoted.payout_status == P2pPayoutPolicyStatus::Verified);
    CHECK(plan_promoted.registry_inserted);
    CHECK(plan_registry.size() == 1);

    // Re-promoting the exact verified context is safe and idempotent.
    const P2pMiningContextTrustResult repeated = promote_p2p_mining_context(
        trusted_work,
        peer,
        envelope,
        local_anchor,
        payout);
    CHECK(repeated.status == P2pMiningContextTrustStatus::Trusted);
    CHECK(!repeated.registry_inserted);
    CHECK(trusted_work.size() == 1);

    // Capability negotiation remains ahead of all expensive proof work and no
    // context is promoted when the peer did not advertise mining-context sync.
    P2pHandshake no_context_peer = peer;
    no_context_peer.capabilities &= ~kP2pCapabilityMiningContext;
    P2pTrustedWorkRegistry capability_registry;
    const auto capability_result = promote_p2p_mining_context(
        capability_registry,
        no_context_peer,
        envelope,
        local_anchor,
        payout);
    CHECK(capability_result.status ==
          P2pMiningContextTrustStatus::CapabilityMissing);
    CHECK(capability_registry.size() == 0);

    // A proposal that cannot be anchored to the local daemon view never reaches
    // the proof gate or trusted registry.
    P2pMiningAnchor wrong_anchor = local_anchor;
    wrong_anchor.prev_hash[0] ^= 0x01U;
    P2pTrustedWorkRegistry anchor_registry;
    const auto anchor_result = promote_p2p_mining_context(
        anchor_registry,
        peer,
        envelope,
        wrong_anchor,
        payout);
    CHECK(anchor_result.status == P2pMiningContextTrustStatus::AnchorMismatch);
    CHECK(anchor_registry.size() == 0);

    // Even a fully anchored proposal cannot cross the trust boundary when its
    // miner transaction does not pay the locally expected public payout keys.
    P2pPayoutAddress wrong_payout = payout;
    wrong_payout.spend_public_key = {};
    P2pTrustedWorkRegistry payout_registry;
    const auto payout_result = promote_p2p_mining_context(
        payout_registry,
        peer,
        envelope,
        local_anchor,
        wrong_payout);
    CHECK(payout_result.status == P2pMiningContextTrustStatus::ProofsRejected);
    CHECK(payout_result.proof_status ==
          P2pMinerTxProofStatus::PayoutPolicyFailed);
    CHECK(payout_registry.size() == 0);

    // A peer context also cannot cross when the locally computed PPLNS split
    // disagrees, even though the proposal still advertises the same total reward.
    PplnsCoinbasePlan wrong_plan = plan;
    wrong_plan.destinations.front().amount = 1;
    P2pTrustedWorkRegistry wrong_plan_registry;
    const auto wrong_plan_result = promote_p2p_mining_context(
        wrong_plan_registry,
        peer,
        envelope,
        local_anchor,
        wrong_plan);
    CHECK(wrong_plan_result.status ==
          P2pMiningContextTrustStatus::ProofsRejected);
    CHECK(wrong_plan_result.proof_status ==
          P2pMinerTxProofStatus::PayoutPolicyFailed);
    CHECK(wrong_plan_result.payout_status ==
          P2pPayoutPolicyStatus::PayoutPlanMismatch);
    CHECK(wrong_plan_registry.size() == 0);

    // A corrupted BPP+/aggregation payload is also rejected without promotion.
    std::vector<std::uint8_t> bad_range = range_proof;
    CHECK(bad_range.size() > 1);
    bad_range[1] ^= 0x01U;
    const P2pMiningContextProposal bad_proposal =
        make_proposal(prefix, balance_proof, bad_range);
    P2pTrustedWorkRegistry proof_registry;
    const auto proof_result = promote_p2p_mining_context(
        proof_registry,
        peer,
        make_p2p_mining_context_envelope(bad_proposal),
        anchor_for(bad_proposal),
        payout);
    CHECK(proof_result.status == P2pMiningContextTrustStatus::ProofsRejected);
    CHECK(proof_result.proof_status == P2pMinerTxProofStatus::InvalidRangeProof);
    CHECK(proof_registry.size() == 0);

    return 0;
}
