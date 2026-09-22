#include "zano_p2pool/crypto_hash.hpp"
#include "zano_p2pool/p2p_node.hpp"
#include "test_check.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string_view>
#include <thread>

namespace {

using namespace zano_p2pool;
using namespace std::chrono_literals;

Hash256 hash_from_hex(std::string_view hex) {
    const auto bytes = hex_to_bytes(hex);
    CHECK(bytes.size() == 32);
    Hash256 hash{};
    std::copy(bytes.begin(), bytes.end(), hash.begin());
    return hash;
}

NodeId node_id_from(std::uint8_t seed) {
    NodeId id{};
    for (std::size_t i = 0; i < id.size(); ++i) {
        id[i] = static_cast<std::uint8_t>(seed + i);
    }
    return id;
}

P2pHandshake make_handshake(std::uint8_t seed) {
    P2pHandshake handshake;
    handshake.network = P2pNetwork::Testnet;
    handshake.node_id = node_id_from(seed);
    handshake.capabilities = kP2pCapabilitiesV1;
    return handshake;
}

Share make_parent() {
    Share share;
    share.timestamp = 1'700'200'000;
    share.zano_height = 0;
    share.mining_header_hash = hash_from_hex(
        "ffeeddccbbaa9988776655443322110000112233445566778899aabbccddeeff");
    share.nonce = UINT64_C(0x123456789abcdef0);
    share.share_difficulty = difficulty128_from_decimal("3");
    share.network_difficulty = difficulty128_from_decimal("4");
    share.miner_id[31] = 0x61;
    return share;
}

Share make_child(const Share& parent) {
    Share child = parent;
    child.parent_id = share_id(parent);
    child.share_height = parent.share_height + 1;
    child.timestamp = parent.timestamp + 1;
    child.miner_id[31] = 0x62;
    return child;
}

ShareWorkContext context_for(const Share& share) {
    return ShareWorkContext{
        share.zano_height,
        share.mining_header_hash,
        share.network_difficulty,
    };
}

bool wait_for(
    const std::function<bool()>& predicate,
    std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return predicate();
}

}  // namespace

