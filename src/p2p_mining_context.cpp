#include "zano_p2pool/p2p_mining_context.hpp"

#include "zano_p2pool/crypto_hash.hpp"
#include "zano_p2pool/mining_header.hpp"
#include "zano_p2pool/pow_target.hpp"

#include <json-c/json.h>

#include <algorithm>
#include <cctype>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace zano_p2pool {
namespace {

using JsonPtr = std::unique_ptr<json_object, decltype(&json_object_put)>;

void append_u32_be(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xffU));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xffU));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xffU));
    out.push_back(static_cast<std::uint8_t>(value & 0xffU));
}

void append_u64_be(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

[[nodiscard]] std::uint32_t read_u32_be(
    std::span<const std::uint8_t> bytes,
    std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4) {
        throw std::runtime_error("truncated P2P mining-context uint32");
    }
    return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
           static_cast<std::uint32_t>(bytes[offset + 3]);
}

[[nodiscard]] std::uint64_t read_u64_be(
    std::span<const std::uint8_t> bytes,
    std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 8) {
        throw std::runtime_error("truncated P2P mining-context uint64");
    }
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value = (value << 8) | static_cast<std::uint64_t>(bytes[offset + i]);
    }
    return value;
}

[[nodiscard]] std::size_t encoded_varint_size(std::uint64_t value) noexcept {
    std::size_t size = 1;
    while (value >= 0x80U) {
        value >>= 7;
        ++size;
    }
    return size;
}

[[nodiscard]] std::uint64_t read_varint(
    std::span<const std::uint8_t> bytes,
    std::size_t& offset) {
    std::uint64_t value = 0;
    unsigned shift = 0;
    const std::size_t start = offset;

    for (unsigned i = 0; i < 10; ++i) {
        if (offset >= bytes.size()) {
            throw std::runtime_error("truncated Zano varint in mining context");
        }
        const std::uint8_t byte = bytes[offset++];
        if (shift == 63 && (byte & 0x7eU) != 0U) {
            throw std::runtime_error("Zano varint overflows uint64 in mining context");
        }
        value |= static_cast<std::uint64_t>(byte & 0x7fU) << shift;
        if ((byte & 0x80U) == 0U) {
            if (offset - start != encoded_varint_size(value)) {
                throw std::runtime_error("non-canonical Zano varint in mining context");
            }
            return value;
        }
        shift += 7;
    }
    throw std::runtime_error("Zano varint too long in mining context");
}

[[nodiscard]] Hash256 hash_from_hex32(
    std::string_view hex,
    const char* field_name) {
    const auto bytes = hex_to_bytes(hex);
    if (bytes.size() != 32) {
        throw std::runtime_error(
            std::string("invalid 32-byte hex field: ") + field_name);
    }
    Hash256 result{};
    std::copy(bytes.begin(), bytes.end(), result.begin());
    return result;
}

void require_valid_tgc_json(std::string_view json_text) {
    if (json_text.empty()) {
        throw std::runtime_error("P2P mining context has empty miner_tx_tgc");
    }

    json_tokener* tokener = json_tokener_new();
    if (tokener == nullptr) {
        throw std::runtime_error("json_tokener_new failed");
    }
    json_object* parsed = json_tokener_parse_ex(
        tokener,
        json_text.data(),
        static_cast<int>(json_text.size()));
    const auto error = json_tokener_get_error(tokener);
    json_tokener_free(tokener);

    JsonPtr root(parsed, &json_object_put);
    if (error != json_tokener_success || !root ||
        json_object_get_type(root.get()) != json_type_object) {
        throw std::runtime_error("invalid P2P miner_tx_tgc JSON object");
    }

    json_object* tx_key = nullptr;
    if (!json_object_object_get_ex(root.get(), "tx_key", &tx_key) ||
        tx_key == nullptr || json_object_get_type(tx_key) != json_type_string) {
        throw std::runtime_error("P2P miner_tx_tgc has no string tx_key");
    }

    const std::string key = json_object_get_string(tx_key);
    if (key.empty() || (key.size() % 2) != 0 ||
        !std::all_of(key.begin(), key.end(), [](unsigned char ch) {
            return std::isxdigit(ch) != 0;
        })) {
        throw std::runtime_error("P2P miner_tx_tgc tx_key is not non-empty hex");
    }
}

