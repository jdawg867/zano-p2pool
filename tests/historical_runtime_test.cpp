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
#include <optional>
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
    std::optional<HistoricalAnchorStatus> anchor_status;
    std::optional<HistoricalPayoutStatus> payout_status;
    std::optional<P2pMiningContextTrustStatus> promotion_status;
    std::optional<P2pMinerTxProofStatus> proof_status;
    std::optional<P2pPayoutPolicyStatus> payout_policy_status;
    P2pShareReceiveStatus share_status{
        P2pShareReceiveStatus::Rejected};
    bool historical_share_retried{false};
    bool failed{false};
    std::size_t historical_admitted_count{};
    std::size_t trusted_work_count{};
    std::size_t connected_share_count{};
    bool candidate_present{false};
    bool candidate_validated_ancestry{false};
    ShareId candidate_id{};
    int lookup_calls{};
    int historical_pow_lookup_calls{};
    int work_requests{};
    P2pHistoricalRetrySummary autonomous_retry{};
};

[[nodiscard]] RuntimeCaseResult run_runtime_case(
    bool corrupt_remote_seed,
    bool replay_candidate = false,
    bool receiver_has_local_observation = true,
    bool fresh_root = false,
    bool receiver_has_historical_pow_context = true,
    bool receiver_parent_matches = true,
    bool retry_after_pow_unavailable = false,
    bool receiver_parent_payout_mismatch = false) {
    TemporaryDirectory temp("zano-historical-runtime");
    RuntimeFixture fixture = make_runtime_fixture();

    Share candidate = fixture.candidate;
    Share receiver_parent = fixture.parent;

    if (fresh_root) {
        candidate.parent_id = ShareId{};
        candidate.share_height = 0;
    } else if (receiver_parent_payout_mismatch) {
        const ZanoCurveKey alternate_payout_key =
            key_from_hex(kNativeCoinAssetId1Div8Hex);

        receiver_parent.payout =
            PayoutPublicKeys{
                alternate_payout_key,
                alternate_payout_key,
            };
        receiver_parent.miner_id =
            miner_id_from_payout(*receiver_parent.payout);

        candidate.parent_id =
            share_id(receiver_parent);
    }

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
    //
    // Some regression cases intentionally omit this observation to model a
    // fresh independent node that did not sample the provider's exact work.
    if (receiver_has_local_observation) {
        static_cast<void>(receiver_archive.put(
            serialize_p2p_mining_context_payload(fixture.proposal)));
    }

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

    if (!fresh_root) {
        const ShareWorkContext parent_context{
            receiver_parent.zano_height,
            receiver_parent.mining_header_hash,
            receiver_parent.network_difficulty,
        };
        CHECK(receiver_chain.submit_share(
                  receiver_parent,
                  parent_context,
                  200,
                  ProgPowZContextMode::Light)
                  .disposition == ShareDisposition::Connected);
        CHECK(receiver_chain.find(
                  share_id(receiver_parent)) != nullptr);
        CHECK(receiver_chain.find(
                  share_id(receiver_parent))->validated_ancestry);
    }

    if (replay_candidate) {
        CHECK(receiver_chain.add_share_unchecked(
                  candidate).disposition ==
              ShareDisposition::Connected);

        const ConnectedShare* replayed =
            receiver_chain.find(
                share_id(candidate));

        CHECK(replayed != nullptr);
        CHECK(!replayed->validated_ancestry);
    }

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
    std::atomic<int> historical_pow_lookup_calls{0};
    std::atomic<bool> historical_pow_context_available{
        receiver_has_historical_pow_context};

    receiver_node.set_historical_trust_sources(
        fixture.params,
        [&receiver_archive] {
            return load_local_mining_anchors(
                receiver_archive);
        },
        [&fixture, &lookup_calls, receiver_parent_matches](
            std::uint64_t height) {
            ++lookup_calls;
            CHECK(height == fixture.proposal.zano_height - 1);

            Hash256 canonical_parent = fixture.proposal.prev_hash;
            if (!receiver_parent_matches) {
                canonical_parent[0] ^= 0x01U;
            }

            return RpcCanonicalHeader{
                height,
                canonical_parent,
            };
        },
        [&fixture,
         &historical_pow_lookup_calls,
         &historical_pow_context_available](
            std::uint64_t height)
            -> std::optional<RpcHistoricalPowContext> {
            ++historical_pow_lookup_calls;
            CHECK(height == fixture.proposal.zano_height);

            if (!historical_pow_context_available.load()) {
                return std::nullopt;
            }

            return RpcHistoricalPowContext{
                fixture.proposal.zano_height,
                fixture.proposal.prev_hash,
                fixture.proposal.network_difficulty,
                fixture.proposal.block_reward_without_fee,
                fixture.proposal.zano_height + 2,
            };
        });

    const P2pHandshake provider_handshake =
        runtime_handshake(0x20, chain_id);
    const P2pHandshake receiver_handshake =
        runtime_handshake(0x70, chain_id);

    std::atomic<bool> completed{false};
    std::atomic<bool> failed{false};
    std::atomic<bool> first_pow_unavailable{false};
    std::atomic<int> work_requests{0};
    P2pHistoricalRetrySummary autonomous_retry;
    std::atomic<int> trust_status{
        static_cast<int>(
            HistoricalTrustStatus::CandidateMismatch)};
    std::atomic<int> anchor_status{-1};
    std::atomic<int> payout_status{-1};
    std::atomic<int> promotion_status{-1};
    std::atomic<int> proof_status{-1};
    std::atomic<int> payout_policy_status{-1};
    std::atomic<int> share_status{
        static_cast<int>(
            P2pShareReceiveStatus::Rejected)};
    std::atomic<bool> retried{false};
    std::atomic<std::size_t> admitted_count{0};

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
                if (envelope.type ==
                    P2pMessageType::MiningWorkRequest) {
                    ++work_requests;
                }

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
                if (result.historical_trust_status.has_value()) {
                    trust_status.store(
                        static_cast<int>(
                            *result.historical_trust_status));

                    anchor_status.store(
                        result.historical_anchor_status.has_value()
                            ? static_cast<int>(
                                  *result.historical_anchor_status)
                            : -1);

                    payout_status.store(
                        result.historical_payout_status.has_value()
                            ? static_cast<int>(
                                  *result.historical_payout_status)
                            : -1);

                    promotion_status.store(
                        result.historical_promotion_status.has_value()
                            ? static_cast<int>(
                                  *result.historical_promotion_status)
                            : -1);

                    proof_status.store(
                        result.historical_proof_status.has_value()
                            ? static_cast<int>(
                                  *result.historical_proof_status)
                            : -1);

                    payout_policy_status.store(
                        result.historical_payout_policy_status.has_value()
                            ? static_cast<int>(
                                  *result.historical_payout_policy_status)
                            : -1);

                    share_status.store(
                        static_cast<int>(result.share_status));
                    retried.store(
                        result.historical_share_retried);
                    admitted_count.store(
                        result.historical_admitted_shares.size());

                    const bool transient_unavailable =
                        *result.historical_trust_status ==
                            HistoricalTrustStatus::AnchorRejected &&
                        result.historical_anchor_status.has_value() &&
                        *result.historical_anchor_status ==
                            HistoricalAnchorStatus::
                                CanonicalPowContextUnavailable;

                    if (retry_after_pow_unavailable &&
                        transient_unavailable &&
                        !first_pow_unavailable.exchange(true)) {
                        return;
                    }

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
            candidate));

    if (retry_after_pow_unavailable) {
        CHECK(wait_for([&] {
            return first_pow_unavailable.load() ||
                   failed.load();
        }));

        if (!failed.load()) {
            // A local retry while the daemon is still missing historical PoW
            // authority must remain fail-closed and stay registered.
            const P2pHistoricalRetrySummary still_unavailable =
                receiver_node.retry_historical_pow_unavailable(
                    receiver_runtime,
                    201,
                    ProgPowZContextMode::Light);

            CHECK(still_unavailable.attempted == 1);
            CHECK(still_unavailable.trusted == 0);
            CHECK(still_unavailable.connected == 0);
            CHECK(still_unavailable.admitted_shares.empty());
            CHECK(still_unavailable.remaining == 1);

            // The local daemon can now reconstruct the historical PoW
            // context. Retry locally again without waiting for the peer to
            // reannounce the exact share.
            historical_pow_context_available.store(true);

            autonomous_retry =
                receiver_node.retry_historical_pow_unavailable(
                    receiver_runtime,
                    202,
                    ProgPowZContextMode::Light);

            completed.store(true);
        }
    }

    CHECK(wait_for([&] {
        return completed.load() || failed.load();
    }));

    receiver_runtime.stop();
    provider_runtime.stop();

    RuntimeCaseResult result;
    result.trust_status =
        static_cast<HistoricalTrustStatus>(
            trust_status.load());
    if (anchor_status.load() >= 0) {
        result.anchor_status =
            static_cast<HistoricalAnchorStatus>(
                anchor_status.load());
    }

    if (payout_status.load() >= 0) {
        result.payout_status =
            static_cast<HistoricalPayoutStatus>(
                payout_status.load());
    }

    if (promotion_status.load() >= 0) {
        result.promotion_status =
            static_cast<P2pMiningContextTrustStatus>(
                promotion_status.load());
    }

    if (proof_status.load() >= 0) {
        result.proof_status =
            static_cast<P2pMinerTxProofStatus>(
                proof_status.load());
    }

    if (payout_policy_status.load() >= 0) {
        result.payout_policy_status =
            static_cast<P2pPayoutPolicyStatus>(
                payout_policy_status.load());
    }

    result.share_status =
        static_cast<P2pShareReceiveStatus>(
            share_status.load());
    result.historical_share_retried = retried.load();
    result.failed = failed.load();
    result.historical_admitted_count =
        admitted_count.load();
    result.trusted_work_count =
        receiver_node.trusted_work_count();
    result.connected_share_count =
        receiver_node.connected_share_count();
    result.lookup_calls = lookup_calls.load();
    result.historical_pow_lookup_calls =
        historical_pow_lookup_calls.load();
    result.work_requests = work_requests.load();
    result.autonomous_retry = autonomous_retry;
    result.candidate_id = share_id(candidate);

    {
        std::lock_guard lock(receiver_state_mutex);
        const ConnectedShare* connected =
            receiver_chain.find(
                share_id(candidate));
        result.candidate_present = connected != nullptr;
        result.candidate_validated_ancestry =
            connected != nullptr &&
            connected->validated_ancestry;
    }

    return result;
}


