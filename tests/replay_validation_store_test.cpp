#include "zano_p2pool/replay_validation_store.hpp"
#include "test_check.hpp"

#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace zano_p2pool;

namespace {

bool rejects(const std::function<void()>& fn) {
    try {
        fn();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

struct Temporary {
    std::filesystem::path path;

    Temporary() {
        std::string pattern =
            (std::filesystem::temp_directory_path() /
             "zano-validation-test-XXXXXX")
                .string();

        if (!mkdtemp(pattern.data())) {
            throw std::runtime_error("mkdtemp failed");
        }

        path = pattern;
    }

    ~Temporary() {
        std::filesystem::remove_all(path);
    }
};

std::vector<std::uint8_t> read_bytes(
    const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);

    return {
        std::istreambuf_iterator<char>(in),
        std::istreambuf_iterator<char>(),
    };
}

void overwrite(
    const std::filesystem::path& path,
    const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(
        path,
        std::ios::binary | std::ios::trunc);

    out.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));

    if (!out) {
        throw std::runtime_error("test overwrite failed");
    }
}

CandidateValidation share_validation(std::uint8_t tag) {
    CandidateValidation validation;
    validation.pow.final_hash[0] = tag;
    validation.pow.final_hash[31] =
        static_cast<std::uint8_t>(tag + 1);
    validation.pow.mix_hash[0] =
        static_cast<std::uint8_t>(tag + 2);
    validation.pow.mix_hash[31] =
        static_cast<std::uint8_t>(tag + 3);
    validation.meets_share_difficulty = true;
    validation.meets_network_difficulty = false;
    validation.classification =
        CandidateClassification::Share;
    return validation;
}

CandidateValidation block_validation(std::uint8_t tag) {
    CandidateValidation validation =
        share_validation(tag);

    validation.meets_network_difficulty = true;
    validation.classification =
        CandidateClassification::Block;

    return validation;
}

void check_validation_equal(
    const CandidateValidation& left,
    const CandidateValidation& right) {
    CHECK(left.pow.final_hash == right.pow.final_hash);
    CHECK(left.pow.mix_hash == right.pow.mix_hash);
    CHECK(
        left.meets_share_difficulty ==
        right.meets_share_difficulty);
    CHECK(
        left.meets_network_difficulty ==
        right.meets_network_difficulty);
    CHECK(left.classification == right.classification);
}

}  // namespace

