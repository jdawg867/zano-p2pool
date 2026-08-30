#pragma once

#include "zano_p2pool/sidechain_params.hpp"

#include <cstdint>
#include <span>

namespace zano_p2pool {

struct SidechainDifficultySample {
    std::uint64_t timestamp{0};
    ChainWork cumulative_work{};
};

// Calculate the difficulty required for the next share built on a particular
// parent branch. `history` must be ordered newest-to-oldest and should start at
// that parent. At most `params.difficulty_window_shares` samples are used.
//
// The calculation follows the established P2Pool retarget shape: discard the
// oldest/newest 10% by timestamp, measure cumulative work across the retained
// timestamp band, and scale that work to `target_share_seconds`. The result is
// floored at the sidechain minimum and capped at the current parent-network
// difficulty so it remains compatible with the existing share<=network rule.
[[nodiscard]] Difficulty128 calculate_next_sidechain_difficulty(
    std::span<const SidechainDifficultySample> history,
    const SidechainParameters& params,
    const Difficulty128& network_difficulty);

}  // namespace zano_p2pool
