#include "zano_p2pool/replay_validation_store.hpp"

#include "zano_p2pool/crypto_hash.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <span>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

#include <sys/stat.h>

namespace zano_p2pool {
namespace {

constexpr std::array<std::uint8_t, 8> kMagic{
    'Z', 'P', '2', 'V', 'A', 'L', '0', '1',
};

constexpr std::size_t kPrefixSize =
    kMagic.size() +
    SidechainId{}.size() +
    sizeof(std::uint64_t) +
    Hash256{}.size() +
    sizeof(std::uint64_t);

constexpr std::size_t kRecordSize =
    ShareId{}.size() +
    Hash256{}.size() +
    Hash256{}.size() +
    3;

constexpr std::size_t kDigestSize = Hash256{}.size();
constexpr std::size_t kMaxRecords = 1'000'000;

void append_bytes(
    std::vector<std::uint8_t>& out,
    std::span<const std::uint8_t> bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
}

void append_u64_be(
    std::vector<std::uint8_t>& out,
    std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(
            static_cast<std::uint8_t>(value >> shift));
    }
}

std::uint64_t read_u64_be(
    std::span<const std::uint8_t> bytes,
    std::size_t& offset) {
    if (offset > bytes.size() ||
        bytes.size() - offset < sizeof(std::uint64_t)) {
        throw std::runtime_error(
            "truncated replay-validation u64");
    }

    std::uint64_t value = 0;
    for (std::size_t i = 0; i < sizeof(std::uint64_t); ++i) {
        value =
            (value << 8) |
            static_cast<std::uint64_t>(bytes[offset++]);
    }
    return value;
}

template <typename Array>
Array read_array(
    std::span<const std::uint8_t> bytes,
    std::size_t& offset,
    const char* context) {
    Array result{};

    if (offset > bytes.size() ||
        bytes.size() - offset < result.size()) {
        throw std::runtime_error(
            std::string("truncated replay-validation ") +
            context);
    }

    std::copy_n(
        bytes.begin() + static_cast<std::ptrdiff_t>(offset),
        result.size(),
        result.begin());

    offset += result.size();
    return result;
}

void validate_record(
    const ReplayValidationRecord& record) {
    if (is_zero_share_id(record.share_id)) {
        throw std::runtime_error(
            "replay-validation record has zero ShareId");
    }

    if (!record.validation.meets_share_difficulty) {
        throw std::runtime_error(
            "replay-validation record does not meet share difficulty");
    }

    switch (record.validation.classification) {
    case CandidateClassification::Share:
        if (record.validation.meets_network_difficulty) {
            throw std::runtime_error(
                "share-class replay-validation record "
                "unexpectedly meets network difficulty");
        }
        break;

    case CandidateClassification::Block:
        if (!record.validation.meets_network_difficulty) {
            throw std::runtime_error(
                "block-class replay-validation record "
                "does not meet network difficulty");
        }
        break;

    case CandidateClassification::Invalid:
        throw std::runtime_error(
            "invalid candidate cannot be persisted as validated");
    }
}

std::uint8_t classification_byte(
    CandidateClassification classification) {
    switch (classification) {
    case CandidateClassification::Share:
        return 1;
    case CandidateClassification::Block:
        return 2;
    case CandidateClassification::Invalid:
        break;
    }

    throw std::runtime_error(
        "invalid replay-validation classification");
}

CandidateClassification classification_from_byte(
    std::uint8_t value) {
    switch (value) {
    case 1:
        return CandidateClassification::Share;
    case 2:
        return CandidateClassification::Block;
    default:
        throw std::runtime_error(
            "invalid replay-validation classification byte");
    }
}

bool bool_from_byte(
    std::uint8_t value,
    const char* context) {
    if (value > 1) {
        throw std::runtime_error(
            std::string("invalid replay-validation boolean: ") +
            context);
    }
    return value != 0;
}

std::vector<std::uint8_t> serialize_snapshot(
    const SidechainId& sidechain_id,
    const ReplayValidationSnapshot& snapshot) {
    if (snapshot.checkpoint.block_hash == Hash256{}) {
        throw std::runtime_error(
            "replay-validation checkpoint hash must be nonzero");
    }

    if (snapshot.records.size() > kMaxRecords) {
        throw std::runtime_error(
            "replay-validation record count exceeds limit");
    }

    std::vector<ReplayValidationRecord> records =
        snapshot.records;

    std::sort(
        records.begin(),
        records.end(),
        [](const ReplayValidationRecord& left,
           const ReplayValidationRecord& right) {
            return left.share_id < right.share_id;
        });

    for (std::size_t i = 0; i < records.size(); ++i) {
        validate_record(records[i]);

        if (i != 0 &&
            records[i - 1].share_id == records[i].share_id) {
            throw std::runtime_error(
                "replay-validation snapshot contains duplicate ShareId");
        }
    }

    if (records.size() >
        (std::numeric_limits<std::size_t>::max() -
         kPrefixSize -
         kDigestSize) /
            kRecordSize) {
        throw std::runtime_error(
            "replay-validation snapshot size overflow");
    }

    std::vector<std::uint8_t> bytes;
    bytes.reserve(
        kPrefixSize +
        records.size() * kRecordSize +
        kDigestSize);

    append_bytes(bytes, kMagic);
    append_bytes(bytes, sidechain_id);
    append_u64_be(
        bytes,
        snapshot.checkpoint.zano_height);
    append_bytes(
        bytes,
        snapshot.checkpoint.block_hash);
    append_u64_be(
        bytes,
        static_cast<std::uint64_t>(records.size()));

    for (const ReplayValidationRecord& record : records) {
        append_bytes(bytes, record.share_id);
        append_bytes(
            bytes,
            record.validation.pow.final_hash);
        append_bytes(
            bytes,
            record.validation.pow.mix_hash);

        bytes.push_back(
            record.validation.meets_share_difficulty ? 1 : 0);
        bytes.push_back(
            record.validation.meets_network_difficulty ? 1 : 0);
        bytes.push_back(
            classification_byte(
                record.validation.classification));
    }

    const Hash256 digest =
        cn_fast_hash(
            std::span<const std::uint8_t>(
                bytes.data(),
                bytes.size()));

    append_bytes(bytes, digest);
    return bytes;
}

std::vector<std::uint8_t> read_file(
    const std::filesystem::path& path) {
    std::ifstream in(
        path,
        std::ios::binary | std::ios::ate);

    if (!in) {
        throw std::runtime_error(
            "failed to open replay-validation store");
    }

    const std::streampos end = in.tellg();
    if (end < 0) {
        throw std::runtime_error(
            "failed to size replay-validation store");
    }

    const auto size_u =
        static_cast<std::uintmax_t>(end);

    if (size_u >
        static_cast<std::uintmax_t>(
            std::numeric_limits<std::size_t>::max())) {
        throw std::runtime_error(
            "replay-validation store is too large");
    }

    const std::size_t size =
        static_cast<std::size_t>(size_u);

    std::vector<std::uint8_t> bytes(size);

    in.seekg(0, std::ios::beg);
    if (!in) {
        throw std::runtime_error(
            "failed to seek replay-validation store");
    }

    if (!bytes.empty()) {
        in.read(
            reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));

        if (!in ||
            static_cast<std::size_t>(in.gcount()) !=
                bytes.size()) {
            throw std::runtime_error(
                "failed to read replay-validation store");
        }
    }

