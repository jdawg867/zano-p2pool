#pragma once

#include "zano_p2pool/p2p_protocol.hpp"
#include "zano_p2pool/share_chain.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>

namespace zano_p2pool {

[[nodiscard]] P2pEnvelope make_p2p_share_announce_envelope(
    const Share& share);
[[nodiscard]] Share parse_p2p_share_announce_envelope(
    const P2pEnvelope& envelope);

// Only locally derived/validated work contexts belong here. Peer-provided
// context data must never be inserted directly. The registry lets P2P share
// admission preserve ShareChain::submit_share()'s trusted-context boundary.
class P2pTrustedWorkRegistry {
public:
    void remember(const ShareWorkContext& context);

    [[nodiscard]] const ShareWorkContext* find(
        std::uint64_t zano_height,
        const Hash256& mining_header_hash) const noexcept;

    [[nodiscard]] std::size_t size() const noexcept;

private:
    using Key = std::pair<std::uint64_t, Hash256>;
    std::map<Key, ShareWorkContext> contexts_;
};

enum class P2pShareReceiveStatus {
    Connected,
    Orphan,
    Duplicate,
    Rejected,
    UnknownWorkContext,
    CapabilityMissing,
};

struct P2pShareReceiveResult {
    P2pShareReceiveStatus status{P2pShareReceiveStatus::Rejected};
    AddShareResult chain_result{};
};

class P2pShareReceiver {
public:
    P2pShareReceiver(
        ShareChain& chain,
        const P2pTrustedWorkRegistry& trusted_work) noexcept;

    [[nodiscard]] P2pShareReceiveResult receive(
        const P2pHandshake& peer,
        const P2pEnvelope& envelope,
        std::uint64_t now,
        ProgPowZContextMode mode = ProgPowZContextMode::Light);

private:
    ShareChain& chain_;
    const P2pTrustedWorkRegistry& trusted_work_;
};

[[nodiscard]] const char* p2p_share_receive_status_name(
    P2pShareReceiveStatus status) noexcept;

}  // namespace zano_p2pool
