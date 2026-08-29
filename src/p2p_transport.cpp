#include "zano_p2pool/p2p_transport.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace zano_p2pool {
namespace {

[[nodiscard]] std::runtime_error socket_error(const std::string& what) {
    return std::runtime_error(what + ": " + std::strerror(errno));
}

void close_fd(int& fd) noexcept {
    if (fd >= 0) {
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
        fd = -1;
    }
}

void send_all(int fd, const std::uint8_t* data, std::size_t size) {
    std::size_t sent = 0;
    while (sent < size) {
        const ssize_t result = ::send(
            fd,
            data + sent,
            size - sent,
            MSG_NOSIGNAL);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw socket_error("P2P send failed");
        }
        if (result == 0) {
            throw std::runtime_error("P2P socket closed during send");
        }
        sent += static_cast<std::size_t>(result);
    }
}

void recv_exact(int fd, std::uint8_t* data, std::size_t size) {
    std::size_t received = 0;
    while (received < size) {
        const ssize_t result = ::recv(fd, data + received, size - received, 0);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw socket_error("P2P receive failed");
        }
        if (result == 0) {
            throw std::runtime_error("P2P socket closed during receive");
        }
        received += static_cast<std::size_t>(result);
    }
}

[[nodiscard]] std::uint16_t read_u16_be(
    const std::array<std::uint8_t, kP2pEnvelopeHeaderSize>& header,
    std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(header[offset]) << 8) |
        static_cast<std::uint16_t>(header[offset + 1]));
}

