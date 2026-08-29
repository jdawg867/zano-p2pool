#include "zano_p2pool/crypto_hash.hpp"
#include "zano_p2pool/share.hpp"
#include "zano_p2pool/stratum_server.hpp"
#include "test_check.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

zano_p2pool::Hash256 hash_from_hex(std::string_view hex) {
    const auto bytes = zano_p2pool::hex_to_bytes(hex);
    CHECK(bytes.size() == 32);
    zano_p2pool::Hash256 hash{};
    std::copy(bytes.begin(), bytes.end(), hash.begin());
    return hash;
}

void send_all(int fd, std::string_view data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const ssize_t sent = ::send(
            fd,
            data.data() + static_cast<std::ptrdiff_t>(offset),
            data.size() - offset,
            0);
        if (sent > 0) {
            offset += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        throw std::runtime_error("socket send failed");
    }
}

std::string read_line(int fd) {
    std::string line;
    while (true) {
        char c = 0;
        const ssize_t received = ::recv(fd, &c, 1, 0);
        if (received == 1) {
            if (c == '\n') {
                return line;
            }
            line.push_back(c);
            continue;
        }
        if (received < 0 && errno == EINTR) {
            continue;
        }
        throw std::runtime_error("socket receive failed before newline");
    }
}

int connect_loopback(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error("client socket creation failed");
    }

    timeval timeout{};
    timeout.tv_sec = 3;
    CHECK(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    CHECK(::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    CHECK(::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(fd);
        throw std::runtime_error("client connect failed");
    }
    return fd;
}

}  // namespace

int main() {
    using namespace zano_p2pool;

    StratumServerConfig defaults;
    CHECK(defaults.bind_address == "127.0.0.1");
    CHECK(defaults.port == 3333);

    StratumServerConfig config;
    config.port = 0;  // ephemeral port for the integration test
    config.sessions.default_share_difficulty = 3;
    config.sessions.minimum_share_difficulty = 1;
    config.sessions.maximum_share_difficulty = 10;

    const Hash256 header = hash_from_hex(
        "ffeeddccbbaa9988776655443322110000112233445566778899aabbccddeeff");
    Hash256 seed{};
    seed[31] = 1;

    StratumTcpServer server(config);
    CHECK(server.publish_template(
              header,
              seed,
              0,
              difficulty128_from_decimal("4")) == 1);
    server.start();
    CHECK(server.running());
    CHECK(server.bound_port() != 0);

    const int client = connect_loopback(server.bound_port());

    send_all(client,
        R"({"jsonrpc":"2.0","id":1,"method":"eth_submitLogin","params":["miner-3","x"],"worker":"rig-1"})"
        "\n");
    const std::string login_response = read_line(client);
    CHECK(login_response ==
          R"({"jsonrpc":"2.0","id":1,"result":true})");

    const std::string initial_work = read_line(client);
    CHECK(initial_work.find(R"("jsonrpc":"2.0")") != std::string::npos);
    CHECK(initial_work.find(
        "0xffeeddccbbaa9988776655443322110000112233445566778899aabbccddeeff") !=
        std::string::npos);
    CHECK(initial_work.find(
        "0x5555555555555555555555555555555555555555555555555555555555555555") !=
        std::string::npos);
    CHECK(initial_work.find("0x0000000000000000") != std::string::npos);

    send_all(client,
        R"({"jsonrpc":"2.0","id":2,"method":"eth_getWork","params":[]})"
        "\n");
    const std::string get_work = read_line(client);
    CHECK(get_work.find(R"("id":2)") != std::string::npos);
    CHECK(get_work.find(
        "0xffeeddccbbaa9988776655443322110000112233445566778899aabbccddeeff") !=
        std::string::npos);

    send_all(client,
        R"({"jsonrpc":"2.0","id":3,"method":"no_such_method","params":[]})"
        "\n");
    const std::string unknown_method = read_line(client);
    CHECK(unknown_method.find(R"("code":-32601)") != std::string::npos);

    send_all(client, "{not-json}\n");
    const std::string parse_error = read_line(client);
    CHECK(parse_error.find(R"("id":null)") != std::string::npos);
    CHECK(parse_error.find(R"("code":-32700)") != std::string::npos);

    const std::string submit =
        R"({"jsonrpc":"2.0","id":4,"method":"eth_submitWork","params":["0x123456789abcdef0","0xffeeddccbbaa9988776655443322110000112233445566778899aabbccddeeff","0x0000000000000000000000000000000000000000000000000000000000000000"]})"
        "\n";
    send_all(client, submit);
    const std::string submit_response = read_line(client);

#ifdef ZANO_P2POOL_HAVE_PROGPOWZ
    CHECK(submit_response ==
          R"({"jsonrpc":"2.0","id":4,"result":true})");
    CHECK(server.connected_share_count() == 1);
#else
    CHECK(submit_response.find(R"("id":4)") != std::string::npos);
    CHECK(submit_response.find("pow-backend-unavailable") != std::string::npos);
    CHECK(server.connected_share_count() == 0);
#endif

    send_all(client, submit);
    const std::string duplicate = read_line(client);
    CHECK(duplicate.find("duplicate work") != std::string::npos);

    ::shutdown(client, SHUT_RDWR);
    ::close(client);
    server.stop();
    CHECK(!server.running());
    CHECK(server.bound_port() == 0);

    return 0;
}
