#include "test_check.hpp"

#include "zano_p2pool/crypto_hash.hpp"
#include "zano_p2pool/p2p_miner_tx_proofs.hpp"
#include "zano_p2pool/p2p_mining_context.hpp"
#include "zano_p2pool/zano_curve.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#ifdef ZANO_P2POOL_HAVE_ZANO_CURVE
#include "crypto/zarcanum.h"
#endif

namespace {

using namespace zano_p2pool;

constexpr const char* kScalarOneHex =
    "0100000000000000000000000000000000000000000000000000000000000000";
constexpr const char* kZeroScalarHex =
    "0000000000000000000000000000000000000000000000000000000000000000";
constexpr const char* kEd25519BasepointHex =
    "5866666666666666666666666666666666666666666666666666666666666666";
constexpr const char* kNativeCoinAssetId1Div8Hex =
    "74c32d3eaafafc623bf483e858d42e8bf4ec7df064ada2e34934469cff6b6268";

[[nodiscard]] ZanoCurveKey key_from_hex(const std::string& hex) {
    const auto bytes = hex_to_bytes(hex);
    CHECK(bytes.size() == 32);
    ZanoCurveKey key{};
    std::copy(bytes.begin(), bytes.end(), key.begin());
    return key;
}

void append_key(std::vector<std::uint8_t>& out, const ZanoCurveKey& key) {
    out.insert(out.end(), key.begin(), key.end());
}

[[nodiscard]] std::vector<std::uint8_t> make_prefix(
    const ZanoCurveKey& tx_public_key,
    const std::vector<ZanoCurveKey>& stealth_addresses,
    const ZanoCurveKey& native_asset) {
    std::vector<std::uint8_t> prefix;
    prefix.push_back(0x04);  // transaction version 4
    prefix.push_back(0x01);  // one input
    prefix.push_back(0x00);  // txin_gen
    prefix.push_back(0x01);  // height 1
    prefix.push_back(0x01);  // one extra
    prefix.push_back(0x16);  // transaction public key
    append_key(prefix, tx_public_key);

    CHECK(stealth_addresses.size() == 2);
    prefix.push_back(0x02);  // two miner outputs
    for (const auto& stealth : stealth_addresses) {
        prefix.push_back(0x3f);  // tx_out_zarcanum
        prefix.push_back(0x00);  // output serialization version 0
        append_key(prefix, stealth);
        append_key(prefix, tx_public_key);  // valid point; concealing point isn't used here
        append_key(prefix, native_asset);   // amount 1, zero mask => 1/8 * H
        append_key(prefix, native_asset);   // explicit native asset 1/8 * H
        prefix.insert(prefix.end(), 8 + 8 + 1, 0);
    }
    prefix.push_back(0x06);  // hardfork_id 6
    return prefix;
}

[[nodiscard]] std::string make_tgc() {
    return std::string("{\"tx_key\":\"") +
           kEd25519BasepointHex + kScalarOneHex +
           "\",\"amounts\":\"" + kScalarOneHex + ", " + kScalarOneHex +
           "\",\"amount_blinding_masks\":\"" + kZeroScalarHex + ", " + kZeroScalarHex +
           "\",\"asset_id_blinding_masks\":\"" + kZeroScalarHex + ", " + kZeroScalarHex +
           "\"}";
}

[[nodiscard]] std::array<std::uint8_t, 96> make_balance_proof(
    const std::vector<std::uint8_t>& prefix,
    const ZanoCurveKey& tx_public_key) {
    std::array<std::uint8_t, 96> serialized{};
#ifdef ZANO_P2POOL_HAVE_ZANO_CURVE
    const Hash256 tx_id_bytes = cn_fast_hash(prefix);
    crypto::hash tx_id{};
    crypto::public_key tx_public{};
    crypto::secret_key tx_secret{};
    const ZanoCurveKey scalar_one = key_from_hex(kScalarOneHex);

    std::memcpy(&tx_id, tx_id_bytes.data(), tx_id_bytes.size());
    std::memcpy(&tx_public, tx_public_key.data(), tx_public_key.size());
    std::memcpy(&tx_secret, scalar_one.data(), scalar_one.size());

    crypto::generic_double_schnorr_sig proof{};
    const crypto::point_t balance = crypto::c_point_0;
    CHECK(crypto::generate_double_schnorr_sig<crypto::gt_G, crypto::gt_G>(
        tx_id,
        balance,
        crypto::scalar_t(0),
        crypto::point_t(tx_public),
        crypto::scalar_t(tx_secret),
        proof));
    static_assert(sizeof(proof) == 96);
    std::memcpy(serialized.data(), &proof, serialized.size());
#else
    static_cast<void>(prefix);
    static_cast<void>(tx_public_key);
#endif
    return serialized;
}

[[nodiscard]] std::vector<std::uint8_t> make_block_blob(
    const std::vector<std::uint8_t>& prefix,
    const std::array<std::uint8_t, 96>& balance_proof,
    std::uint8_t range_fill = 0xa5) {
    std::vector<std::uint8_t> blob;
    blob.push_back(0x03);            // block major version
    blob.insert(blob.end(), 8, 0);   // zero template nonce
    blob.insert(blob.end(), 32, 0x11);  // previous block hash
    blob.push_back(0x00);            // minor version
    blob.push_back(0x01);            // timestamp
    blob.push_back(0x00);            // PoW flags
    blob.insert(blob.end(), prefix.begin(), prefix.end());

    // Current HF6 coinbase suffix. Range proof is deliberately dummy in 6B.3a.
    blob.push_back(0x00);  // attachments count
    blob.push_back(0x00);  // signatures count
    blob.push_back(0x02);  // proof count
    blob.push_back(0x2f);  // zc_outs_range_proof tag 47
    blob.insert(blob.end(), 16, range_fill);
    blob.push_back(0x30);  // zc_balance_proof tag 48
    blob.insert(blob.end(), balance_proof.begin(), balance_proof.end());
    blob.push_back(0x00);  // zero regular transaction hashes
    return blob;
}

[[nodiscard]] P2pMiningContextProposal make_proposal(
    const std::vector<std::uint8_t>& prefix,
    const std::array<std::uint8_t, 96>& balance_proof,
    std::uint8_t range_fill = 0xa5) {
    P2pMiningContextProposal proposal;
    proposal.zano_height = 1;
    proposal.prev_hash.fill(0x11);
    proposal.network_difficulty = difficulty128_from_decimal("1");
    proposal.seed.fill(0x22);
    proposal.block_reward_without_fee = 2;
    proposal.block_reward = 2;
    proposal.txs_fee = 12345;  // current HF6 fees are burned
    proposal.block_template_blob = make_block_blob(prefix, balance_proof, range_fill);
    proposal.miner_tx_tgc_json = make_tgc();
    return proposal;
}

[[nodiscard]] P2pMiningAnchor anchor_for(
    const P2pMiningContextProposal& proposal) {
    return P2pMiningAnchor{
        proposal.zano_height,
        proposal.prev_hash,
        proposal.network_difficulty,
        proposal.seed,
        proposal.block_reward_without_fee,
    };
}

[[nodiscard]] P2pHandshake make_peer() {
    P2pHandshake peer;
    peer.network = P2pNetwork::Testnet;
    peer.node_id.fill(0x42);
    peer.capabilities = kP2pCapabilitiesV1;
    return peer;
}

[[nodiscard]] P2pMiningContextCheckResult inspect(
    const P2pMiningContextProposal& proposal) {
    return inspect_p2p_mining_context(
        make_peer(),
        make_p2p_mining_context_envelope(proposal),
        anchor_for(proposal));
}

}  // namespace