    return bytes;
}

ReplayValidationSnapshot parse_snapshot(
    const SidechainId& expected_sidechain_id,
    const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < kPrefixSize + kDigestSize) {
        throw std::runtime_error(
            "replay-validation store is truncated");
    }

    const std::size_t digest_offset =
        bytes.size() - kDigestSize;

    const Hash256 actual_digest =
        cn_fast_hash(
            std::span<const std::uint8_t>(
                bytes.data(),
                digest_offset));

    std::size_t digest_read_offset = digest_offset;
    const Hash256 stored_digest =
        read_array<Hash256>(
            bytes,
            digest_read_offset,
            "digest");

    if (actual_digest != stored_digest) {
        throw std::runtime_error(
            "replay-validation store digest mismatch");
    }

    std::size_t offset = 0;

    const auto magic =
        read_array<std::array<std::uint8_t, 8>>(
            bytes,
            offset,
            "magic");

    if (magic != kMagic) {
        throw std::runtime_error(
            "invalid replay-validation store magic");
    }

    const SidechainId sidechain_id =
        read_array<SidechainId>(
            bytes,
            offset,
            "sidechain id");

    if (sidechain_id != expected_sidechain_id) {
        throw std::runtime_error(
            "replay-validation sidechain id mismatch");
    }

    ReplayValidationSnapshot snapshot;

    snapshot.checkpoint.zano_height =
        read_u64_be(bytes, offset);

    snapshot.checkpoint.block_hash =
        read_array<Hash256>(
            bytes,
            offset,
            "checkpoint hash");

    if (snapshot.checkpoint.block_hash == Hash256{}) {
        throw std::runtime_error(
            "replay-validation checkpoint hash is zero");
    }

    const std::uint64_t record_count_u64 =
        read_u64_be(bytes, offset);

    if (record_count_u64 > kMaxRecords ||
        record_count_u64 >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max())) {
        throw std::runtime_error(
            "replay-validation record count exceeds limit");
    }

    const std::size_t record_count =
        static_cast<std::size_t>(record_count_u64);

    if (record_count >
        (std::numeric_limits<std::size_t>::max() -
         kPrefixSize -
         kDigestSize) /
            kRecordSize) {
        throw std::runtime_error(
            "replay-validation record size overflow");
    }

    const std::size_t expected_size =
        kPrefixSize +
        record_count * kRecordSize +
        kDigestSize;

    if (bytes.size() != expected_size) {
        throw std::runtime_error(
            "replay-validation store size mismatch");
    }

    snapshot.records.reserve(record_count);

    for (std::size_t i = 0; i < record_count; ++i) {
        ReplayValidationRecord record;

        record.share_id =
            read_array<ShareId>(
                bytes,
                offset,
                "ShareId");

        record.validation.pow.final_hash =
            read_array<Hash256>(
                bytes,
                offset,
                "PoW final hash");

        record.validation.pow.mix_hash =
            read_array<Hash256>(
                bytes,
                offset,
                "PoW mix hash");

        if (offset + 3 > digest_offset) {
            throw std::runtime_error(
                "truncated replay-validation record flags");
        }

        record.validation.meets_share_difficulty =
            bool_from_byte(
                bytes[offset++],
                "meets-share-difficulty");

        record.validation.meets_network_difficulty =
            bool_from_byte(
                bytes[offset++],
                "meets-network-difficulty");

        record.validation.classification =
            classification_from_byte(
                bytes[offset++]);

        validate_record(record);

        if (!snapshot.records.empty() &&
            snapshot.records.back().share_id >=
                record.share_id) {
            throw std::runtime_error(
                "replay-validation records are "
                "not strictly ordered");
        }

        snapshot.records.push_back(
            std::move(record));
    }

    if (offset != digest_offset) {
        throw std::runtime_error(
            "replay-validation parser did not consume "
            "the complete record region");
    }

    return snapshot;
}

}  // namespace

