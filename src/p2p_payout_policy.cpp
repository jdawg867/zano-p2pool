#include "zano_p2pool/p2p_payout_policy.hpp"

#include "zano_p2pool/crypto_hash.hpp"
#include "zano_p2pool/mining_header.hpp"

#include <json-c/json.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace zano_p2pool {
namespace {

using JsonPtr = std::unique_ptr<json_object, decltype(&json_object_put)>;

constexpr std::size_t kCurrentMinerMinOutputs = 2;
constexpr std::size_t kCurrentMinerMaxOutputs = 32;

// Zano currency::native_coin_asset_id_1div8 at audited commit
// 1508cf6ae3ef44a52d66137d30f800b06ce917ee.
constexpr ZanoCurveKey kNativeCoinAssetId1Div8{
    0x74, 0xc3, 0x2d, 0x3e, 0xaa, 0xfa, 0xfc, 0x62,
    0x3b, 0xf4, 0x83, 0xe8, 0x58, 0xd4, 0x2e, 0x8b,
    0xf4, 0xec, 0x7d, 0xf0, 0x64, 0xad, 0xa2, 0xe3,
    0x49, 0x34, 0x46, 0x9c, 0xff, 0x6b, 0x62, 0x68,
};

class Cursor {
public:
    explicit Cursor(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

    [[nodiscard]] std::uint8_t read_u8() {
        require(1);
        return bytes_[offset_++];
    }

    [[nodiscard]] std::uint64_t read_varint() {
        std::uint64_t value = 0;
        unsigned shift = 0;
        const std::size_t start = offset_;

        for (unsigned i = 0; i < 10; ++i) {
            const std::uint8_t byte = read_u8();
            if (shift == 63 && (byte & 0x7eU) != 0U) {
                throw std::runtime_error("miner-tx varint overflows uint64");
            }
            value |= static_cast<std::uint64_t>(byte & 0x7fU) << shift;
            if ((byte & 0x80U) == 0U) {
                std::size_t expected = 1;
                std::uint64_t tmp = value;
                while (tmp >= 0x80U) {
                    tmp >>= 7;
                    ++expected;
                }
                if (offset_ - start != expected) {
                    throw std::runtime_error("non-canonical miner-tx varint");
                }
                return value;
            }
            shift += 7;
        }
        throw std::runtime_error("miner-tx varint is too long");
    }

    void skip(std::size_t count) {
        require(count);
        offset_ += count;
    }

    void skip_blob() {
        const std::uint64_t count = read_varint();
        if (count > std::numeric_limits<std::size_t>::max()) {
            throw std::runtime_error("miner-tx blob length does not fit size_t");
        }
        skip(static_cast<std::size_t>(count));
    }

    [[nodiscard]] ZanoCurveKey read_key() {
        require(32);
        ZanoCurveKey result{};
        std::copy_n(
            bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
            result.size(),
            result.begin());
        offset_ += result.size();
        return result;
    }

    [[nodiscard]] bool at_end() const noexcept {
        return offset_ == bytes_.size();
    }

private:
    void require(std::size_t count) const {
        if (offset_ > bytes_.size() || count > bytes_.size() - offset_) {
            throw std::runtime_error("truncated miner transaction prefix");
        }
    }

    std::span<const std::uint8_t> bytes_;
    std::size_t offset_{0};
};

void skip_current_extra(Cursor& cursor, std::uint8_t tag) {
    switch (tag) {
    case 11:  // tx_derivation_hint
    case 19:  // extra_user_data
    case 21:  // extra_padding
        cursor.skip_blob();
        return;
    case 14:  // unlock time
    case 15:  // expiration time
    case 16:  // flags
    case 78:  // coinbase cumulative size
        static_cast<void>(cursor.read_varint());
        return;
    case 18:  // attachment info
        static_cast<void>(cursor.read_varint());
        cursor.skip(32);
        static_cast<void>(cursor.read_varint());
        return;
    case 22:  // transaction public key
        cursor.skip(32);
        return;
    default:
        throw std::runtime_error("unsupported current miner-tx extra tag");
    }
}

struct ParsedPayoutOutput {
    ZanoCurveKey stealth_address{};
    ZanoCurveKey concealing_point{};
    ZanoCurveKey amount_commitment{};
    ZanoCurveKey blinded_asset_id{};
};

[[nodiscard]] std::vector<ParsedPayoutOutput> parse_current_outputs(
    std::span<const std::uint8_t> miner_tx_prefix) {
    Cursor cursor(miner_tx_prefix);

    if (cursor.read_varint() != 4) {
        throw std::runtime_error("unsupported miner transaction version");
    }
    if (cursor.read_varint() != 1 || cursor.read_u8() != 0) {
        throw std::runtime_error("current PoW miner transaction must have txin_gen");
    }
    static_cast<void>(cursor.read_varint());  // txin_gen.height

    const std::uint64_t extra_count = cursor.read_varint();
    if (extra_count > 1024) {
        throw std::runtime_error("unreasonable miner transaction extra count");
    }
    for (std::uint64_t i = 0; i < extra_count; ++i) {
        skip_current_extra(cursor, cursor.read_u8());
    }

    const std::uint64_t output_count = cursor.read_varint();
    if (output_count > kCurrentMinerMaxOutputs) {
        throw std::runtime_error("miner transaction has too many outputs");
    }

    std::vector<ParsedPayoutOutput> outputs;
    outputs.reserve(static_cast<std::size_t>(output_count));
    for (std::uint64_t i = 0; i < output_count; ++i) {
        if (cursor.read_u8() != 63) {
            throw std::runtime_error("current miner output is not tx_out_zarcanum");
        }
        if (cursor.read_u8() != 0) {
            throw std::runtime_error("unsupported tx_out_zarcanum version");
        }

        ParsedPayoutOutput output;
        output.stealth_address = cursor.read_key();
        output.concealing_point = cursor.read_key();
        output.amount_commitment = cursor.read_key();
        output.blinded_asset_id = cursor.read_key();
        cursor.skip(8 + 8 + 1);  // encrypted amount/payment id + mix_attr
        outputs.push_back(output);
    }

    // Current HF6 miner transaction prefix carries hardfork_id = 6.
    if (cursor.read_u8() != 6 || !cursor.at_end()) {
        throw std::runtime_error("unexpected current miner hardfork/trailing data");
    }
    return outputs;
}

[[nodiscard]] JsonPtr parse_tgc_json(std::string_view json_text) {
    if (json_text.empty() ||
        json_text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("invalid miner_tx_tgc JSON size");
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
        throw std::runtime_error("invalid miner_tx_tgc JSON");
    }
    return root;
}

[[nodiscard]] std::string get_string_field(
    json_object* root,
    const char* name) {
    json_object* value = nullptr;
    if (!json_object_object_get_ex(root, name, &value) ||
        value == nullptr || json_object_get_type(value) != json_type_string) {
        throw std::runtime_error(std::string("missing TGC string field: ") + name);
    }
    return json_object_get_string(value);
}

[[nodiscard]] std::vector<ZanoCurveKey> parse_scalar_list(
    std::string_view text) {
    std::vector<ZanoCurveKey> result;
    std::size_t pos = 0;

    while (pos < text.size()) {
        while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t')) {
            ++pos;
        }
        if (pos == text.size()) {
            break;
        }

        const std::size_t comma = text.find(',', pos);
        std::size_t end = comma == std::string_view::npos ? text.size() : comma;
        while (end > pos && (text[end - 1] == ' ' || text[end - 1] == '\t')) {
            --end;
        }
        if (end == pos) {
            throw std::runtime_error("empty scalar in TGC list");
        }

        const auto bytes = hex_to_bytes(text.substr(pos, end - pos));
        if (bytes.size() != 32) {
            throw std::runtime_error("TGC scalar must be exactly 32 bytes");
        }
        ZanoCurveKey scalar{};
        std::copy(bytes.begin(), bytes.end(), scalar.begin());
        result.push_back(scalar);

        if (comma == std::string_view::npos) {
            pos = text.size();
        } else {
            pos = comma + 1;
        }
    }
    return result;
}

[[nodiscard]] std::uint64_t scalar_to_u64(const ZanoCurveKey& scalar) {
    if (std::any_of(
            scalar.begin() + 8,
            scalar.end(),
            [](std::uint8_t byte) { return byte != 0; })) {
        throw std::runtime_error("TGC amount does not fit uint64");
    }
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(scalar[i]) << (8 * i);
    }
    return value;
}

