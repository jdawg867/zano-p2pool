#include "zano_p2pool/mining_work_archive.hpp"
#include "zano_p2pool/p2p_mining_context.hpp"
#include "test_check.hpp"
#include <filesystem>
#include <fstream>
#include <functional>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

using namespace zano_p2pool;
namespace {
bool rejects(const std::function<void()>& fn) {
    try { fn(); } catch (const std::exception&) { return true; }
    return false;
}
struct Temporary {
    std::filesystem::path path;
    Temporary() {
        std::string pattern = (std::filesystem::temp_directory_path() / "zano-work-test-XXXXXX").string();
        if (!mkdtemp(pattern.data())) throw std::runtime_error("mkdtemp failed");
        path = pattern;
    }
    ~Temporary() { std::filesystem::remove_all(path); }
};
void overwrite(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

void append_u32_be(
    std::vector<std::uint8_t>& out,
    std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 24));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

// Test-only fast path for constructing a valid durable-record image without
// calling MiningWorkArchive::put() 10,001 times (put intentionally fsyncs each
// publication). This mirrors the on-disk framing so the production reader and
// checksum/ID validation still inspect every record.
void write_valid_archive_record(
    const std::filesystem::path& directory,
    const Hash256& sidechain,
    std::span<const std::uint8_t> payload) {

    static constexpr std::array<std::uint8_t, 8> magic{
        'Z','P','2','W','O','R','K','1'
    };

    const auto id = cn_fast_hash(payload);

    std::vector<std::uint8_t> record(
        magic.begin(),
        magic.end());

    record.insert(
        record.end(),
        sidechain.begin(),
        sidechain.end());

    append_u32_be(
        record,
        static_cast<std::uint32_t>(payload.size()));

    record.insert(
        record.end(),
        payload.begin(),
        payload.end());

    const auto checksum = cn_fast_hash(record);

    record.insert(
        record.end(),
        checksum.begin(),
        checksum.end());

    overwrite(
        directory / (hash_to_hex(id) + ".work"),
        record);
}
}
int main() {
    Temporary temporary;
    Hash256 sidechain{}; sidechain[0] = 1;
    // Storage treats serialized proposals as opaque bounded evidence. These
    // bytes are deliberately not proof-valid: storage must not imply trust.
    std::vector<std::uint8_t> payload(kP2pMiningContextFixedPayloadSize + 200, 0xa5);
    MiningWorkArchive archive(temporary.path / "work", sidechain);
    CHECK(archive.verify_all() == 0);
    const auto id = archive.put(payload);
    CHECK(id == cn_fast_hash(payload));
    CHECK(archive.read(id) == payload);
    CHECK(archive.put(payload) == id);
    CHECK(archive.verify_all() == 1);
    const auto file = archive.path() / (hash_to_hex(id) + ".work");
    struct stat st{};
    CHECK(stat(file.c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0600);
    MiningWorkArchive recovered(archive.path(), sidechain);
    CHECK(recovered.verify_all() == 1);
    CHECK(recovered.read(id) == payload);
    std::ofstream(archive.path() / ".tmp-interrupted") << "incomplete";
    CHECK(recovered.verify_all() == 1);

    auto other_chain = sidechain; other_chain[0] = 2;
    MiningWorkArchive wrong_network(archive.path(), other_chain);
    CHECK(rejects([&] { static_cast<void>(wrong_network.verify_all()); }));
    CHECK(rejects([&] { static_cast<void>(wrong_network.put(payload)); }));
    CHECK(rejects([&] { static_cast<void>(archive.put({})); }));
    std::vector<std::uint8_t> oversized(kP2pMaxPayloadSize + 1, 0);
    CHECK(rejects([&] { static_cast<void>(archive.put(oversized)); }));

    auto second = payload; second.back() = 7;
    std::exception_ptr failure_a, failure_b;
    std::thread a([&] { try { static_cast<void>(archive.put(second)); } catch (...) { failure_a = std::current_exception(); } });
    std::thread b([&] { try { static_cast<void>(archive.put(second)); } catch (...) { failure_b = std::current_exception(); } });
    a.join(); b.join();
    CHECK(!failure_a && !failure_b);
    CHECK(archive.verify_all() == 2);
    CHECK(archive.read(cn_fast_hash(second)) == second);

    // Explicit bounded enumeration still fails closed instead of returning a
    // partial archive. Only the arbitrary default lifetime ceiling is removed.
    CHECK(rejects([&] {
        static_cast<void>(
            archive.list_ids(1));
    }));

    std::ifstream in(file, std::ios::binary);
    const std::vector<std::uint8_t> original((std::istreambuf_iterator<char>(in)), {});
    in.close();
    for (std::size_t i : {std::size_t(0), std::size_t(8), std::size_t(40), std::size_t(44), original.size()-1}) {
        auto corrupted = original; corrupted[i] ^= 1;
        overwrite(file, corrupted);
        CHECK(rejects([&] { static_cast<void>(archive.verify_all()); }));
        CHECK(rejects([&] { static_cast<void>(archive.put(payload)); }));
    }
    overwrite(file, original);
    for (std::size_t length = 0; length < original.size(); ++length) {
        std::filesystem::resize_file(file, length);
        CHECK(rejects([&] { static_cast<void>(archive.read(id)); }));
        overwrite(file, original);
    }
    auto trailing = original; trailing.push_back(0);
    overwrite(file, trailing);
    CHECK(rejects([&] { static_cast<void>(archive.read(id)); }));
    overwrite(file, original);
    CHECK(archive.verify_all() == 2);
    std::filesystem::rename(file, archive.path() / (std::string(64, '0') + ".work"));
    CHECK(rejects([&] { static_cast<void>(archive.verify_all()); }));

    // Regression: a healthy append-only mining-work archive must not become
    // invalid merely because its lifetime record count crosses 10,000.
    //
    // Production hit this exact boundary during sustained mining: historical
    // recovery began rejecting every attempt after the archive grew past the
    // old default scan ceiling.
    Temporary growth;
    MiningWorkArchive grown_archive(
        growth.path / "work",
        sidechain);

    constexpr std::size_t kGrowthRecords = 10'001;

    std::vector<std::uint8_t> growth_payload(
        kP2pMiningContextFixedPayloadSize + 32,
        0x5a);

    for (std::uint64_t i = 0; i < kGrowthRecords; ++i) {
        // Keep every payload distinct so every filename/content ID is distinct.
        // Storage treats the payload as bounded opaque evidence; structural
        // mining-context validation belongs to the trust layer, not the archive.
        for (unsigned byte = 0; byte < 8; ++byte) {
            growth_payload[
                growth_payload.size() - 1 - byte] =
                    static_cast<std::uint8_t>(
                        i >> (byte * 8));
        }

        write_valid_archive_record(
            grown_archive.path(),
            sidechain,
            growth_payload);
    }

    CHECK(grown_archive.verify_all() == kGrowthRecords);

    return 0;
}
