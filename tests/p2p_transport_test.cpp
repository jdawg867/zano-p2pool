#include "zano_p2pool/p2p_transport.hpp"
#include "test_check.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <future>
#include <stdexcept>
#include <utility>

namespace {

using namespace zano_p2pool;

NodeId node_id_from(std::uint8_t first) {
    NodeId id{};
    for (std::size_t i = 0; i < id.size(); ++i) {
        id[i] = static_cast<std::uint8_t>(first + i);
    }
    return id;
}

ShareId share_id_from(std::uint8_t first) {
    ShareId id{};
    for (std::size_t i = 0; i < id.size(); ++i) {
        id[i] = static_cast<std::uint8_t>(first + i);
    }
    return id;
}

P2pHandshake make_handshake(
    P2pNetwork network,
    std::uint8_t node_seed,
    std::uint16_t listen_port = 0) {
    P2pHandshake handshake;
    handshake.network = network;
    handshake.sidechain_id = canonical_p2p_sidechain_id(network);
    handshake.node_id = node_id_from(node_seed);
    handshake.capabilities = kP2pCapabilitiesV1;
    handshake.listen_port = listen_port;
    handshake.best_share_id = share_id_from(
        static_cast<std::uint8_t>(node_seed + 0x40U));
    handshake.best_share_height = 17;
    return handshake;
}

template <typename Fn>
bool throws_runtime(Fn&& fn) {
    try {
        std::forward<Fn>(fn)();
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

int raw_connect(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error("raw test socket failed");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    CHECK(::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);

    if (::connect(
            fd,
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)) != 0) {
        ::close(fd);
        throw std::runtime_error("raw test connect failed");
    }
    return fd;
}

void raw_send_all(int fd, const std::uint8_t* data, std::size_t size) {
    std::size_t sent = 0;
    while (sent < size) {
        const ssize_t result = ::send(fd, data + sent, size - sent, 0);
        if (result <= 0) {
            throw std::runtime_error("raw test send failed");
        }
        sent += static_cast<std::size_t>(result);
    }
}

void expect_rejected_raw_header(
    const std::array<std::uint8_t, kP2pEnvelopeHeaderSize>& header) {
    P2pTcpListener listener(
        P2pEndpoint{"127.0.0.1", 0},
        make_handshake(P2pNetwork::Testnet, 0x10));
    listener.start();
    CHECK(listener.port() != 0);

    auto accepted = std::async(std::launch::async, [&listener] {
        return listener.accept_peer();
    });

    const int fd = raw_connect(listener.port());
    raw_send_all(fd, header.data(), header.size());
    ::shutdown(fd, SHUT_RDWR);
    ::close(fd);

    CHECK(accepted.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    CHECK(throws_runtime([&] { (void)accepted.get(); }));
}

}  // namespace

int main() {
    const P2pHandshake server_initial =
        make_handshake(P2pNetwork::Testnet, 0x10);
    const P2pHandshake client_handshake =
        make_handshake(P2pNetwork::Testnet, 0x50, 41001);

    // A locally constructed compatibility handshake may omit sidechain_id, but
    // transport must immediately normalize its stored runtime identity to the
    // canonical sidechain before any socket is opened.
    {
        P2pHandshake implicit =
            make_handshake(P2pNetwork::Testnet, 0x08);
        implicit.sidechain_id.fill(0);
        P2pTcpListener normalized(
            P2pEndpoint{"127.0.0.1", 0}, implicit);
        CHECK(normalized.local_handshake().sidechain_id ==
              canonical_p2p_sidechain_id(P2pNetwork::Testnet));
    }

    P2pTcpListener listener(
        P2pEndpoint{"127.0.0.1", 0}, server_initial);
    CHECK(!listener.running());
    listener.start();
    CHECK(listener.running());
    CHECK(listener.port() != 0);
    CHECK(listener.local_handshake().listen_port == listener.port());
    CHECK(listener.local_handshake().sidechain_id == server_initial.sidechain_id);

    auto accepted = std::async(std::launch::async, [&listener] {
        return listener.accept_peer();
    });

    P2pTcpConnection outbound = connect_p2p_peer(
        P2pEndpoint{"127.0.0.1", listener.port()}, client_handshake);
    CHECK(outbound.valid());
    CHECK(!outbound.inbound());
    CHECK(outbound.peer_handshake() == listener.local_handshake());

    CHECK(accepted.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    P2pTcpConnection inbound = accepted.get();
    CHECK(inbound.valid());
    CHECK(inbound.inbound());
    CHECK(inbound.peer_handshake() == client_handshake);

    // Prove that an established connection continues to use the bounded frame
    // reader/writer after the initial handshake rather than being one-shot.
    outbound.send_envelope(make_p2p_handshake_envelope(client_handshake));
    const P2pEnvelope repeated = inbound.receive_envelope();
    CHECK(parse_p2p_handshake_envelope(repeated) == client_handshake);

    outbound.close();
    inbound.close();
    CHECK(!outbound.valid());
    CHECK(!inbound.valid());
    listener.stop();
    CHECK(!listener.running());

    // Wrong-network peers are rejected on the actual socket handshake path.
    {
        P2pTcpListener testnet_listener(
            P2pEndpoint{"127.0.0.1", 0},
            make_handshake(P2pNetwork::Testnet, 0x20));
        testnet_listener.start();
        auto server_result = std::async(std::launch::async, [&testnet_listener] {
            return testnet_listener.accept_peer();
        });
        CHECK(throws_runtime([&] {
            (void)connect_p2p_peer(
                P2pEndpoint{"127.0.0.1", testnet_listener.port()},
                make_handshake(P2pNetwork::Mainnet, 0x60));
        }));
        CHECK(server_result.wait_for(std::chrono::seconds(2)) ==
              std::future_status::ready);
        CHECK(throws_runtime([&] { (void)server_result.get(); }));
    }

    // Same-parent-network peers with different sidechain consensus identities
    // are also rejected before a runtime peer can be established.
    {
        P2pTcpListener sidechain_listener(
            P2pEndpoint{"127.0.0.1", 0},
            make_handshake(P2pNetwork::Testnet, 0x24));
        sidechain_listener.start();
        auto server_result = std::async(std::launch::async, [&sidechain_listener] {
            return sidechain_listener.accept_peer();
        });

        P2pHandshake incompatible =
            make_handshake(P2pNetwork::Testnet, 0x64);
        incompatible.sidechain_id[0] ^= 0x80U;
        CHECK(throws_runtime([&] {
            (void)connect_p2p_peer(
                P2pEndpoint{"127.0.0.1", sidechain_listener.port()},
                incompatible);
        }));
        CHECK(server_result.wait_for(std::chrono::seconds(2)) ==
              std::future_status::ready);
        CHECK(throws_runtime([&] { (void)server_result.get(); }));
    }

    // A node cannot establish a connection to another endpoint advertising the
    // same stable node identifier.
    {
        const P2pHandshake local =
            make_handshake(P2pNetwork::Testnet, 0x30);
        P2pTcpListener self_listener(
            P2pEndpoint{"127.0.0.1", 0}, local);
        self_listener.start();
        auto server_result = std::async(std::launch::async, [&self_listener] {
            return self_listener.accept_peer();
        });
        CHECK(throws_runtime([&] {
            (void)connect_p2p_peer(
                P2pEndpoint{"127.0.0.1", self_listener.port()}, local);
        }));
        CHECK(server_result.wait_for(std::chrono::seconds(2)) ==
              std::future_status::ready);
        CHECK(throws_runtime([&] { (void)server_result.get(); }));
    }

    // Oversized payload claims are rejected immediately from the 12-byte
    // stream header, before payload allocation/read.
    {
        std::array<std::uint8_t, kP2pEnvelopeHeaderSize> header{
            'Z', 'P', '2', 'P',
            kP2pProtocolVersion,
            static_cast<std::uint8_t>(P2pMessageType::Handshake),
            0, 0,
            0, 1, 0, 1};  // 65537 bytes
        expect_rejected_raw_header(header);
    }

    // Unsupported protocol versions are rejected on the socket path too.
    {
        std::array<std::uint8_t, kP2pEnvelopeHeaderSize> header{
            'Z', 'P', '2', 'P',
            static_cast<std::uint8_t>(kP2pProtocolVersion + 1),
            static_cast<std::uint8_t>(P2pMessageType::Handshake),
            0, 0,
            0, 0, 0, 0};
        expect_rejected_raw_header(header);
    }

    return 0;
}