struct ParsedTgcPayoutData {
    ZanoCurveKey tx_secret_key{};
    std::vector<std::uint64_t> amounts;
    std::vector<ZanoCurveKey> amount_blinding_masks;
    std::vector<ZanoCurveKey> asset_id_blinding_masks;
};

[[nodiscard]] ParsedTgcPayoutData parse_tgc_payout_data(
    std::string_view json_text) {
    JsonPtr root = parse_tgc_json(json_text);

    const auto tx_key_bytes = hex_to_bytes(get_string_field(root.get(), "tx_key"));
    if (tx_key_bytes.size() != 64) {
        throw std::runtime_error("TGC tx_key must be exactly 64 bytes");
    }

    ParsedTgcPayoutData result;
    std::copy_n(tx_key_bytes.begin() + 32, 32, result.tx_secret_key.begin());

    const auto amount_scalars =
        parse_scalar_list(get_string_field(root.get(), "amounts"));
    result.amounts.reserve(amount_scalars.size());
    for (const auto& scalar : amount_scalars) {
        result.amounts.push_back(scalar_to_u64(scalar));
    }

    result.amount_blinding_masks = parse_scalar_list(
        get_string_field(root.get(), "amount_blinding_masks"));
    result.asset_id_blinding_masks = parse_scalar_list(
        get_string_field(root.get(), "asset_id_blinding_masks"));
    return result;
}

