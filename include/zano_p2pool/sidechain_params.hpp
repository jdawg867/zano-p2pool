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

inline constexpr std::uint8_t kSidechainParameterVersion = 1;
inline constexpr std::array<std::uint8_t, 8> kSidechainIdDomain{
    'Z', 'P', '2', 'S', 'I', 'D', 'V', '1'};

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
    std::uint8_t minimum_share_version{kShareVersion1};
    std::uint8_t maximum_share_version{kShareVersion2};
    std::uint64_t max_future_seconds{kShareMaxFutureSeconds};
    std::uint64_t max_parent_backstep_seconds{kShareMaxParentBackstepSeconds};

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

// Canonical parameter encoding used only to derive SidechainId. It is separate
// from the P2P wire protocol and intentionally commits only to rules already
// enforced by the share chain today. New consensus parameters must be appended
// under a new parameter-version/domain rather than silently changing this v1
// encoding.
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

    std::vector<std::uint8_t> bytes;
    bytes.reserve(kSidechainIdDomain.size() + 3 + 16);
    bytes.insert(
        bytes.end(),
        kSidechainIdDomain.begin(),
        kSidechainIdDomain.end());
    bytes.push_back(static_cast<std::uint8_t>(params.parent_network));
    bytes.push_back(params.minimum_share_version);
    bytes.push_back(params.maximum_share_version);
    detail::append_u64_be(bytes, params.max_future_seconds);
    detail::append_u64_be(bytes, params.max_parent_backstep_seconds);
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
