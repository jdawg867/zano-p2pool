#include "zano_p2pool/p2p_runtime.hpp"
#include "zano_p2pool/p2p_tip.hpp"
#include "test_check.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace {

using namespace zano_p2pool;
using namespace std::chrono_literals;

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

struct Inbox {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<P2pEnvelope> messages;

    void push(const P2pEnvelope& envelope) {
        {
            std::lock_guard lock(mutex);
            messages.push_back(envelope);
        }
        cv.notify_all();
    }

    bool wait_for_count(std::size_t count) {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, 2s, [&] { return messages.size() >= count; });
    }

    std::size_t size() {
        std::lock_guard lock(mutex);
        return messages.size();
    }
};

bool wait_for_peers(
    const P2pRuntime& left,
    const P2pRuntime& right,
    std::size_t expected) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (left.peer_count() == expected && right.peer_count() == expected) {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

bool wait_for_counts(
    const P2pRuntime& first,
    std::size_t first_expected,
    const P2pRuntime& second,
    std::size_t second_expected) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (first.peer_count() == first_expected &&
            second.peer_count() == second_expected) {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return first.peer_count() == first_expected &&
           second.peer_count() == second_expected;
}

}  // namespace

int main() {
    Inbox inbox_a;
    Inbox inbox_b;
    Inbox inbox_c;

    P2pRuntime node_a(
        P2pRuntimeConfig{
            P2pEndpoint{"127.0.0.1", 0},
            make_handshake(0x10),
        },
        [&inbox_a](const P2pHandshake&, const P2pEnvelope& envelope) {
            inbox_a.push(envelope);
        });

    P2pRuntime node_b(
        P2pRuntimeConfig{
            P2pEndpoint{"127.0.0.1", 0},
            make_handshake(0x50),
        },
        [&inbox_b](const P2pHandshake&, const P2pEnvelope& envelope) {
            inbox_b.push(envelope);
        });

    P2pRuntime node_c(
        P2pRuntimeConfig{
            P2pEndpoint{"127.0.0.1", 0},
            make_handshake(0x90),
        },
        [&inbox_c](const P2pHandshake&, const P2pEnvelope& envelope) {
            inbox_c.push(envelope);
        });

    CHECK(!node_a.running());
    CHECK(!node_b.running());
    CHECK(!node_c.running());
    node_a.start();
    node_b.start();
    node_c.start();
    CHECK(node_a.running());
    CHECK(node_b.running());
    CHECK(node_c.running());
    CHECK(node_a.listen_port() != 0);
    CHECK(node_b.listen_port() != 0);
    CHECK(node_c.listen_port() != 0);
    CHECK(node_a.local_handshake().listen_port == node_a.listen_port());
    CHECK(node_b.local_handshake().listen_port == node_b.listen_port());
    CHECK(node_c.local_handshake().listen_port == node_c.listen_port());

    node_a.connect_peer(P2pEndpoint{"127.0.0.1", node_b.listen_port()});
    CHECK(wait_for_peers(node_a, node_b, 1));
    node_a.connect_peer(P2pEndpoint{"127.0.0.1", node_c.listen_port()});
    CHECK(wait_for_counts(node_a, 2, node_c, 1));
    CHECK(node_a.peer_count() == 2);

    P2pTipHint tip_a;
    tip_a.share_id[31] = 0xa1;
    tip_a.share_height = 41;
    node_a.broadcast(make_p2p_tip_announce_envelope(tip_a));
    CHECK(inbox_b.wait_for_count(1));
    CHECK(inbox_c.wait_for_count(1));
    {
        std::lock_guard lock(inbox_b.mutex);
        CHECK(inbox_b.messages.size() == 1);
        CHECK(parse_p2p_tip_announce_envelope(inbox_b.messages[0]) == tip_a);
    }
    {
        std::lock_guard lock(inbox_c.mutex);
        CHECK(inbox_c.messages.size() == 1);
        CHECK(parse_p2p_tip_announce_envelope(inbox_c.messages[0]) == tip_a);
    }

    P2pTipHint relay_tip;
    relay_tip.share_id[31] = 0xc3;
    relay_tip.share_height = 43;
    node_a.broadcast_except(
        node_b.local_handshake().node_id,
        make_p2p_tip_announce_envelope(relay_tip));
    CHECK(inbox_c.wait_for_count(2));
    std::this_thread::sleep_for(50ms);
    CHECK(inbox_b.size() == 1);
    {
        std::lock_guard lock(inbox_c.mutex);
        CHECK(parse_p2p_tip_announce_envelope(inbox_c.messages[1]) == relay_tip);
    }

    P2pTipHint tip_b;
    tip_b.share_id[31] = 0xb2;
    tip_b.share_height = 42;
    CHECK(node_b.send_to(
        node_a.local_handshake().node_id,
        make_p2p_tip_announce_envelope(tip_b)));
    CHECK(inbox_a.wait_for_count(1));
    {
        std::lock_guard lock(inbox_a.mutex);
        CHECK(inbox_a.messages.size() == 1);
        CHECK(parse_p2p_tip_announce_envelope(inbox_a.messages[0]) == tip_b);
    }

    NodeId unknown{};
    unknown[31] = 0xee;
    CHECK(!node_b.send_to(
        unknown,
        make_p2p_tip_announce_envelope(tip_b)));

    // stop() must interrupt the reader threads currently blocked waiting for
    // another frame and complete without requiring the peer to send anything.
    node_a.stop();
    CHECK(!node_a.running());
    CHECK(node_a.peer_count() == 0);

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline &&
           (node_b.peer_count() != 0 || node_c.peer_count() != 0)) {
        std::this_thread::sleep_for(10ms);
    }
    CHECK(node_b.peer_count() == 0);
    CHECK(node_c.peer_count() == 0);

    node_b.stop();
    node_c.stop();
    CHECK(!node_b.running());
    CHECK(!node_c.running());
    CHECK(node_b.peer_count() == 0);
    CHECK(node_c.peer_count() == 0);

    // A configured outbound endpoint remains managed even if the first dial
    // fails. Once the endpoint appears, the runtime must connect without a new
    // connect_peer() call. After a later disconnect it must back off, keep
    // retrying, and reconnect again when a replacement listener appears.
    P2pTcpListener port_reserver(
        P2pEndpoint{"127.0.0.1", 0},
        make_handshake(0xc0));
    port_reserver.start();
    const std::uint16_t reconnect_port = port_reserver.port();
    CHECK(reconnect_port != 0);
    port_reserver.stop();

    Inbox reconnect_inbox;
    P2pRuntime reconnect_client(
        P2pRuntimeConfig{
            P2pEndpoint{"127.0.0.1", 0},
            make_handshake(0xd0),
            25ms,
            100ms,
        },
        [&reconnect_inbox](
            const P2pHandshake&,
            const P2pEnvelope& envelope) {
            reconnect_inbox.push(envelope);
        });
    reconnect_client.start();

    bool initial_connect_failed = false;
    try {
        reconnect_client.connect_peer(
            P2pEndpoint{"127.0.0.1", reconnect_port});
    } catch (...) {
        initial_connect_failed = true;
    }
    CHECK(initial_connect_failed);
    CHECK(reconnect_client.peer_count() == 0);

    P2pRuntime reconnect_server(
        P2pRuntimeConfig{
            P2pEndpoint{"127.0.0.1", reconnect_port},
            make_handshake(0xe0),
        });
    reconnect_server.start();
    CHECK(wait_for_peers(reconnect_client, reconnect_server, 1));

    P2pTipHint first_reconnect_tip;
    first_reconnect_tip.share_id[31] = 0xd1;
    first_reconnect_tip.share_height = 101;
    CHECK(reconnect_server.send_to(
        reconnect_client.local_handshake().node_id,
        make_p2p_tip_announce_envelope(first_reconnect_tip)));
    CHECK(reconnect_inbox.wait_for_count(1));

    reconnect_server.stop();
    const auto disconnect_deadline =
        std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < disconnect_deadline &&
           reconnect_client.peer_count() != 0) {
        std::this_thread::sleep_for(10ms);
    }
    CHECK(reconnect_client.peer_count() == 0);

    // Leave the target unavailable long enough to force failed retries and
    // exercise the bounded backoff path before bringing the endpoint back.
    std::this_thread::sleep_for(150ms);

    P2pRuntime restarted_server(
        P2pRuntimeConfig{
            P2pEndpoint{"127.0.0.1", reconnect_port},
            make_handshake(0xf0),
        });
    restarted_server.start();
    CHECK(wait_for_peers(reconnect_client, restarted_server, 1));

    P2pTipHint second_reconnect_tip;
    second_reconnect_tip.share_id[31] = 0xd2;
    second_reconnect_tip.share_height = 102;
    CHECK(restarted_server.send_to(
        reconnect_client.local_handshake().node_id,
        make_p2p_tip_announce_envelope(second_reconnect_tip)));
    CHECK(reconnect_inbox.wait_for_count(2));
    {
        std::lock_guard lock(reconnect_inbox.mutex);
        CHECK(reconnect_inbox.messages.size() == 2);
        CHECK(
            parse_p2p_tip_announce_envelope(reconnect_inbox.messages[0]) ==
            first_reconnect_tip);
        CHECK(
            parse_p2p_tip_announce_envelope(reconnect_inbox.messages[1]) ==
            second_reconnect_tip);
    }

    restarted_server.stop();
    reconnect_client.stop();
    CHECK(!reconnect_client.running());
    CHECK(reconnect_client.peer_count() == 0);

    // Peer scoring is identity-based and only advances through explicit
    // protocol-violation reports. Sub-threshold penalties leave the connection
    // live. Crossing the threshold disconnects the peer, suppresses the managed
    // outbound reconnect for the ban window, then permits a clean reconnect with
    // a reset score after the temporary ban expires.
    Inbox scored_inbox;
    P2pRuntime scored_client(
        P2pRuntimeConfig{
            P2pEndpoint{"127.0.0.1", 0},
            make_handshake(0x21),
            25ms,
            100ms,
            P2pPeerScoreConfig{50, 200ms},
        },
        [&scored_inbox](
            const P2pHandshake&,
            const P2pEnvelope& envelope) {
            scored_inbox.push(envelope);
        });
    P2pRuntime scored_server(
        P2pRuntimeConfig{
            P2pEndpoint{"127.0.0.1", 0},
            make_handshake(0x61),
        });

    scored_client.start();
    scored_server.start();
    scored_client.connect_peer(
        P2pEndpoint{"127.0.0.1", scored_server.listen_port()});
    CHECK(wait_for_peers(scored_client, scored_server, 1));

    const NodeId scored_server_id = scored_server.local_handshake().node_id;
    CHECK(scored_client.peer_score(scored_server_id) == 0);
    CHECK(!scored_client.peer_banned(scored_server_id));

    scored_client.report_peer_misbehavior(scored_server_id, 20);
    CHECK(scored_client.peer_score(scored_server_id) == 20);
    CHECK(!scored_client.peer_banned(scored_server_id));
    CHECK(scored_client.peer_count() == 1);

    scored_client.report_peer_misbehavior(scored_server_id, 30);
    CHECK(scored_client.peer_score(scored_server_id) == 50);
    CHECK(scored_client.peer_banned(scored_server_id));
    CHECK(wait_for_peers(scored_client, scored_server, 0));

    std::this_thread::sleep_for(100ms);
    CHECK(scored_client.peer_banned(scored_server_id));
    CHECK(scored_client.peer_count() == 0);
    CHECK(scored_server.peer_count() == 0);

    CHECK(wait_for_peers(scored_client, scored_server, 1));
    CHECK(!scored_client.peer_banned(scored_server_id));
    CHECK(scored_client.peer_score(scored_server_id) == 0);

    P2pTipHint post_ban_tip;
    post_ban_tip.share_id[31] = 0x73;
    post_ban_tip.share_height = 203;
    CHECK(scored_server.send_to(
        scored_client.local_handshake().node_id,
        make_p2p_tip_announce_envelope(post_ban_tip)));
    CHECK(scored_inbox.wait_for_count(1));
    {
        std::lock_guard lock(scored_inbox.mutex);
        CHECK(scored_inbox.messages.size() == 1);
        CHECK(
            parse_p2p_tip_announce_envelope(scored_inbox.messages[0]) ==
            post_ban_tip);
    }

    scored_server.stop();
    scored_client.stop();
    CHECK(!scored_client.running());
    CHECK(scored_client.peer_count() == 0);

    return 0;
}
