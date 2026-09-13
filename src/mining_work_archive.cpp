#include "zano_p2pool/mining_work_archive.hpp"
#include "zano_p2pool/p2p_mining_context.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unistd.h>
#include <sys/stat.h>

namespace zano_p2pool {
namespace {
constexpr std::array<std::uint8_t, 8> magic{'Z','P','2','W','O','R','K','1'};
constexpr std::size_t header_size = 8 + 32 + 4;
constexpr std::size_t digest_size = 32;

void check_payload_size(std::size_t size) {
    if (size < kP2pMiningContextFixedPayloadSize || size > kP2pMaxPayloadSize) {
        throw std::runtime_error("invalid mining-work archive payload length");
    }
}

struct Fd {
    int value;
    ~Fd() { if (value >= 0) ::close(value); }
};

void sync_fd(int fd) {
    int result;
    do { result = ::fsync(fd); } while (result < 0 && errno == EINTR);
    if (result < 0) throw std::system_error(errno, std::generic_category(), "fsync mining-work archive");
}

void ensure_directory(const std::filesystem::path& path) {
    if (std::filesystem::exists(path)) {
        if (!std::filesystem::is_directory(path))
            throw std::runtime_error("mining-work archive path is not a directory");
        return;
    }
    const auto parent = path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
    ensure_directory(parent);
    if (::mkdir(path.c_str(), 0700) < 0 && errno != EEXIST)
        throw std::system_error(errno, std::generic_category(), "create mining-work archive directory");
    if (!std::filesystem::is_directory(path))
        throw std::runtime_error("mining-work archive path is not a directory");
    Fd parent_fd{::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    if (parent_fd.value < 0)
        throw std::system_error(errno, std::generic_category(), "open archive parent directory");
    sync_fd(parent_fd.value);
}

void write_all(int fd, std::span<const std::uint8_t> bytes) {
    while (!bytes.empty()) {
        const auto n = ::write(fd, bytes.data(), bytes.size());
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) throw std::system_error(n < 0 ? errno : EIO, std::generic_category(), "write mining-work archive");
        bytes = bytes.subspan(static_cast<std::size_t>(n));
    }
}

Hash256 id_from_filename(const std::filesystem::path& path) {
    const auto name = path.stem().string();
    if (name.size() != 64 || !std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    })) throw std::runtime_error("invalid mining-work archive filename");
    const auto bytes = hex_to_bytes(name);
    Hash256 id{};
    std::copy(bytes.begin(), bytes.end(), id.begin());
    return id;
}
} // namespace

MiningWorkArchive::MiningWorkArchive(std::filesystem::path directory, Hash256 sidechain_id)
    : directory_(std::move(directory)), sidechain_id_(sidechain_id) {
    if (directory_.empty() || sidechain_id_ == Hash256{})
        throw std::invalid_argument("mining-work archive requires directory and sidechain ID");
    ensure_directory(directory_);
}

std::vector<std::uint8_t> MiningWorkArchive::read(const Hash256& id) const {
    const auto file = directory_ / (hash_to_hex(id) + ".work");
    if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(file)))
        throw std::runtime_error("mining-work archive record is not a regular file");
    const auto size = std::filesystem::file_size(file);
    if (size < header_size + digest_size || size > header_size + kP2pMaxPayloadSize + digest_size)
        throw std::runtime_error("invalid mining-work archive record size");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    std::ifstream in(file, std::ios::binary);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), bytes.size()) || in.peek() != EOF)
        throw std::runtime_error("failed to read mining-work archive record");
    if (!std::equal(magic.begin(), magic.end(), bytes.begin()) ||
        !std::equal(sidechain_id_.begin(), sidechain_id_.end(), bytes.begin() + 8))
        throw std::runtime_error("mining-work archive version/sidechain mismatch");
    std::size_t length = 0;
    for (std::size_t i = 40; i < header_size; ++i) length = (length << 8) | bytes[i];
    check_payload_size(length);
    if (size != header_size + length + digest_size)
        throw std::runtime_error("mining-work archive length mismatch");
    const std::span<const std::uint8_t> payload(bytes.data() + header_size, length);
    const auto checksum = cn_fast_hash(std::span<const std::uint8_t>(bytes).first(header_size + length));
    if (cn_fast_hash(payload) != id ||
        !std::equal(checksum.begin(), checksum.end(), bytes.end() - digest_size))
        throw std::runtime_error("mining-work archive checksum/ID mismatch");
    return {payload.begin(), payload.end()};
}

Hash256 MiningWorkArchive::put(std::span<const std::uint8_t> payload) {
    check_payload_size(payload.size());
    const auto id = cn_fast_hash(payload);
    const auto destination = directory_ / (hash_to_hex(id) + ".work");
    Fd directory_fd{::open(directory_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    if (directory_fd.value < 0)
        throw std::system_error(errno, std::generic_category(), "open mining-work archive directory");
    if (std::filesystem::exists(destination)) {
        const auto existing = read(id);
        if (!std::equal(existing.begin(), existing.end(), payload.begin(), payload.end()))
            throw std::runtime_error("conflicting mining-work archive record");
        sync_fd(directory_fd.value);
        return id;
    }
    std::vector<std::uint8_t> record(magic.begin(), magic.end());
    record.insert(record.end(), sidechain_id_.begin(), sidechain_id_.end());
    for (int shift = 24; shift >= 0; shift -= 8)
        record.push_back(static_cast<std::uint8_t>(payload.size() >> shift));
    record.insert(record.end(), payload.begin(), payload.end());
    const auto checksum = cn_fast_hash(record);
    record.insert(record.end(), checksum.begin(), checksum.end());

    auto temporary = (directory_ / ".tmp-XXXXXX").string();
    Fd fd{::mkstemp(temporary.data())}; // mode 0600, exclusive creation
    if (fd.value < 0)
        throw std::system_error(errno, std::generic_category(), "create mining-work archive record");
    try {
        write_all(fd.value, record);
        sync_fd(fd.value);
        // Atomic no-clobber publication, even with concurrent identical puts.
        if (::link(temporary.c_str(), destination.c_str()) < 0) {
            if (errno != EEXIST)
                throw std::system_error(errno, std::generic_category(), "publish mining-work archive record");
            const auto existing = read(id);
            if (!std::equal(existing.begin(), existing.end(), payload.begin(), payload.end()))
                throw std::runtime_error("conflicting mining-work archive record");
        }
        if (::unlink(temporary.c_str()) < 0)
            throw std::system_error(errno, std::generic_category(), "remove mining-work archive temporary");
        sync_fd(directory_fd.value);
    } catch (...) {
        ::unlink(temporary.c_str());
        throw;
    }
    return id;
}

std::vector<Hash256> MiningWorkArchive::list_ids(
    std::size_t max_records) const {
    std::vector<Hash256> ids;
    for (const auto& entry : std::filesystem::directory_iterator(directory_)) {
        if (entry.path().filename().string().starts_with(".tmp-")) continue;
        if (entry.path().extension() != ".work")
            throw std::runtime_error("unexpected file in mining-work archive");
        if (ids.size() >= max_records)
            throw std::runtime_error(
                "mining-work archive scan limit exceeded");
        const Hash256 id = id_from_filename(entry.path());
        static_cast<void>(read(id));
        ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

std::size_t MiningWorkArchive::verify_all() const {
    return list_ids().size();
}
} // namespace zano_p2pool