void require_proposal_serializable(const P2pMiningContextProposal& proposal) {
    if (proposal.version != kP2pMiningContextVersion1) {
        throw std::runtime_error("unsupported P2P mining-context version");
    }
    if (difficulty128_is_zero(proposal.network_difficulty)) {
        throw std::runtime_error("P2P mining context has zero network difficulty");
    }
    if (proposal.block_template_blob.empty()) {
        throw std::runtime_error("P2P mining context has empty block template");
    }
    require_valid_tgc_json(proposal.miner_tx_tgc_json);

    if (proposal.block_template_blob.size() >
        kP2pMaxPayloadSize - kP2pMiningContextFixedPayloadSize) {
        throw std::runtime_error("P2P mining-context block blob is too large");
    }
    const std::size_t remaining =
        kP2pMaxPayloadSize - kP2pMiningContextFixedPayloadSize -
        proposal.block_template_blob.size();
    if (proposal.miner_tx_tgc_json.size() > remaining) {
        throw std::runtime_error("P2P mining-context payload exceeds maximum size");
    }
    if (proposal.block_template_blob.size() >
            std::numeric_limits<std::uint32_t>::max() ||
        proposal.miner_tx_tgc_json.size() >
            std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("P2P mining-context component exceeds uint32");
    }
}

[[nodiscard]] bool anchor_matches(
    const P2pMiningContextProposal& proposal,
    const P2pMiningAnchor& anchor) noexcept {
    return proposal.zano_height == anchor.zano_height &&
           proposal.prev_hash == anchor.prev_hash &&
           proposal.network_difficulty == anchor.network_difficulty &&
           proposal.seed == anchor.seed &&
           proposal.block_reward_without_fee == anchor.block_reward_without_fee;
}

void require_current_hf6_structural_bindings(
    const P2pMiningContextProposal& proposal,
    const MiningHeaderWork& work) {
    const auto blob = std::span<const std::uint8_t>(proposal.block_template_blob);
    if (blob.size() < 42) {
        throw std::runtime_error("P2P mining-context block blob is too short");
    }

    if (!std::all_of(blob.begin() + 1, blob.begin() + 9,
                     [](std::uint8_t byte) { return byte == 0; })) {
        throw std::runtime_error("P2P mining-context template nonce is not zero");
    }

    Hash256 serialized_prev{};
    std::copy_n(blob.begin() + 9, serialized_prev.size(), serialized_prev.begin());
    if (serialized_prev != proposal.prev_hash) {
        throw std::runtime_error(
            "P2P mining-context block previous hash does not match metadata");
    }

    std::size_t header_pos = 41;
    static_cast<void>(read_varint(blob, header_pos));  // minor_version
    static_cast<void>(read_varint(blob, header_pos));  // timestamp
    if (header_pos >= blob.size()) {
        throw std::runtime_error("truncated P2P mining-context block flags");
    }
    const std::uint8_t flags = blob[header_pos++];
    if (flags != 0) {
        throw std::runtime_error("P2P mining context is not a PoW block template");
    }
    if (header_pos != work.block_header.serialized_size) {
        throw std::runtime_error("P2P mining-context header boundary mismatch");
    }

    if (work.miner_tx_prefix.version != 4 ||
        work.miner_tx_prefix.hardfork_id != 6) {
        throw std::runtime_error("unsupported P2P mining-context miner transaction");
    }

    std::size_t tx_pos = work.block_header.serialized_size;
    const std::uint64_t tx_version = read_varint(blob, tx_pos);
    const std::uint64_t vin_count = read_varint(blob, tx_pos);
    if (tx_version != 4 || vin_count != 1 || tx_pos >= blob.size()) {
        throw std::runtime_error("invalid current P2P mining-context coinbase inputs");
    }
    const std::uint8_t input_tag = blob[tx_pos++];
    if (input_tag != 0) {
        throw std::runtime_error("P2P mining-context coinbase is not txin_gen");
    }
    const std::uint64_t miner_height = read_varint(blob, tx_pos);
    if (miner_height != proposal.zano_height) {
        throw std::runtime_error("P2P mining-context coinbase height mismatch");
    }
}

}  // namespace

