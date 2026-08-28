#include "zano_p2pool/share_validation.hpp"
#include "test_check.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace {

zano_p2pool::Hash256 from_hex(std::string_view hex) {
    CHECK(hex.size() == 64);

    auto nibble = [](char ch) -> std::uint8_t {
        if (ch >= '0' && ch <= '9') {
            return static_cast<std::uint8_t>(ch - '0');
        }
        if (ch >= 'a' && ch <= 'f') {
            return static_cast<std::uint8_t>(10 + ch - 'a');
        }
        if (ch >= 'A' && ch <= 'F') {
            return static_cast<std::uint8_t>(10 + ch - 'A');
        }
        CHECK(false && "invalid hex digit");
        return 0;
    };

    zano_p2pool::Hash256 bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::uint8_t>(
            (nibble(hex[i * 2]) << 4) | nibble(hex[i * 2 + 1]));
    }
    return bytes;
}

}  // namespace

int main() {
    using zano_p2pool::CandidateClassification;
    using zano_p2pool::candidate_classification_name;
    using zano_p2pool::classify_pow_hash;

    const auto official_pow = from_hex(
        "fa70fbf9979f80ec3db2c3f118a5e683fcf5f54ea7edc41b0b5d336508694cb8");

    CHECK(classify_pow_hash(official_pow, "1", "2") ==
          CandidateClassification::Share);
    CHECK(classify_pow_hash(official_pow, "2", "3") ==
          CandidateClassification::Invalid);
    CHECK(classify_pow_hash(official_pow, "1", "1") ==
          CandidateClassification::Block);

    zano_p2pool::Hash256 zero_hash{};
    CHECK(classify_pow_hash(zero_hash, "1000000000000", "2000000000000") ==
          CandidateClassification::Block);

    CHECK(std::string_view(candidate_classification_name(
              CandidateClassification::Invalid)) == "invalid");
    CHECK(std::string_view(candidate_classification_name(
              CandidateClassification::Share)) == "share");
    CHECK(std::string_view(candidate_classification_name(
              CandidateClassification::Block)) == "block");

    return 0;
}
