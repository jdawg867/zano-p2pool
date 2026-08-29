#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace zano_p2pool {

using ZanoCurveKey = std::array<std::uint8_t, 32>;

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

}  // namespace zano_p2pool
