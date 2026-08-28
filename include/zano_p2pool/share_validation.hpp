#pragma once

#include "zano_p2pool/progpowz.hpp"

#include <cstdint>
#include <string_view>

namespace zano_p2pool {

enum class CandidateClassification {
    Invalid,
    Share,
    Block,
};

struct CandidateValidation {
    ProgPowZResult pow{};
    bool meets_share_difficulty{false};
    bool meets_network_difficulty{false};
    CandidateClassification classification{CandidateClassification::Invalid};
};

// Classify an already-computed PoW hash against independent P2Pool share and
// Zano network difficulties. A full network solution is always classified as
// Block even if it also satisfies the lower share difficulty.
[[nodiscard]] CandidateClassification classify_pow_hash(
    const Hash256& pow_hash,
    std::string_view share_difficulty,
    std::string_view network_difficulty);

// Compute ProgPoWZ locally and classify the candidate without submitting
// anything to zanod.
[[nodiscard]] CandidateValidation validate_candidate(
    std::uint64_t height,
    const Hash256& mining_header_hash,
    std::uint64_t nonce,
    std::string_view share_difficulty,
    std::string_view network_difficulty,
    ProgPowZContextMode mode = ProgPowZContextMode::Light);

[[nodiscard]] const char* candidate_classification_name(
    CandidateClassification classification) noexcept;

}  // namespace zano_p2pool
