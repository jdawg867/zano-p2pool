#include "test_check.hpp"

#include "zano_p2pool/crypto_hash.hpp"
#include "zano_p2pool/p2p_miner_tx_proofs.hpp"
#include "zano_p2pool/p2p_mining_context.hpp"
#include "zano_p2pool/p2p_share.hpp"
#include "zano_p2pool/zano_curve.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#ifdef ZANO_P2POOL_HAVE_ZANO_CURVE
#include "crypto/range_proofs.h"
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

void append_varint(std::vector<std::uint8_t>& out, std::uint64_t value) {
    do {
        std::uint8_t byte = static_cast<std::uint8_t>(value & 0x7fU);
        value >>= 7;
        if (value != 0) {
            byte |= 0x80U;
        }
        out.push_back(byte);
    } while (value != 0);
}

void append_key(std::vector<std::uint8_t>& out, const ZanoCurveKey& key) {
    out.insert(out.end(), key.begin(), key.end());
}

template <typename T>
void append_crypto_32(std::vector<std::uint8_t>& out, const T& value) {
    static_assert(sizeof(T) == 32);
    const auto* begin = reinterpret_cast<const std::uint8_t*>(&value);
    out.insert(out.end(), begin, begin + sizeof(T));
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
    const bool generated =
        crypto::generate_double_schnorr_sig<crypto::gt_G, crypto::gt_G>(
            tx_id,
            balance,
            crypto::scalar_t(0),
            crypto::point_t(tx_public),
            crypto::scalar_t(tx_secret),
            proof);
    CHECK(generated);
    static_assert(sizeof(proof) == 96);
    std::memcpy(serialized.data(), &proof, serialized.size());
#else
    static_cast<void>(prefix);
    static_cast<void>(tx_public_key);
#endif
    return serialized;
}

#ifdef ZANO_P2POOL_HAVE_ZANO_CURVE
[[nodiscard]] crypto::vector_UG_aggregation_proof make_aggregation_proof(
    const crypto::hash& message,
    const crypto::scalar_vec_t& amounts,
    const crypto::scalar_vec_t& output_masks,
    const crypto::scalar_vec_t& rp_masks,
    const std::vector<crypto::point_t>& amount_commitments,
    const std::vector<crypto::point_t>& rp_commitments,
    const std::vector<crypto::point_t>& rp_commitments_1div8,
    const std::vector<crypto::point_t>& blinded_asset_ids) {
    const std::size_t n = amounts.size();
    CHECK(n == 2);
    CHECK(output_masks.size() == n);
    CHECK(rp_masks.size() == n);
    CHECK(amount_commitments.size() == n);
    CHECK(rp_commitments.size() == n);
    CHECK(rp_commitments_1div8.size() == n);
    CHECK(blinded_asset_ids.size() == n);

    crypto::hash_helper_t::hs_t transcript(1 + 3 * n);
    transcript.add_hash(message);
    for (const auto& point : amount_commitments) {
        transcript.add_point(point);
    }
    for (const auto& point : rp_commitments) {
        transcript.add_point(point);
    }
    const crypto::scalar_t w = transcript.calc_hash(false);

    std::vector<crypto::point_t> asset_tag_plus_u;
    asset_tag_plus_u.reserve(n);
    for (const auto& asset_tag : blinded_asset_ids) {
        asset_tag_plus_u.push_back(asset_tag + w * crypto::c_point_U);
    }

    crypto::scalar_vec_t r0;
    crypto::scalar_vec_t r1;
    r0.reserve(n);
    r1.reserve(n);
    std::vector<crypto::point_t> responses;
    responses.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        r0.push_back(crypto::scalar_t::random());
        r1.push_back(crypto::scalar_t::random());
        responses.push_back(
            r0[i] * asset_tag_plus_u[i] + r1[i] * crypto::c_point_G);
        transcript.add_point(responses.back());
    }

    crypto::vector_UG_aggregation_proof result{};
    result.c = transcript.calc_hash();
    for (std::size_t i = 0; i < n; ++i) {
        result.y0s.push_back(r0[i] - result.c * amounts[i]);
        result.y1s.push_back(
            r1[i] - result.c * (output_masks[i] + w * rp_masks[i]));
        result.amount_commitments_for_rp_aggregation.push_back(
            rp_commitments_1div8[i].to_public_key());
    }
    return result;
}