[[nodiscard]] bool is_zero_key(const ZanoCurveKey& key) noexcept {
    return std::all_of(
        key.begin(), key.end(), [](std::uint8_t byte) { return byte == 0; });
}

}  // namespace

P2pPayoutPolicyResult verify_miner_tx_payout_policy(
    std::span<const std::uint8_t> miner_tx_prefix,
    std::string_view miner_tx_tgc_json,
    std::uint64_t block_reward_without_fee,
    std::uint64_t block_reward,
    std::uint64_t txs_fee,
    const P2pPayoutAddress& expected_payout) noexcept {
    P2pPayoutPolicyResult result;

    const auto binding = verify_miner_tx_tgc_key_binding(
        miner_tx_prefix, miner_tx_tgc_json);
    result.binding_status = binding.status;
    if (binding.status != P2pMinerTxBindingStatus::Verified) {
        result.status = binding.status == P2pMinerTxBindingStatus::BackendUnavailable
            ? P2pPayoutPolicyStatus::BackendUnavailable
            : P2pPayoutPolicyStatus::KeyBindingFailed;
        return result;
    }

    if (!zano_curve_backend_available()) {
        result.status = P2pPayoutPolicyStatus::BackendUnavailable;
        return result;
    }

    // Since HF4, Zano burns transaction fees instead of adding them to the
    // miner reward. txs_fee is intentionally not added here.
    static_cast<void>(txs_fee);
    if (block_reward_without_fee == 0 ||
        block_reward != block_reward_without_fee) {
        result.status = P2pPayoutPolicyStatus::RewardMetadataMismatch;
        return result;
    }

    std::vector<ParsedPayoutOutput> outputs;
    try {
        outputs = parse_current_outputs(miner_tx_prefix);
    } catch (...) {
        result.status = P2pPayoutPolicyStatus::MalformedMinerTxPrefix;
        return result;
    }
    result.output_count = outputs.size();
    if (outputs.size() < kCurrentMinerMinOutputs ||
        outputs.size() > kCurrentMinerMaxOutputs) {
        result.status = P2pPayoutPolicyStatus::InvalidOutputCount;
        return result;
    }

    ParsedTgcPayoutData tgc;
    try {
        tgc = parse_tgc_payout_data(miner_tx_tgc_json);
    } catch (...) {
        result.status = P2pPayoutPolicyStatus::MalformedTgc;
        return result;
    }

    if (tgc.amounts.size() != outputs.size() ||
        tgc.amount_blinding_masks.size() != outputs.size() ||
        tgc.asset_id_blinding_masks.size() != outputs.size()) {
        result.status = P2pPayoutPolicyStatus::TgcOutputCountMismatch;
        return result;
    }

    std::uint64_t reward_sum = 0;
    for (std::size_t i = 0; i < outputs.size(); ++i) {
        const auto& output = outputs[i];

        if (output.blinded_asset_id != kNativeCoinAssetId1Div8 ||
            !is_zero_key(tgc.asset_id_blinding_masks[i])) {
            result.status = P2pPayoutPolicyStatus::NonNativeAsset;
            return result;
        }

        ZanoCurveKey expected_stealth{};
        if (!zano_derive_output_public_key(
                tgc.tx_secret_key,
                expected_payout.spend_public_key,
                expected_payout.view_public_key,
                i,
                expected_stealth) ||
            expected_stealth != output.stealth_address) {
            result.status = P2pPayoutPolicyStatus::DestinationMismatch;
            return result;
        }

        if (!zano_amount_commitment_matches(
                tgc.amounts[i],
                tgc.amount_blinding_masks[i],
                output.blinded_asset_id,
                output.amount_commitment)) {
            result.status = P2pPayoutPolicyStatus::AmountCommitmentMismatch;
            return result;
        }

        if (tgc.amounts[i] > std::numeric_limits<std::uint64_t>::max() - reward_sum) {
            result.status = P2pPayoutPolicyStatus::RewardSumMismatch;
            return result;
        }
        reward_sum += tgc.amounts[i];
    }

    result.verified_reward = reward_sum;
    if (reward_sum != block_reward) {
        result.status = P2pPayoutPolicyStatus::RewardSumMismatch;
        return result;
    }

    result.status = P2pPayoutPolicyStatus::Verified;
    return result;
}

