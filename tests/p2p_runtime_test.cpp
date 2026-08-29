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

}  // namespace

int main() {
    Inbox inbox_a;
    Inbox inbox_b;

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

    CHECK(!node_a.running());
    CHECK(!node_b.running());
    node_a.start();
    node_b.start();
    CHECK(node_a.running());
    CHECK(node_b.running());
    CHECK(node_a.listen_port() != 0);
    CHECK(node_b.listen_port() != 0);
    CHECK(node_a.local_handshake().listen_port == node_a.listen_port());
    CHECK(node_b.local_handshake().listen_port == node_b.listen_port());

    node_a.connect_peer(P2pEndpoint{"127.0.0.1", node_b.listen_port()});
    CHECK(wait_for_peers(node_a, node_b, 1));

    P2pTipHint tip_a;
    tip_a.share_id[31] = 0xa1;
    tip_a.share_height = 41;
    node_a.broadcast(make_p2p_tip_announce_envelope(tip_a));
    CHECK(inbox_b.wait_for_count(1));
    {
        std::lock_guard lock(inbox_b.mutex);
        CHECK(inbox_b.messages.size() == 1);
        CHECK(parse_p2p_tip_announce_envelope(inbox_b.messages[0]) == tip_a);
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
           node_b.peer_count() != 0) {
        std::this_thread::sleep_for(10ms);
    }
    CHECK(node_b.peer_count() == 0);

    node_b.stop();
    CHECK(!node_b.running());
    CHECK(node_b.peer_count() == 0);

    return 0;
}
