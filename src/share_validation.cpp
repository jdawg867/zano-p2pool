#include "zano_p2pool/share_validation.hpp"

#include "zano_p2pool/pow_target.hpp"

namespace zano_p2pool {

CandidateClassification classify_pow_hash(
    const Hash256& pow_hash,
    std::string_view share_difficulty,
    std::string_view network_difficulty) {
    if (hash_meets_difficulty(pow_hash, network_difficulty)) {
        return CandidateClassification::Block;
    }

    if (hash_meets_difficulty(pow_hash, share_difficulty)) {
        return CandidateClassification::Share;
    }

    return CandidateClassification::Invalid;
}

CandidateValidation validate_candidate(
    std::uint64_t height,
    const Hash256& mining_header_hash,
    std::uint64_t nonce,
    std::string_view share_difficulty,
    std::string_view network_difficulty,
    ProgPowZContextMode mode) {
    CandidateValidation validation{};
    validation.pow = progpowz_hash(height, mining_header_hash, nonce, mode);
    validation.meets_share_difficulty =
        hash_meets_difficulty(validation.pow.final_hash, share_difficulty);
    validation.meets_network_difficulty =
        hash_meets_difficulty(validation.pow.final_hash, network_difficulty);

    if (validation.meets_network_difficulty) {
        validation.classification = CandidateClassification::Block;
    } else if (validation.meets_share_difficulty) {
        validation.classification = CandidateClassification::Share;
    } else {
        validation.classification = CandidateClassification::Invalid;
    }

    return validation;
}

const char* candidate_classification_name(
    CandidateClassification classification) noexcept {
    switch (classification) {
        case CandidateClassification::Invalid:
            return "invalid";
        case CandidateClassification::Share:
            return "share";
        case CandidateClassification::Block:
            return "block";
    }

    return "invalid";
}

}  // namespace zano_p2pool