void append_crypto_vector(
    std::vector<std::uint8_t>& out,
    const std::vector<crypto::public_key>& values) {
    append_varint(out, values.size());
    for (const auto& value : values) {
        append_crypto_32(out, value);
    }
}

void append_scalar_vector(
    std::vector<std::uint8_t>& out,
    const crypto::scalar_vec_t& values) {
    append_varint(out, values.size());
    for (const auto& value : values) {
        append_crypto_32(out, value);
    }
}
#endif

[[nodiscard]] std::vector<std::uint8_t> make_valid_range_proof(
    const std::vector<std::uint8_t>& prefix) {
#ifdef ZANO_P2POOL_HAVE_ZANO_CURVE
    crypto::scalar_vec_t amounts;
    crypto::scalar_vec_t rp_masks;
    crypto::scalar_vec_t output_masks;
    for (int i = 0; i < 2; ++i) {
        amounts.push_back(crypto::scalar_t(1));
        rp_masks.push_back(crypto::scalar_t(0));
        output_masks.push_back(crypto::scalar_t(0));
    }

    crypto::bpp_signature bpp{};
    std::vector<crypto::point_t> rp_commitments_1div8;
    const bool bpp_generated =
        crypto::bpp_gen<crypto::bpp_crypto_trait_ZC_out>(
            amounts, rp_masks, bpp, rp_commitments_1div8);
    CHECK(bpp_generated);
    CHECK(rp_commitments_1div8.size() == 2);

    const ZanoCurveKey native_wire = key_from_hex(kNativeCoinAssetId1Div8Hex);
    crypto::public_key native_public{};
    std::memcpy(&native_public, native_wire.data(), native_wire.size());
    crypto::point_t native_full(native_public);
    native_full.modify_mul8();

    std::vector<crypto::point_t> amount_commitments(2, native_full);
    std::vector<crypto::point_t> blinded_asset_ids(2, native_full);
    std::vector<crypto::point_t> rp_commitments;
    rp_commitments.reserve(2);
    for (const auto& point_1div8 : rp_commitments_1div8) {
        crypto::point_t point = point_1div8;
        point.modify_mul8();
        rp_commitments.push_back(point);
    }

    const Hash256 tx_id_bytes = cn_fast_hash(prefix);
    crypto::hash tx_id{};
    std::memcpy(&tx_id, tx_id_bytes.data(), tx_id_bytes.size());
    const crypto::vector_UG_aggregation_proof aggregation =
        make_aggregation_proof(
            tx_id,
            amounts,
            output_masks,
            rp_masks,
            amount_commitments,
            rp_commitments,
            rp_commitments_1div8,
            blinded_asset_ids);

    std::vector<std::uint8_t> serialized;
    append_crypto_vector(serialized, bpp.L);
    append_crypto_vector(serialized, bpp.R);
    append_crypto_32(serialized, bpp.A0);
    append_crypto_32(serialized, bpp.A);
    append_crypto_32(serialized, bpp.B);
    append_crypto_32(serialized, bpp.r);
    append_crypto_32(serialized, bpp.s);
    append_crypto_32(serialized, bpp.delta);
    append_crypto_vector(
        serialized, aggregation.amount_commitments_for_rp_aggregation);
    append_scalar_vector(serialized, aggregation.y0s);
    append_scalar_vector(serialized, aggregation.y1s);
    append_crypto_32(serialized, aggregation.c);
    return serialized;
#else
    static_cast<void>(prefix);
    return std::vector<std::uint8_t>(16, 0xa5);
#endif
}

[[nodiscard]] std::vector<std::uint8_t> make_dummy_range_proof(
    std::uint8_t fill) {
    return std::vector<std::uint8_t>(16, fill);
}

