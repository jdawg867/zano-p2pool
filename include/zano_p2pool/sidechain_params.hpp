#pragma once

#include "zano_p2pool/crypto_hash.hpp"
#include "zano_p2pool/share.hpp"
#include "zano_p2pool/share_chain.hpp"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace zano_p2pool {

using SidechainId = Hash256;

inline constexpr std::uint8_t kSidechainParameterVersion = 4;
inline constexpr std::array<std::uint8_t, 8> kSidechainIdDomain{
    'Z', 'P', '2', 'S', 'I', 'D', 'V', '4'};

inline constexpr std::uint64_t kOperatorFeeBasisPointDenominator = 10000;
inline constexpr std::uint64_t kCanonicalOperatorFeeBasisPoints = 100;

// Zano permits at most 32 current PoW coinbase outputs. V4 permanently
// reserves one logical destination for the operator fee, leaving at most 31
// distinct miner payout identities in one canonical PPLNS window.
inline constexpr std::uint64_t kCanonicalPplnsWindowShares = 31;

// Public-only operator fee payout identity. These are the spend/view public
// keys decoded from the canonical operator fee Zano address. No wallet secret
// material is present in consensus parameters.
inline constexpr PayoutPublicKeys kCanonicalOperatorFeePayout{
    Hash256{
        0x0a, 0x7e, 0x53, 0xf7, 0x00, 0xf4, 0x6f, 0x2e,
        0x5d, 0x28, 0x3a, 0xaa, 0xcb, 0xfc, 0x6e, 0x69,
        0xc9, 0x53, 0x2a, 0x56, 0x28, 0xc9, 0xfd, 0xf8,
        0x07, 0xf2, 0x58, 0xe0, 0xcb, 0xba, 0x0c, 0xe1,
    },
    Hash256{
        0xdd, 0x5d, 0xd9, 0x0b, 0x2c, 0xc8, 0xc1, 0x97,
        0xea, 0x29, 0x7e, 0x06, 0x56, 0x19, 0x29, 0xc0,
        0x42, 0x2b, 0xfe, 0x9b, 0xff, 0x49, 0x5c, 0x5d,
        0x62, 0x72, 0xc2, 0x37, 0x3f, 0x5f, 0x12, 0x9e,
    },
};

// Values deliberately match the existing P2P network tags. The sidechain
// parameter layer is independent of the transport protocol so its identity can
// remain stable across future P2P framing/version changes.
enum class SidechainParentNetwork : std::uint8_t {
    Testnet = 1,
    Mainnet = 2,
};

struct SidechainParameters {
    std::uint8_t parameter_version{kSidechainParameterVersion};
    SidechainParentNetwork parent_network{SidechainParentNetwork::Testnet};

    // Public PPLNS consensus requires payout-capable v2 shares.
    std::uint8_t minimum_share_version{kShareVersion2};
    std::uint8_t maximum_share_version{kShareVersion2};

    std::uint64_t max_future_seconds{kShareMaxFutureSeconds};
    std::uint64_t max_parent_backstep_seconds{kShareMaxParentBackstepSeconds};

    std::uint64_t target_share_seconds{10};
    std::uint64_t minimum_share_difficulty{100000000};

    std::uint64_t difficulty_window_shares{2160};
    std::uint64_t pplns_window_shares{kCanonicalPplnsWindowShares};
    std::uint64_t pplns_max_network_difficulty_multiplier{2};

    // V4 economic rule: exactly 100 basis points (1.00%) of each canonical
    // PPLNS block reward is assigned to this public operator payout identity.
    std::uint64_t operator_fee_basis_points{kCanonicalOperatorFeeBasisPoints};
    PayoutPublicKeys operator_fee_payout{kCanonicalOperatorFeePayout};

    bool operator==(const SidechainParameters&) const = default;
};

[[nodiscard]] inline SidechainParameters canonical_sidechain_parameters(
    SidechainParentNetwork network) {
    switch (network) {
    case SidechainParentNetwork::Testnet:
    case SidechainParentNetwork::Mainnet:
        break;
    default:
        throw std::invalid_argument("unsupported sidechain parent network");
    }

    SidechainParameters params;
    params.parent_network = network;
    return params;
}

