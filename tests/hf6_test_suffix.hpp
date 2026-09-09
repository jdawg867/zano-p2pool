#pragma once
#include <cstdint>
#include <vector>

inline std::vector<std::uint8_t> make_structural_range_proof(
    std::uint8_t fill = 0xa5) {
    std::vector<std::uint8_t> bytes;
    const auto keys = [&](std::uint8_t count) {
        bytes.push_back(count);
        bytes.insert(bytes.end(), count * 32, fill);
    };
    keys(7);
    keys(7);
    bytes.insert(bytes.end(), 6 * 32, fill);
    keys(2);
    keys(2);
    keys(2);
    bytes.insert(bytes.end(), 32, fill);
    return bytes;
}
