#include "zano_p2pool/share_store.hpp"
#include "zano_p2pool/sidechain_params.hpp"
#include "test_check.hpp"

#include <cstdlib>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>

namespace {

struct Temporary {
    std::filesystem::path path;

    Temporary() {
        std::string pattern =
            (std::filesystem::temp_directory_path() /
             "zano-persistence-network-XXXXXX").string();

        if (mkdtemp(pattern.data()) == nullptr) {
            throw std::runtime_error("mkdtemp failed");
        }

        path = pattern;
    }

    ~Temporary() {
        std::filesystem::remove_all(path);
    }
};

bool rejects(const std::function<void()>& fn) {
    try {
        fn();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

void prove_cross_network_rejection(
    const std::filesystem::path& path,
    zano_p2pool::SidechainParentNetwork writer_network,
    zano_p2pool::SidechainParentNetwork reader_network) {

    using namespace zano_p2pool;

    const SidechainParameters writer_params =
        canonical_sidechain_parameters(writer_network);
    const SidechainParameters reader_params =
        canonical_sidechain_parameters(reader_network);

    const SidechainId writer_id = sidechain_id(writer_params);
    const SidechainId reader_id = sidechain_id(reader_params);

    CHECK(writer_id != reader_id);

    // An empty store is enough to pin the exact canonical SidechainId in the
    // durable header. No synthetic share data is needed to prove the network
    // boundary.
    ShareStore writer(path, writer_id);
    writer.rewrite({});

    ShareChain matching_chain(writer_params);
    const ShareStoreLoadResult matching =
        writer.load_into(matching_chain);
    CHECK(matching.records_loaded == 0);
    CHECK(matching.connected_shares == 0);
    CHECK(matching.orphan_shares == 0);

    ShareStore wrong_network_store(path, reader_id);
    ShareChain wrong_network_chain(reader_params);

    CHECK(rejects([&] {
        static_cast<void>(
            wrong_network_store.load_into(wrong_network_chain));
    }));

    // The same sidechain binding protects mutation paths too. Opening a store
    // under the opposite network must not rewrite its header.
    CHECK(rejects([&] {
        wrong_network_store.rewrite({});
    }));

    // Verify that the original network can still read the store after both
    // rejected cross-network operations.
    ShareChain final_matching_chain(writer_params);
    const ShareStoreLoadResult final_matching =
        writer.load_into(final_matching_chain);
    CHECK(final_matching.records_loaded == 0);
}

}  // namespace

int main() {
    using namespace zano_p2pool;

    Temporary temporary;

    prove_cross_network_rejection(
        temporary.path / "testnet-store.dat",
        SidechainParentNetwork::Testnet,
        SidechainParentNetwork::Mainnet);

    prove_cross_network_rejection(
        temporary.path / "mainnet-store.dat",
        SidechainParentNetwork::Mainnet,
        SidechainParentNetwork::Testnet);

    return 0;
}
