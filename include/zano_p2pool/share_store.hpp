#pragma once

#include "zano_p2pool/share.hpp"
#include "zano_p2pool/share_chain.hpp"
#include "zano_p2pool/sidechain_params.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace zano_p2pool {

inline constexpr std::array<std::uint8_t, 8> kShareStoreMagic{
    'Z', 'P', '2', 'S', 'T', 'O', 'R', '1'};
inline constexpr std::size_t kShareStoreHeaderSize =
    kShareStoreMagic.size() + SidechainId{}.size();

struct ShareStoreLoadResult {
    std::size_t records_loaded{0};
    std::size_t connected_shares{0};
    std::size_t orphan_shares{0};
    bool truncated_tail_repaired{false};
    std::optional<ShareId> best_tip_id;
};

// Append-only persistence for already-validated sidechain shares.
//
// File format:
//   8 bytes  magic/version ("ZP2STOR1")
//   32 bytes canonical SidechainId
//   repeated records:
//     4 bytes  big-endian serialized-share length
//     N bytes  canonical serialize_share() payload
//     32 bytes share_id(payload)
//
// The SidechainId prevents cross-network/consensus-profile replay. The record
// hash detects full-record corruption. A short final record is treated as an
// interrupted append and can be truncated back to the last complete record.
// Replay intentionally uses add_share_unchecked(): every record was validated
// before it was persisted, while the canonical serialization, record hash,
// structural checks, parent linkage and deterministic cumulative-work/tip logic
// are re-established during recovery.
class ShareStore {
public:
    ShareStore(std::filesystem::path path, SidechainId expected_sidechain_id)
        : path_(std::move(path)),
          expected_sidechain_id_(expected_sidechain_id) {
        if (path_.empty()) {
            throw std::invalid_argument("share store path must not be empty");
        }
        if (is_zero_sidechain_id(expected_sidechain_id_)) {
            throw std::invalid_argument("share store sidechain id must be nonzero");
        }
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

    void append(const Share& share) {
        const std::vector<std::uint8_t> payload = serialize_share(share);
        if (payload.size() != kShareV1SerializedSize &&
            payload.size() != kShareV2SerializedSize) {
            throw std::logic_error("unexpected serialized share size");
        }
        const ShareId id = share_id(share);

        std::lock_guard lock(mutex_);
        ensure_header();

        std::ofstream out(path_, std::ios::binary | std::ios::app);
        if (!out) {
            throw std::runtime_error("failed to open share store for append");
        }

        write_u32_be(out, static_cast<std::uint32_t>(payload.size()));
        write_bytes(out, payload);
        write_bytes(out, id);
        out.flush();
        if (!out) {
            throw std::runtime_error("failed to append share store record");
        }
    }

    [[nodiscard]] ShareStoreLoadResult load_into(
        ShareChain& chain,
        bool repair_truncated_tail = true) const {
        std::lock_guard lock(mutex_);

        ShareStoreLoadResult result;
        if (!std::filesystem::exists(path_)) {
            return result;
        }

        const std::uintmax_t file_size = std::filesystem::file_size(path_);
        if (file_size < kShareStoreHeaderSize) {
            throw std::runtime_error("share store header is truncated");
        }

        std::ifstream in(path_, std::ios::binary);
        if (!in) {
            throw std::runtime_error("failed to open share store for recovery");
        }

        verify_header(in);
        std::uintmax_t last_good_offset = kShareStoreHeaderSize;

        while (true) {
            std::array<std::uint8_t, 4> length_bytes{};
            in.read(
                reinterpret_cast<char*>(length_bytes.data()),
                static_cast<std::streamsize>(length_bytes.size()));
            const std::streamsize length_read = in.gcount();
            if (length_read == 0 && in.eof()) {
                break;
            }
            if (length_read != static_cast<std::streamsize>(length_bytes.size())) {
                repair_tail_or_throw(last_good_offset, repair_truncated_tail);
                result.truncated_tail_repaired = true;
                break;
            }

            const std::uint32_t length = read_u32_be(length_bytes);
            if (length != kShareV1SerializedSize &&
                length != kShareV2SerializedSize) {
                throw std::runtime_error("share store record has invalid length");
            }

            std::vector<std::uint8_t> payload(length);
            in.read(
                reinterpret_cast<char*>(payload.data()),
                static_cast<std::streamsize>(payload.size()));
            if (in.gcount() != static_cast<std::streamsize>(payload.size())) {
                repair_tail_or_throw(last_good_offset, repair_truncated_tail);
                result.truncated_tail_repaired = true;
                break;
            }

            ShareId stored_id{};
            in.read(
                reinterpret_cast<char*>(stored_id.data()),
                static_cast<std::streamsize>(stored_id.size()));
            if (in.gcount() != static_cast<std::streamsize>(stored_id.size())) {
                repair_tail_or_throw(last_good_offset, repair_truncated_tail);
                result.truncated_tail_repaired = true;
                break;
            }

            const Share share = deserialize_share(payload);
            if (share_id(share) != stored_id) {
                throw std::runtime_error("share store record hash mismatch");
            }

            const AddShareResult added = chain.add_share_unchecked(share);
            if (added.disposition == ShareDisposition::Duplicate) {
                throw std::runtime_error("share store contains duplicate record");
            }
            if (added.disposition == ShareDisposition::Rejected) {
                throw std::runtime_error(
                    std::string("share store replay rejected share: ") +
                    share_reject_reason_name(added.reject_reason));
            }

            ++result.records_loaded;
            last_good_offset +=
                length_bytes.size() + payload.size() + stored_id.size();
        }

        result.connected_shares = chain.connected_size();
        result.orphan_shares = chain.orphan_size();
        if (const ConnectedShare* tip = chain.best_tip(); tip != nullptr) {
            result.best_tip_id = tip->id;
        }
        return result;
    }

private:
    static void write_u32_be(std::ostream& out, std::uint32_t value) {
        const std::array<std::uint8_t, 4> bytes{
            static_cast<std::uint8_t>(value >> 24),
            static_cast<std::uint8_t>(value >> 16),
            static_cast<std::uint8_t>(value >> 8),
            static_cast<std::uint8_t>(value),
        };
        write_bytes(out, bytes);
    }

    template <typename Container>
    static void write_bytes(std::ostream& out, const Container& bytes) {
        out.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    }

    static std::uint32_t read_u32_be(
        const std::array<std::uint8_t, 4>& bytes) noexcept {
        return (static_cast<std::uint32_t>(bytes[0]) << 24) |
               (static_cast<std::uint32_t>(bytes[1]) << 16) |
               (static_cast<std::uint32_t>(bytes[2]) << 8) |
               static_cast<std::uint32_t>(bytes[3]);
    }

    void ensure_header() const {
        if (std::filesystem::exists(path_)) {
            if (std::filesystem::file_size(path_) < kShareStoreHeaderSize) {
                throw std::runtime_error("existing share store header is truncated");
            }
            std::ifstream in(path_, std::ios::binary);
            if (!in) {
                throw std::runtime_error("failed to open existing share store");
            }
            verify_header(in);
            return;
        }

        const std::filesystem::path parent = path_.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent);
        }

