#include "zano_p2pool/p2p_miner_tx_proofs.hpp"

#include "zano_p2pool/mining_header.hpp"
#include "zano_p2pool/zano_curve.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace zano_p2pool {
namespace {

constexpr std::size_t kBalanceProofPayloadSize = 96;
constexpr std::size_t kBalanceProofSerializedSize = 1 + kBalanceProofPayloadSize;
constexpr std::uint8_t kRangeProofTag = 47;
constexpr std::uint8_t kBalanceProofTag = 48;
constexpr std::size_t kCurrentMinerMinOutputs = 2;
constexpr std::size_t kCurrentMinerMaxOutputs = 32;
constexpr std::size_t kBppLog2N = 6;  // bpp_crypto_trait_ZC_out uses N=64.

class Cursor {
public:
    explicit Cursor(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

    [[nodiscard]] std::size_t position() const noexcept { return offset_; }

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

    [[nodiscard]] std::vector<ZanoCurveKey> read_key_vector(
        std::size_t max_count) {
        const std::uint64_t count = read_varint();
        if (count > max_count) {
            throw std::runtime_error("proof vector exceeds current HF6 bound");
        }
        std::vector<ZanoCurveKey> result;
        result.reserve(static_cast<std::size_t>(count));
        for (std::uint64_t i = 0; i < count; ++i) {
            result.push_back(read_key());
        }
        return result;
    }

    [[nodiscard]] bool at_end() const noexcept {
        return offset_ == bytes_.size();
    }

private:
    void require(std::size_t count) const {
        if (offset_ > bytes_.size() || count > bytes_.size() - offset_) {
            throw std::runtime_error("truncated miner transaction/proof data");
        }
    }

    std::span<const std::uint8_t> bytes_;
    std::size_t offset_{0};
};

void parse_current_extra(
    Cursor& cursor,
    std::uint8_t tag,
    bool& found_tx_public_key,
    ZanoCurveKey& tx_public_key) {
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
        if (found_tx_public_key) {
            throw std::runtime_error("multiple miner transaction public keys");
        }
        tx_public_key = cursor.read_key();
        found_tx_public_key = true;
        return;
    default:
        throw std::runtime_error(
            "unsupported current miner-tx extra tag: " + std::to_string(tag));
    }
}

struct ParsedProofInputs {
    ZanoCurveKey tx_public_key{};
    std::vector<ZanoCurveKey> amount_commitments;
    std::vector<ZanoCurveKey> blinded_asset_ids;
};

[[nodiscard]] ParsedProofInputs parse_proof_inputs(
    std::span<const std::uint8_t> miner_tx_prefix) {
    Cursor cursor(miner_tx_prefix);
    if (cursor.read_varint() != 4) {
        throw std::runtime_error("unsupported miner transaction version");
    }
    if (cursor.read_varint() != 1 || cursor.read_u8() != 0) {
        throw std::runtime_error("current PoW miner transaction must have txin_gen");
    }
    static_cast<void>(cursor.read_varint());  // txin_gen.height

    ParsedProofInputs result;
    bool found_tx_public_key = false;
    const std::uint64_t extra_count = cursor.read_varint();
    if (extra_count > 1024) {
        throw std::runtime_error("unreasonable miner transaction extra count");
    }
    for (std::uint64_t i = 0; i < extra_count; ++i) {
        parse_current_extra(
            cursor,
            cursor.read_u8(),
            found_tx_public_key,
            result.tx_public_key);
    }
    if (!found_tx_public_key) {
        throw std::runtime_error("miner transaction has no public key");
    }

    const std::uint64_t output_count = cursor.read_varint();
    if (output_count < kCurrentMinerMinOutputs ||
        output_count > kCurrentMinerMaxOutputs) {
        throw std::runtime_error("invalid current miner output count");
    }
    result.amount_commitments.reserve(static_cast<std::size_t>(output_count));
    result.blinded_asset_ids.reserve(static_cast<std::size_t>(output_count));

    for (std::uint64_t i = 0; i < output_count; ++i) {
        if (cursor.read_u8() != 63 || cursor.read_u8() != 0) {
            throw std::runtime_error("unsupported current miner output");
        }
        cursor.skip(32);  // stealth_address
        cursor.skip(32);  // concealing_point
        result.amount_commitments.push_back(cursor.read_key());
        result.blinded_asset_ids.push_back(cursor.read_key());
        cursor.skip(8 + 8 + 1);  // encrypted fields + mix_attr
    }

    if (cursor.read_u8() != 6 || !cursor.at_end()) {
        throw std::runtime_error("unexpected miner hardfork/trailing prefix data");
    }
    return result;
}

[[nodiscard]] std::size_t ceil_log2(std::size_t value) noexcept {
    if (value <= 1) {
        return 0;
    }
    --value;
    std::size_t result = 0;
    while (value != 0) {
        value >>= 1;
        ++result;
    }
    return result;
}

struct ParsedRangeProof {
    ZanoBppSignature bpp;
    ZanoUgAggregationProof aggregation;
};

[[nodiscard]] ParsedRangeProof parse_current_range_proof(
    std::span<const std::uint8_t> bytes,
    std::size_t output_count) {
    if (output_count < kCurrentMinerMinOutputs ||
        output_count > kCurrentMinerMaxOutputs) {
        throw std::runtime_error("invalid output count for range proof");
    }

    Cursor cursor(bytes);
    ParsedRangeProof result;

    const std::size_t expected_rounds = kBppLog2N + ceil_log2(output_count);
    result.bpp.left = cursor.read_key_vector(kBppLog2N + 5);
    result.bpp.right = cursor.read_key_vector(kBppLog2N + 5);
    if (result.bpp.left.size() != expected_rounds ||
        result.bpp.right.size() != expected_rounds) {
        throw std::runtime_error("unexpected current BPP round count");
    }
    result.bpp.a0 = cursor.read_key();
    result.bpp.a = cursor.read_key();
    result.bpp.b = cursor.read_key();
    result.bpp.r = cursor.read_key();
    result.bpp.s = cursor.read_key();
    result.bpp.delta = cursor.read_key();

    result.aggregation.amount_commitments_for_rp_aggregation =
        cursor.read_key_vector(kCurrentMinerMaxOutputs);
    result.aggregation.y0s = cursor.read_key_vector(kCurrentMinerMaxOutputs);
    result.aggregation.y1s = cursor.read_key_vector(kCurrentMinerMaxOutputs);
    result.aggregation.c = cursor.read_key();

    if (result.aggregation.amount_commitments_for_rp_aggregation.size() !=
            output_count ||
        result.aggregation.y0s.size() != output_count ||
        result.aggregation.y1s.size() != output_count || !cursor.at_end()) {
        throw std::runtime_error(
            "range-proof aggregation count/trailing-data mismatch");
    }
    return result;
}

[[nodiscard]] std::span<const std::uint8_t> current_range_proof_payload(
    const P2pMiningContextProposal& proposal,
    const MiningHeaderWork& work) {
    const std::size_t miner_suffix_offset =
        work.block_header.serialized_size + work.miner_tx_prefix.serialized_size;
    const std::size_t tx_hashes_offset = work.tx_hashes.serialized_offset;

    if (miner_suffix_offset > proposal.block_template_blob.size() ||
        tx_hashes_offset > proposal.block_template_blob.size() ||
        tx_hashes_offset < kBalanceProofSerializedSize ||
        miner_suffix_offset + 4 > tx_hashes_offset) {
        throw std::runtime_error("invalid current miner proof offsets");
    }

    const auto& blob = proposal.block_template_blob;
    if (blob[miner_suffix_offset] != 0 ||       // attachments count
        blob[miner_suffix_offset + 1] != 0 ||   // signatures count
        blob[miner_suffix_offset + 2] != 2 ||   // proof count
        blob[miner_suffix_offset + 3] != kRangeProofTag) {
        throw std::runtime_error("unexpected current miner proof vector shape");
    }

    const std::size_t balance_tag_offset =
        tx_hashes_offset - kBalanceProofSerializedSize;
    if (balance_tag_offset <= miner_suffix_offset + 4 ||
        blob[balance_tag_offset] != kBalanceProofTag) {
        throw std::runtime_error("missing current balance proof after range proof");
    }

    const std::size_t range_begin = miner_suffix_offset + 4;
    return std::span<const std::uint8_t>(
        blob.data() + range_begin,
        balance_tag_offset - range_begin);
}

[[nodiscard]] P2pMinerTxProofResult verify_balance_after_payout(
    const P2pMiningContextProposal& proposal,
    const P2pMiningContextCheckResult& anchored_check,
    const P2pPayoutPolicyResult& payout) noexcept {
    P2pMinerTxProofResult result;
    result.payout_status = payout.status;
    if (payout.status != P2pPayoutPolicyStatus::Verified) {
        if (payout.status == P2pPayoutPolicyStatus::NotAnchored) {
            result.status = P2pMinerTxProofStatus::NotAnchored;
        } else if (payout.status == P2pPayoutPolicyStatus::BackendUnavailable) {
            result.status = P2pMinerTxProofStatus::BackendUnavailable;
        } else {
            result.status = P2pMinerTxProofStatus::PayoutPolicyFailed;
        }
        return result;
    }

    if (!zano_curve_backend_available()) {
        result.status = P2pMinerTxProofStatus::BackendUnavailable;
        return result;
    }

    try {
        const auto work = derive_mining_header_work(proposal.block_template_blob);
        if (work.header_hash != anchored_check.mining_header_hash ||
            work.miner_tx_prefix.hash == Hash256{}) {
            result.status = P2pMinerTxProofStatus::NotAnchored;
            return result;
        }

        const ParsedProofInputs proof_inputs =
            parse_proof_inputs(work.miner_tx_prefix.serialized);

        const std::size_t tx_hashes_offset = work.tx_hashes.serialized_offset;
        if (tx_hashes_offset < kBalanceProofSerializedSize ||
            tx_hashes_offset > proposal.block_template_blob.size()) {
            result.status = P2pMinerTxProofStatus::MalformedBlock;
            return result;
        }
        const std::size_t balance_tag_offset =
            tx_hashes_offset - kBalanceProofSerializedSize;
        if (proposal.block_template_blob[balance_tag_offset] != kBalanceProofTag) {
            result.status = P2pMinerTxProofStatus::MalformedBlock;
            return result;
        }

        const auto proof = std::span<const std::uint8_t>(
            proposal.block_template_blob.data() + balance_tag_offset + 1,
            kBalanceProofPayloadSize);

        if (!zano_verify_hf6_miner_balance_proof(
                work.miner_tx_prefix.hash,
                proposal.block_reward,
                proof_inputs.tx_public_key,
                proof_inputs.amount_commitments,
                proof)) {
            result.status = P2pMinerTxProofStatus::InvalidBalanceProof;
            return result;
        }

        result.status = P2pMinerTxProofStatus::BalanceVerifiedRangePending;
        return result;
    } catch (...) {
        result.status = P2pMinerTxProofStatus::MalformedBlock;
        return result;
    }
}

[[nodiscard]] P2pMinerTxProofResult verify_range_after_balance(
    const P2pMiningContextProposal& proposal,
    const P2pMiningContextCheckResult& anchored_check,
    P2pMinerTxProofResult result) noexcept {
    if (result.status != P2pMinerTxProofStatus::BalanceVerifiedRangePending) {
        return result;
    }

    try {
        const auto work = derive_mining_header_work(proposal.block_template_blob);
        if (work.header_hash != anchored_check.mining_header_hash ||
            work.miner_tx_prefix.hash == Hash256{}) {
            result.status = P2pMinerTxProofStatus::NotAnchored;
            return result;
        }

        const ParsedProofInputs proof_inputs =
            parse_proof_inputs(work.miner_tx_prefix.serialized);
        const auto range_payload = current_range_proof_payload(proposal, work);
        const ParsedRangeProof range = parse_current_range_proof(
            range_payload, proof_inputs.amount_commitments.size());

        if (!zano_verify_hf6_miner_range_proof(
                work.miner_tx_prefix.hash,
                proof_inputs.amount_commitments,
                proof_inputs.blinded_asset_ids,
                range.bpp,
                range.aggregation)) {
            result.status = P2pMinerTxProofStatus::InvalidRangeProof;
            return result;
        }

        result.status = P2pMinerTxProofStatus::ProofsVerified;
        return result;
    } catch (const std::runtime_error&) {
        result.status = P2pMinerTxProofStatus::MalformedRangeProof;
        return result;
    } catch (...) {
        result.status = P2pMinerTxProofStatus::MalformedBlock;
        return result;
    }
}

}  // namespace