P2pMiningAnchor p2p_mining_anchor_from_template(
    const BlockTemplate& block_template) {
    P2pMiningAnchor anchor;
    anchor.zano_height = block_template.height;
    anchor.prev_hash = hash_from_hex32(block_template.prev_hash, "prev_hash");
    anchor.network_difficulty =
        difficulty128_from_decimal(block_template.difficulty);
    anchor.seed = hash_from_hex32(block_template.seed, "seed");
    anchor.block_reward_without_fee = block_template.block_reward_without_fee;
    return anchor;
}

P2pMiningContextProposal p2p_mining_context_proposal_from_template(
    const BlockTemplate& block_template) {
    P2pMiningContextProposal proposal;
    proposal.zano_height = block_template.height;
    proposal.prev_hash = hash_from_hex32(block_template.prev_hash, "prev_hash");
    proposal.network_difficulty =
        difficulty128_from_decimal(block_template.difficulty);
    proposal.seed = hash_from_hex32(block_template.seed, "seed");
    proposal.block_reward_without_fee = block_template.block_reward_without_fee;
    proposal.block_reward = block_template.block_reward;
    proposal.txs_fee = block_template.txs_fee;
    proposal.block_template_blob = hex_to_bytes(block_template.blocktemplate_blob);
    proposal.miner_tx_tgc_json = block_template.miner_tx_tgc_json;
    require_proposal_serializable(proposal);
    return proposal;
}

std::vector<std::uint8_t> serialize_p2p_mining_context_payload(
    const P2pMiningContextProposal& proposal) {
    require_proposal_serializable(proposal);

    std::vector<std::uint8_t> out;
    out.reserve(
        kP2pMiningContextFixedPayloadSize +
        proposal.block_template_blob.size() +
        proposal.miner_tx_tgc_json.size());

    out.push_back(proposal.version);
    append_u64_be(out, proposal.zano_height);
    out.insert(out.end(), proposal.prev_hash.begin(), proposal.prev_hash.end());
    out.insert(
        out.end(),
        proposal.network_difficulty.begin(),
        proposal.network_difficulty.end());
    out.insert(out.end(), proposal.seed.begin(), proposal.seed.end());
    append_u64_be(out, proposal.block_reward_without_fee);
    append_u64_be(out, proposal.block_reward);
    append_u64_be(out, proposal.txs_fee);
    append_u32_be(
        out, static_cast<std::uint32_t>(proposal.block_template_blob.size()));
    append_u32_be(
        out, static_cast<std::uint32_t>(proposal.miner_tx_tgc_json.size()));
    out.insert(
        out.end(),
        proposal.block_template_blob.begin(),
        proposal.block_template_blob.end());
    out.insert(
        out.end(),
        proposal.miner_tx_tgc_json.begin(),
        proposal.miner_tx_tgc_json.end());

    if (out.size() > kP2pMaxPayloadSize) {
        throw std::runtime_error("internal P2P mining-context size mismatch");
    }
    return out;
}