        const std::filesystem::path temporary = path_.string() + ".tmp";
        {
            std::ofstream out(
                temporary,
                std::ios::binary | std::ios::trunc);
            if (!out) {
                throw std::runtime_error("failed to create share store header");
            }
            write_bytes(out, kShareStoreMagic);
            write_bytes(out, expected_sidechain_id_);
            out.flush();
            if (!out) {
                throw std::runtime_error("failed to write share store header");
            }
        }

        std::error_code ec;
        std::filesystem::rename(temporary, path_, ec);
        if (ec) {
            std::filesystem::remove(temporary);
            if (!std::filesystem::exists(path_)) {
                throw std::runtime_error(
                    "failed to install share store header: " + ec.message());
            }
            std::ifstream in(path_, std::ios::binary);
            if (!in) {
                throw std::runtime_error("failed to open concurrently created share store");
            }
            verify_header(in);
        }
    }

    void verify_header(std::istream& in) const {
        std::array<std::uint8_t, kShareStoreMagic.size()> magic{};
        in.read(
            reinterpret_cast<char*>(magic.data()),
            static_cast<std::streamsize>(magic.size()));
        if (in.gcount() != static_cast<std::streamsize>(magic.size()) ||
            magic != kShareStoreMagic) {
            throw std::runtime_error("invalid share store magic/version");
        }

        SidechainId stored_sidechain_id{};
        in.read(
            reinterpret_cast<char*>(stored_sidechain_id.data()),
            static_cast<std::streamsize>(stored_sidechain_id.size()));
        if (in.gcount() != static_cast<std::streamsize>(stored_sidechain_id.size())) {
            throw std::runtime_error("share store sidechain id is truncated");
        }
        if (stored_sidechain_id != expected_sidechain_id_) {
            throw std::runtime_error("share store belongs to a different sidechain");
        }
    }

    void repair_tail_or_throw(
        std::uintmax_t last_good_offset,
        bool repair_truncated_tail) const {
        if (!repair_truncated_tail) {
            throw std::runtime_error("share store final record is truncated");
        }
        std::filesystem::resize_file(path_, last_good_offset);
    }

    std::filesystem::path path_;
    SidechainId expected_sidechain_id_{};
    mutable std::mutex mutex_;
};

}  // namespace zano_p2pool
