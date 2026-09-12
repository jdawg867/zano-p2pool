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
    return 0;
}