int main() {
    Temporary temporary;

    SidechainId sidechain{};
    sidechain[0] = 0x41;

    const auto path =
        temporary.path / "shares.dat.validation";

    ReplayValidationStore store(path, sidechain);

    CHECK(!store.load().has_value());

    ReplayValidationSnapshot snapshot;
    snapshot.checkpoint.zano_height = 200'000;
    snapshot.checkpoint.block_hash[0] = 0xa5;
    snapshot.checkpoint.block_hash[31] = 0x5a;

    ShareId first_id{};
    first_id[0] = 1;
    first_id[31] = 7;

    ShareId second_id{};
    second_id[0] = 2;
    second_id[31] = 9;

    const CandidateValidation first_validation =
        share_validation(0x10);
    const CandidateValidation second_validation =
        block_validation(0x20);

    // Supply reverse order: save() must canonicalize by ShareId.
    snapshot.records.push_back(
        ReplayValidationRecord{
            second_id,
            second_validation,
        });

    snapshot.records.push_back(
        ReplayValidationRecord{
            first_id,
            first_validation,
        });

    store.save(snapshot);

    CHECK(std::filesystem::exists(path));
    CHECK(!std::filesystem::exists(
        path.string() + ".rewrite.tmp"));

    struct stat st{};
    CHECK(::stat(path.c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0600);

    const auto recovered = store.load();
    CHECK(recovered.has_value());
    CHECK(
        recovered->checkpoint.zano_height ==
        snapshot.checkpoint.zano_height);
    CHECK(
        recovered->checkpoint.block_hash ==
        snapshot.checkpoint.block_hash);
    CHECK(recovered->records.size() == 2);
    CHECK(recovered->records[0].share_id == first_id);
    CHECK(recovered->records[1].share_id == second_id);

    check_validation_equal(
        recovered->records[0].validation,
        first_validation);

    check_validation_equal(
        recovered->records[1].validation,
        second_validation);

    // Rewriting identical logical data must produce byte-identical
    // deterministic storage.
    const auto canonical_bytes = read_bytes(path);
    CHECK(!canonical_bytes.empty());

    store.save(snapshot);
    CHECK(read_bytes(path) == canonical_bytes);

    // Sidechain binding is mandatory even when the file itself is valid.
    SidechainId other_sidechain = sidechain;
    other_sidechain[0] ^= 0x7f;

    ReplayValidationStore wrong_sidechain(
        path,
        other_sidechain);

    CHECK(rejects([&] {
        static_cast<void>(wrong_sidechain.load());
    }));

    // Duplicate ShareIds are rejected before replacement.
    ReplayValidationSnapshot duplicate = snapshot;
    duplicate.records.push_back(
        duplicate.records.front());

    CHECK(rejects([&] {
        store.save(duplicate);
    }));

    CHECK(read_bytes(path) == canonical_bytes);

    // Only successful candidate states may cross this persistence
    // boundary.
    ReplayValidationSnapshot invalid = snapshot;
    invalid.records[0].validation =
        CandidateValidation{};

    CHECK(rejects([&] {
        store.save(invalid);
    }));

    CHECK(read_bytes(path) == canonical_bytes);

    ReplayValidationSnapshot inconsistent_share =
        snapshot;

    inconsistent_share.records[0].
        validation.meets_network_difficulty = true;
    inconsistent_share.records[0].
        validation.classification =
            CandidateClassification::Share;

    CHECK(rejects([&] {
        store.save(inconsistent_share);
    }));

    CHECK(read_bytes(path) == canonical_bytes);

    ReplayValidationSnapshot inconsistent_block =
        snapshot;

    inconsistent_block.records[0].
        validation.meets_network_difficulty = false;
    inconsistent_block.records[0].
        validation.classification =
            CandidateClassification::Block;

    CHECK(rejects([&] {
        store.save(inconsistent_block);
    }));

    CHECK(read_bytes(path) == canonical_bytes);

    ReplayValidationSnapshot zero_checkpoint =
        snapshot;

    zero_checkpoint.checkpoint.block_hash = {};

    CHECK(rejects([&] {
        store.save(zero_checkpoint);
    }));

    CHECK(read_bytes(path) == canonical_bytes);

    ReplayValidationSnapshot zero_share =
        snapshot;

    zero_share.records[0].share_id = {};

    CHECK(rejects([&] {
        store.save(zero_share);
    }));

    CHECK(read_bytes(path) == canonical_bytes);

    // Whole-file corruption must fail closed. Exercise magic,
    // sidechain binding, checkpoint, record payload, and final digest.
    for (const std::size_t index : {
             std::size_t{0},
             std::size_t{8},
             std::size_t{45},
             std::size_t{100},
             canonical_bytes.size() - 1}) {
        auto corrupted = canonical_bytes;
        corrupted[index] ^= 1;

        overwrite(path, corrupted);

        CHECK(rejects([&] {
            static_cast<void>(store.load());
        }));

        overwrite(path, canonical_bytes);
    }

    // Every proper prefix is a truncated snapshot and must fail
    // closed rather than partially restoring validation state.
    for (std::size_t length = 0;
         length < canonical_bytes.size();
         ++length) {
        std::filesystem::resize_file(path, length);

        CHECK(rejects([&] {
            static_cast<void>(store.load());
        }));

        overwrite(path, canonical_bytes);
    }

    auto trailing = canonical_bytes;
    trailing.push_back(0);

    overwrite(path, trailing);

    CHECK(rejects([&] {
        static_cast<void>(store.load());
    }));

    overwrite(path, canonical_bytes);

    const auto final_recovered = store.load();
    CHECK(final_recovered.has_value());
    CHECK(final_recovered->records.size() == 2);

    // Empty validated-set snapshots are legitimate: the canonical
    // checkpoint may still be useful while no connected share has yet
    // crossed validation.
    ReplayValidationSnapshot empty;
    empty.checkpoint = snapshot.checkpoint;

    store.save(empty);

    const auto empty_recovered = store.load();
    CHECK(empty_recovered.has_value());
    CHECK(empty_recovered->records.empty());
    CHECK(
        empty_recovered->checkpoint.zano_height ==
        snapshot.checkpoint.zano_height);
    CHECK(
        empty_recovered->checkpoint.block_hash ==
        snapshot.checkpoint.block_hash);

    CHECK(!std::filesystem::exists(
        path.string() + ".rewrite.tmp"));

    return 0;
}
