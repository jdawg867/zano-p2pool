#pragma once

#include "zano_p2pool/p2p_mining_context.hpp"
#include "zano_p2pool/zano_curve.hpp"

#include <cstdint>
#include <span>
#include <string_view>

namespace zano_p2pool {

enum class P2pMinerTxBindingStatus : std::uint8_t {
    Verified,
    NotAnchored,
    BackendUnavailable,
    MalformedTgc,
    MalformedMinerTxPrefix,
    TgcKeyPairMismatch,
    PrefixPublicKeyMismatch,
};

struct P2pMinerTxBindingResult {
    P2pMinerTxBindingStatus status{P2pMinerTxBindingStatus::NotAnchored};
    ZanoCurveKey tx_public_key{};
};

// Low-level deterministic gate used by tests and the high-level context path.
// The miner transaction prefix must be the exact prefix committed by the mining
// header. miner_tx_tgc_json must contain Zano's serialized 64-byte keypair
// (public key followed by secret key) in its tx_key field.
[[nodiscard]] P2pMinerTxBindingResult verify_miner_tx_tgc_key_binding(
    std::span<const std::uint8_t> miner_tx_prefix,
    std::string_view miner_tx_tgc_json) noexcept;

// Runs only after checkpoint-6A anchoring. This function still does NOT promote
// work into P2pTrustedWorkRegistry; it proves only the transaction-key binding.
[[nodiscard]] P2pMinerTxBindingResult verify_p2p_mining_context_key_binding(
    const P2pMiningContextProposal& proposal,
    const P2pMiningContextCheckResult& anchored_check) noexcept;

[[nodiscard]] const char* p2p_miner_tx_binding_status_name(
    P2pMinerTxBindingStatus status) noexcept;

}  // namespace zano_p2pool