P2pMinerTxProofResult verify_p2p_mining_context_balance_proof(
    const P2pMiningContextProposal& proposal,
    const P2pMiningContextCheckResult& anchored_check,
    const P2pPayoutAddress& expected_payout) noexcept {
    return verify_balance_after_payout(
        proposal,
        anchored_check,
        verify_p2p_mining_context_payout_policy(
            proposal, anchored_check, expected_payout));
}

P2pMinerTxProofResult verify_p2p_mining_context_balance_proof(
    const P2pMiningContextProposal& proposal,
    const P2pMiningContextCheckResult& anchored_check,
    const PplnsCoinbasePlan& expected_plan) noexcept {
    return verify_balance_after_payout(
        proposal,
        anchored_check,
        verify_p2p_mining_context_payout_policy(
            proposal, anchored_check, expected_plan));
}

P2pMinerTxProofResult verify_p2p_mining_context_proofs(
    const P2pMiningContextProposal& proposal,
    const P2pMiningContextCheckResult& anchored_check,
    const P2pPayoutAddress& expected_payout) noexcept {
    return verify_range_after_balance(
        proposal,
        anchored_check,
        verify_p2p_mining_context_balance_proof(
            proposal, anchored_check, expected_payout));
}

P2pMinerTxProofResult verify_p2p_mining_context_proofs(
    const P2pMiningContextProposal& proposal,
    const P2pMiningContextCheckResult& anchored_check,
    const PplnsCoinbasePlan& expected_plan) noexcept {
    return verify_range_after_balance(
        proposal,
        anchored_check,
        verify_p2p_mining_context_balance_proof(
            proposal, anchored_check, expected_plan));
}

const char* p2p_miner_tx_proof_status_name(
    P2pMinerTxProofStatus status) noexcept {
    switch (status) {
    case P2pMinerTxProofStatus::BalanceVerifiedRangePending:
        return "balance-verified-range-pending";
    case P2pMinerTxProofStatus::ProofsVerified:
        return "proofs-verified";
    case P2pMinerTxProofStatus::NotAnchored:
        return "not-anchored";
    case P2pMinerTxProofStatus::PayoutPolicyFailed:
        return "payout-policy-failed";
    case P2pMinerTxProofStatus::BackendUnavailable:
        return "backend-unavailable";
    case P2pMinerTxProofStatus::MalformedBlock:
        return "malformed-block";
    case P2pMinerTxProofStatus::MalformedRangeProof:
        return "malformed-range-proof";
    case P2pMinerTxProofStatus::InvalidBalanceProof:
        return "invalid-balance-proof";
    case P2pMinerTxProofStatus::InvalidRangeProof:
        return "invalid-range-proof";
    }
    return "unknown";
}

}  // namespace zano_p2pool
