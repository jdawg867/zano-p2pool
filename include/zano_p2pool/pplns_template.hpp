#pragma once

#include "zano_p2pool/block_template.hpp"
#include "zano_p2pool/crypto_hash.hpp"
#include "zano_p2pool/mining_header.hpp"
#include "zano_p2pool/pplns_coinbase.hpp"
#include "zano_p2pool/pow_target.hpp"
#include "zano_p2pool/sidechain_params.hpp"
#include "zano_p2pool/zano_miner_tx.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace zano_p2pool {

enum class PplnsTemplateStatus : std::uint8_t {
    Ready,
    PayoutPlanUnavailable,
    BackendUnavailable,
    InvalidDaemonTemplate,
    BlockTooLarge,
};

struct PplnsTemplateResult {
    PplnsTemplateStatus status{PplnsTemplateStatus::InvalidDaemonTemplate};
    PplnsCoinbasePlan plan{};
    BlockTemplate block{};
    MiningHeaderWork mining_work{};
    std::string error;
};

// Replace the daemon-provided miner transaction with the exact HF6 transaction
// implied by the locally verified sidechain PPLNS window. The block header and
// regular-transaction trailer are copied byte-for-byte from the daemon template;
// only the complete serialized miner transaction is replaced. The returned
// BlockTemplate also carries the matching canonical miner_tx_tgc JSON so peers
// can independently verify the rebuilt transaction.
//
// This helper is intentionally fail-closed. In particular, an empty/incomplete
// payout window or a window containing shares without payout identities never
// produces mining work.
[[nodiscard]] inline PplnsTemplateResult build_canonical_pplns_template(
    const BlockTemplate& daemon_template,
    const ShareChain& chain,
    const SidechainParameters& params,
    std::string_view extra_text = "zano-p2pool") noexcept {
    PplnsTemplateResult result;
    result.block = daemon_template;

    try {
        const Difficulty128 network_difficulty =
            difficulty128_from_decimal(daemon_template.difficulty);
        const PplnsWindow window = build_sidechain_pplns_window(
            chain, params, network_difficulty);
        result.plan = make_pplns_coinbase_plan(
            window, daemon_template.block_reward);
        if (result.plan.status != PplnsCoinbasePlanStatus::Ready) {
            result.status = PplnsTemplateStatus::PayoutPlanUnavailable;
            result.error = pplns_coinbase_plan_status_name(result.plan.status);
            return result;
        }

        if (!zano_miner_tx_builder_available()) {
            result.status = PplnsTemplateStatus::BackendUnavailable;
            result.error = "exact Zano miner-tx builder is unavailable";
            return result;
        }

        const std::vector<std::uint8_t> daemon_blob =
            hex_to_bytes(daemon_template.blocktemplate_blob);
        const MiningHeaderWork daemon_work =
            derive_mining_header_work(daemon_blob);
        if (daemon_work.block_header.serialized_size >
                daemon_work.tx_hashes.serialized_offset ||
            daemon_work.tx_hashes.serialized_offset > daemon_blob.size()) {
            result.status = PplnsTemplateStatus::InvalidDaemonTemplate;
            result.error = "invalid daemon miner-transaction offsets";
            return result;
        }

        const ZanoMinerTxResult miner_tx = build_zano_hf6_pplns_miner_tx(
            daemon_template.height,
            daemon_template.txs_fee,
            daemon_template.block_reward,
            result.plan,
            extra_text);
        const std::vector<std::uint8_t> miner_tx_bytes =
            hex_to_bytes(miner_tx.tx_blob_hex);

        std::vector<std::uint8_t> rebuilt;
        rebuilt.reserve(
            daemon_work.block_header.serialized_size +
            miner_tx_bytes.size() +
            (daemon_blob.size() - daemon_work.tx_hashes.serialized_offset));
        rebuilt.insert(
            rebuilt.end(),
            daemon_blob.begin(),
            daemon_blob.begin() + static_cast<std::ptrdiff_t>(
                daemon_work.block_header.serialized_size));
        rebuilt.insert(rebuilt.end(), miner_tx_bytes.begin(), miner_tx_bytes.end());
        rebuilt.insert(
            rebuilt.end(),
            daemon_blob.begin() + static_cast<std::ptrdiff_t>(
                daemon_work.tx_hashes.serialized_offset),
            daemon_blob.end());

        // The exact miner-tx adapter currently constructs the no-penalty HF6
        // reward branch. Do not publish a rebuilt candidate outside that audited
        // full-reward zone.
        if (rebuilt.size() > kZanoFullRewardZoneBytes) {
            result.status = PplnsTemplateStatus::BlockTooLarge;
            result.error = "rebuilt block exceeds the audited full-reward zone";
            return result;
        }

        result.mining_work = derive_mining_header_work(rebuilt);
        if (result.mining_work.block_header.serialized !=
                daemon_work.block_header.serialized ||
            result.mining_work.tx_hashes.hashes != daemon_work.tx_hashes.hashes) {
            result.status = PplnsTemplateStatus::InvalidDaemonTemplate;
            result.error = "PPLNS rebuild changed daemon header or regular transactions";
            return result;
        }

        result.block.blocktemplate_blob = bytes_to_hex(rebuilt);
        result.block.miner_tx_tgc_json = miner_tx.miner_tx_tgc_json;
        result.status = PplnsTemplateStatus::Ready;
        return result;
    } catch (const std::exception& error) {
        result.status = PplnsTemplateStatus::InvalidDaemonTemplate;
        result.error = error.what();
        return result;
    } catch (...) {
        result.status = PplnsTemplateStatus::InvalidDaemonTemplate;
        result.error = "unknown PPLNS template construction failure";
        return result;
    }
}

[[nodiscard]] inline const char* pplns_template_status_name(
    PplnsTemplateStatus status) noexcept {
    switch (status) {
    case PplnsTemplateStatus::Ready:
        return "ready";
    case PplnsTemplateStatus::PayoutPlanUnavailable:
        return "payout-plan-unavailable";
    case PplnsTemplateStatus::BackendUnavailable:
        return "backend-unavailable";
    case PplnsTemplateStatus::InvalidDaemonTemplate:
        return "invalid-daemon-template";
    case PplnsTemplateStatus::BlockTooLarge:
        return "block-too-large";
    }
    return "unknown";
}

}  // namespace zano_p2pool