int main() {
    const Share parent = make_parent();
    const Share child = make_child(parent);
    const ShareId parent_id = share_id(parent);
    const ShareId child_id = share_id(child);
    const ShareWorkContext trusted = context_for(parent);

    // Regression: an outer ShareAnnounce can begin as unknown-work and then
    // cross historical trust inside the same P2P handler. In that case the
    // final share_status is Connected while historical_admitted_shares also
    // contains the exact same share. Durable admission must contain it once.
    {
        P2pNodeMessageResult result;
        result.status = P2pNodeMessageStatus::ShareProcessed;
        result.share_status = P2pShareReceiveStatus::Connected;
        result.historical_admitted_shares.push_back(parent);

        const auto admitted =
            p2p_node_unique_admitted_shares(
                result,
                make_p2p_share_announce_envelope(parent));

        CHECK(admitted.size() == 1);
        CHECK(share_id(admitted[0]) == parent_id);
    }

    // Do not solve the duplicate bug by suppressing the historical list.
    // One directly admitted share may unlock a distinct deferred descendant
    // during the same outer handler pass; both unique shares must persist.
    {
        P2pNodeMessageResult result;
        result.status = P2pNodeMessageStatus::ShareProcessed;
        result.share_status = P2pShareReceiveStatus::Connected;
        result.historical_admitted_shares.push_back(parent);
        result.historical_admitted_shares.push_back(child);
        result.historical_admitted_shares.push_back(child);

        const auto admitted =
            p2p_node_unique_admitted_shares(
                result,
                make_p2p_share_announce_envelope(parent));

        CHECK(admitted.size() == 2);
        CHECK(share_id(admitted[0]) == parent_id);
        CHECK(share_id(admitted[1]) == child_id);
    }

    ShareChain provider_chain;
    ShareChain requester_chain;
    ShareChain leaf_chain;
    P2pTrustedWorkRegistry provider_work;
    P2pTrustedWorkRegistry requester_work;
    P2pTrustedWorkRegistry leaf_work;
    std::mutex provider_mutex;
    std::mutex requester_mutex;
    std::mutex leaf_mutex;

    // The root and its child deliberately reuse one synthetic work context in
    // this test. Parent-bound trusted work therefore needs one authorization
    // for the zero-parent root and one for shares extending the root.
    provider_work.remember(trusted);
    provider_work.remember(trusted, child.parent_id);
    requester_work.remember(trusted);
    requester_work.remember(trusted, child.parent_id);
    leaf_work.remember(trusted);
    leaf_work.remember(trusted, child.parent_id);

#ifdef ZANO_P2POOL_HAVE_PROGPOWZ
    CHECK(provider_chain.submit_share(
              parent,
              trusted,
              child.timestamp,
              ProgPowZContextMode::Light).disposition ==
          ShareDisposition::Connected);
    CHECK(provider_chain.submit_share(
              child,
              trusted,
              child.timestamp,
              ProgPowZContextMode::Light).disposition ==
          ShareDisposition::Connected);
#else
    CHECK(provider_chain.add_share_unchecked(parent).disposition ==
          ShareDisposition::Connected);
    CHECK(provider_chain.add_share_unchecked(child).disposition ==
          ShareDisposition::Connected);
#endif

    P2pNodeProtocol provider_protocol(
        provider_chain, provider_work, provider_mutex);
    P2pNodeProtocol requester_protocol(
        requester_chain, requester_work, requester_mutex);
    P2pNodeProtocol leaf_protocol(
        leaf_chain, leaf_work, leaf_mutex);

    {
        Share prefix = make_parent();
        prefix.zano_height = 100;
        prefix.mining_header_hash[0] ^= 0x11U;

        Share stale = make_child(prefix);
        stale.zano_height = 101;
        stale.mining_header_hash[0] ^= 0x22U;

        Share descendant = make_child(stale);
        descendant.zano_height = 102;
        descendant.mining_header_hash[0] ^= 0x33U;

        const ShareId prefix_id = share_id(prefix);
        const ShareId stale_id = share_id(stale);
        const ShareId descendant_id = share_id(descendant);

        ShareChain audit_chain;
        CHECK(audit_chain.add_share_unchecked(prefix).disposition ==
              ShareDisposition::Connected);
        CHECK(audit_chain.add_share_unchecked(stale).disposition ==
              ShareDisposition::Connected);
        CHECK(audit_chain.add_share_unchecked(descendant).disposition ==
              ShareDisposition::Connected);

        P2pTrustedWorkRegistry audit_work;

        Hash256 prefix_zano_parent{};
        prefix_zano_parent.front() = 0x40;

        Hash256 displaced_zano_parent{};
        displaced_zano_parent.front() = 0x41;

        Hash256 canonical_zano_parent{};
        canonical_zano_parent.front() = 0x42;

        Hash256 descendant_zano_parent{};
        descendant_zano_parent.front() = 0x43;

        audit_work.remember(
            context_for(prefix),
            ShareId{},
            prefix_zano_parent);
        audit_work.remember(
            context_for(stale),
            prefix_id,
            displaced_zano_parent);
        audit_work.remember(
            context_for(descendant),
            stale_id,
            descendant_zano_parent);

        ShareWorkContext stale_offchain = context_for(stale);
        stale_offchain.mining_header_hash[0] ^= 0x51U;
        ShareId stale_offchain_parent{};
        stale_offchain_parent.back() = 0x51;
        audit_work.remember(
            stale_offchain,
            stale_offchain_parent,
            displaced_zano_parent);

        ShareWorkContext canonical_same_height = context_for(stale);
        canonical_same_height.mining_header_hash[0] ^= 0x61U;
        ShareId canonical_same_height_parent{};
        canonical_same_height_parent.back() = 0x61;
        audit_work.remember(
            canonical_same_height,
            canonical_same_height_parent,
            canonical_zano_parent);

        std::mutex audit_mutex;
        P2pNodeProtocol audit_protocol(
            audit_chain,
            audit_work,
            audit_mutex);

        const std::vector<std::uint64_t> audit_heights =
            audit_protocol.trusted_work_provenance_heights();
        CHECK(audit_heights.size() == 3);
        CHECK(audit_heights[0] == 100);
        CHECK(audit_heights[1] == 101);
        CHECK(audit_heights[2] == 102);

        const P2pCanonicalReconciliationResult audit_result =
            audit_protocol.reconcile_canonical_parent(
                101,
                canonical_zano_parent);

        CHECK(audit_result.pruned_connected_shares == 2);
        CHECK(audit_result.revoked_trusted_work == 2);

        CHECK(audit_chain.find(prefix_id) != nullptr);
        CHECK(audit_chain.find(stale_id) == nullptr);
        CHECK(audit_chain.find(descendant_id) == nullptr);
        CHECK(audit_chain.connected_size() == 1);
        CHECK(audit_chain.best_tip() != nullptr);
        CHECK(audit_chain.best_tip()->id == prefix_id);

        CHECK(audit_work.find(
                  stale.zano_height,
                  stale.mining_header_hash,
                  prefix_id) == nullptr);
        CHECK(audit_work.find(
                  stale_offchain.zano_height,
                  stale_offchain.mining_header_hash,
                  stale_offchain_parent) == nullptr);

        CHECK(audit_work.find(
                  canonical_same_height.zano_height,
                  canonical_same_height.mining_header_hash,
                  canonical_same_height_parent) != nullptr);

        // Later-height work may remain authorized, but its sidechain-parent
        // binding prevents it from reconnecting after that parent was pruned.
        CHECK(audit_work.find(
                  descendant.zano_height,
                  descendant.mining_header_hash,
                  stale_id) != nullptr);
    }

    {
        Share rollback_prefix = make_parent();
        rollback_prefix.zano_height = 200;
        rollback_prefix.mining_header_hash[0] ^= 0x71U;

        Share rollback_future = make_child(rollback_prefix);
        rollback_future.zano_height = 203;
        rollback_future.mining_header_hash[0] ^= 0x72U;

        Share rollback_descendant = make_child(rollback_future);
        rollback_descendant.zano_height = 204;
        rollback_descendant.mining_header_hash[0] ^= 0x73U;

        const ShareId rollback_prefix_id =
            share_id(rollback_prefix);
        const ShareId rollback_future_id =
            share_id(rollback_future);
        const ShareId rollback_descendant_id =
            share_id(rollback_descendant);

        ShareChain rollback_chain;
        CHECK(rollback_chain.add_share_unchecked(
                  rollback_prefix).disposition ==
              ShareDisposition::Connected);
        CHECK(rollback_chain.add_share_unchecked(
                  rollback_future).disposition ==
              ShareDisposition::Connected);
        CHECK(rollback_chain.add_share_unchecked(
                  rollback_descendant).disposition ==
              ShareDisposition::Connected);

        P2pTrustedWorkRegistry rollback_work;

        Hash256 rollback_parent{};
        rollback_parent.front() = 0x81;

        Hash256 future_parent{};
        future_parent.front() = 0x82;

        Hash256 descendant_parent{};
        descendant_parent.front() = 0x83;

        rollback_work.remember(
            context_for(rollback_prefix),
            ShareId{},
            rollback_parent);
        rollback_work.remember(
            context_for(rollback_future),
            rollback_prefix_id,
            future_parent);
        rollback_work.remember(
            context_for(rollback_descendant),
            rollback_future_id,
            descendant_parent);

        ShareWorkContext future_offchain =
            context_for(rollback_future);
        future_offchain.mining_header_hash[0] ^= 0x55U;

        ShareId future_offchain_parent{};
        future_offchain_parent.back() = 0x55;

        rollback_work.remember(
            future_offchain,
            future_offchain_parent,
            future_parent);

        std::mutex rollback_mutex;
        P2pNodeProtocol rollback_protocol(
            rollback_chain,
            rollback_work,
            rollback_mutex);

        Share replay_without_work =
            make_child(rollback_prefix);
        replay_without_work.zano_height = 205;
        replay_without_work.mining_header_hash[0] ^= 0x74U;

        const ShareId replay_without_work_id =
            share_id(replay_without_work);

        CHECK(rollback_chain.add_share_unchecked(
                  replay_without_work).disposition ==
              ShareDisposition::Connected);

        ShareWorkContext compatibility_future =
            context_for(rollback_future);
        compatibility_future.zano_height = 206;
        compatibility_future.mining_header_hash[0] ^= 0x56U;
        rollback_work.remember(compatibility_future);

        const P2pCanonicalReconciliationResult rollback_result =
            rollback_protocol.reconcile_unavailable_work_above(
                rollback_prefix.zano_height);

        CHECK(rollback_result.pruned_connected_shares == 3);
        CHECK(rollback_result.revoked_trusted_work == 4);

        CHECK(rollback_chain.find(rollback_prefix_id) != nullptr);
        CHECK(rollback_chain.find(rollback_future_id) == nullptr);
        CHECK(rollback_chain.find(rollback_descendant_id) == nullptr);
        CHECK(rollback_chain.find(replay_without_work_id) == nullptr);
        CHECK(rollback_chain.connected_size() == 1);
        CHECK(rollback_chain.best_tip() != nullptr);
        CHECK(rollback_chain.best_tip()->id == rollback_prefix_id);

        CHECK(rollback_work.find(
                  rollback_future.zano_height,
                  rollback_future.mining_header_hash,
                  rollback_prefix_id) == nullptr);
        CHECK(rollback_work.find(
                  future_offchain.zano_height,
                  future_offchain.mining_header_hash,
                  future_offchain_parent) == nullptr);

        CHECK(rollback_work.find(
                  rollback_prefix.zano_height,
                  rollback_prefix.mining_header_hash,
                  ShareId{}) != nullptr);

        CHECK(rollback_work.find(
                  compatibility_future.zano_height,
                  compatibility_future.mining_header_hash) == nullptr);
    }

    // Regression: a long-lived provider can prune the share captured in its
    // connection-time handshake. A reconnecting peer first requests that stale
    // ID. The provider must preserve the explicit NotFound response and then
    // directly advertise its current application-level tip, otherwise initial
    // synchronization dead-ends on the stale handshake snapshot.
    {
        P2pHandshake stale_provider_handshake =
            make_handshake(0x31);

        ShareId stale_tip_id = child_id;
        stale_tip_id[0] ^= 0xffU;

        CHECK(stale_tip_id != child_id);
        CHECK(provider_chain.find(stale_tip_id) == nullptr);

        stale_provider_handshake.best_share_id =
            stale_tip_id;
        stale_provider_handshake.best_share_height =
            child.share_height + 100;

        std::atomic<bool> saw_not_found{false};
        std::atomic<bool> saw_fresh_tip{false};

        P2pRuntime* stale_provider_runtime_ptr = nullptr;

        P2pRuntime stale_provider_runtime(
            P2pRuntimeConfig{
                P2pEndpoint{"127.0.0.1", 0},
                stale_provider_handshake,
            },
            [&](const P2pHandshake& peer,
                const P2pEnvelope& envelope) {
                if (stale_provider_runtime_ptr != nullptr) {
                    static_cast<void>(
                        provider_protocol.handle(
                            *stale_provider_runtime_ptr,
                            peer,
                            envelope,
                            child.timestamp,
                            ProgPowZContextMode::Light));
                }
            });

        P2pRuntime stale_requester_runtime(
            P2pRuntimeConfig{
                P2pEndpoint{"127.0.0.1", 0},
                make_handshake(0x32),
            },
            [&](const P2pHandshake&,
                const P2pEnvelope& envelope) {
                if (envelope.type ==
                    P2pMessageType::ShareResponse) {
                    const P2pShareResponse response =
                        parse_p2p_share_response_envelope(
                            envelope);

                    if (response.code ==
                            P2pShareResponseCode::NotFound &&
                        response.requested_id ==
                            stale_tip_id) {
                        saw_not_found.store(true);
                    }
                    return;
                }

                if (envelope.type ==
                    P2pMessageType::TipAnnounce) {
                    const P2pTipHint hint =
                        parse_p2p_tip_announce_envelope(
                            envelope);

                    if (hint.share_id == child_id &&
                        hint.share_height ==
                            child.share_height) {
                        saw_fresh_tip.store(true);
                    }
                }
            },
            [&](const P2pHandshake& peer)
                -> std::optional<P2pEnvelope> {
                return requester_protocol
                    .initial_sync_request(peer);
            });

        stale_provider_runtime_ptr =
            &stale_provider_runtime;

        stale_provider_runtime.start();
        stale_requester_runtime.start();

        stale_requester_runtime.connect_peer(
            P2pEndpoint{
                "127.0.0.1",
                stale_provider_runtime.listen_port(),
            });

        CHECK(wait_for([&] {
            return saw_not_found.load() &&
                   saw_fresh_tip.load();
        }));

        stale_requester_runtime.stop();
        stale_provider_runtime.stop();

        CHECK(saw_not_found.load());
        CHECK(saw_fresh_tip.load());
    }

    std::atomic<std::size_t> provider_messages{0};
    std::atomic<std::size_t> requester_messages{0};
    std::atomic<std::size_t> leaf_messages{0};
    std::atomic<bool> requester_relayed_share{false};
    std::atomic<bool> requester_relayed_tip{false};
    P2pRuntime* provider_runtime_ptr = nullptr;
    P2pRuntime* requester_runtime_ptr = nullptr;
    P2pRuntime* leaf_runtime_ptr = nullptr;

    P2pRuntime provider_runtime(
        P2pRuntimeConfig{
            P2pEndpoint{"127.0.0.1", 0},
            make_handshake(0x20),
        },
        [&](const P2pHandshake& peer, const P2pEnvelope& envelope) {
            ++provider_messages;
            if (provider_runtime_ptr != nullptr) {
                static_cast<void>(provider_protocol.handle(
                    *provider_runtime_ptr,
                    peer,
                    envelope,
                    child.timestamp,
                    ProgPowZContextMode::Light));
            }
        });

    P2pRuntime requester_runtime(
        P2pRuntimeConfig{
            P2pEndpoint{"127.0.0.1", 0},
            make_handshake(0x70),
        },
        [&](const P2pHandshake& peer, const P2pEnvelope& envelope) {
            ++requester_messages;
            if (requester_runtime_ptr != nullptr) {
                const P2pNodeMessageResult result = requester_protocol.handle(
                    *requester_runtime_ptr,
                    peer,
                    envelope,
                    child.timestamp,
                    ProgPowZContextMode::Light);
                if (result.relayed_share) {
                    requester_relayed_share.store(true);
                }
                if (result.relayed_tip) {
                    requester_relayed_tip.store(true);
                }
            }
        });

    P2pRuntime leaf_runtime(
        P2pRuntimeConfig{
            P2pEndpoint{"127.0.0.1", 0},
            make_handshake(0xb0),
        },
        [&](const P2pHandshake& peer, const P2pEnvelope& envelope) {
            ++leaf_messages;
            if (leaf_runtime_ptr != nullptr) {
                static_cast<void>(leaf_protocol.handle(
                    *leaf_runtime_ptr,
                    peer,
                    envelope,
                    child.timestamp,
                    ProgPowZContextMode::Light));
            }
        });

    provider_runtime_ptr = &provider_runtime;
    requester_runtime_ptr = &requester_runtime;
    leaf_runtime_ptr = &leaf_runtime;

    provider_runtime.start();
    requester_runtime.start();
    leaf_runtime.start();
    requester_runtime.connect_peer(
        P2pEndpoint{"127.0.0.1", provider_runtime.listen_port()});
    leaf_runtime.connect_peer(
        P2pEndpoint{"127.0.0.1", requester_runtime.listen_port()});

    CHECK(wait_for([&] {
        return provider_runtime.peer_count() == 1 &&
               requester_runtime.peer_count() == 2 &&
               leaf_runtime.peer_count() == 1;
    }));

    // Child-first gossip must trigger the full targeted sync path on the relay:
    // requester orphan -> ShareRequest(parent) -> provider response -> local
    // parent revalidation -> deterministic child promotion. The now-connected
    // parent and best-tip hint are then relayed to the leaf without echoing the
    // source, causing the leaf to request and validate the promoted child too.
    provider_runtime.broadcast(make_p2p_share_announce_envelope(child));

#ifdef ZANO_P2POOL_HAVE_PROGPOWZ
    CHECK(wait_for([&] {
        std::lock_guard lock(requester_mutex);
        return requester_chain.contains(parent_id) &&
               requester_chain.contains(child_id) &&
               !requester_chain.is_orphan(child_id);
    }));
    CHECK(wait_for([&] {
        std::lock_guard lock(leaf_mutex);
        return leaf_chain.contains(parent_id) &&
               leaf_chain.contains(child_id) &&
               !leaf_chain.is_orphan(child_id);
    }));
    CHECK(requester_messages.load() >= 2);
    CHECK(provider_messages.load() >= 1);
    CHECK(leaf_messages.load() >= 2);
    CHECK(requester_relayed_share.load());
    CHECK(requester_relayed_tip.load());

    {
        std::lock_guard lock(requester_mutex);
        CHECK(requester_chain.connected_size() == 2);
        CHECK(requester_chain.orphan_size() == 0);
        CHECK(requester_chain.best_tip() != nullptr);
        CHECK(requester_chain.best_tip()->id == child_id);
    }
    {
        std::lock_guard lock(leaf_mutex);
        CHECK(leaf_chain.connected_size() == 2);
        CHECK(leaf_chain.orphan_size() == 0);
        CHECK(leaf_chain.best_tip() != nullptr);
        CHECK(leaf_chain.best_tip()->id == child_id);
    }
#else
    // Lightweight builds receive the live gossip frame but fail closed at
    // local PoW admission without the exact backend, so nothing is relayed.
    CHECK(wait_for([&] { return requester_messages.load() >= 1; }));
    {
        std::lock_guard lock(requester_mutex);
        CHECK(requester_chain.connected_size() == 0);
        CHECK(!requester_chain.contains(parent_id));
        CHECK(!requester_chain.contains(child_id));
    }
    {
        std::lock_guard lock(leaf_mutex);
        CHECK(leaf_chain.connected_size() == 0);
        CHECK(!leaf_chain.contains(parent_id));
        CHECK(!leaf_chain.contains(child_id));
    }
    CHECK(!requester_relayed_share.load());
    CHECK(!requester_relayed_tip.load());
#endif

    // Benign or ambiguous synchronization/trust states must not accrue
    // reputation damage. Generic share rejection can be caused by local
    // validation inability, so only explicitly attributable capability misuse
    // is scoreable on the share paths.
    P2pNodeMessageResult neutral_result;
    neutral_result.status = P2pNodeMessageStatus::MiningContextProcessed;
    neutral_result.mining_context_status =
        P2pMiningContextTrustStatus::AnchorMismatch;
    CHECK(p2p_node_message_penalty(neutral_result) == 0);
    neutral_result.status = P2pNodeMessageStatus::ShareProcessed;
    neutral_result.share_status = P2pShareReceiveStatus::Duplicate;
    CHECK(p2p_node_message_penalty(neutral_result) == 0);
    neutral_result.share_status = P2pShareReceiveStatus::UnknownWorkContext;
    CHECK(p2p_node_message_penalty(neutral_result) == 0);
    neutral_result.share_status = P2pShareReceiveStatus::Rejected;
    CHECK(p2p_node_message_penalty(neutral_result) == 0);
    neutral_result.share_status = P2pShareReceiveStatus::CapabilityMissing;
    CHECK(p2p_node_message_penalty(neutral_result) ==
          kP2pProtocolViolationPenalty);

    neutral_result.status = P2pNodeMessageStatus::ShareResponseProcessed;
    neutral_result.sync_status = P2pShareSyncReceiveStatus::ShareProcessed;
    neutral_result.share_status = P2pShareReceiveStatus::Rejected;
    CHECK(p2p_node_message_penalty(neutral_result) == 0);
    neutral_result.share_status = P2pShareReceiveStatus::CapabilityMissing;
    CHECK(p2p_node_message_penalty(neutral_result) ==
          kP2pProtocolViolationPenalty);
    neutral_result.sync_status = P2pShareSyncReceiveStatus::CapabilityMissing;
    CHECK(p2p_node_message_penalty(neutral_result) ==
          kP2pProtocolViolationPenalty);

    // Explicit capability abuse is attributable to the advertised node ID. It
    // must reach the ban threshold without changing any local consensus state.
    const std::size_t adversarial_connected_before =
        requester_protocol.connected_share_count();
    const P2pTipHint adversarial_tip_before = requester_protocol.local_tip();

    P2pHandshake capability_abuser = make_handshake(0x35);
    capability_abuser.capabilities = 0;
    const P2pEnvelope capability_abuse =
        make_p2p_share_announce_envelope(parent);
    for (std::uint32_t i = 1; i <= 4; ++i) {
        const P2pNodeMessageResult result = requester_protocol.handle(
            requester_runtime,
            capability_abuser,
            capability_abuse,
            child.timestamp,
            ProgPowZContextMode::Light);
        CHECK(result.status == P2pNodeMessageStatus::ShareProcessed);
        CHECK(result.share_status ==
              P2pShareReceiveStatus::CapabilityMissing);
        CHECK(requester_runtime.peer_score(capability_abuser.node_id) ==
              i * kP2pProtocolViolationPenalty);
        CHECK(requester_protocol.connected_share_count() ==
              adversarial_connected_before);
        CHECK(requester_protocol.local_tip() == adversarial_tip_before);
    }
    CHECK(requester_runtime.peer_banned(capability_abuser.node_id));

    // Unknown mining contexts are not attributable consensus abuse. A rotating
    // stream must stay reputation-neutral and cannot create shares; transport
    // rate limiting is the separate resource-exhaustion control.
    const P2pHandshake unknown_work_peer = make_handshake(0x36);
    for (std::uint64_t i = 0; i < 256; ++i) {
        Share unknown_work = parent;
        unknown_work.zano_height = 50'000 + i;
        unknown_work.mining_header_hash.fill(
            static_cast<std::uint8_t>(0xa0U + (i & 0x0fU)));
        unknown_work.nonce = UINT64_C(0x8000000000000000) + i;
        const P2pNodeMessageResult result = requester_protocol.handle(
            requester_runtime,
            unknown_work_peer,
            make_p2p_share_announce_envelope(unknown_work),
            child.timestamp,
            ProgPowZContextMode::Light);
        CHECK(result.status == P2pNodeMessageStatus::ShareProcessed);
        CHECK(result.share_status ==
              P2pShareReceiveStatus::UnknownWorkContext);
    }
    CHECK(requester_runtime.peer_score(unknown_work_peer.node_id) == 0);
    CHECK(!requester_runtime.peer_banned(unknown_work_peer.node_id));
    CHECK(requester_protocol.connected_share_count() ==
          adversarial_connected_before);
    CHECK(requester_protocol.local_tip() == adversarial_tip_before);

    // A parsed but impossible in-session handshake is an unambiguous protocol
    // violation. P2pNodeProtocol must report it to the runtime automatically.
    const P2pHandshake scored_peer = make_handshake(0x33);
    P2pEnvelope unexpected_handshake;
    unexpected_handshake.type = P2pMessageType::Handshake;
    for (std::uint32_t i = 1; i <= 4; ++i) {
        const P2pNodeMessageResult result = requester_protocol.handle(
            requester_runtime,
            scored_peer,
            unexpected_handshake,
            child.timestamp,
            ProgPowZContextMode::Light);
        CHECK(result.status == P2pNodeMessageStatus::UnexpectedHandshake);
        CHECK(requester_runtime.peer_score(scored_peer.node_id) ==
              i * kP2pProtocolViolationPenalty);
    }
    CHECK(requester_runtime.peer_banned(scored_peer.node_id));

    // Generic parse/validation exceptions still propagate, but remain
    // reputation-neutral until peer-attributable protocol violations have a
    // dedicated exception type. This avoids banning a peer for a local/internal
    // failure that happens to use the same broad exception class.
    const P2pHandshake malformed_peer = make_handshake(0x44);
    P2pEnvelope malformed_tip;
    malformed_tip.type = P2pMessageType::TipAnnounce;
    malformed_tip.payload = {0x01};
    bool malformed_threw = false;
    try {
        static_cast<void>(requester_protocol.handle(
            requester_runtime,
            malformed_peer,
            malformed_tip,
            child.timestamp,
            ProgPowZContextMode::Light));
    } catch (...) {
        malformed_threw = true;
    }
    CHECK(malformed_threw);
    CHECK(requester_runtime.peer_score(malformed_peer.node_id) == 0);
    CHECK(!requester_runtime.peer_banned(malformed_peer.node_id));
    CHECK(requester_protocol.connected_share_count() ==
          adversarial_connected_before);
    CHECK(requester_protocol.local_tip() == adversarial_tip_before);

    leaf_runtime.stop();
    requester_runtime.stop();
    provider_runtime.stop();
    return 0;
}
