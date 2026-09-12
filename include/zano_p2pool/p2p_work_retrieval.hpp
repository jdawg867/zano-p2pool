#pragma once
#include "zano_p2pool/mining_work_archive.hpp"
#include "zano_p2pool/p2p_mining_context.hpp"
#include <map>
#include <mutex>
#include <optional>

namespace zano_p2pool {
using MiningWorkKey = std::pair<std::uint64_t, Hash256>;
inline constexpr std::size_t kMiningWorkChunkSize = 16 * 1024;
inline constexpr std::size_t kMiningWorkMaxPending = 16;
inline constexpr std::size_t kMiningWorkMaxReceived = 8;
inline constexpr std::uint64_t kMiningWorkRequestLifetime = 60;
struct MiningWorkRequest { MiningWorkKey key; std::uint32_t offset{0}; };
struct MiningWorkResponse {
    MiningWorkRequest request;
    std::uint32_t total_size{0}; // zero means not found
    std::vector<std::uint8_t> chunk;
};
[[nodiscard]] P2pEnvelope make_mining_work_request(const MiningWorkRequest& request);
[[nodiscard]] MiningWorkRequest parse_mining_work_request(const P2pEnvelope& envelope);
[[nodiscard]] P2pEnvelope make_mining_work_response(const MiningWorkResponse& response);
[[nodiscard]] MiningWorkResponse parse_mining_work_response(const P2pEnvelope& envelope);
struct MiningWorkReceiveResult {
    std::optional<P2pEnvelope> followup;
    std::optional<Hash256> received_id; // structurally checked, NOT trusted
};

// This object has no access to ShareChain or P2pTrustedWorkRegistry. Receipt
// cannot admit shares or transfer the sender's trust. All state is bounded
// except the local archive index (one small entry per archived work key).
class P2pWorkRetrieval {
public:
    explicit P2pWorkRetrieval(MiningWorkArchive& archive);
    // Call only after the local proposal has been durably archived.
    void remember_local(const P2pMiningContextProposal& proposal);
    [[nodiscard]] std::optional<P2pEnvelope> begin(
        const P2pHandshake& peer, const MiningWorkKey& key, std::uint64_t now);
    [[nodiscard]] P2pEnvelope answer(const P2pHandshake& peer, const P2pEnvelope& request);
    [[nodiscard]] MiningWorkReceiveResult receive(
        const P2pHandshake& peer, const P2pEnvelope& response, std::uint64_t now);
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> take_untrusted(const MiningWorkKey& key);
private:
    struct Pending {
        std::uint64_t started;
        std::uint32_t total{0};
        bool finished{false};
        std::vector<std::uint8_t> bytes;
    };
    void expire(std::uint64_t now);
    MiningWorkArchive& archive_;
    std::mutex mutex_;
    std::map<MiningWorkKey, Hash256> local_;
    std::map<std::pair<NodeId, MiningWorkKey>, Pending> pending_;
    std::map<MiningWorkKey, std::vector<std::uint8_t>> received_;
};
}