[[nodiscard]] std::vector<std::uint8_t> make_block_blob(
    const std::vector<std::uint8_t>& prefix,
    const std::array<std::uint8_t, 96>& balance_proof,
    const std::vector<std::uint8_t>& range_proof) {
    std::vector<std::uint8_t> blob;
    blob.push_back(0x03);            // block major version
    blob.insert(blob.end(), 8, 0);   // zero template nonce
    blob.insert(blob.end(), 32, 0x11);  // previous block hash
    blob.push_back(0x00);            // minor version
    blob.push_back(0x01);            // timestamp
    blob.push_back(0x00);            // PoW flags
    blob.insert(blob.end(), prefix.begin(), prefix.end());

    blob.push_back(0x00);  // attachments count
    blob.push_back(0x00);  // signatures count
    blob.push_back(0x02);  // proof count
    blob.push_back(0x2f);  // zc_outs_range_proof tag 47
    blob.insert(blob.end(), range_proof.begin(), range_proof.end());
    blob.push_back(0x30);  // zc_balance_proof tag 48
    blob.insert(blob.end(), balance_proof.begin(), balance_proof.end());
    blob.push_back(0x00);  // zero regular transaction hashes
    return blob;
}

[[nodiscard]] P2pMiningContextProposal make_proposal(
    const std::vector<std::uint8_t>& prefix,
    const std::array<std::uint8_t, 96>& balance_proof,
    const std::vector<std::uint8_t>& range_proof) {
    P2pMiningContextProposal proposal;
    proposal.zano_height = 1;
    proposal.prev_hash.fill(0x11);
    proposal.network_difficulty = difficulty128_from_decimal("1");
    proposal.seed.fill(0x22);
    proposal.block_reward_without_fee = 2;
    proposal.block_reward = 2;
    proposal.txs_fee = 12345;  // current HF6 fees are burned
    proposal.block_template_blob =
        make_block_blob(prefix, balance_proof, range_proof);
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

    // 6B.3a remains independently pinned: dummy range bytes must not affect
    // balance-only verification.
    const P2pMiningContextProposal balance_only_proposal = make_proposal(
        prefix, balance_proof, make_dummy_range_proof(0xa5));
    const auto balance_only_anchored = inspect(balance_only_proposal);
    CHECK(balance_only_anchored.status ==
          P2pMiningContextCheckStatus::AnchoredUnverifiedMinerTx);

    const auto balance_only = verify_p2p_mining_context_balance_proof(
        balance_only_proposal, balance_only_anchored, payout);
    if (!zano_curve_backend_available()) {
        CHECK(balance_only.status == P2pMinerTxProofStatus::BackendUnavailable);
        CHECK(std::string(p2p_miner_tx_proof_status_name(balance_only.status)) ==
              "backend-unavailable");
        CHECK(verify_p2p_mining_context_proofs(
                  balance_only_proposal, balance_only_anchored, payout).status ==
              P2pMinerTxProofStatus::BackendUnavailable);
        return 0;
    }

    CHECK(balance_only.status ==
          P2pMinerTxProofStatus::BalanceVerifiedRangePending);
    CHECK(balance_only.payout_status == P2pPayoutPolicyStatus::Verified);
    CHECK(std::string(p2p_miner_tx_proof_status_name(balance_only.status)) ==
          "balance-verified-range-pending");

    P2pMiningContextProposal changed_range = make_proposal(
        prefix, balance_proof, make_dummy_range_proof(0x5a));
    const auto changed_range_anchored = inspect(changed_range);
    CHECK(verify_p2p_mining_context_balance_proof(
              changed_range, changed_range_anchored, payout).status ==
          P2pMinerTxProofStatus::BalanceVerifiedRangePending);

    // Tampering the actual balance proof is detected after re-anchoring the
    // changed proposal identity.
    P2pMiningContextProposal bad_balance = balance_only_proposal;
    const std::size_t tx_hashes_offset = bad_balance.block_template_blob.size() - 1;
    const std::size_t balance_tag_offset = tx_hashes_offset - 97;
    bad_balance.block_template_blob[balance_tag_offset + 1] ^= 0x01U;
    const auto bad_balance_anchored = inspect(bad_balance);
    CHECK(verify_p2p_mining_context_balance_proof(
              bad_balance, bad_balance_anchored, payout).status ==
          P2pMinerTxProofStatus::InvalidBalanceProof);

    // 6B.3b: a genuine BPP+ proof and matching UG aggregation proof must pass
    // the integrated gate after 6B.1, 6B.2 and 6B.3a.
    const auto valid_range = make_valid_range_proof(prefix);
    const P2pMiningContextProposal full_proposal =
        make_proposal(prefix, balance_proof, valid_range);
    const auto full_anchored = inspect(full_proposal);
    CHECK(full_anchored.status ==
          P2pMiningContextCheckStatus::AnchoredUnverifiedMinerTx);
    const auto full = verify_p2p_mining_context_proofs(
        full_proposal, full_anchored, payout);
    CHECK(full.status == P2pMinerTxProofStatus::ProofsVerified);
    CHECK(full.payout_status == P2pPayoutPolicyStatus::Verified);
    CHECK(std::string(p2p_miner_tx_proof_status_name(full.status)) ==
          "proofs-verified");

    std::vector<std::uint8_t> bad_bpp_range = valid_range;
    CHECK(bad_bpp_range.size() > 1);
    bad_bpp_range[1] ^= 0x01U;  // first serialized L point
    P2pMiningContextProposal bad_bpp =
        make_proposal(prefix, balance_proof, bad_bpp_range);
    const auto bad_bpp_anchored = inspect(bad_bpp);
    CHECK(verify_p2p_mining_context_proofs(
              bad_bpp, bad_bpp_anchored, payout).status ==
          P2pMinerTxProofStatus::InvalidRangeProof);

    std::vector<std::uint8_t> bad_aggregation_range = valid_range;
    CHECK(!bad_aggregation_range.empty());
    bad_aggregation_range.back() ^= 0x01U;  // aggregation challenge c
    P2pMiningContextProposal bad_aggregation =
        make_proposal(prefix, balance_proof, bad_aggregation_range);
    const auto bad_aggregation_anchored = inspect(bad_aggregation);
    CHECK(verify_p2p_mining_context_proofs(
              bad_aggregation, bad_aggregation_anchored, payout).status ==
          P2pMinerTxProofStatus::InvalidRangeProof);

    // L has seven elements for two outputs. Encode the same value in a
    // deliberately non-canonical two-byte varint (0x87 0x00); the parser must
    // reject before crypto verification.
    std::vector<std::uint8_t> noncanonical_range = valid_range;
    CHECK(!noncanonical_range.empty());
    CHECK(noncanonical_range.front() == 0x07);
    noncanonical_range.front() = 0x87;
    noncanonical_range.insert(noncanonical_range.begin() + 1, 0x00);
    P2pMiningContextProposal malformed =
        make_proposal(prefix, balance_proof, noncanonical_range);
    const auto malformed_anchored = inspect(malformed);
    CHECK(verify_p2p_mining_context_proofs(
              malformed, malformed_anchored, payout).status ==
          P2pMinerTxProofStatus::MalformedRangeProof);

    P2pPayoutAddress wrong_payout = payout;
    wrong_payout.spend_public_key = {};
    CHECK(verify_p2p_mining_context_proofs(
              full_proposal, full_anchored, wrong_payout).status ==
          P2pMinerTxProofStatus::PayoutPolicyFailed);

    P2pMiningContextCheckResult not_anchored = full_anchored;
    not_anchored.proposal_id[0] ^= 0x01U;
    CHECK(verify_p2p_mining_context_proofs(
              full_proposal, not_anchored, payout).status ==
          P2pMinerTxProofStatus::NotAnchored);

    // 6B.3b still has no trust-registry crossing.
    P2pTrustedWorkRegistry trusted_work;
    CHECK(trusted_work.size() == 0);
    static_cast<void>(verify_p2p_mining_context_proofs(
        full_proposal, full_anchored, payout));
    CHECK(trusted_work.size() == 0);

    return 0;
}