#define main p2p_miner_tx_proofs_fixture_main
#include "p2p_miner_tx_proofs_test.cpp"
#undef main

#include "zano_p2pool/historical_trust.hpp"
#include "zano_p2pool/p2p_node.hpp"
#include "zano_p2pool/p2p_work_retrieval.hpp"
#include "zano_p2pool/progpowz.hpp"
#include "zano_p2pool/sidechain_params.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <unistd.h>

namespace {

using namespace zano_p2pool;
using namespace std::chrono_literals;

struct TemporaryDirectory {
    std::filesystem::path path;

    explicit TemporaryDirectory(std::string_view prefix) {
        std::string pattern =
            (std::filesystem::temp_directory_path() /
             (std::string(prefix) + "-XXXXXX"))
                .string();
        if (mkdtemp(pattern.data()) == nullptr) {
            throw std::runtime_error("mkdtemp failed");
        }
        path = pattern;
    }

    ~TemporaryDirectory() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

[[nodiscard]] bool wait_for(
    const std::function<bool()>& predicate,
    std::chrono::milliseconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return predicate();
}

[[nodiscard]] NodeId runtime_node_id(std::uint8_t seed) {
    NodeId id{};
    for (std::size_t i = 0; i < id.size(); ++i) {
        id[i] = static_cast<std::uint8_t>(seed + i);
    }
    return id;
}

[[nodiscard]] P2pHandshake runtime_handshake(
    std::uint8_t seed,
    const SidechainId& sidechain) {
    P2pHandshake handshake;
    handshake.network = P2pNetwork::Testnet;
    handshake.node_id = runtime_node_id(seed);
    handshake.capabilities =
        kP2pCapabilitiesV1 | kP2pCapabilityWorkRetrieval;
    handshake.sidechain_id = sidechain;
    return handshake;
}

[[nodiscard]] Share runtime_parent_share(
    const PayoutPublicKeys& payout) {
    Share share;
    share.version = kShareVersion2;
    share.share_height = 0;
    share.timestamp = 100;
    share.zano_height = 1;
    share.mining_header_hash.fill(0x44);
    share.nonce = 7;
    share.share_difficulty = difficulty128_from_decimal("1");
    share.network_difficulty = difficulty128_from_decimal("1");
    share.payout = payout;
    share.miner_id = miner_id_from_payout(payout);
    return share;
}

[[nodiscard]] Share runtime_candidate_share(
    const P2pMiningContextProposal& proposal,
    const ShareId& parent_id,
    const PayoutPublicKeys& payout) {
    Share share;
    share.version = kShareVersion2;
    share.parent_id = parent_id;
    share.share_height = 1;
    share.timestamp = 101;
    share.zano_height = proposal.zano_height;
    share.mining_header_hash =
        validate_p2p_mining_context_structure(proposal);
    share.nonce = 9;
    share.share_difficulty = difficulty128_from_decimal("1");
    share.network_difficulty = proposal.network_difficulty;
    share.payout = payout;
    share.miner_id = miner_id_from_payout(payout);
    return share;
}

struct RuntimeFixture {
    SidechainParameters params;
    PayoutPublicKeys payout;
    P2pMiningContextProposal proposal;
    Share parent;
    Share candidate;
};

[[nodiscard]] RuntimeFixture make_runtime_fixture() {
    const ZanoCurveKey scalar_one = key_from_hex(kScalarOneHex);
    const ZanoCurveKey basepoint = key_from_hex(kEd25519BasepointHex);
    const ZanoCurveKey native_asset =
        key_from_hex(kNativeCoinAssetId1Div8Hex);
    const PayoutPublicKeys payout{basepoint, basepoint};

    std::vector<ZanoCurveKey> stealths(2, basepoint);
    if (zano_curve_backend_available()) {
        CHECK(zano_derive_output_public_key(
            scalar_one,
            payout.spend_public_key,
            payout.view_public_key,
            0,
            stealths[0]));
        CHECK(zano_derive_output_public_key(
            scalar_one,
            payout.spend_public_key,
            payout.view_public_key,
            1,
            stealths[1]));
    }

    const auto prefix =
        make_prefix(basepoint, stealths, native_asset);
    const auto balance_proof =
        make_balance_proof(prefix, basepoint);
    const auto range_proof =
        make_valid_range_proof(prefix);

    P2pMiningContextProposal proposal =
        make_proposal(prefix, balance_proof, range_proof);
    proposal.seed = progpowz_seed(proposal.zano_height);

    SidechainParameters params =
        canonical_sidechain_parameters(
            SidechainParentNetwork::Testnet);
    params.minimum_share_difficulty = 1;

    const Share parent = runtime_parent_share(payout);
    const ShareId parent_id = share_id(parent);
    const Share candidate =
        runtime_candidate_share(proposal, parent_id, payout);

    return RuntimeFixture{
        params,
        payout,
        proposal,
        parent,
        candidate,
    };
}

struct RuntimeCaseResult {
    HistoricalTrustStatus trust_status{
        HistoricalTrustStatus::CandidateMismatch};
    P2pShareReceiveStatus share_status{
        P2pShareReceiveStatus::Rejected};
    bool historical_share_retried{false};
    bool failed{false};
    std::size_t trusted_work_count{};
    std::size_t connected_share_count{};
    bool candidate_present{false};
    bool candidate_validated_ancestry{false};
    int lookup_calls{};
};

[[nodiscard]] RuntimeCaseResult run_runtime_case(
    bool corrupt_remote_seed) {
    TemporaryDirectory temp("zano-historical-runtime");
    RuntimeFixture fixture = make_runtime_fixture();

    const SidechainId chain_id = sidechain_id(fixture.params);

    MiningWorkArchive provider_archive(
        temp.path / "provider-work",
        chain_id);
    MiningWorkArchive receiver_archive(
        temp.path / "receiver-work",
        chain_id);

    // The receiver's archive is independent local daemon evidence. The
    // candidate-specific proposal still arrives through peer retrieval and
    // remains untrusted until HistoricalTrustStatus::Trusted.
    static_cast<void>(receiver_archive.put(
        serialize_p2p_mining_context_payload(fixture.proposal)));

    P2pMiningContextProposal remote_proposal = fixture.proposal;
    if (corrupt_remote_seed) {
        remote_proposal.seed[0] ^= 0x01U;
    }
    static_cast<void>(provider_archive.put(
        serialize_p2p_mining_context_payload(remote_proposal)));

    P2pWorkRetrieval provider_retrieval(provider_archive);
    P2pWorkRetrieval receiver_retrieval(receiver_archive);

    ShareChain provider_chain(fixture.params);
    ShareChain receiver_chain(fixture.params);
    P2pTrustedWorkRegistry provider_trusted_work;
    P2pTrustedWorkRegistry receiver_trusted_work;
    std::mutex provider_state_mutex;
    std::mutex receiver_state_mutex;

    const ShareWorkContext parent_context{
        fixture.parent.zano_height,
        fixture.parent.mining_header_hash,
        fixture.parent.network_difficulty,
    };
    CHECK(receiver_chain.submit_share(
              fixture.parent,
              parent_context,
              200,
              ProgPowZContextMode::Light)
              .disposition == ShareDisposition::Connected);
    CHECK(receiver_chain.find(
              share_id(fixture.parent)) != nullptr);
    CHECK(receiver_chain.find(
              share_id(fixture.parent))->validated_ancestry);

    P2pNodeProtocol provider_node(
        provider_chain,
        provider_trusted_work,
        provider_state_mutex);
    P2pNodeProtocol receiver_node(
        receiver_chain,
        receiver_trusted_work,
        receiver_state_mutex);

    provider_node.set_work_retrieval(&provider_retrieval);
    receiver_node.set_work_retrieval(&receiver_retrieval);

    std::atomic<int> lookup_calls{0};
    receiver_node.set_historical_trust_sources(
        fixture.params,
        [&receiver_archive] {
            return load_local_mining_anchors(
                receiver_archive);
        },
        [&fixture, &lookup_calls](std::uint64_t height) {
            ++lookup_calls;
            CHECK(height == fixture.proposal.zano_height - 1);
            return RpcCanonicalHeader{
                height,
                fixture.proposal.prev_hash,
            };
        });

    const P2pHandshake provider_handshake =
        runtime_handshake(0x20, chain_id);
    const P2pHandshake receiver_handshake =
        runtime_handshake(0x70, chain_id);

    std::atomic<bool> completed{false};
    std::atomic<bool> failed{false};
    std::atomic<int> trust_status{
        static_cast<int>(
            HistoricalTrustStatus::CandidateMismatch)};
    std::atomic<int> share_status{
        static_cast<int>(
            P2pShareReceiveStatus::Rejected)};
    std::atomic<bool> retried{false};

    P2pRuntime* provider_runtime_ptr = nullptr;
    P2pRuntime* receiver_runtime_ptr = nullptr;

    P2pRuntime provider_runtime(
        P2pRuntimeConfig{
            P2pEndpoint{"127.0.0.1", 0},
            provider_handshake,
        },
        [&](const P2pHandshake& peer,
            const P2pEnvelope& envelope) {
            try {
                if (provider_runtime_ptr != nullptr) {
                    static_cast<void>(
                        provider_node.handle(
                            *provider_runtime_ptr,
                            peer,
                            envelope,
                            200,
                            ProgPowZContextMode::Light));
                }
            } catch (...) {
                failed.store(true);
            }
        });

    P2pRuntime receiver_runtime(
        P2pRuntimeConfig{
            P2pEndpoint{"127.0.0.1", 0},
            receiver_handshake,
        },
        [&](const P2pHandshake& peer,
            const P2pEnvelope& envelope) {
            try {
                if (receiver_runtime_ptr == nullptr) {
                    return;
                }
                const P2pNodeMessageResult result =
                    receiver_node.handle(
                        *receiver_runtime_ptr,
                        peer,
                        envelope,
                        200,
                        ProgPowZContextMode::Light);
                if (result.status ==
                        P2pNodeMessageStatus::
                            MiningWorkResponseProcessed &&
                    result.historical_trust_status.has_value()) {
                    trust_status.store(
                        static_cast<int>(
                            *result.historical_trust_status));
                    share_status.store(
                        static_cast<int>(result.share_status));
                    retried.store(
                        result.historical_share_retried);
                    completed.store(true);
                }
            } catch (...) {
                failed.store(true);
            }
        });

    provider_runtime_ptr = &provider_runtime;
    receiver_runtime_ptr = &receiver_runtime;

    provider_runtime.start();
    receiver_runtime.start();
    receiver_runtime.connect_peer(
        P2pEndpoint{
            "127.0.0.1",
            provider_runtime.listen_port(),
        });

    CHECK(wait_for([&] {
        return provider_runtime.peer_count() == 1 &&
               receiver_runtime.peer_count() == 1;
    }));

    provider_runtime.broadcast(
        make_p2p_share_announce_envelope(
            fixture.candidate));

    CHECK(wait_for([&] {
        return completed.load() || failed.load();
    }));

    receiver_runtime.stop();
    provider_runtime.stop();

    RuntimeCaseResult result;
    result.trust_status =
        static_cast<HistoricalTrustStatus>(
            trust_status.load());
    result.share_status =
        static_cast<P2pShareReceiveStatus>(
            share_status.load());
    result.historical_share_retried = retried.load();
    result.failed = failed.load();
    result.trusted_work_count =
        receiver_node.trusted_work_count();
    result.connected_share_count =
        receiver_node.connected_share_count();
    result.lookup_calls = lookup_calls.load();

    {
        std::lock_guard lock(receiver_state_mutex);
        const ConnectedShare* connected =
            receiver_chain.find(
                share_id(fixture.candidate));
        result.candidate_present = connected != nullptr;
        result.candidate_validated_ancestry =
            connected != nullptr &&
            connected->validated_ancestry;
    }

    return result;
}

}  // namespace

int main() {
    using namespace zano_p2pool;

    // The checked historical parent and final candidate admission both require
    // the exact ProgPoWZ backend. The trust primitive already has separate
    // fail-closed coverage for lightweight builds.
    if (!progpowz_available()) {
        return 0;
    }

    const RuntimeCaseResult rejected =
        run_runtime_case(true);
    CHECK(!rejected.failed);
    CHECK(rejected.trust_status ==
          HistoricalTrustStatus::AnchorRejected);
    CHECK(!rejected.historical_share_retried);
    CHECK(rejected.trusted_work_count == 0);
    CHECK(rejected.connected_share_count == 1);
    CHECK(!rejected.candidate_present);
    CHECK(!rejected.candidate_validated_ancestry);
    CHECK(rejected.lookup_calls == 2);

    // Without the exact Zano curve/proof backend the runtime must never turn a
    // retrieved peer proposal into trusted work. HistoricalTrustTest covers the
    // explicit BackendUnavailable status; there is no successful runtime case.
    if (!zano_curve_backend_available()) {
        return 0;
    }

    const RuntimeCaseResult trusted =
        run_runtime_case(false);
    CHECK(!trusted.failed);
    CHECK(trusted.trust_status ==
          HistoricalTrustStatus::Trusted);
    CHECK(trusted.historical_share_retried);
    CHECK(trusted.share_status ==
          P2pShareReceiveStatus::Connected);
    CHECK(trusted.trusted_work_count == 1);
    CHECK(trusted.connected_share_count == 2);
    CHECK(trusted.candidate_present);
    CHECK(trusted.candidate_validated_ancestry);
    CHECK(trusted.lookup_calls == 4);

    return 0;
}
