#include "zano_p2pool/sidechain_difficulty.hpp"

#include "zano_p2pool/pow_target.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

#include <boost/multiprecision/cpp_int.hpp>

namespace zano_p2pool {
namespace {

using boost::multiprecision::cpp_int;

[[nodiscard]] cpp_int big_endian_to_int(std::span<const std::uint8_t> bytes) {
    cpp_int value = 0;
    for (const std::uint8_t byte : bytes) {
        value <<= 8;
        value += byte;
    }
    return value;
}

[[nodiscard]] Difficulty128 int_to_difficulty128(cpp_int value) {
    if (value <= 0) {
        throw std::runtime_error("sidechain difficulty must be positive");
    }

    Difficulty128 result{};
    for (std::size_t i = result.size(); i-- > 0;) {
        result[i] = static_cast<std::uint8_t>(
            (value & 0xff).convert_to<unsigned int>());
        value >>= 8;
    }
    if (value != 0) {
        throw std::runtime_error("sidechain difficulty exceeds 128 bits");
    }
    return result;
}

}  // namespace

Difficulty128 calculate_next_sidechain_difficulty(
    std::span<const SidechainDifficultySample> history,
    const SidechainParameters& params,
    const Difficulty128& network_difficulty) {
    if (params.target_share_seconds == 0 ||
        params.minimum_share_difficulty == 0 ||
        params.difficulty_window_shares < 2) {
        throw std::invalid_argument("invalid sidechain difficulty parameters");
    }
    if (difficulty128_is_zero(network_difficulty)) {
        throw std::invalid_argument("network difficulty must be nonzero");
    }

    const cpp_int network = big_endian_to_int(network_difficulty);
    const cpp_int minimum = params.minimum_share_difficulty;
    const cpp_int effective_minimum = std::min(minimum, network);

    if (history.empty()) {
        return int_to_difficulty128(effective_minimum);
    }

    const std::size_t sample_count = std::min<std::size_t>(
        history.size(),
        static_cast<std::size_t>(params.difficulty_window_shares));
    const auto samples = history.first(sample_count);

    std::uint64_t oldest_timestamp = std::numeric_limits<std::uint64_t>::max();
    for (const auto& sample : samples) {
        oldest_timestamp = std::min(oldest_timestamp, sample.timestamp);
    }

    std::vector<std::uint32_t> timestamp_deltas;
    timestamp_deltas.reserve(sample_count);
    for (const auto& sample : samples) {
        const std::uint64_t delta = sample.timestamp - oldest_timestamp;
        if (delta > std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error("sidechain difficulty timestamp range is too large");
        }
        timestamp_deltas.push_back(static_cast<std::uint32_t>(delta));
    }

    const std::size_t cut_size = (sample_count + 9) / 10;
    const std::size_t index1 = cut_size - 1;
    const std::size_t index2 = sample_count - cut_size;

    std::nth_element(
        timestamp_deltas.begin(),
        timestamp_deltas.begin() + static_cast<std::ptrdiff_t>(index1),
        timestamp_deltas.end());
    const std::uint64_t timestamp1 =
        oldest_timestamp + timestamp_deltas[index1];

    std::nth_element(
        timestamp_deltas.begin(),
        timestamp_deltas.begin() + static_cast<std::ptrdiff_t>(index2),
        timestamp_deltas.end());
    const std::uint64_t timestamp2 =
        oldest_timestamp + timestamp_deltas[index2];

    const std::uint64_t delta_index =
        index2 > index1 ? static_cast<std::uint64_t>(index2 - index1) : 1U;
    const std::uint64_t timestamp_span =
        timestamp2 > timestamp1 ? timestamp2 - timestamp1 : 0U;
    const std::uint64_t delta_t = std::max(timestamp_span, delta_index);

    bool found = false;
    cpp_int minimum_work = 0;
    cpp_int maximum_work = 0;
    for (const auto& sample : samples) {
        if (sample.timestamp < timestamp1 || sample.timestamp > timestamp2) {
            continue;
        }

        const cpp_int work = big_endian_to_int(sample.cumulative_work);
        if (!found) {
            minimum_work = work;
            maximum_work = work;
            found = true;
        } else {
            minimum_work = std::min(minimum_work, work);
            maximum_work = std::max(maximum_work, work);
        }
    }

    if (!found) {
        throw std::runtime_error("sidechain difficulty retained no samples");
    }

    cpp_int next =
        (maximum_work - minimum_work) * params.target_share_seconds / delta_t;
    next = std::max(next, effective_minimum);
    next = std::min(next, network);
    return int_to_difficulty128(next);
}

}  // namespace zano_p2pool
