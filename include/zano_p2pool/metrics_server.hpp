#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace zano_p2pool {

struct MetricsServerConfig {
    std::string bind_address{"127.0.0.1"};
    std::uint16_t port{37890};
    std::size_t max_request_bytes{8 * 1024};
};

using MetricsSnapshotProvider = std::function<std::string()>;

// Minimal read-only HTTP endpoint for Prometheus-compatible text metrics.
// The server defaults to loopback and intentionally supports one request per
// connection. GET /metrics returns the caller-provided snapshot and GET /healthz
// returns a fixed liveness response. All other paths/methods fail closed.
class MetricsHttpServer {
public:
    explicit MetricsHttpServer(
        MetricsServerConfig config,
        MetricsSnapshotProvider snapshot_provider);
    ~MetricsHttpServer();

    MetricsHttpServer(const MetricsHttpServer&) = delete;
    MetricsHttpServer& operator=(const MetricsHttpServer&) = delete;

    void start();
    void stop() noexcept;

    [[nodiscard]] bool running() const noexcept { return running_.load(); }
    [[nodiscard]] std::uint16_t bound_port() const noexcept {
        return bound_port_.load();
    }
    [[nodiscard]] const MetricsServerConfig& config() const noexcept {
        return config_;
    }

private:
    void accept_loop() noexcept;
    void handle_client(int client_fd) noexcept;

    MetricsServerConfig config_{};
    MetricsSnapshotProvider snapshot_provider_;

    std::atomic<bool> running_{false};
    std::atomic<std::uint16_t> bound_port_{0};
    int listen_fd_{-1};
    std::thread accept_thread_;
};

}  // namespace zano_p2pool