P2pPayoutPolicyResult verify_miner_tx_payout_policy(
    std::span<const std::uint8_t> miner_tx_prefix,
    std::string_view miner_tx_tgc_json,
    std::uint64_t block_reward_without_fee,
    std::uint64_t block_reward,
    std::uint64_t txs_fee,
    const PplnsCoinbasePlan& expected_plan) noexcept {
    P2pPayoutPolicyResult result;

    const auto binding = verify_miner_tx_tgc_key_binding(
        miner_tx_prefix, miner_tx_tgc_json);
    result.binding_status = binding.status;
    if (binding.status != P2pMinerTxBindingStatus::Verified) {
        result.status = binding.status == P2pMinerTxBindingStatus::BackendUnavailable
            ? P2pPayoutPolicyStatus::BackendUnavailable
            : P2pPayoutPolicyStatus::KeyBindingFailed;
        return result;
    }

    if (!zano_curve_backend_available()) {
        result.status = P2pPayoutPolicyStatus::BackendUnavailable;
        return result;
    }

    static_cast<void>(txs_fee);
    if (block_reward_without_fee == 0 ||
        block_reward != block_reward_without_fee) {
        result.status = P2pPayoutPolicyStatus::RewardMetadataMismatch;
        return result;
    }

    if (expected_plan.status != PplnsCoinbasePlanStatus::Ready ||
        expected_plan.reward_atomic != block_reward ||
        expected_plan.destinations.empty() ||
        expected_plan.destinations.size() > kCurrentMinerMaxOutputs) {
        result.status = P2pPayoutPolicyStatus::PayoutPlanMismatch;
        return result;
    }

    std::uint64_t expected_sum = 0;
    for (std::size_t i = 0; i < expected_plan.destinations.size(); ++i) {
        const auto& destination = expected_plan.destinations[i];
        if (destination.amount == 0 ||
            miner_id_from_payout(destination.payout) != destination.miner_id ||
            destination.amount >
                std::numeric_limits<std::uint64_t>::max() - expected_sum) {
            result.status = P2pPayoutPolicyStatus::PayoutPlanMismatch;
            return result;
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (expected_plan.destinations[j].payout == destination.payout) {
                result.status = P2pPayoutPolicyStatus::PayoutPlanMismatch;
                return result;
            }
        }
        expected_sum += destination.amount;
    }
    if (expected_sum != expected_plan.reward_atomic) {
        result.status = P2pPayoutPolicyStatus::PayoutPlanMismatch;
        return result;
    }

    std::vector<ParsedPayoutOutput> outputs;
    try {
        outputs = parse_current_outputs(miner_tx_prefix);
    } catch (...) {
        result.status = P2pPayoutPolicyStatus::MalformedMinerTxPrefix;
        return result;
    }
    result.output_count = outputs.size();
    if (outputs.size() < kCurrentMinerMinOutputs ||
        outputs.size() > kCurrentMinerMaxOutputs) {
        result.status = P2pPayoutPolicyStatus::InvalidOutputCount;
        return result;
    }

    ParsedTgcPayoutData tgc;
    try {
        tgc = parse_tgc_payout_data(miner_tx_tgc_json);
    } catch (...) {
        result.status = P2pPayoutPolicyStatus::MalformedTgc;
        return result;
    }

    if (tgc.amounts.size() != outputs.size() ||
        tgc.amount_blinding_masks.size() != outputs.size() ||
        tgc.asset_id_blinding_masks.size() != outputs.size()) {
        result.status = P2pPayoutPolicyStatus::TgcOutputCountMismatch;
        return result;
    }

    std::vector<std::uint64_t> credited(
        expected_plan.destinations.size(), 0);
    std::uint64_t reward_sum = 0;

    for (std::size_t i = 0; i < outputs.size(); ++i) {
        const auto& output = outputs[i];
        if (output.blinded_asset_id != kNativeCoinAssetId1Div8 ||
            !is_zero_key(tgc.asset_id_blinding_masks[i])) {
            result.status = P2pPayoutPolicyStatus::NonNativeAsset;
            return result;
        }

        std::size_t matched_destination = expected_plan.destinations.size();
        for (std::size_t j = 0; j < expected_plan.destinations.size(); ++j) {
            const auto& payout = expected_plan.destinations[j].payout;
            ZanoCurveKey expected_stealth{};
            if (!zano_derive_output_public_key(
                    tgc.tx_secret_key,
                    payout.spend_public_key,
                    payout.view_public_key,
                    i,
                    expected_stealth)) {
                result.status = P2pPayoutPolicyStatus::DestinationMismatch;
                return result;
            }
            if (expected_stealth == output.stealth_address) {
                if (matched_destination != expected_plan.destinations.size()) {
                    result.status = P2pPayoutPolicyStatus::PayoutPlanMismatch;
                    return result;
                }
                matched_destination = j;
            }
        }
        if (matched_destination == expected_plan.destinations.size()) {
            result.status = P2pPayoutPolicyStatus::DestinationMismatch;
            return result;
        }

        if (!zano_amount_commitment_matches(
                tgc.amounts[i],
                tgc.amount_blinding_masks[i],
                output.blinded_asset_id,
                output.amount_commitment)) {
            result.status = P2pPayoutPolicyStatus::AmountCommitmentMismatch;
            return result;
        }

        if (tgc.amounts[i] >
                std::numeric_limits<std::uint64_t>::max() - reward_sum ||
            tgc.amounts[i] >
                std::numeric_limits<std::uint64_t>::max() -
                    credited[matched_destination]) {
            result.status = P2pPayoutPolicyStatus::RewardSumMismatch;
            return result;
        }
        reward_sum += tgc.amounts[i];
        credited[matched_destination] += tgc.amounts[i];
    }

    result.verified_reward = reward_sum;
    if (reward_sum != block_reward) {
        result.status = P2pPayoutPolicyStatus::RewardSumMismatch;
        return result;
    }
    for (std::size_t i = 0; i < expected_plan.destinations.size(); ++i) {
        if (credited[i] != expected_plan.destinations[i].amount) {
            result.status = P2pPayoutPolicyStatus::PayoutPlanMismatch;
            return result;
        }
    }

    result.status = P2pPayoutPolicyStatus::Verified;
    return result;
}