[[nodiscard]] std::uint32_t read_u32_be(
    const std::array<std::uint8_t, kP2pEnvelopeHeaderSize>& header,
    std::size_t offset) noexcept {
    return (static_cast<std::uint32_t>(header[offset]) << 24) |
           (static_cast<std::uint32_t>(header[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(header[offset + 2]) << 8) |
           static_cast<std::uint32_t>(header[offset + 3]);
}

[[nodiscard]] std::uint32_t validate_stream_header(
    const std::array<std::uint8_t, kP2pEnvelopeHeaderSize>& header) {
    if (!std::equal(kP2pMagic.begin(), kP2pMagic.end(), header.begin())) {
        throw std::runtime_error("invalid P2P envelope magic");
    }
    if (header[4] != kP2pProtocolVersion) {
        throw std::runtime_error("unsupported P2P protocol version");
    }
    if (header[5] != static_cast<std::uint8_t>(P2pMessageType::Handshake)) {
        throw std::runtime_error("unsupported P2P message type");
    }
    if (read_u16_be(header, 6) != 0) {
        throw std::runtime_error("unsupported P2P envelope flags");
    }

    const std::uint32_t payload_size = read_u32_be(header, 8);
    if (payload_size > kP2pMaxPayloadSize) {
        throw std::runtime_error("P2P payload exceeds maximum size");
    }
    return payload_size;
}

[[nodiscard]] P2pEnvelope receive_envelope_fd(int fd) {
    std::array<std::uint8_t, kP2pEnvelopeHeaderSize> header{};
    recv_exact(fd, header.data(), header.size());
    const std::uint32_t payload_size = validate_stream_header(header);

    std::vector<std::uint8_t> frame;
    frame.reserve(header.size() + payload_size);
    frame.insert(frame.end(), header.begin(), header.end());

    if (payload_size != 0) {
        const std::size_t offset = frame.size();
        frame.resize(offset + payload_size);
        recv_exact(fd, frame.data() + offset, payload_size);
    }
    return deserialize_p2p_envelope(frame);
}

void send_envelope_fd(int fd, const P2pEnvelope& envelope) {
    const auto bytes = serialize_p2p_envelope(envelope);
    send_all(fd, bytes.data(), bytes.size());
}

[[nodiscard]] std::string port_string(std::uint16_t port) {
    return std::to_string(static_cast<unsigned int>(port));
}

[[nodiscard]] int open_listen_socket(
    const P2pEndpoint& endpoint,
    std::uint16_t& bound_port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    addrinfo* results = nullptr;
    const std::string service = port_string(endpoint.port);
    const char* host = endpoint.host.empty() ? nullptr : endpoint.host.c_str();
    const int gai = ::getaddrinfo(host, service.c_str(), &hints, &results);
    if (gai != 0) {
        throw std::runtime_error(
            std::string("P2P getaddrinfo(bind) failed: ") + gai_strerror(gai));
    }

    int listen_fd = -1;
    for (addrinfo* current = results; current != nullptr; current = current->ai_next) {
        const int fd = ::socket(
            current->ai_family,
            current->ai_socktype,
            current->ai_protocol);
        if (fd < 0) {
            continue;
        }

        int reuse = 1;
        (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        if (::bind(fd, current->ai_addr, current->ai_addrlen) == 0 &&
            ::listen(fd, 16) == 0) {
            listen_fd = fd;
            break;
        }
        ::close(fd);
    }
    ::freeaddrinfo(results);

    if (listen_fd < 0) {
        throw socket_error("P2P bind/listen failed");
    }

    sockaddr_storage address{};
    socklen_t address_length = sizeof(address);
    if (::getsockname(
            listen_fd,
            reinterpret_cast<sockaddr*>(&address),
            &address_length) != 0) {
        ::close(listen_fd);
        throw socket_error("P2P getsockname failed");
    }

    if (address.ss_family == AF_INET) {
        const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(&address);
        bound_port = ntohs(ipv4->sin_port);
    } else if (address.ss_family == AF_INET6) {
        const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(&address);
        bound_port = ntohs(ipv6->sin6_port);
    } else {
        ::close(listen_fd);
        throw std::runtime_error("P2P listener has unsupported address family");
    }

    return listen_fd;
}

[[nodiscard]] int open_connected_socket(const P2pEndpoint& endpoint) {
    if (endpoint.port == 0) {
        throw std::runtime_error("P2P connect port must be non-zero");
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* results = nullptr;
    const std::string service = port_string(endpoint.port);
    const int gai = ::getaddrinfo(
        endpoint.host.c_str(), service.c_str(), &hints, &results);
    if (gai != 0) {
        throw std::runtime_error(
            std::string("P2P getaddrinfo(connect) failed: ") + gai_strerror(gai));
    }

    int connected_fd = -1;
    for (addrinfo* current = results; current != nullptr; current = current->ai_next) {
        const int fd = ::socket(
            current->ai_family,
            current->ai_socktype,
            current->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (::connect(fd, current->ai_addr, current->ai_addrlen) == 0) {
            connected_fd = fd;
            break;
        }
        ::close(fd);
    }
    ::freeaddrinfo(results);

    if (connected_fd < 0) {
        throw socket_error("P2P connect failed");
    }
    return connected_fd;
}

void require_accepted_handshake(
    const P2pHandshake& peer,
    const P2pHandshake& local) {
    switch (validate_p2p_handshake(peer, local.network, local.node_id)) {
        case P2pHandshakeStatus::Accept:
            return;
        case P2pHandshakeStatus::WrongNetwork:
            throw std::runtime_error("P2P peer is on the wrong network");
        case P2pHandshakeStatus::SelfConnection:
            throw std::runtime_error("P2P self-connection rejected");
    }
    throw std::runtime_error("invalid P2P handshake status");
}

}  // namespace

P2pTcpConnection::P2pTcpConnection(
    int socket_fd,
    P2pHandshake peer_handshake,
    bool inbound) noexcept
    : socket_fd_(socket_fd),
      peer_handshake_(std::move(peer_handshake)),
      inbound_(inbound) {}

P2pTcpConnection::~P2pTcpConnection() {
    close();
}

P2pTcpConnection::P2pTcpConnection(P2pTcpConnection&& other) noexcept
    : socket_fd_(std::exchange(other.socket_fd_, -1)),
      peer_handshake_(std::move(other.peer_handshake_)),
      inbound_(other.inbound_) {}

P2pTcpConnection& P2pTcpConnection::operator=(P2pTcpConnection&& other) noexcept {
    if (this != &other) {
        close();
        socket_fd_ = std::exchange(other.socket_fd_, -1);
        peer_handshake_ = std::move(other.peer_handshake_);
        inbound_ = other.inbound_;
    }
    return *this;
}

bool P2pTcpConnection::valid() const noexcept {
    return socket_fd_ >= 0;
}

bool P2pTcpConnection::inbound() const noexcept {
    return inbound_;
}

const P2pHandshake& P2pTcpConnection::peer_handshake() const noexcept {
    return peer_handshake_;
}

void P2pTcpConnection::send_envelope(const P2pEnvelope& envelope) {
    if (!valid()) {
        throw std::runtime_error("P2P connection is closed");
    }
    send_envelope_fd(socket_fd_, envelope);
}

P2pEnvelope P2pTcpConnection::receive_envelope() {
    if (!valid()) {
        throw std::runtime_error("P2P connection is closed");
    }
    return receive_envelope_fd(socket_fd_);
}

void P2pTcpConnection::close() noexcept {
    close_fd(socket_fd_);
}

P2pTcpListener::P2pTcpListener(
    P2pEndpoint endpoint,
    P2pHandshake local_handshake)
    : endpoint_(std::move(endpoint)),
      local_handshake_(std::move(local_handshake)) {
    (void)serialize_p2p_handshake_payload(local_handshake_);
}

P2pTcpListener::~P2pTcpListener() {
    stop();
}

void P2pTcpListener::start() {
    if (running()) {
        throw std::runtime_error("P2P listener already running");
    }

    std::uint16_t actual_port = 0;
    listen_fd_ = open_listen_socket(endpoint_, actual_port);
    bound_port_ = actual_port;
    if (local_handshake_.listen_port == 0) {
        local_handshake_.listen_port = bound_port_;
    }
}

void P2pTcpListener::stop() noexcept {
    close_fd(listen_fd_);
    bound_port_ = 0;
}

bool P2pTcpListener::running() const noexcept {
    return listen_fd_ >= 0;
}

std::uint16_t P2pTcpListener::port() const noexcept {
    return bound_port_;
}

const P2pHandshake& P2pTcpListener::local_handshake() const noexcept {
    return local_handshake_;
}

P2pTcpConnection P2pTcpListener::accept_peer() {
    if (!running()) {
        throw std::runtime_error("P2P listener is not running");
    }

    sockaddr_storage address{};
    socklen_t address_length = sizeof(address);
    int peer_fd = -1;
    for (;;) {
        peer_fd = ::accept(
            listen_fd_,
            reinterpret_cast<sockaddr*>(&address),
            &address_length);
        if (peer_fd >= 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        throw socket_error("P2P accept failed");
    }

    try {
        const P2pEnvelope peer_envelope = receive_envelope_fd(peer_fd);
        const P2pHandshake peer_handshake =
            parse_p2p_handshake_envelope(peer_envelope);
        require_accepted_handshake(peer_handshake, local_handshake_);

        send_envelope_fd(
            peer_fd,
            make_p2p_handshake_envelope(local_handshake_));
        return P2pTcpConnection(peer_fd, peer_handshake, true);
    } catch (...) {
        close_fd(peer_fd);
        throw;
    }
}

P2pTcpConnection connect_p2p_peer(
    const P2pEndpoint& endpoint,
    const P2pHandshake& local_handshake) {
    (void)serialize_p2p_handshake_payload(local_handshake);

    int socket_fd = open_connected_socket(endpoint);
    try {
        send_envelope_fd(
            socket_fd,
            make_p2p_handshake_envelope(local_handshake));
        const P2pEnvelope peer_envelope = receive_envelope_fd(socket_fd);
        const P2pHandshake peer_handshake =
            parse_p2p_handshake_envelope(peer_envelope);
        require_accepted_handshake(peer_handshake, local_handshake);
        return P2pTcpConnection(socket_fd, peer_handshake, false);
    } catch (...) {
        close_fd(socket_fd);
        throw;
    }
}

}  // namespace zano_p2pool