namespace detail {

inline void append_u64_be(
    std::vector<std::uint8_t>& bytes,
    std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

}  // namespace detail

// Canonical parameter encoding used only to derive SidechainId. V4 extends the
// v3 economic identity with the 1% operator-fee rule and its public payout
// identity. Any future consensus change must use another version/domain.
[[nodiscard]] inline std::vector<std::uint8_t> serialize_sidechain_parameters(
    const SidechainParameters& params) {
    if (params.parameter_version != kSidechainParameterVersion) {
        throw std::invalid_argument("unsupported sidechain parameter version");
    }
    if (params.parent_network != SidechainParentNetwork::Testnet &&
        params.parent_network != SidechainParentNetwork::Mainnet) {
        throw std::invalid_argument("unsupported sidechain parent network");
    }
    if (params.minimum_share_version == 0 ||
        params.maximum_share_version < params.minimum_share_version) {
        throw std::invalid_argument("invalid sidechain share-version range");
    }
    if (params.target_share_seconds == 0) {
        throw std::invalid_argument(
            "sidechain target share interval must be nonzero");
    }
    if (params.minimum_share_difficulty == 0) {
        throw std::invalid_argument(
            "sidechain minimum share difficulty must be nonzero");
    }
    if (params.difficulty_window_shares < 2) {
        throw std::invalid_argument(
            "sidechain difficulty window must contain at least 2 shares");
    }
    if (params.pplns_window_shares == 0 ||
        params.pplns_window_shares > kCanonicalPplnsWindowShares) {
        throw std::invalid_argument(
            "sidechain PPLNS share window must be between 1 and 31");
    }
    if (params.pplns_max_network_difficulty_multiplier == 0) {
        throw std::invalid_argument(
            "sidechain PPLNS work multiplier must be nonzero");
    }
    if (params.operator_fee_basis_points == 0 ||
        params.operator_fee_basis_points >=
            kOperatorFeeBasisPointDenominator) {
        throw std::invalid_argument(
            "sidechain operator fee must be between 1 and 9999 basis points");
    }
    if (params.operator_fee_payout.spend_public_key == Hash256{} ||
        params.operator_fee_payout.view_public_key == Hash256{}) {
        throw std::invalid_argument(
            "sidechain operator fee payout identity must be nonzero");
    }

    std::vector<std::uint8_t> bytes;
    bytes.reserve(139);

    bytes.insert(
        bytes.end(),
        kSidechainIdDomain.begin(),
        kSidechainIdDomain.end());

    bytes.push_back(static_cast<std::uint8_t>(params.parent_network));
    bytes.push_back(params.minimum_share_version);
    bytes.push_back(params.maximum_share_version);

    detail::append_u64_be(bytes, params.max_future_seconds);
    detail::append_u64_be(bytes, params.max_parent_backstep_seconds);
    detail::append_u64_be(bytes, params.target_share_seconds);
    detail::append_u64_be(bytes, params.minimum_share_difficulty);
    detail::append_u64_be(bytes, params.difficulty_window_shares);
    detail::append_u64_be(bytes, params.pplns_window_shares);
    detail::append_u64_be(
        bytes,
        params.pplns_max_network_difficulty_multiplier);
    detail::append_u64_be(
        bytes,
        params.operator_fee_basis_points);

    bytes.insert(
        bytes.end(),
        params.operator_fee_payout.spend_public_key.begin(),
        params.operator_fee_payout.spend_public_key.end());
    bytes.insert(
        bytes.end(),
        params.operator_fee_payout.view_public_key.begin(),
        params.operator_fee_payout.view_public_key.end());

    return bytes;
}

[[nodiscard]] inline SidechainId sidechain_id(
    const SidechainParameters& params) {
    return cn_fast_hash(serialize_sidechain_parameters(params));
}

[[nodiscard]] inline bool is_zero_sidechain_id(
    const SidechainId& id) noexcept {
    for (const std::uint8_t byte : id) {
        if (byte != 0) {
            return false;
        }
    }
    return true;
}

}  // namespace zano_p2pool
