#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace zano_p2pool {

using ZanoCurveKey = std::array<std::uint8_t, 32>;

// Neutral wire representation of the fields serialized by
// crypto::bpp_signature_serialized. Keeping Zano's concrete crypto types out of
// the public API lets lightweight builds parse/fail closed without importing
// pinned consensus headers.
struct ZanoBppSignature {
    std::vector<ZanoCurveKey> left;
    std::vector<ZanoCurveKey> right;
    ZanoCurveKey a0{};
    ZanoCurveKey a{};
    ZanoCurveKey b{};
    ZanoCurveKey r{};
    ZanoCurveKey s{};
    ZanoCurveKey delta{};
};

// Neutral wire representation of crypto::vector_UG_aggregation_proof_serialized.
struct ZanoUgAggregationProof {
    std::vector<ZanoCurveKey> amount_commitments_for_rp_aggregation;
    std::vector<ZanoCurveKey> y0s;
    std::vector<ZanoCurveKey> y1s;
    ZanoCurveKey c{};
};

[[nodiscard]] bool zano_curve_backend_available() noexcept;

// Uses Zano's exact Ed25519/CryptoNote secret_key_to_public_key operation when
// the pinned Zano crypto backend is compiled in. Lightweight builds fail closed
// by returning false and must check zano_curve_backend_available() separately.
[[nodiscard]] bool zano_secret_key_matches_public(
    const ZanoCurveKey& secret_key,
    const ZanoCurveKey& public_key) noexcept;

// Derives the one-time output public key exactly as current Zano does for a
// normal Zarcanum output: Hs(8*r*V, output_index)*G + S.
[[nodiscard]] bool zano_derive_output_public_key(
    const ZanoCurveKey& tx_secret_key,
    const ZanoCurveKey& spend_public_key,
    const ZanoCurveKey& view_public_key,
    std::size_t output_index,
    ZanoCurveKey& output_public_key) noexcept;

// Verifies the current HF6 Zarcanum amount commitment using the exact Zano
// point/scalar implementation. Both serialized points are stored premultiplied
// by 1/8, so this compares 8*E against amount*(8*T) + mask*G.
[[nodiscard]] bool zano_amount_commitment_matches(
    std::uint64_t amount,
    const ZanoCurveKey& amount_blinding_mask,
    const ZanoCurveKey& blinded_asset_id,
    const ZanoCurveKey& amount_commitment) noexcept;

// Verifies the current HF6 PoW miner-transaction balance proof using Zano's
// exact G,G double-Schnorr verifier. amount_commitments are the serialized
// tx_out_zarcanum commitments (each premultiplied by 1/8); serialized_proof is
// the 96-byte generic_double_schnorr_sig payload after proof variant tag 48.
[[nodiscard]] bool zano_verify_hf6_miner_balance_proof(
    const ZanoCurveKey& tx_id,
    std::uint64_t block_reward,
    const ZanoCurveKey& tx_public_key,
    std::span<const ZanoCurveKey> amount_commitments,
    std::span<const std::uint8_t> serialized_proof) noexcept;

// Verifies both parts of current HF6 zc_outs_range_proof:
//   1. Bulletproof+ over E'_j = amount_j*U + mask'_j*G using Zano's pinned
//      bpp_verify<bpp_crypto_trait_ZC_out>();
//   2. the UG aggregation proof binding each E'_j back to the actual output
//      commitment E_j and blinded asset tag T'_j.
// All point arguments are the 32-byte on-wire values premultiplied by 1/8.
[[nodiscard]] bool zano_verify_hf6_miner_range_proof(
    const ZanoCurveKey& tx_id,
    std::span<const ZanoCurveKey> amount_commitments,
    std::span<const ZanoCurveKey> blinded_asset_ids,
    const ZanoBppSignature& bpp,
    const ZanoUgAggregationProof& aggregation) noexcept;

}  // namespace zano_p2pool
