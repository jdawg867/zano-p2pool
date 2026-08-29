#pragma once

#include "zano_p2pool/p2p_payout_policy.hpp"

#include <cstdint>

namespace zano_p2pool {

// Proof-gate states are deliberately explicit about the trust boundary. Only
// ProofsVerified means both current-HF6 proof variants passed; even that status
// does not itself insert anything into P2pTrustedWorkRegistry (6B.4 owns that
// single trust-promotion crossing).
enum class P2pMinerTxProofStatus : std::uint8_t {
    BalanceVerifiedRangePending,
    ProofsVerified,
    NotAnchored,
    PayoutPolicyFailed,
    BackendUnavailable,
    MalformedBlock,
    MalformedRangeProof,
    InvalidBalanceProof,
    InvalidRangeProof,
};

struct P2pMinerTxProofResult {
    P2pMinerTxProofStatus status{P2pMinerTxProofStatus::MalformedBlock};
    P2pPayoutPolicyStatus payout_status{
        P2pPayoutPolicyStatus::MalformedMinerTxPrefix};
};

// Checkpoint 6B.3a: runs only after 6A anchoring and 6B.1/6B.2 payout-policy
// validation. This verifies Zano's current HF6 zc_balance_proof but
// deliberately does not verify zc_outs_range_proof yet. There is intentionally
// no trusted-work registry parameter in this API.
[[nodiscard]] P2pMinerTxProofResult verify_p2p_mining_context_balance_proof(
    const P2pMiningContextProposal& proposal,
    const P2pMiningContextCheckResult& anchored_check,
    const P2pPayoutAddress& expected_payout) noexcept;

// Checkpoint 6B.3b: requires the 6B.3a balance gate and then parses/verifies the
// current-HF6 tag-47 Bulletproof+ plus its UG aggregation proof. Success means
// all miner-tx proof gates are valid, but still does NOT promote trusted work.
[[nodiscard]] P2pMinerTxProofResult verify_p2p_mining_context_proofs(
    const P2pMiningContextProposal& proposal,
    const P2pMiningContextCheckResult& anchored_check,
    const P2pPayoutAddress& expected_payout) noexcept;

[[nodiscard]] const char* p2p_miner_tx_proof_status_name(
    P2pMinerTxProofStatus status) noexcept;

}  // namespace zano_p2pool