P2pPayoutPolicyResult verify_p2p_mining_context_payout_policy(
    const P2pMiningContextProposal& proposal,
    const P2pMiningContextCheckResult& anchored_check,
    const P2pPayoutAddress& expected_payout) noexcept {
    P2pPayoutPolicyResult result;
    if (anchored_check.status !=
            P2pMiningContextCheckStatus::AnchoredUnverifiedMinerTx ||
        anchored_check.proposal_id != p2p_mining_context_id(proposal)) {
        result.status = P2pPayoutPolicyStatus::NotAnchored;
        return result;
    }

    try {
        const auto work = derive_mining_header_work(proposal.block_template_blob);
        if (work.header_hash != anchored_check.mining_header_hash) {
            result.status = P2pPayoutPolicyStatus::NotAnchored;
            return result;
        }
        return verify_miner_tx_payout_policy(
            work.miner_tx_prefix.serialized,
            proposal.miner_tx_tgc_json,
            proposal.block_reward_without_fee,
            proposal.block_reward,
            proposal.txs_fee,
            expected_payout);
    } catch (...) {
        result.status = P2pPayoutPolicyStatus::MalformedMinerTxPrefix;
        return result;
    }
}

P2pPayoutPolicyResult verify_p2p_mining_context_payout_policy(
    const P2pMiningContextProposal& proposal,
    const P2pMiningContextCheckResult& anchored_check,
    const PplnsCoinbasePlan& expected_plan) noexcept {
    P2pPayoutPolicyResult result;
    if (anchored_check.status !=
            P2pMiningContextCheckStatus::AnchoredUnverifiedMinerTx ||
        anchored_check.proposal_id != p2p_mining_context_id(proposal)) {
        result.status = P2pPayoutPolicyStatus::NotAnchored;
        return result;
    }

    try {
        const auto work = derive_mining_header_work(proposal.block_template_blob);
        if (work.header_hash != anchored_check.mining_header_hash) {
            result.status = P2pPayoutPolicyStatus::NotAnchored;
            return result;
        }
        return verify_miner_tx_payout_policy(
            work.miner_tx_prefix.serialized,
            proposal.miner_tx_tgc_json,
            proposal.block_reward_without_fee,
            proposal.block_reward,
            proposal.txs_fee,
            expected_plan);
    } catch (...) {
        result.status = P2pPayoutPolicyStatus::MalformedMinerTxPrefix;
        return result;
    }
}