struct RecursiveParentSyncResult {
    bool failed{false};
    bool completed{false};
    bool admitted_order_matches{false};
    std::size_t admitted_count{};
    std::size_t trusted_work_count{};
    std::size_t connected_share_count{};
    std::size_t descendant_count{};
    bool all_descendants_validated{false};
    bool grandparent_present{false};
    bool parent_present{false};
    bool child_present{false};
    bool grandparent_validated{false};
    bool parent_validated{false};
    bool child_validated{false};
    bool unadvertised_fork_present{false};
    bool unadvertised_fork_validated{false};
    bool idle_frontier_present{false};
    bool idle_frontier_validated{false};
    P2pReplayRecoverySummary idle_recovery{};
    bool ancillary_frontier_present{false};
    P2pNodeMessageResult ancillary_result{};
    int lookup_calls{};
    int observation_loads{};
    int work_requests{};
    int share_requests{};
};

[[nodiscard]] RecursiveParentSyncResult
run_recursive_parent_sync_case(
    bool replay_existing = false,
    bool stale_provider_handshake = false,
    bool advance_recovery_clock = false,
    bool add_unadvertised_fork = false,
    bool inject_idle_frontier = false,
    bool inject_idle_parent_mismatch = false,
    bool inject_ancillary_parent_mismatch = false) {
    TemporaryDirectory temp("zano-historical-parent-sync");
    RuntimeFixture fixture = make_runtime_fixture();

    const SidechainId chain_id = sidechain_id(fixture.params);

    MiningWorkArchive provider_archive(
        temp.path / "provider-work",
        chain_id);
    MiningWorkArchive receiver_archive(
        temp.path / "receiver-work",
        chain_id);

    static_cast<void>(provider_archive.put(
        serialize_p2p_mining_context_payload(fixture.proposal)));
    static_cast<void>(receiver_archive.put(
        serialize_p2p_mining_context_payload(fixture.proposal)));

    P2pWorkRetrieval provider_retrieval(provider_archive);
    P2pWorkRetrieval receiver_retrieval(receiver_archive);

    const Share base = fixture.parent;
    const ShareId base_id = share_id(base);

    Share grandparent = fixture.candidate;
    grandparent.parent_id = base_id;
    grandparent.share_height = 1;
    grandparent.timestamp = 101;
    grandparent.nonce = 9;
    const ShareId grandparent_id = share_id(grandparent);

    Share parent = fixture.candidate;
    parent.parent_id = grandparent_id;
    parent.share_height = 2;
    parent.timestamp = 102;
    parent.nonce = 10;
    const ShareId parent_id = share_id(parent);

    Share child = fixture.candidate;
    child.parent_id = parent_id;
    child.share_height = 3;
    child.timestamp = 103;
    child.nonce = 11;
    const ShareId child_id = share_id(child);

    std::vector<Share> descendants{
        grandparent,
        parent,
        child,
    };

    // The original regression covered only three replayed descendants. The
    // production failure occurred after sixteen deferred ancestors, so make
    // the authenticated-handshake replay case cross that exact old boundary.
    if (replay_existing) {
        const std::size_t target_descendants =
            advance_recovery_clock ? 70 : 18;
        while (descendants.size() < target_descendants) {
            Share next = fixture.candidate;
            next.parent_id = share_id(descendants.back());
            next.share_height =
                descendants.back().share_height + 1;
            next.timestamp =
                descendants.back().timestamp + 1;
            next.nonce =
                descendants.back().nonce + 1;
            descendants.push_back(next);
        }
    }

    const ShareId replay_tip_id =
        share_id(descendants.back());

    // Model the production restart condition where structural history contains
    // another replay frontier that is not the peer's advertised best tip.
    // It shares the already validated base but must independently cross the
    // historical trust boundary before Stratum may regard all connected replay
    // history as validated.
    std::optional<Share> unadvertised_fork;
    ShareId unadvertised_fork_id{};

    if (replay_existing && add_unadvertised_fork) {
        Share fork = fixture.candidate;
        fork.parent_id = base_id;
        fork.share_height = 1;
        fork.timestamp = 250;
        fork.nonce = 250;

        unadvertised_fork_id = share_id(fork);
        unadvertised_fork = fork;
    }

    ShareChain provider_chain(fixture.params);
    CHECK(provider_chain.add_share_unchecked(base).disposition ==
          ShareDisposition::Connected);
    for (const Share& descendant : descendants) {
        CHECK(provider_chain.add_share_unchecked(
                  descendant).disposition ==
              ShareDisposition::Connected);
    }

    if (unadvertised_fork.has_value()) {
        CHECK(provider_chain.add_share_unchecked(
                  *unadvertised_fork).disposition ==
              ShareDisposition::Connected);
    }

    ShareChain receiver_chain(fixture.params);
    P2pTrustedWorkRegistry provider_trusted_work;
    P2pTrustedWorkRegistry receiver_trusted_work;
    std::mutex provider_state_mutex;
    std::mutex receiver_state_mutex;

    const ShareWorkContext base_context{
        base.zano_height,
        base.mining_header_hash,
        base.network_difficulty,
    };
    CHECK(receiver_chain.submit_share(
              base,
              base_context,
              200,
              ProgPowZContextMode::Light)
              .disposition == ShareDisposition::Connected);
    CHECK(receiver_chain.find(base_id) != nullptr);
    CHECK(receiver_chain.find(base_id)->validated_ancestry);

    if (replay_existing) {
        for (const Share& descendant : descendants) {
            CHECK(receiver_chain.add_share_unchecked(
                      descendant).disposition ==
                  ShareDisposition::Connected);

            const ConnectedShare* replayed =
                receiver_chain.find(
                    share_id(descendant));
            CHECK(replayed != nullptr);
            CHECK(!replayed->validated_ancestry);
        }

        if (unadvertised_fork.has_value()) {
            CHECK(receiver_chain.add_share_unchecked(
                      *unadvertised_fork).disposition ==
                  ShareDisposition::Connected);

            const ConnectedShare* replayed_fork =
                receiver_chain.find(unadvertised_fork_id);

            CHECK(replayed_fork != nullptr);
            CHECK(!replayed_fork->validated_ancestry);
        }
    }

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
    std::atomic<int> observation_loads{0};
    std::atomic<bool> force_parent_mismatch{false};

    receiver_node.set_historical_trust_sources(
        fixture.params,
        [&receiver_archive, &observation_loads] {
            ++observation_loads;
            return load_local_mining_anchors(
                receiver_archive);
        },
        [&fixture,
         &lookup_calls,
         &force_parent_mismatch](std::uint64_t height) {
            ++lookup_calls;
            CHECK(height == fixture.proposal.zano_height - 1);

            Hash256 canonical_parent =
                fixture.proposal.prev_hash;

            if (force_parent_mismatch.load()) {
                canonical_parent[0] ^=
                    static_cast<std::uint8_t>(0x80);
            }

            return RpcCanonicalHeader{
                height,
                canonical_parent,
            };
        },
        [&fixture](std::uint64_t height)
            -> std::optional<RpcHistoricalPowContext> {
            CHECK(height == fixture.proposal.zano_height);
            return RpcHistoricalPowContext{
                fixture.proposal.zano_height,
                fixture.proposal.prev_hash,
                fixture.proposal.network_difficulty,
                fixture.proposal.block_reward_without_fee,
                fixture.proposal.zano_height,
            };
        });

    P2pHandshake provider_handshake =
        runtime_handshake(0x24, chain_id);
    if (replay_existing && !stale_provider_handshake) {
        provider_handshake.best_share_id =
            replay_tip_id;
        provider_handshake.best_share_height =
            descendants.back().share_height;
    }

    const P2pHandshake receiver_handshake =
        runtime_handshake(0x74, chain_id);

    std::atomic<bool> completed{false};
    std::atomic<bool> failed{false};
    std::atomic<bool> admitted_order_matches{false};
    std::atomic<std::size_t> admitted_count{0};
    std::atomic<int> work_requests{0};
    std::atomic<int> share_requests{0};
    std::atomic<std::uint64_t> receiver_now{200};

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
                if (envelope.type ==
                    P2pMessageType::MiningWorkRequest) {
                    ++work_requests;
                } else if (
                    envelope.type ==
                    P2pMessageType::ShareRequest) {
                    ++share_requests;
                }
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
        },
        [&](const P2pHandshake& peer)
            -> std::optional<P2pEnvelope> {
            if (!stale_provider_handshake) {
                return std::nullopt;
            }
            return provider_node.initial_sync_request(peer);
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
                const std::uint64_t message_now =
                    advance_recovery_clock
                        ? receiver_now.fetch_add(1)
                        : 200;

                const P2pNodeMessageResult result =
                    receiver_node.handle(
                        *receiver_runtime_ptr,
                        peer,
                        envelope,
                        message_now,
                        ProgPowZContextMode::Light);

                if (!replay_existing &&
                    result.historical_admitted_shares.size() == 3) {
                    admitted_count.store(
                        result.historical_admitted_shares.size());
                    admitted_order_matches.store(
                        share_id(result.historical_admitted_shares[0]) ==
                            grandparent_id &&
                        share_id(result.historical_admitted_shares[1]) ==
                            parent_id &&
                        share_id(result.historical_admitted_shares[2]) ==
                            child_id);
                    completed.store(true);
                }

                if (replay_existing &&
                    result.historical_share_connected) {
                    std::lock_guard lock(receiver_state_mutex);
                    const ConnectedShare* connected_tip =
                        receiver_chain.find(
                            replay_tip_id);
                    if (connected_tip != nullptr &&
                        connected_tip->validated_ancestry) {
                        completed.store(true);
                    }
                }
            } catch (...) {
                failed.store(true);
            }
        },
        [&](const P2pHandshake& peer)
            -> std::optional<P2pEnvelope> {
            return receiver_node.initial_sync_request(peer);
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

    if (replay_existing) {
        // No application-level announcement is sent here. The provider's
        // authenticated handshake advertised the structurally known replayed
        // child as its best tip, so connection establishment itself must begin
        // exact-share recovery.
    } else {
        // Only the child is announced. The receiver must walk backward with
        // two ShareRequest messages, then resume forward once the grandparent
        // can be validated against the already trusted base share.
        provider_runtime.broadcast(
            make_p2p_share_announce_envelope(child));
    }

    CHECK(wait_for(
        [&] {
            return completed.load() || failed.load();
        },
        advance_recovery_clock ? 15s : 5s));

    if (unadvertised_fork.has_value() &&
        completed.load() &&
        !failed.load()) {
        // Give the autonomous post-tip replay scheduler an opportunity to
        // select and recover the second structural frontier.
        static_cast<void>(
            wait_for(
                [&] {
                    std::lock_guard lock(receiver_state_mutex);
                    const ConnectedShare* fork =
                        receiver_chain.find(
                            unadvertised_fork_id);

                    return fork != nullptr &&
                           fork->validated_ancestry;
                },
                1s));
    }

    std::optional<ShareId> idle_frontier_id;
    P2pReplayRecoverySummary idle_recovery;
    std::optional<ShareId> ancillary_frontier_id;
    P2pNodeMessageResult ancillary_result;

    if (replay_existing &&
        (inject_idle_frontier ||
         inject_idle_parent_mismatch) &&
        completed.load() &&
        !failed.load()) {

        // Inject a structurally replayed branch only after normal peer-driven
        // recovery has gone quiet. Its exact payout parent is the already
        // validated fixture base and the receiver already owns the immutable
        // mining-work proposal, so one periodic replay tick should be enough
        // to cross trust without requiring a new inbound P2P message.
        Share idle = fixture.candidate;
        idle.parent_id = base_id;
        idle.share_height = 1;
        idle.timestamp = 251;
        idle.nonce = 251;

        idle_frontier_id = share_id(idle);

        {
            std::lock_guard lock(receiver_state_mutex);

            CHECK(receiver_chain.add_share_unchecked(
                      idle).disposition ==
                  ShareDisposition::Connected);

            const ConnectedShare* replayed_idle =
                receiver_chain.find(
                    *idle_frontier_id);

            CHECK(replayed_idle != nullptr);
            CHECK(!replayed_idle->validated_ancestry);
        }

        if (inject_idle_parent_mismatch) {
            force_parent_mismatch.store(true);
        }

        idle_recovery =
            receiver_node.advance_replay_recovery(
                receiver_runtime,
                260,
                ProgPowZContextMode::Light);

        CHECK(
            idle_recovery.attempted_share_id ==
            idle_frontier_id);

        CHECK(
            idle_recovery.attempted_parent_id ==
            std::optional<ShareId>{base_id});

        if (inject_idle_parent_mismatch) {
            CHECK(
                idle_recovery.historical_trust_status ==
                std::optional<HistoricalTrustStatus>{
                    HistoricalTrustStatus::AnchorRejected});

            CHECK(
                idle_recovery.historical_anchor_status ==
                std::optional<HistoricalAnchorStatus>{
                    HistoricalAnchorStatus::ParentMismatch});

            CHECK(
                idle_recovery.
                    pruned_connected_shares == 1);

            CHECK(
                idle_recovery.pruned_share_ids ==
                std::vector<ShareId>{
                    *idle_frontier_id});

            CHECK(
                idle_recovery.pruned_connected_shares ==
                idle_recovery.pruned_share_ids.size());

            CHECK(idle_recovery.connected == 0);
            CHECK(idle_recovery.remaining == 0);
        } else {
            CHECK(
                idle_recovery.historical_trust_status ==
                std::optional<HistoricalTrustStatus>{
                    HistoricalTrustStatus::Trusted});

            CHECK(idle_recovery.pruned_share_ids.empty());
            CHECK(idle_recovery.pruned_connected_shares == 0);
        }
    }

    if (replay_existing &&
        inject_ancillary_parent_mismatch &&
        completed.load() &&
        !failed.load()) {

        // Regression for the ancillary replay scheduler inside handle().
        // The outer TipAnnounce itself does not prune anything. Its ancillary
        // replay pass finds this locally recoverable structural frontier,
        // proves its archived Zano parent stale, and removes it. The returned
        // message result must preserve that aggregate chain mutation.
        Share ancillary = fixture.candidate;
        ancillary.parent_id = base_id;
        ancillary.share_height = 1;
        ancillary.timestamp = 252;
        ancillary.nonce = 252;

        ancillary_frontier_id = share_id(ancillary);

        {
            std::lock_guard lock(receiver_state_mutex);

            CHECK(receiver_chain.add_share_unchecked(
                      ancillary).disposition ==
                  ShareDisposition::Connected);

            const ConnectedShare* replayed =
                receiver_chain.find(
                    *ancillary_frontier_id);

            CHECK(replayed != nullptr);
            CHECK(!replayed->validated_ancestry);
        }

        force_parent_mismatch.store(true);

        ancillary_result =
            receiver_node.handle(
                receiver_runtime,
                provider_handshake,
                make_p2p_tip_announce_envelope(
                    P2pTipHint{
                        base_id,
                        base.share_height,
                    }),
                261,
                ProgPowZContextMode::Light);

        CHECK(
            ancillary_result.
                historical_pruned_share_ids ==
            std::vector<ShareId>{
                *ancillary_frontier_id});

        CHECK(
            ancillary_result.
                historical_pruned_connected_shares ==
            ancillary_result.
                historical_pruned_share_ids.size());

        force_parent_mismatch.store(false);
    }

    receiver_runtime.stop();
    provider_runtime.stop();

    RecursiveParentSyncResult result;
    result.failed = failed.load();
    result.completed = completed.load();
    result.admitted_order_matches =
        admitted_order_matches.load();
    result.admitted_count = admitted_count.load();
    result.trusted_work_count =
        receiver_node.trusted_work_count();
    result.connected_share_count =
        receiver_node.connected_share_count();
    result.descendant_count =
        descendants.size();
    result.lookup_calls = lookup_calls.load();
    result.observation_loads = observation_loads.load();
    result.work_requests = work_requests.load();
    result.share_requests = share_requests.load();
    result.ancillary_result = ancillary_result;

    {
        std::lock_guard lock(receiver_state_mutex);

        result.all_descendants_validated = true;
        for (const Share& descendant : descendants) {
            const ConnectedShare* connected =
                receiver_chain.find(
                    share_id(descendant));
            if (connected == nullptr ||
                !connected->validated_ancestry) {
                result.all_descendants_validated = false;
                break;
            }
        }

        const ConnectedShare* connected_grandparent =
            receiver_chain.find(grandparent_id);
        const ConnectedShare* connected_parent =
            receiver_chain.find(parent_id);
        const ConnectedShare* connected_child =
            receiver_chain.find(child_id);

        result.grandparent_present =
            connected_grandparent != nullptr;
        result.parent_present =
            connected_parent != nullptr;
        result.child_present =
            connected_child != nullptr;
        result.grandparent_validated =
            connected_grandparent != nullptr &&
            connected_grandparent->validated_ancestry;
        result.parent_validated =
            connected_parent != nullptr &&
            connected_parent->validated_ancestry;
        result.child_validated =
            connected_child != nullptr &&
            connected_child->validated_ancestry;

        if (unadvertised_fork.has_value()) {
            const ConnectedShare* connected_fork =
                receiver_chain.find(
                    unadvertised_fork_id);

            result.unadvertised_fork_present =
                connected_fork != nullptr;
            result.unadvertised_fork_validated =
                connected_fork != nullptr &&
                connected_fork->validated_ancestry;
        }

        if (idle_frontier_id.has_value()) {
            const ConnectedShare* connected_idle =
                receiver_chain.find(
                    *idle_frontier_id);

            result.idle_frontier_present =
                connected_idle != nullptr;
            result.idle_frontier_validated =
                connected_idle != nullptr &&
                connected_idle->validated_ancestry;
        }

        if (ancillary_frontier_id.has_value()) {
            result.ancillary_frontier_present =
                receiver_chain.find(
                    *ancillary_frontier_id) != nullptr;
        }
    }

    result.idle_recovery = idle_recovery;

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
    CHECK(rejected.historical_admitted_count == 0);
    CHECK(rejected.trusted_work_count == 0);
    CHECK(rejected.connected_share_count == 1);
    CHECK(!rejected.candidate_present);
    CHECK(!rejected.candidate_validated_ancestry);
    CHECK(rejected.lookup_calls == 2);
    CHECK(rejected.anchor_status.has_value());
    CHECK(*rejected.anchor_status ==
          HistoricalAnchorStatus::SeedMismatch);
    CHECK(!rejected.payout_status.has_value());

    // A fresh independent receiver may have no locally archived observation.
    // If its own daemon has not yet advanced far enough to reconstruct the
    // historical PoW context, trust must remain fail-closed.
    const RuntimeCaseResult historical_oracle_unavailable =
        run_runtime_case(
            false,
            false,
            false,
            false,
            false);

    CHECK(!historical_oracle_unavailable.failed);
    CHECK(historical_oracle_unavailable.trust_status ==
          HistoricalTrustStatus::AnchorRejected);
    CHECK(historical_oracle_unavailable.anchor_status.has_value());
    CHECK(*historical_oracle_unavailable.anchor_status ==
          HistoricalAnchorStatus::CanonicalPowContextUnavailable);
    CHECK(!historical_oracle_unavailable.payout_status.has_value());
    CHECK(!historical_oracle_unavailable.historical_share_retried);
    CHECK(historical_oracle_unavailable.historical_admitted_count == 0);
    CHECK(historical_oracle_unavailable.trusted_work_count == 0);
    CHECK(historical_oracle_unavailable.connected_share_count == 1);
    CHECK(!historical_oracle_unavailable.candidate_present);
    CHECK(!historical_oracle_unavailable.candidate_validated_ancestry);
    CHECK(historical_oracle_unavailable.lookup_calls == 2);
    CHECK(historical_oracle_unavailable.historical_pow_lookup_calls == 1);

    // Liveness regression from the multinode soak. A structurally valid work
    // item remains untrusted while the local daemon cannot reconstruct its
    // historical PoW context, but that temporary condition must not destroy
    // the already retrieved evidence. Once the local oracle becomes available,
    // autonomous local recovery reruns the entire trust crossing without a
    // second mining-work download.
    const RuntimeCaseResult historical_oracle_recovers =
        run_runtime_case(
            false,  // remote seed is intact
            false,  // candidate was not structurally replayed
            false,  // receiver has no exact local work observation
            false,  // normal parent-bound historical share
            false,  // historical PoW initially unavailable
            true,   // canonical parent still matches
            true);  // retry after local oracle becomes available

    CHECK(!historical_oracle_recovers.failed);

    // The first network-driven attempt remains fail-closed.
    CHECK(historical_oracle_recovers.trust_status ==
          HistoricalTrustStatus::AnchorRejected);
    CHECK(historical_oracle_recovers.anchor_status.has_value());
    CHECK(*historical_oracle_recovers.anchor_status ==
          HistoricalAnchorStatus::CanonicalPowContextUnavailable);

    // The autonomous local retry consumes the retained candidate and crosses
    // trust only after the canonical daemon oracle becomes available.
    CHECK(historical_oracle_recovers.autonomous_retry.attempted == 1);
    CHECK(historical_oracle_recovers.autonomous_retry.trusted == 1);
    CHECK(historical_oracle_recovers.autonomous_retry.connected == 1);

    // This candidate was absent from structural history before the autonomous
    // retry. The summary must therefore carry the exact newly admitted share
    // so the runtime can make that admission durable before later state.
    CHECK(
        historical_oracle_recovers.
            autonomous_retry.
                admitted_shares.size() == 1);
    CHECK(
        share_id(
            historical_oracle_recovers.
                autonomous_retry.
                    admitted_shares.front()) ==
        historical_oracle_recovers.candidate_id);

    CHECK(
        historical_oracle_recovers.
            autonomous_retry.
                pruned_connected_shares == 0);
    CHECK(
        historical_oracle_recovers.
            autonomous_retry.
                pruned_share_ids.empty());
    CHECK(historical_oracle_recovers.autonomous_retry.remaining == 0);

    CHECK(historical_oracle_recovers.trusted_work_count == 1);
    CHECK(historical_oracle_recovers.connected_share_count == 2);
    CHECK(historical_oracle_recovers.candidate_present);
    CHECK(historical_oracle_recovers.candidate_validated_ancestry);

    // First attempt performs one unavailable oracle lookup. The successful
    // retry performs both the initial and final anchor audits.
    CHECK(historical_oracle_recovers.historical_pow_lookup_calls >= 4);

    // Most important liveness assertion: autonomous recovery reused retained
    // structurally checked evidence instead of downloading the work again.
    CHECK(historical_oracle_recovers.work_requests == 1);

    // Reorg regression from the multinode soak: work anchored to a Zano
    // parent that is no longer canonical must remain fail-closed.
    const RuntimeCaseResult historical_parent_reorg_mismatch =
        run_runtime_case(
            false,
            false,
            true,
            false,
            true,
            false);

    CHECK(!historical_parent_reorg_mismatch.failed);
    CHECK(historical_parent_reorg_mismatch.trust_status ==
          HistoricalTrustStatus::AnchorRejected);
    CHECK(historical_parent_reorg_mismatch.anchor_status.has_value());
    CHECK(*historical_parent_reorg_mismatch.anchor_status ==
          HistoricalAnchorStatus::ParentMismatch);
    CHECK(!historical_parent_reorg_mismatch.payout_status.has_value());
    CHECK(!historical_parent_reorg_mismatch.historical_share_retried);
    CHECK(historical_parent_reorg_mismatch.historical_admitted_count == 0);
    CHECK(historical_parent_reorg_mismatch.trusted_work_count == 0);
    CHECK(historical_parent_reorg_mismatch.connected_share_count == 1);
    CHECK(!historical_parent_reorg_mismatch.candidate_present);
    CHECK(!historical_parent_reorg_mismatch.candidate_validated_ancestry);
    CHECK(historical_parent_reorg_mismatch.lookup_calls == 2);
    CHECK(historical_parent_reorg_mismatch.historical_pow_lookup_calls == 0);

    // A fresh receiver also cannot currently cross the historical payout
    // boundary for the provider's first zero-parent sidechain share. This is
    // intentionally captured as a failing bootstrap condition, not silently
    // A fresh receiver may now admit the first zero-parent sidechain share
    // after independent historical anchoring and the bootstrap miner-tx proof
    // crossing. The root share's own v2 payout identity establishes subsequent
    // sidechain payout history; it is not compared to the older bootstrap
    // template's node-specific coinbase recipient.
    const RuntimeCaseResult fresh_root =
        run_runtime_case(false, false, true, true);

    CHECK(!fresh_root.failed);
    CHECK(fresh_root.trust_status ==
          HistoricalTrustStatus::Trusted);
    CHECK(!fresh_root.anchor_status.has_value());
    CHECK(!fresh_root.payout_status.has_value());
    CHECK(fresh_root.share_status ==
          P2pShareReceiveStatus::Connected);
    CHECK(fresh_root.historical_share_retried);
    CHECK(fresh_root.historical_admitted_count == 1);
    CHECK(fresh_root.trusted_work_count == 1);
    CHECK(fresh_root.connected_share_count == 1);
    CHECK(fresh_root.candidate_present);
    CHECK(fresh_root.candidate_validated_ancestry);

    // This is the fresh-node/VPS-B regression: the receiver never observed
    // the provider's exact template locally. Canonical local Zano history
    // reconstructs the missing historical PoW context, after which the same
    // bootstrap proof crossing must admit and validate the root share.
    const RuntimeCaseResult fresh_root_from_canonical_history =
        run_runtime_case(
            false,  // remote seed is intact
            false,  // candidate was not structurally replayed
            false,  // no exact local mining-work observation
            true,   // zero-parent root
            true);  // canonical historical PoW context is available

    CHECK(!fresh_root_from_canonical_history.failed);
    CHECK(fresh_root_from_canonical_history.trust_status ==
          HistoricalTrustStatus::Trusted);
    CHECK(!fresh_root_from_canonical_history.anchor_status.has_value());
    CHECK(!fresh_root_from_canonical_history.payout_status.has_value());
    CHECK(fresh_root_from_canonical_history.share_status ==
          P2pShareReceiveStatus::Connected);
    CHECK(fresh_root_from_canonical_history.historical_share_retried);
    CHECK(fresh_root_from_canonical_history.historical_admitted_count == 1);
    CHECK(fresh_root_from_canonical_history.trusted_work_count == 1);
    CHECK(fresh_root_from_canonical_history.connected_share_count == 1);
    CHECK(fresh_root_from_canonical_history.candidate_present);
    CHECK(fresh_root_from_canonical_history.candidate_validated_ancestry);
    CHECK(fresh_root_from_canonical_history.lookup_calls > 0);
    CHECK(fresh_root_from_canonical_history.historical_pow_lookup_calls > 0);

    // Bootstrap destination independence is not peer authority. If this fresh
    // receiver has neither an exact local observation nor reconstructable
    // canonical historical PoW context, the root must remain untrusted.
    const RuntimeCaseResult fresh_root_without_local_authority =
        run_runtime_case(
            false,
            false,
            false,
            true,
            false);

    CHECK(!fresh_root_without_local_authority.failed);
    CHECK(fresh_root_without_local_authority.trust_status ==
          HistoricalTrustStatus::AnchorRejected);
    CHECK(fresh_root_without_local_authority.anchor_status.has_value());
    CHECK(*fresh_root_without_local_authority.anchor_status ==
          HistoricalAnchorStatus::CanonicalPowContextUnavailable);
    CHECK(!fresh_root_without_local_authority.payout_status.has_value());
    CHECK(!fresh_root_without_local_authority.historical_share_retried);
    CHECK(fresh_root_without_local_authority.historical_admitted_count == 0);
    CHECK(fresh_root_without_local_authority.trusted_work_count == 0);
    CHECK(fresh_root_without_local_authority.connected_share_count == 0);
    CHECK(!fresh_root_without_local_authority.candidate_present);
    CHECK(!fresh_root_without_local_authority.candidate_validated_ancestry);
    CHECK(fresh_root_without_local_authority.historical_pow_lookup_calls > 0);

    // Without the exact Zano curve/proof backend the runtime must never turn a
    // retrieved peer proposal into trusted work. HistoricalTrustTest covers the
    // explicit BackendUnavailable status; there is no successful runtime case.
    if (!zano_curve_backend_available()) {
        return 0;
    }

    // Observability regression from the multinode soak: when historical
    // candidate binding, canonical anchoring and payout-plan reconstruction
    // all succeed but the final miner-tx promotion rejects the provider's
    // payout against this receiver's validated parent history, preserve the
    // exact final-gate diagnostics in P2pNodeMessageResult.
    const RuntimeCaseResult promotion_rejected =
        run_runtime_case(
            false,  // remote seed is intact
            false,  // candidate was not structurally replayed
            true,   // receiver has exact local mining-work observation
            false,  // normal parent-bound historical share
            true,   // historical PoW context is available
            true,   // canonical Zano parent still matches
            false,  // no autonomous PoW retry
            true);  // validated parent has a different payout identity

    CHECK(!promotion_rejected.failed);
    CHECK(promotion_rejected.trust_status ==
          HistoricalTrustStatus::PromotionRejected);
    CHECK(!promotion_rejected.anchor_status.has_value());
    CHECK(!promotion_rejected.payout_status.has_value());

    CHECK(promotion_rejected.promotion_status.has_value());
    CHECK(*promotion_rejected.promotion_status ==
          P2pMiningContextTrustStatus::ProofsRejected);

    CHECK(promotion_rejected.proof_status.has_value());
    CHECK(*promotion_rejected.proof_status ==
          P2pMinerTxProofStatus::PayoutPolicyFailed);

    CHECK(promotion_rejected.payout_policy_status.has_value());
    CHECK(*promotion_rejected.payout_policy_status ==
          P2pPayoutPolicyStatus::DestinationMismatch);

    CHECK(!promotion_rejected.historical_share_retried);
    CHECK(promotion_rejected.historical_admitted_count == 0);
    CHECK(promotion_rejected.trusted_work_count == 0);
    CHECK(promotion_rejected.connected_share_count == 1);
    CHECK(!promotion_rejected.candidate_present);
    CHECK(!promotion_rejected.candidate_validated_ancestry);

    // With no matching local archive record, a fresh independent receiver can
    // now reconstruct the missing authority entirely from its own canonical
    // Zano history and then cross the unchanged payout/proof trust boundary.
    const RuntimeCaseResult reconstructed_fresh_receiver =
        run_runtime_case(
            false,
            false,
            false,
            false,
            true);

    CHECK(!reconstructed_fresh_receiver.failed);
    CHECK(reconstructed_fresh_receiver.trust_status ==
          HistoricalTrustStatus::Trusted);
    CHECK(!reconstructed_fresh_receiver.anchor_status.has_value());
    CHECK(!reconstructed_fresh_receiver.payout_status.has_value());
    CHECK(reconstructed_fresh_receiver.historical_share_retried);
    CHECK(reconstructed_fresh_receiver.historical_admitted_count == 1);
    CHECK(reconstructed_fresh_receiver.share_status ==
          P2pShareReceiveStatus::Connected);
    CHECK(reconstructed_fresh_receiver.trusted_work_count == 1);
    CHECK(reconstructed_fresh_receiver.connected_share_count == 2);
    CHECK(reconstructed_fresh_receiver.candidate_present);
    CHECK(reconstructed_fresh_receiver.candidate_validated_ancestry);
    CHECK(reconstructed_fresh_receiver.lookup_calls == 4);
    CHECK(
        reconstructed_fresh_receiver.historical_pow_lookup_calls ==
        2);

    const RuntimeCaseResult trusted =
        run_runtime_case(false);
    CHECK(!trusted.failed);
    CHECK(trusted.trust_status ==
          HistoricalTrustStatus::Trusted);
    CHECK(trusted.historical_share_retried);
    CHECK(trusted.historical_admitted_count == 1);
    CHECK(trusted.share_status ==
          P2pShareReceiveStatus::Connected);
    CHECK(trusted.trusted_work_count == 1);
    CHECK(trusted.connected_share_count == 2);
    CHECK(trusted.candidate_present);
    CHECK(trusted.candidate_validated_ancestry);
    CHECK(trusted.lookup_calls == 4);

    // An unchecked ShareStore replay must cross the exact same historical
    // trust boundary. Success upgrades the existing record in place and must
    // not report a second structural admission.
    const RuntimeCaseResult replayed =
        run_runtime_case(false, true);

    CHECK(!replayed.failed);
    CHECK(replayed.trust_status ==
          HistoricalTrustStatus::Trusted);
    CHECK(replayed.historical_share_retried);
    CHECK(replayed.historical_admitted_count == 0);
    CHECK(replayed.share_status ==
          P2pShareReceiveStatus::Connected);
    CHECK(replayed.trusted_work_count == 1);
    CHECK(replayed.connected_share_count == 2);
    CHECK(replayed.candidate_present);
    CHECK(replayed.candidate_validated_ancestry);
    CHECK(replayed.lookup_calls == 4);

    // End-to-end backward historical synchronization. Child and parent both
    // initially fail only because their explicit sidechain parents are
    // missing. The grandparent is validated against an existing checked base;
    // then parent and child are fully re-audited and admitted in order.
    const RecursiveParentSyncResult recursive =
        run_recursive_parent_sync_case();
    CHECK(!recursive.failed);
    CHECK(recursive.completed);
    CHECK(recursive.admitted_count == 3);
    CHECK(recursive.admitted_order_matches);
    CHECK(recursive.trusted_work_count == 3);
    CHECK(recursive.connected_share_count == 4);
    CHECK(recursive.grandparent_present);
    CHECK(recursive.parent_present);
    CHECK(recursive.child_present);
    CHECK(recursive.grandparent_validated);
    CHECK(recursive.parent_validated);
    CHECK(recursive.child_validated);

    // One mining-work retrieval is enough: the same untrusted proposal is
    // retained only after the first child reaches ParentMissing, and each
    // candidate still reruns the full historical trust crossing.
    CHECK(recursive.work_requests == 1);
    CHECK(recursive.share_requests == 2);
    CHECK(recursive.observation_loads == 5);
    CHECK(recursive.lookup_calls == 16);

    // Same ancestry, but every descendant was restored by unchecked durable
    // replay before P2P starts. The authenticated connection handshake alone
    // must initiate recovery. The initial exact-tip ShareRequest plus the two
    // parent requests walks back to the validated base, after which deferred
    // evidence revalidates all three existing records in place.
    const RecursiveParentSyncResult replay_recursive =
        run_recursive_parent_sync_case(true);

    CHECK(!replay_recursive.failed);
    CHECK(replay_recursive.completed);
    CHECK(replay_recursive.admitted_count == 0);
    CHECK(replay_recursive.descendant_count == 18);
    CHECK(replay_recursive.descendant_count >
          kMiningWorkMaxPending);
    CHECK(replay_recursive.all_descendants_validated);
    CHECK(replay_recursive.trusted_work_count ==
          replay_recursive.descendant_count);
    CHECK(replay_recursive.connected_share_count ==
          replay_recursive.descendant_count + 1);
    CHECK(replay_recursive.grandparent_present);
    CHECK(replay_recursive.parent_present);
    CHECK(replay_recursive.child_present);
    CHECK(replay_recursive.grandparent_validated);
    CHECK(replay_recursive.parent_validated);
    CHECK(replay_recursive.child_validated);
    // Every replayed ShareResponse may reuse the exact immutable proposal
    // already present in this receiver's own archive. No peer mining-work
    // download is required; each share still reruns the complete historical
    // trust crossing against its explicit parent.
    CHECK(replay_recursive.work_requests == 0);
    CHECK(replay_recursive.share_requests ==
          replay_recursive.descendant_count);
    CHECK(replay_recursive.observation_loads ==
          (2 * replay_recursive.descendant_count) - 1);
    CHECK(replay_recursive.lookup_calls ==
          (6 * replay_recursive.descendant_count) - 2);

    // Multinode-soak regression: restart replay may contain more than one
    // unvalidated frontier. Recovering only the peer-advertised best-tip walk
    // is insufficient because another structurally connected replay branch
    // remains fail-closed and keeps canonical payout/Stratum gated.
    const RecursiveParentSyncResult multi_frontier =
        run_recursive_parent_sync_case(
            true,   // unchecked durable replay already exists
            false,  // current provider handshake tip
            false,  // normal recovery clock
            true);  // add a second unadvertised replay frontier

    CHECK(!multi_frontier.failed);
    CHECK(multi_frontier.completed);
    CHECK(multi_frontier.unadvertised_fork_present);

    CHECK(multi_frontier.unadvertised_fork_validated);

    // The second replay frontier already has exact immutable local work, so
    // recovering it must not require downloading identical work from the peer.
    CHECK(multi_frontier.work_requests == 0);

    // Runtime-liveness regression from the 42a20c5 soak. After peer-driven
    // recovery goes quiet, a newly eligible durable replay frontier must still
    // be advanced by the node's periodic runtime tick; waiting for another
    // inbound P2P message can otherwise leave Stratum gated indefinitely.
    const RecursiveParentSyncResult idle_tick =
        run_recursive_parent_sync_case(
            true,   // unchecked durable replay already exists
            false,  // current provider handshake tip
            false,  // normal recovery clock
            false,  // no earlier unadvertised fork
            true);  // inject replay frontier after peer traffic goes quiet

    CHECK(!idle_tick.failed);
    CHECK(idle_tick.completed);
    CHECK(idle_tick.idle_frontier_present);

    // One periodic tick must advance the quiet eligible replay frontier.
    CHECK(idle_tick.idle_recovery.attempted == 1);
    CHECK(idle_tick.idle_recovery.connected == 1);
    CHECK(idle_tick.idle_recovery.remaining == 0);
    CHECK(idle_tick.idle_frontier_validated);

    // Production regression from the 59222d9 diagnostic soak. Once ordinary
    // replay recovery has established a validated parent, a remaining durable
    // frontier can independently prove that its archived Zano parent is no
    // longer canonical. That result is terminal for the active in-memory
    // replay branch: prune it rather than retrying ParentMismatch forever.
    const RecursiveParentSyncResult idle_parent_mismatch =
        run_recursive_parent_sync_case(
            true,   // unchecked durable replay already exists
            false,  // current provider handshake tip
            false,  // normal recovery clock
            false,  // no earlier unadvertised fork
            false,  // do not inject the ordinary trusted idle case
            true);  // inject idle frontier with canonical-parent mismatch

    CHECK(!idle_parent_mismatch.failed);
    CHECK(idle_parent_mismatch.completed);

    CHECK(
        idle_parent_mismatch.idle_recovery.attempted == 1);
    CHECK(
        idle_parent_mismatch.idle_recovery.connected == 0);
    CHECK(
        idle_parent_mismatch.idle_recovery.
            pruned_connected_shares == 1);
    CHECK(
        idle_parent_mismatch.idle_recovery.remaining == 0);

    CHECK(
        idle_parent_mismatch.idle_recovery.
            historical_trust_status ==
        std::optional<HistoricalTrustStatus>{
            HistoricalTrustStatus::AnchorRejected});

    CHECK(
        idle_parent_mismatch.idle_recovery.
            historical_anchor_status ==
        std::optional<HistoricalAnchorStatus>{
            HistoricalAnchorStatus::ParentMismatch});

    CHECK(!idle_parent_mismatch.idle_frontier_present);
    CHECK(!idle_parent_mismatch.idle_frontier_validated);

    // Ancillary-scheduler prune accounting regression. handle() snapshots the
    // outer message result before opportunistically advancing other replay
    // frontiers. A stale locally recoverable frontier may be pruned during
    // that ancillary pass; the returned result must report that mutation.
    const RecursiveParentSyncResult ancillary_parent_mismatch =
        run_recursive_parent_sync_case(
            true,   // unchecked durable replay already exists
            false,  // current provider handshake
            false,  // normal recovery clock
            false,  // no earlier unadvertised fork
            false,  // no periodic idle frontier
            false,  // no periodic mismatch case
            true);  // mismatch handled by ancillary scheduler

    CHECK(!ancillary_parent_mismatch.failed);
    CHECK(ancillary_parent_mismatch.completed);

    CHECK(
        ancillary_parent_mismatch.ancillary_result.status ==
        P2pNodeMessageStatus::TipProcessed);

    CHECK(
        ancillary_parent_mismatch.
            ancillary_result.
            historical_pruned_connected_shares == 1);

    CHECK(
        !ancillary_parent_mismatch.
            ancillary_frontier_present);

    // Regression from the multinode soak: ancestry recovery is serial. A
    // healthy peer can therefore spend longer than the mining-work request
    // lifetime walking backward to the first trusted parent. Deferred
    // descendants must not expire merely because the complete walk is older
    // than 60 seconds while valid recovery messages continue arriving.
    const RecursiveParentSyncResult timed_replay_recursive =
        run_recursive_parent_sync_case(
            true,   // unchecked durable replay already exists
            false,  // provider handshake advertises the current tip
            true);  // advance logical time during ancestry recovery

    CHECK(!timed_replay_recursive.failed);
    CHECK(timed_replay_recursive.completed);
    CHECK(timed_replay_recursive.admitted_count == 0);
    CHECK(timed_replay_recursive.descendant_count == 70);
    CHECK(timed_replay_recursive.all_descendants_validated);
    CHECK(timed_replay_recursive.trusted_work_count ==
          timed_replay_recursive.descendant_count);
    CHECK(timed_replay_recursive.connected_share_count ==
          timed_replay_recursive.descendant_count + 1);
    // The transient peer-evidence cache still has its independent 60-second
    // lifetime, but this replay receiver also owns the exact immutable proposal
    // in its durable local archive. After transient evidence expires, recovery
    // can reread that local evidence rather than downloading identical bytes
    // from the peer. The full historical trust crossing is still repeated.
    CHECK(timed_replay_recursive.work_requests == 0);
    CHECK(timed_replay_recursive.share_requests ==
          timed_replay_recursive.descendant_count);

    // Regression: a long-running provider may have advanced its local
    // sidechain after its transport handshake was created. Reconnecting peers
    // must not depend on that stale handshake tip to restart historical
    // ancestry recovery.
    const RecursiveParentSyncResult stale_provider_tip =
        run_recursive_parent_sync_case(true, true);

    CHECK(!stale_provider_tip.failed);
    CHECK(stale_provider_tip.completed);
    CHECK(stale_provider_tip.admitted_count == 0);
    CHECK(stale_provider_tip.descendant_count == 18);
    CHECK(stale_provider_tip.all_descendants_validated);
    CHECK(stale_provider_tip.trusted_work_count ==
          stale_provider_tip.descendant_count);
    CHECK(stale_provider_tip.connected_share_count ==
          stale_provider_tip.descendant_count + 1);
    CHECK(stale_provider_tip.grandparent_present);
    CHECK(stale_provider_tip.parent_present);
    CHECK(stale_provider_tip.child_present);
    CHECK(stale_provider_tip.grandparent_validated);
    CHECK(stale_provider_tip.parent_validated);
    CHECK(stale_provider_tip.child_validated);
    // The fresh-tip redirect repairs synchronization, while the receiver's
    // own exact archived proposal supplies mining-work evidence for the replay
    // walk. No peer mining-work download is necessary.
    CHECK(stale_provider_tip.work_requests == 0);
    CHECK(stale_provider_tip.share_requests ==
          stale_provider_tip.descendant_count);

    return 0;
}
