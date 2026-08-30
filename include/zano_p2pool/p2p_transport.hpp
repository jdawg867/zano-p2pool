#pragma once

#include "zano_p2pool/p2p_protocol.hpp"

#include <cstdint>
#include <string>

namespace zano_p2pool {

struct P2pEndpoint {
    std::string host{"127.0.0.1"};
    std::uint16_t port{0};
};

class P2pTcpConnection {
public:
    P2pTcpConnection() noexcept = default;
    ~P2pTcpConnection();

    P2pTcpConnection(const P2pTcpConnection&) = delete;
    P2pTcpConnection& operator=(const P2pTcpConnection&) = delete;

    P2pTcpConnection(P2pTcpConnection&& other) noexcept;
    P2pTcpConnection& operator=(P2pTcpConnection&& other) noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool inbound() const noexcept;
    [[nodiscard]] const P2pHandshake& peer_handshake() const noexcept;

    void send_envelope(const P2pEnvelope& envelope);
    [[nodiscard]] P2pEnvelope receive_envelope();

    // Interrupt blocking socket I/O without releasing or mutating the owned
    // descriptor. Long-lived peer runtimes use this to wake receive_envelope(),
    // join the reader thread, and only then call close().
    void shutdown() noexcept;
    void close() noexcept;

private:
    friend class P2pTcpListener;
    friend P2pTcpConnection connect_p2p_peer(
        const P2pEndpoint& endpoint,
        const P2pHandshake& local_handshake);

    P2pTcpConnection(
        int socket_fd,
        P2pHandshake peer_handshake,
        bool inbound) noexcept;

    int socket_fd_{-1};
    P2pHandshake peer_handshake_{};
    bool inbound_{false};
};

class P2pTcpListener {
public:
    P2pTcpListener(P2pEndpoint endpoint, P2pHandshake local_handshake);
    ~P2pTcpListener();

    P2pTcpListener(const P2pTcpListener&) = delete;
    P2pTcpListener& operator=(const P2pTcpListener&) = delete;

    void start();
    void stop() noexcept;

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] std::uint16_t port() const noexcept;
    [[nodiscard]] const P2pHandshake& local_handshake() const noexcept;

    [[nodiscard]] P2pTcpConnection accept_peer();

private:
    P2pEndpoint endpoint_;
    P2pHandshake local_handshake_;
    int listen_fd_{-1};
    std::uint16_t bound_port_{0};
};

[[nodiscard]] P2pTcpConnection connect_p2p_peer(
    const P2pEndpoint& endpoint,
    const P2pHandshake& local_handshake);

}  // namespace zano_p2pool
