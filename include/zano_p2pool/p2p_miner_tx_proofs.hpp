#pragma once

#include "zano_p2pool/p2p_payout_policy.hpp"

#include <cstdint>

namespace zano_p2pool {

// Checkpoint 6B.3a validates only the current HF6 balance proof. A successful
// result explicitly remains range-proof-pending and MUST NOT be treated as a
// trusted mining context until the Bulletproof+/aggregation gate also passes.
enum class P2pMinerTxProofStatus : std::uint8_t {
    BalanceVerifiedRangePending,
    NotAnchored,
    PayoutPolicyFailed,
    BackendUnavailable,
    MalformedBlock,
    InvalidBalanceProof,
};

struct P2pMinerTxProofResult {
    P2pMinerTxProofStatus status{P2pMinerTxProofStatus::MalformedBlock};
    P2pPayoutPolicyStatus payout_status{
        P2pPayoutPolicyStatus::MalformedMinerTxPrefix};
};

// Runs only after 6A anchoring and 6B.1/6B.2 payout-policy validation. This
// verifies Zano's current HF6 zc_balance_proof but deliberately does not verify
// zc_outs_range_proof yet. There is intentionally no trusted-work registry
// parameter in this API.
[[nodiscard]] P2pMinerTxProofResult verify_p2p_mining_context_balance_proof(
    const P2pMiningContextProposal& proposal,
    const P2pMiningContextCheckResult& anchored_check,
    const P2pPayoutAddress& expected_payout) noexcept;

[[nodiscard]] const char* p2p_miner_tx_proof_status_name(
    P2pMinerTxProofStatus status) noexcept;

}  // namespace zano_p2pool
