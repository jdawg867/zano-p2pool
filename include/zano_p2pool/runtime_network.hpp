#pragma once

#include "zano_p2pool/p2p_protocol.hpp"
#include "zano_p2pool/sidechain_params.hpp"

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace zano_p2pool {

enum class RuntimeNetwork : std::uint8_t {
    Mainnet,
    Testnet,
};

inline constexpr std::string_view kRuntimeMainnetRpcUrl =
    "http://127.0.0.1:11211/json_rpc";
inline constexpr std::string_view kRuntimeTestnetRpcUrl =
    "http://127.0.0.1:12111/json_rpc";

[[nodiscard]] RuntimeNetwork parse_runtime_network(std::string_view value);

[[nodiscard]] std::string_view runtime_network_name(
    RuntimeNetwork network);

[[nodiscard]] std::string_view runtime_default_rpc_url(
    RuntimeNetwork network);

[[nodiscard]] std::filesystem::path runtime_default_share_store_path(
    RuntimeNetwork network,
    const std::filesystem::path& home);

[[nodiscard]] P2pNetwork runtime_p2p_network(
    RuntimeNetwork network);

[[nodiscard]] SidechainParentNetwork runtime_sidechain_parent_network(
    RuntimeNetwork network);

// While mainnet readiness is still under audit, long-lived mainnet operation
// requires an explicit operator acknowledgement. One-shot read-only template
// queries remain available without the opt-in so compatibility can be audited
// before launch.
[[nodiscard]] bool runtime_network_operation_allowed(
    RuntimeNetwork network,
    bool long_lived_runtime,
    bool experimental_mainnet_opt_in) noexcept;

}  // namespace zano_p2pool