int main() {
    const ZanoCurveKey scalar_one = key_from_hex(kScalarOneHex);
    const ZanoCurveKey basepoint = key_from_hex(kEd25519BasepointHex);
    const ZanoCurveKey native_asset = key_from_hex(kNativeCoinAssetId1Div8Hex);
    const P2pPayoutAddress payout{basepoint, basepoint};

    std::vector<ZanoCurveKey> stealths(2, basepoint);
    if (zano_curve_backend_available()) {
        CHECK(zano_derive_output_public_key(
            scalar_one, payout.spend_public_key, payout.view_public_key, 0, stealths[0]));
        CHECK(zano_derive_output_public_key(
            scalar_one, payout.spend_public_key, payout.view_public_key, 1, stealths[1]));
    }

    const auto prefix = make_prefix(basepoint, stealths, native_asset);
    const auto balance_proof = make_balance_proof(prefix, basepoint);
    const P2pMiningContextProposal proposal = make_proposal(prefix, balance_proof);
    const auto anchored = inspect(proposal);
    CHECK(anchored.status == P2pMiningContextCheckStatus::AnchoredUnverifiedMinerTx);

    const auto good = verify_p2p_mining_context_balance_proof(
        proposal, anchored, payout);
    if (!zano_curve_backend_available()) {
        CHECK(good.status == P2pMinerTxProofStatus::BackendUnavailable);
        CHECK(std::string(p2p_miner_tx_proof_status_name(good.status)) ==
              "backend-unavailable");
        return 0;
    }

    CHECK(good.status == P2pMinerTxProofStatus::BalanceVerifiedRangePending);
    CHECK(good.payout_status == P2pPayoutPolicyStatus::Verified);
    CHECK(std::string(p2p_miner_tx_proof_status_name(good.status)) ==
          "balance-verified-range-pending");

    // Tampering the actual balance proof is detected after re-anchoring the
    // changed proposal identity.
    P2pMiningContextProposal bad_balance = proposal;
    const std::size_t tx_hashes_offset = bad_balance.block_template_blob.size() - 1;
    const std::size_t balance_tag_offset = tx_hashes_offset - 97;
    bad_balance.block_template_blob[balance_tag_offset + 1] ^= 0x01U;
    const auto bad_balance_anchored = inspect(bad_balance);
    CHECK(verify_p2p_mining_context_balance_proof(
              bad_balance, bad_balance_anchored, payout).status ==
          P2pMinerTxProofStatus::InvalidBalanceProof);

    // 6B.3a deliberately does not validate the preceding range proof yet.
    P2pMiningContextProposal changed_range =
        make_proposal(prefix, balance_proof, 0x5a);
    const auto changed_range_anchored = inspect(changed_range);
    CHECK(verify_p2p_mining_context_balance_proof(
              changed_range, changed_range_anchored, payout).status ==
          P2pMinerTxProofStatus::BalanceVerifiedRangePending);

    P2pPayoutAddress wrong_payout = payout;
    wrong_payout.spend_public_key = {};
    CHECK(verify_p2p_mining_context_balance_proof(
              proposal, anchored, wrong_payout).status ==
          P2pMinerTxProofStatus::PayoutPolicyFailed);

    P2pMiningContextCheckResult not_anchored = anchored;
    not_anchored.proposal_id[0] ^= 0x01U;
    CHECK(verify_p2p_mining_context_balance_proof(
              proposal, not_anchored, payout).status ==
          P2pMinerTxProofStatus::NotAnchored);

    return 0;
}