ReplayValidationStore::ReplayValidationStore(
    std::filesystem::path path,
    SidechainId expected_sidechain_id)
    : path_(std::move(path)),
      expected_sidechain_id_(
          expected_sidechain_id) {
    if (path_.empty()) {
        throw std::invalid_argument(
            "replay-validation store path must not be empty");
    }

    if (expected_sidechain_id_ == SidechainId{}) {
        throw std::invalid_argument(
            "replay-validation sidechain id must be nonzero");
    }
}

void ReplayValidationStore::save(
    const ReplayValidationSnapshot& snapshot) {
    std::lock_guard lock(mutex_);

    const std::vector<std::uint8_t> bytes =
        serialize_snapshot(
            expected_sidechain_id_,
            snapshot);

    const std::filesystem::path parent =
        path_.parent_path();

    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }

    const std::filesystem::path temporary =
        path_.string() + ".rewrite.tmp";

    std::error_code cleanup_error;
    std::filesystem::remove(
        temporary,
        cleanup_error);

    try {
        std::ofstream out(
            temporary,
            std::ios::binary |
                std::ios::trunc);

        if (!out) {
            throw std::runtime_error(
                "failed to create replay-validation rewrite");
        }

        out.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));

        out.flush();

        if (!out) {
            throw std::runtime_error(
                "failed to write replay-validation rewrite");
        }

        out.close();

        if (!out) {
            throw std::runtime_error(
                "failed to close replay-validation rewrite");
        }

        if (::chmod(temporary.c_str(), 0600) != 0) {
            throw std::system_error(
                errno,
                std::generic_category(),
                "failed to chmod replay-validation rewrite");
        }

        std::filesystem::rename(
            temporary,
            path_);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(
            temporary,
            ignored);
        throw;
    }
}

std::optional<ReplayValidationSnapshot>
ReplayValidationStore::load() const {
    std::lock_guard lock(mutex_);

    if (!std::filesystem::exists(path_)) {
        return std::nullopt;
    }

    return parse_snapshot(
        expected_sidechain_id_,
        read_file(path_));
}

}  // namespace zano_p2pool
