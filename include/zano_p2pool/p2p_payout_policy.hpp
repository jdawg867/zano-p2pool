#pragma once

#include "zano_p2pool/p2p_miner_tx_binding.hpp"
#include "zano_p2pool/zano_curve.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace zano_p2pool {

struct P2pPayoutAddress {
    ZanoCurveKey spend_public_key{};
    ZanoCurveKey view_public_key{};
    bool operator==(const P2pPayoutAddress&) const = default;
};

enum class P2pPayoutPolicyStatus {
    Verified,
    NotAnchored,
    KeyBindingFailed,
    BackendUnavailable,
    MalformedTgc,
    MalformedMinerTxPrefix,
    InvalidOutputCount,
    RewardMetadataMismatch,
    TgcOutputCountMismatch,
    NonNativeAsset,
    DestinationMismatch,
    AmountCommitmentMismatch,
    RewardSumMismatch,
};

struct P2pPayoutPolicyResult {
    P2pPayoutPolicyStatus status{P2pPayoutPolicyStatus::MalformedMinerTxPrefix};
    P2pMinerTxBindingStatus binding_status{
        P2pMinerTxBindingStatus::MalformedMinerTxPrefix};
    std::size_t output_count{0};
    std::uint64_t verified_reward{0};
};

// Current HF6 PoW payout gate. The expected payout identity is public-only:
// no wallet secret/view secret is required. This verifies destination and
// amount commitments, but deliberately does not validate the proof suffix;
// that remains the next trust gate.
[[nodiscard]] P2pPayoutPolicyResult verify_miner_tx_payout_policy(
    std::span<const std::uint8_t> miner_tx_prefix,
    std::string_view miner_tx_tgc_json,
    std::uint64_t block_reward_without_fee,
    std::uint64_t block_reward,
    std::uint64_t txs_fee,
    const P2pPayoutAddress& expected_payout) noexcept;

[[nodiscard]] P2pPayoutPolicyResult verify_p2p_mining_context_payout_policy(
    const P2pMiningContextProposal& proposal,
    const P2pMiningContextCheckResult& anchored_check,
    const P2pPayoutAddress& expected_payout) noexcept;

[[nodiscard]] const char* p2p_payout_policy_status_name(
    P2pPayoutPolicyStatus status) noexcept;

}  // namespace zano_p2pool
