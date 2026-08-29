#pragma once

#include "zano_p2pool/block_template.hpp"
#include "zano_p2pool/p2p_protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace zano_p2pool {

inline constexpr std::uint8_t kP2pMiningContextVersion1 = 1;
inline constexpr std::size_t kP2pMiningContextFixedPayloadSize = 121;

using P2pMiningContextId = Hash256;

// Fields that a node can independently anchor to its own synchronized zanod.
// This deliberately excludes the peer-selected miner transaction and timestamp.
struct P2pMiningAnchor {
    std::uint64_t zano_height{0};
    Hash256 prev_hash{};
    Difficulty128 network_difficulty{};
    Hash256 seed{};
    std::uint64_t block_reward_without_fee{0};

    bool operator==(const P2pMiningAnchor&) const = default;
};

// An untrusted proposal for one exact Zano mining context. The full template and
// RPC miner_tx_tgc are transported so a later validator can independently audit
// the miner transaction. Merely receiving/anchoring this object MUST NOT make it
// a ShareWorkContext or insert it into P2pTrustedWorkRegistry.
struct P2pMiningContextProposal {
    std::uint8_t version{kP2pMiningContextVersion1};
    std::uint64_t zano_height{0};
    Hash256 prev_hash{};
    Difficulty128 network_difficulty{};
    Hash256 seed{};
    std::uint64_t block_reward_without_fee{0};
    std::uint64_t block_reward{0};
    std::uint64_t txs_fee{0};
    std::vector<std::uint8_t> block_template_blob;
    std::string miner_tx_tgc_json;

    bool operator==(const P2pMiningContextProposal&) const = default;
};

enum class P2pMiningContextCheckStatus {
    AnchoredUnverifiedMinerTx,
    CapabilityMissing,
    AnchorMismatch,
};

struct P2pMiningContextCheckResult {
    P2pMiningContextCheckStatus status{
        P2pMiningContextCheckStatus::AnchorMismatch};
    P2pMiningContextId proposal_id{};
    Hash256 mining_header_hash{};
    std::size_t regular_transaction_count{0};
};

[[nodiscard]] P2pMiningAnchor p2p_mining_anchor_from_template(
    const BlockTemplate& block_template);
[[nodiscard]] P2pMiningContextProposal p2p_mining_context_proposal_from_template(
    const BlockTemplate& block_template);

[[nodiscard]] std::vector<std::uint8_t> serialize_p2p_mining_context_payload(
    const P2pMiningContextProposal& proposal);
[[nodiscard]] P2pMiningContextProposal deserialize_p2p_mining_context_payload(
    std::span<const std::uint8_t> payload);

[[nodiscard]] P2pEnvelope make_p2p_mining_context_envelope(
    const P2pMiningContextProposal& proposal);
[[nodiscard]] P2pMiningContextProposal parse_p2p_mining_context_envelope(
    const P2pEnvelope& envelope);

[[nodiscard]] P2pMiningContextId p2p_mining_context_id(
    const P2pMiningContextProposal& proposal);

// Checks only locally anchorable metadata and current HF6 structural bindings,
// then derives the mining header locally. A successful result remains explicitly
// unverified with respect to miner-transaction consensus proofs/payout policy.
[[nodiscard]] P2pMiningContextCheckResult inspect_p2p_mining_context(
    const P2pHandshake& peer,
    const P2pEnvelope& envelope,
    const P2pMiningAnchor& local_anchor);

[[nodiscard]] const char* p2p_mining_context_check_status_name(
    P2pMiningContextCheckStatus status) noexcept;

}  // namespace zano_p2pool
