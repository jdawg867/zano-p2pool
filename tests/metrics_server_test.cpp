#include "zano_p2pool/metrics_server.hpp"
#include "zano_p2pool/metrics_snapshot.hpp"
#include "test_check.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

bool send_all(int fd, std::string_view data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const ssize_t sent = ::send(
            fd,
            data.data() + static_cast<std::ptrdiff_t>(offset),
            data.size() - offset,
            0);
        if (sent <= 0) {
            return false;
        }
        offset += static_cast<std::size_t>(sent);
    }
    return true;
}

std::string request(std::uint16_t port, std::string_view payload) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    CHECK(::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
    CHECK(::connect(
              fd,
              reinterpret_cast<const sockaddr*>(&address),
              sizeof(address)) == 0);
    CHECK(send_all(fd, payload));
    static_cast<void>(::shutdown(fd, SHUT_WR));

    std::string response;
    std::array<char, 1024> buffer{};
    while (true) {
        const ssize_t received = ::recv(fd, buffer.data(), buffer.size(), 0);
        if (received <= 0) {
            break;
        }
        response.append(buffer.data(), static_cast<std::size_t>(received));
    }
    ::close(fd);
    return response;
}

template <typename Exception, typename Fn>
void expect_throw(Fn&& fn) {
    bool threw = false;
    try {
        fn();
    } catch (const Exception&) {
        threw = true;
    }
    CHECK(threw);
}

}  // namespace

int main() {
    using namespace zano_p2pool;

    MetricsSnapshot snapshot;
    snapshot.zano_height = 42;
    snapshot.sidechain_connected_shares = 7;
    snapshot.sidechain_orphan_shares = 2;
    snapshot.sidechain_tip_height = 6;
    snapshot.p2p_peers = 3;
    snapshot.p2p_trusted_work_contexts = 4;
    snapshot.stratum_connections = 5;
    snapshot.stratum_template_version = 9;
    snapshot.stratum_accepted_shares_total = 11;
    snapshot.p2p_admitted_shares_total = 12;
    snapshot.block_candidates_total = 13;
    snapshot.blocks_submitted_total = 14;
    snapshot.block_submission_failures_total = 15;
    snapshot.template_refresh_failures_total = 16;
    snapshot.persistence_ok = false;

    const std::string rendered = render_prometheus_metrics(snapshot);
    CHECK(rendered.find("zano_p2pool_up 1\n") != std::string::npos);
    CHECK(rendered.find("zano_p2pool_zano_height 42\n") != std::string::npos);
    CHECK(rendered.find("zano_p2pool_sidechain_connected_shares 7\n") !=
          std::string::npos);
    CHECK(rendered.find("zano_p2pool_sidechain_orphan_shares 2\n") !=
          std::string::npos);
    CHECK(rendered.find("zano_p2pool_sidechain_tip_height 6\n") !=
          std::string::npos);
    CHECK(rendered.find("zano_p2pool_p2p_peers 3\n") != std::string::npos);
    CHECK(rendered.find("zano_p2pool_p2p_trusted_work_contexts 4\n") !=
          std::string::npos);
    CHECK(rendered.find("zano_p2pool_stratum_connections 5\n") !=
          std::string::npos);
    CHECK(rendered.find("zano_p2pool_stratum_template_version 9\n") !=
          std::string::npos);
    CHECK(rendered.find("zano_p2pool_stratum_accepted_shares_total 11\n") !=
          std::string::npos);
    CHECK(rendered.find("zano_p2pool_p2p_admitted_shares_total 12\n") !=
          std::string::npos);
    CHECK(rendered.find("zano_p2pool_block_candidates_total 13\n") !=
          std::string::npos);
    CHECK(rendered.find("zano_p2pool_blocks_submitted_total 14\n") !=
          std::string::npos);
    CHECK(rendered.find("zano_p2pool_block_submission_failures_total 15\n") !=
          std::string::npos);
    CHECK(rendered.find("zano_p2pool_template_refresh_failures_total 16\n") !=
          std::string::npos);
    CHECK(rendered.find("zano_p2pool_persistence_ok 0\n") !=
          std::string::npos);

    std::atomic<std::uint64_t> snapshots{0};
    MetricsServerConfig config;
    config.bind_address = "127.0.0.1";
    config.port = 0;
    config.max_request_bytes = 256;

    MetricsHttpServer server(config, [&] {
        snapshots.fetch_add(1);
        return render_prometheus_metrics(snapshot);
    });
    server.start();
    CHECK(server.running());
    CHECK(server.bound_port() != 0);

    const std::string metrics = request(
        server.bound_port(),
        "GET /metrics HTTP/1.1\r\nHost: localhost\r\n\r\n");
    CHECK(metrics.find("HTTP/1.1 200 OK\r\n") == 0);
    CHECK(metrics.find("Content-Type: text/plain; version=0.0.4; charset=utf-8") !=
          std::string::npos);
    CHECK(metrics.find("zano_p2pool_up 1\n") != std::string::npos);
    CHECK(metrics.find("zano_p2pool_sidechain_connected_shares 7\n") !=
          std::string::npos);
    CHECK(snapshots.load() == 1);

    const std::string health = request(
        server.bound_port(),
        "GET /healthz HTTP/1.0\r\n\r\n");
    CHECK(health.find("HTTP/1.1 200 OK\r\n") == 0);
    CHECK(health.ends_with("\r\n\r\nok\n"));
    CHECK(snapshots.load() == 1);

    const std::string missing = request(
        server.bound_port(),
        "GET /missing HTTP/1.1\r\nHost: localhost\r\n\r\n");
    CHECK(missing.find("HTTP/1.1 404 Not Found\r\n") == 0);

    const std::string method = request(
        server.bound_port(),
        "POST /metrics HTTP/1.1\r\nHost: localhost\r\n\r\n");
    CHECK(method.find("HTTP/1.1 405 Method Not Allowed\r\n") == 0);
    CHECK(method.find("Allow: GET\r\n") != std::string::npos);

    std::string oversized = "GET /metrics HTTP/1.1\r\nX-Test: ";
    oversized.append(300, 'a');
    oversized += "\r\n\r\n";
    const std::string too_large = request(server.bound_port(), oversized);
    CHECK(too_large.find("HTTP/1.1 413 Payload Too Large\r\n") == 0);

    server.stop();
    CHECK(!server.running());
    CHECK(server.bound_port() == 0);

    MetricsServerConfig failure_config;
    failure_config.port = 0;
    MetricsHttpServer failing(failure_config, []() -> std::string {
        throw std::runtime_error("snapshot failed");
    });
    failing.start();
    const std::string failure = request(
        failing.bound_port(),
        "GET /metrics HTTP/1.1\r\nHost: localhost\r\n\r\n");
    CHECK(failure.find("HTTP/1.1 500 Internal Server Error\r\n") == 0);
    failing.stop();

    MetricsServerConfig zero_max;
    zero_max.max_request_bytes = 0;
    expect_throw<std::runtime_error>([&] {
        MetricsHttpServer invalid(zero_max, [] { return std::string{}; });
        static_cast<void>(invalid);
    });

    expect_throw<std::runtime_error>([] {
        MetricsHttpServer invalid(MetricsServerConfig{}, {});
        static_cast<void>(invalid);
    });

    return 0;
}
