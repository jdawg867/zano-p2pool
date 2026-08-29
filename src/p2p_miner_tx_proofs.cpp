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
constexpr std::uint8_t kBalanceProofTag = 48;

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

struct ParsedBalanceInputs {
    ZanoCurveKey tx_public_key{};
    std::vector<ZanoCurveKey> amount_commitments;
};

[[nodiscard]] ParsedBalanceInputs parse_balance_inputs(
    std::span<const std::uint8_t> miner_tx_prefix) {
    Cursor cursor(miner_tx_prefix);
    if (cursor.read_varint() != 4) {
        throw std::runtime_error("unsupported miner transaction version");
    }
    if (cursor.read_varint() != 1 || cursor.read_u8() != 0) {
        throw std::runtime_error("current PoW miner transaction must have txin_gen");
    }
    static_cast<void>(cursor.read_varint());  // txin_gen.height

    ParsedBalanceInputs result;
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
    if (output_count < 2 || output_count > 32) {
        throw std::runtime_error("invalid current miner output count");
    }
    result.amount_commitments.reserve(static_cast<std::size_t>(output_count));

    for (std::uint64_t i = 0; i < output_count; ++i) {
        if (cursor.read_u8() != 63 || cursor.read_u8() != 0) {
            throw std::runtime_error("unsupported current miner output");
        }
        cursor.skip(32);  // stealth_address
        cursor.skip(32);  // concealing_point
        result.amount_commitments.push_back(cursor.read_key());
        cursor.skip(32);       // blinded_asset_id
        cursor.skip(8 + 8 + 1);  // encrypted fields + mix_attr
    }

    if (cursor.read_u8() != 6 || !cursor.at_end()) {
        throw std::runtime_error("unexpected miner hardfork/trailing prefix data");
    }
    return result;
}

}  // namespace

P2pMinerTxProofResult verify_p2p_mining_context_balance_proof(
    const P2pMiningContextProposal& proposal,
    const P2pMiningContextCheckResult& anchored_check,
    const P2pPayoutAddress& expected_payout) noexcept {
    P2pMinerTxProofResult result;

    const auto payout = verify_p2p_mining_context_payout_policy(
        proposal, anchored_check, expected_payout);
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

        const ParsedBalanceInputs balance_inputs =
            parse_balance_inputs(work.miner_tx_prefix.serialized);

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
                balance_inputs.tx_public_key,
                balance_inputs.amount_commitments,
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

const char* p2p_miner_tx_proof_status_name(
    P2pMinerTxProofStatus status) noexcept {
    switch (status) {
    case P2pMinerTxProofStatus::BalanceVerifiedRangePending:
        return "balance-verified-range-pending";
    case P2pMinerTxProofStatus::NotAnchored:
        return "not-anchored";
    case P2pMinerTxProofStatus::PayoutPolicyFailed:
        return "payout-policy-failed";
    case P2pMinerTxProofStatus::BackendUnavailable:
        return "backend-unavailable";
    case P2pMinerTxProofStatus::MalformedBlock:
        return "malformed-block";
    case P2pMinerTxProofStatus::InvalidBalanceProof:
        return "invalid-balance-proof";
    }
    return "unknown";
}

}  // namespace zano_p2pool