P2pMiningContextProposal deserialize_p2p_mining_context_payload(
    std::span<const std::uint8_t> payload) {
    if (payload.size() < kP2pMiningContextFixedPayloadSize ||
        payload.size() > kP2pMaxPayloadSize) {
        throw std::runtime_error("invalid P2P mining-context payload size");
    }

    P2pMiningContextProposal proposal;
    std::size_t offset = 0;
    proposal.version = payload[offset++];
    if (proposal.version != kP2pMiningContextVersion1) {
        throw std::runtime_error("unsupported P2P mining-context version");
    }
    proposal.zano_height = read_u64_be(payload, offset);
    offset += 8;
    std::copy_n(payload.begin() + static_cast<std::ptrdiff_t>(offset),
                proposal.prev_hash.size(), proposal.prev_hash.begin());
    offset += proposal.prev_hash.size();
    std::copy_n(payload.begin() + static_cast<std::ptrdiff_t>(offset),
                proposal.network_difficulty.size(),
                proposal.network_difficulty.begin());
    offset += proposal.network_difficulty.size();
    std::copy_n(payload.begin() + static_cast<std::ptrdiff_t>(offset),
                proposal.seed.size(), proposal.seed.begin());
    offset += proposal.seed.size();
    proposal.block_reward_without_fee = read_u64_be(payload, offset);
    offset += 8;
    proposal.block_reward = read_u64_be(payload, offset);
    offset += 8;
    proposal.txs_fee = read_u64_be(payload, offset);
    offset += 8;
    const std::uint32_t block_blob_size = read_u32_be(payload, offset);
    offset += 4;
    const std::uint32_t tgc_size = read_u32_be(payload, offset);
    offset += 4;

    const std::size_t expected_variable_size =
        static_cast<std::size_t>(block_blob_size) +
        static_cast<std::size_t>(tgc_size);
    if (expected_variable_size != payload.size() - offset) {
        throw std::runtime_error("P2P mining-context component lengths mismatch");
    }

    proposal.block_template_blob.assign(
        payload.begin() + static_cast<std::ptrdiff_t>(offset),
        payload.begin() + static_cast<std::ptrdiff_t>(
            offset + static_cast<std::size_t>(block_blob_size)));
    offset += block_blob_size;
    proposal.miner_tx_tgc_json.assign(
        reinterpret_cast<const char*>(payload.data() + offset),
        static_cast<std::size_t>(tgc_size));

    require_proposal_serializable(proposal);
    return proposal;
}

P2pEnvelope make_p2p_mining_context_envelope(
    const P2pMiningContextProposal& proposal) {
    P2pEnvelope envelope;
    envelope.type = P2pMessageType::MiningContextAnnounce;
    envelope.payload = serialize_p2p_mining_context_payload(proposal);
    return envelope;
}

P2pMiningContextProposal parse_p2p_mining_context_envelope(
    const P2pEnvelope& envelope) {
    if (envelope.version != kP2pProtocolVersion) {
        throw std::runtime_error("unsupported P2P protocol version");
    }
    if (envelope.type != P2pMessageType::MiningContextAnnounce) {
        throw std::runtime_error("P2P envelope is not a mining-context announcement");
    }
    if (envelope.flags != 0) {
        throw std::runtime_error("unsupported P2P envelope flags");
    }
    return deserialize_p2p_mining_context_payload(envelope.payload);
}

P2pMiningContextId p2p_mining_context_id(
    const P2pMiningContextProposal& proposal) {
    const auto payload = serialize_p2p_mining_context_payload(proposal);
    return cn_fast_hash(payload);
}

P2pMiningContextCheckResult inspect_p2p_mining_context(
    const P2pHandshake& peer,
    const P2pEnvelope& envelope,
    const P2pMiningAnchor& local_anchor) {
    P2pMiningContextCheckResult result;
    if ((peer.capabilities & kP2pCapabilityMiningContext) == 0) {
        result.status = P2pMiningContextCheckStatus::CapabilityMissing;
        return result;
    }

    const P2pMiningContextProposal proposal =
        parse_p2p_mining_context_envelope(envelope);
    result.proposal_id = p2p_mining_context_id(proposal);

    if (!anchor_matches(proposal, local_anchor)) {
        result.status = P2pMiningContextCheckStatus::AnchorMismatch;
        return result;
    }

    const MiningHeaderWork work =
        derive_mining_header_work(proposal.block_template_blob);
    require_current_hf6_structural_bindings(proposal, work);
    require_valid_tgc_json(proposal.miner_tx_tgc_json);

    result.status = P2pMiningContextCheckStatus::AnchoredUnverifiedMinerTx;
    result.mining_header_hash = work.header_hash;
    result.regular_transaction_count = work.tx_hashes.hashes.size();
    return result;
}

const char* p2p_mining_context_check_status_name(
    P2pMiningContextCheckStatus status) noexcept {
    switch (status) {
    case P2pMiningContextCheckStatus::AnchoredUnverifiedMinerTx:
        return "anchored-unverified-miner-tx";
    case P2pMiningContextCheckStatus::CapabilityMissing:
        return "capability-missing";
    case P2pMiningContextCheckStatus::AnchorMismatch:
        return "anchor-mismatch";
    }
    return "unknown";
}

}  // namespace zano_p2pool