const char* p2p_payout_policy_status_name(
    P2pPayoutPolicyStatus status) noexcept {
    switch (status) {
    case P2pPayoutPolicyStatus::Verified:
        return "verified";
    case P2pPayoutPolicyStatus::NotAnchored:
        return "not-anchored";
    case P2pPayoutPolicyStatus::KeyBindingFailed:
        return "key-binding-failed";
    case P2pPayoutPolicyStatus::BackendUnavailable:
        return "backend-unavailable";
    case P2pPayoutPolicyStatus::MalformedTgc:
        return "malformed-tgc";
    case P2pPayoutPolicyStatus::MalformedMinerTxPrefix:
        return "malformed-miner-tx-prefix";
    case P2pPayoutPolicyStatus::InvalidOutputCount:
        return "invalid-output-count";
    case P2pPayoutPolicyStatus::RewardMetadataMismatch:
        return "reward-metadata-mismatch";
    case P2pPayoutPolicyStatus::TgcOutputCountMismatch:
        return "tgc-output-count-mismatch";
    case P2pPayoutPolicyStatus::NonNativeAsset:
        return "non-native-asset";
    case P2pPayoutPolicyStatus::DestinationMismatch:
        return "destination-mismatch";
    case P2pPayoutPolicyStatus::AmountCommitmentMismatch:
        return "amount-commitment-mismatch";
    case P2pPayoutPolicyStatus::RewardSumMismatch:
        return "reward-sum-mismatch";
    case P2pPayoutPolicyStatus::PayoutPlanMismatch:
        return "payout-plan-mismatch";
    }
    return "unknown";
}

}  // namespace zano_p2pool
