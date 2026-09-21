#pragma once

#include "zano_p2pool/p2p_protocol.hpp"
#include "zano_p2pool/share_chain.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

namespace zano_p2pool {

[[nodiscard]] P2pEnvelope make_p2p_share_announce_envelope(
    const Share& share);
[[nodiscard]] Share parse_p2p_share_announce_envelope(
    const P2pEnvelope& envelope);

// Only locally derived/validated work contexts belong here. Peer-provided
// context data must never be inserted directly. The registry lets P2P share
// admission preserve ShareChain::submit_share()'s trusted-context boundary.
//
// Canonical PPLNS work is parent-branch specific: the miner transaction was
// validated against the payout window for one exact sidechain parent. The
// three-part key prevents a trusted template from authorizing a share that
// points at a different branch.
class P2pTrustedWorkRegistry {
public:
    // Compatibility/bootstrap helper: binds only to the zero parent.
    void remember(const ShareWorkContext& context);
    void remember(
        const ShareWorkContext& context,
        const ShareId& parent_id);

    // Production trust crossings record the canonical Zano parent that
    // independently authorized this exact work context. The compatibility
    // overloads above remain useful for synthetic tests but carry no canonical
    // parent provenance.
    void remember(
        const ShareWorkContext& context,
        const ShareId& parent_id,
        const Hash256& zano_parent_hash);

    // Compatibility/bootstrap helper: looks up only the zero parent.
    [[nodiscard]] const ShareWorkContext* find(
        std::uint64_t zano_height,
        const Hash256& mining_header_hash) const noexcept;
    [[nodiscard]] const ShareWorkContext* find(
        std::uint64_t zano_height,
        const Hash256& mining_header_hash,
        const ShareId& parent_id) const noexcept;

    // Revoke every trusted work context for one Zano mining height.
    // This is used when that mining height is no longer available after a
    // canonical rollback; parent replacement uses selective provenance checks.
    [[nodiscard]] std::size_t erase_zano_height(
        std::uint64_t zano_height);

    // Revoke every authorization whose Zano mining height is above the
    // daemon's current canonical work-height ceiling.
    [[nodiscard]] std::size_t erase_zano_heights_above(
        std::uint64_t maximum_zano_height);

    // Revoke only work at one Zano height whose recorded canonical-parent
    // provenance conflicts with the currently selected parent. Entries without
    // provenance are left untouched rather than guessed stale.
    [[nodiscard]] std::size_t erase_zano_parent_mismatch(
        std::uint64_t zano_height,
        const Hash256& canonical_parent_hash);

    // Sorted, deduplicated Zano heights for production-trusted work carrying
    // canonical-parent provenance. Compatibility entries without provenance
    // are deliberately excluded from canonical-history auditing.
    [[nodiscard]] std::vector<std::uint64_t>
    provenance_zano_heights() const;

    // Returns the independently established canonical Zano parent recorded
    // when this exact trusted-work key crossed into the registry. Compatibility
    // entries created without provenance return nullptr.
    [[nodiscard]] const Hash256* find_zano_parent_hash(
        std::uint64_t zano_height,
        const Hash256& mining_header_hash,
        const ShareId& parent_id) const noexcept;

    [[nodiscard]] std::size_t size() const noexcept;

private:
    using Key = std::tuple<std::uint64_t, Hash256, ShareId>;

    struct Entry {
        ShareWorkContext context;
        std::optional<Hash256> zano_parent_hash;
    };

    std::map<Key, Entry> contexts_;
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
    std::optional<ShareId> missing_parent_id;
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

    // Common verified admission path for canonical shares received through
    // gossip or synchronization. required_capability identifies which peer
    // capability authorizes the operation; the trusted-work and local PoW
    // rules are identical for both paths.
    [[nodiscard]] P2pShareReceiveResult receive_share(
        const P2pHandshake& peer,
        const Share& share,
        std::uint64_t required_capability,
        std::uint64_t now,
        ProgPowZContextMode mode = ProgPowZContextMode::Light);

private:
    ShareChain& chain_;
    const P2pTrustedWorkRegistry& trusted_work_;
};

[[nodiscard]] const char* p2p_share_receive_status_name(
    P2pShareReceiveStatus status) noexcept;

}  // namespace zano_p2pool
