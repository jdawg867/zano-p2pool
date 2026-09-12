#include "zano_p2pool/historical_work.hpp"
#include "zano_p2pool/mining_work_archive.hpp"
#include "zano_p2pool/sidechain_params.hpp"
#include <algorithm>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 4 || std::string(argv[1]) != "--testnet") {
        std::cerr << "usage: zano-p2pool-work-audit --testnet RECORD.work RPC_URL\n";
        return 2;
    }
    try {
        using namespace zano_p2pool;
        const std::filesystem::path path = std::filesystem::absolute(argv[2]);
        if (!std::filesystem::is_regular_file(path) || path.extension() != ".work" ||
            path.stem().string().size() != 64)
            throw std::runtime_error("expected an existing archive record named CONTENT_ID.work");
        const auto bytes = hex_to_bytes(path.stem().string());
        Hash256 id{};
        std::copy(bytes.begin(), bytes.end(), id.begin());
        MiningWorkArchive archive(path.parent_path(), sidechain_id(
            canonical_sidechain_parameters(SidechainParentNetwork::Testnet)));
        const auto proposal = deserialize_p2p_mining_context_payload(archive.read(id));
        RpcClient rpc(argv[3]);
        const auto result = audit_historical_parent(proposal,
            [&](std::uint64_t height) { return rpc.get_canonical_header(height); });
        std::cout << "work_id=" << hash_to_hex(id) << '\n'
                  << "zano_height=" << proposal.zano_height << '\n'
                  << "mining_header=" << hash_to_hex(result.mining_header_hash) << '\n';
        switch (result.status) {
        case HistoricalParentStatus::ParentMatchedUntrusted:
            std::cout << "parent_check=matched-untrusted\n"; break;
        case HistoricalParentStatus::ParentMismatch:
            std::cout << "parent_check=mismatch\n"; break;
        case HistoricalParentStatus::ParentChangedDuringCheck:
            std::cout << "parent_check=changed-during-check\n"; break;
        }
        std::cout << "trusted=false\n"
                  << "pending=historical-pow-difficulty,base-reward,seed,payout-history,consensus-proofs\n";
        return result.status == HistoricalParentStatus::ParentMatchedUntrusted ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "audit failed: " << e.what() << '\n';
        return 1;
    }
}
