#pragma once

#include "zano_p2pool/p2p_miner_tx_proofs.hpp"
#include "zano_p2pool/p2p_share.hpp"

#include <cstdint>

namespace zano_p2pool {

// Checkpoint 6B.4 is the single peer-mining-context trust crossing. Earlier
// mining-context, key-binding, payout-policy, and proof APIs are deliberately
// side-effect free with respect to P2pTrustedWorkRegistry.
enum class P2pMiningContextTrustStatus : std::uint8_t {
    Trusted,
    CapabilityMissing,
    AnchorMismatch,
    ProofsRejected,
};

struct P2pMiningContextTrustResult {
    P2pMiningContextTrustStatus status{
        P2pMiningContextTrustStatus::ProofsRejected};
    P2pMiningContextCheckStatus check_status{
        P2pMiningContextCheckStatus::AnchorMismatch};
    P2pMinerTxProofStatus proof_status{
        P2pMinerTxProofStatus::NotAnchored};
    P2pPayoutPolicyStatus payout_status{
        P2pPayoutPolicyStatus::MalformedMinerTxPrefix};
    P2pMiningContextId proposal_id{};
    ShareWorkContext trusted_context{};
    bool registry_inserted{false};
};

// Parses and locally anchors the peer proposal, verifies the current-HF6 miner
// transaction payout and both consensus proof variants, and only then promotes
// the independently derived mining header into trusted_work. The trusted
// context is built from the local anchor plus the locally derived header hash;
// no peer-claimed height, difficulty, or header crosses this boundary without
// the preceding checks.
//
// Malformed envelopes/proposals remain protocol errors and are reported through
// the existing exceptions thrown by parsing/structural validation. A repeated
// fully verified proposal is idempotently Trusted with registry_inserted=false.
[[nodiscard]] P2pMiningContextTrustResult promote_p2p_mining_context(
    P2pTrustedWorkRegistry& trusted_work,
    const P2pHandshake& peer,
    const P2pEnvelope& envelope,
    const P2pMiningAnchor& local_anchor,
    const P2pPayoutAddress& expected_payout);

[[nodiscard]] const char* p2p_mining_context_trust_status_name(
    P2pMiningContextTrustStatus status) noexcept;

}  // namespace zano_p2pool
