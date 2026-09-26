#pragma once

#include "zano_p2pool/share_validation.hpp"
#include "zano_p2pool/sidechain_params.hpp"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <vector>

namespace zano_p2pool {

// Canonical Zano history checkpoint authorizing one durable validation
// snapshot.
//
// The checkpoint is intended to represent a canonical block at or beyond every
// historical parent/PoW fact used while validating the recorded shares. Runtime
// wiring must independently confirm this exact height/hash against the local
// daemon before restoring any validation state.
struct ReplayValidationCheckpoint {
    std::uint64_t zano_height{};
    Hash256 block_hash{};
};

struct ReplayValidationRecord {
    ShareId share_id{};
    CandidateValidation validation{};
};

struct ReplayValidationSnapshot {
    ReplayValidationCheckpoint checkpoint{};
    std::vector<ReplayValidationRecord> records;
};

// Durable cache of previously established local share-validation results.
//
// This store is NOT an independent trust source. Loading a syntactically valid
// snapshot does not authorize its records. Callers must first confirm the
// snapshot checkpoint against current local canonical Zano history and then
// restore records only through ShareChain invariants.
//
// File format:
//   8 bytes   magic/version ("ZP2VAL01")
//   32 bytes  canonical SidechainId
//   8 bytes   canonical checkpoint height, big-endian
//   32 bytes  canonical checkpoint block hash
//   8 bytes   record count, big-endian
//   repeated fixed-size records:
//     32 bytes ShareId
//     32 bytes ProgPoWZ final hash
//     32 bytes ProgPoWZ mix hash
//     1 byte   meets share difficulty (0/1)
//     1 byte   meets network difficulty (0/1)
//     1 byte   CandidateClassification (1=Share, 2=Block)
//   32 bytes  cn_fast_hash of every preceding file byte
//
// Records are written in deterministic ShareId order. The final hash detects
// accidental corruption; it is not authentication against a malicious local
// filesystem.
//
// save() uses same-directory temporary-file + rename replacement. This gives
// atomic namespace replacement on the Linux deployment target but does not
// claim fsync-level power-loss durability.
class ReplayValidationStore {
public:
    ReplayValidationStore(
        std::filesystem::path path,
        SidechainId expected_sidechain_id);

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

    void save(const ReplayValidationSnapshot& snapshot);

    // Missing store is represented by nullopt. Existing malformed, corrupt,
    // wrong-sidechain, or internally inconsistent stores fail closed by
    // throwing.
    [[nodiscard]] std::optional<ReplayValidationSnapshot> load() const;

private:
    std::filesystem::path path_;
    SidechainId expected_sidechain_id_{};
    mutable std::mutex mutex_;
};

}  // namespace zano_p2pool
