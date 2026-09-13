#pragma once

#include "zano_p2pool/crypto_hash.hpp"

#include <filesystem>
#include <span>
#include <vector>

namespace zano_p2pool {

// Evidence storage only. Reading an archived proposal never promotes it into
// trusted work. Callers serialize P2pMiningContextProposal before writing and
// must independently anchor and verify it before admitting peer shares.
class MiningWorkArchive {
public:
    MiningWorkArchive(std::filesystem::path directory, Hash256 sidechain_id);
    // Atomically publishes and fsyncs a complete record before returning.
    // The ID is cn_fast_hash(payload), matching p2p_mining_context_id().
    [[nodiscard]] Hash256 put(std::span<const std::uint8_t> payload);
    [[nodiscard]] std::vector<std::uint8_t> read(const Hash256& id) const;
    // Returns every complete record ID in deterministic order while validating
    // each record's sidechain binding, length, content ID and checksum.
    // Interrupted unpublished .tmp-* files are ignored. Any other unexpected
    // directory entry fails closed.
    [[nodiscard]] std::vector<Hash256> list_ids(
        std::size_t max_records = 10000) const;
    // Checks complete records one at a time (bounded memory). Interrupted
    // unpublished .tmp-* files are ignored, never treated as evidence.
    [[nodiscard]] std::size_t verify_all() const;
    [[nodiscard]] const std::filesystem::path& path() const { return directory_; }
private:
    std::filesystem::path directory_;
    Hash256 sidechain_id_;
};

} // namespace zano_p2pool
