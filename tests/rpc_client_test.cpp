#include "zano_p2pool/rpc_client.hpp"
#include "test_check.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

class OneShotRpcServer {
public:
    explicit OneShotRpcServer(std::string response_body)
        : response_body_(std::move(response_body)) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) {
            throw std::runtime_error(std::strerror(errno));
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(fd_, 1) != 0) {
            const std::string error = std::strerror(errno);
            ::close(fd_);
            throw std::runtime_error(error);
        }

        socklen_t length = sizeof(address);
        if (::getsockname(
                fd_, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
            const std::string error = std::strerror(errno);
            ::close(fd_);
            throw std::runtime_error(error);
        }
        port_ = ntohs(address.sin_port);
        worker_ = std::thread([this] { serve(); });
    }

    ~OneShotRpcServer() {
        if (worker_.joinable()) {
            worker_.join();
        }
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    [[nodiscard]] std::string url() const {
        return "http://127.0.0.1:" + std::to_string(port_);
    }

private:
    void serve() noexcept {
        const int client = ::accept(fd_, nullptr, nullptr);
        if (client < 0) {
            return;
        }

        std::array<char, 4096> request{};
        static_cast<void>(::recv(client, request.data(), request.size(), 0));

        const std::string response =
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
            "Content-Length: " + std::to_string(response_body_.size()) +
            "\r\nConnection: close\r\n\r\n" + response_body_;
        std::size_t offset = 0;
        while (offset < response.size()) {
            const auto written = ::send(
                client,
                response.data() + offset,
                response.size() - offset,
                0);
            if (written <= 0) {
                break;
            }
            offset += static_cast<std::size_t>(written);
        }
        ::close(client);
    }

    int fd_{-1};
    std::uint16_t port_{};
    std::string response_body_;
    std::thread worker_;
};

zano_p2pool::RpcBlockSubmissionResult submit_with_response(
    const std::string& response) {
    OneShotRpcServer server(response);
    return zano_p2pool::RpcClient(server.url()).submit_block("00");
}

}  // namespace

int main() {
    const std::string parent_hash(64, 'a');
    const auto header_reply = [&](const std::string& height, const std::string& orphan,
                                  const std::string& hash, const std::string& status) {
        return "{\"jsonrpc\":\"2.0\",\"id\":0,\"result\":{\"status\":\"" + status +
            "\",\"block_header\":{\"height\":" + height + ",\"orphan_status\":" + orphan +
            ",\"hash\":\"" + hash + "\"}}}";
    };
    {
        OneShotRpcServer server(header_reply("42", "false", parent_hash, "OK"));
        const auto header = zano_p2pool::RpcClient(server.url()).get_canonical_header(42);
        CHECK(header.height == 42);
        CHECK(header.hash[0] == 0xaa);
    }
    for (const auto& response : {
        header_reply("41", "false", parent_hash, "OK"),
        header_reply("-1", "false", parent_hash, "OK"),
        header_reply("42.0", "false", parent_hash, "OK"),
        header_reply("42", "true", parent_hash, "OK"),
        header_reply("42", "0", parent_hash, "OK"),
        header_reply("42", "false", "a", "OK"),
        header_reply("42", "false", std::string(64, 'z'), "OK"),
        header_reply("42", "false", std::string(64, '0'), "OK"),
        header_reply("42", "false", parent_hash, "BUSY")}) {
        OneShotRpcServer server(response);
        bool failed = false;
        try { static_cast<void>(zano_p2pool::RpcClient(server.url()).get_canonical_header(42)); }
        catch (const std::exception&) { failed = true; }
        CHECK(failed);
    }

    {
        const std::string parent(64, 'b');
        const std::string block_h(64, 'c');
        const std::string block_h1(64, 'd');
        const std::string block_h2(64, 'e');

        const std::string response =
            "{\"jsonrpc\":\"2.0\",\"id\":0,\"result\":{"
            "\"status\":\"OK\",\"blocks\":["
            "{\"height\":99,\"id\":\"" + parent +
            "\",\"prev_id\":\"" + std::string(64, 'a') +
            "\",\"type\":0,\"difficulty\":\"999999999\","
            "\"base_reward\":1000000000000},"
            "{\"height\":100,\"id\":\"" + block_h +
            "\",\"prev_id\":\"" + parent +
            "\",\"type\":0,\"difficulty\":\"888888888\","
            "\"base_reward\":1000000000000},"
            "{\"height\":101,\"id\":\"" + block_h1 +
            "\",\"prev_id\":\"" + block_h +
            "\",\"type\":0,\"difficulty\":\"777777777\","
            "\"base_reward\":1000000000000},"
            "{\"height\":102,\"id\":\"" + block_h2 +
            "\",\"prev_id\":\"" + block_h1 +
            "\",\"type\":1,\"difficulty\":\"4744728\","
            "\"base_reward\":1000000000000}"
            "]}}";

        OneShotRpcServer server(response);

        const auto historical =
            zano_p2pool::RpcClient(server.url())
                .get_historical_pow_context(100, 8);

        CHECK(historical.has_value());
        CHECK(historical->height == 100);
        CHECK(historical->parent_hash[0] == 0xbb);
        CHECK(
            zano_p2pool::difficulty128_to_decimal(
                historical->network_difficulty) ==
            "4744728");
        CHECK(
            historical->block_reward_without_fee ==
            1'000'000'000'000ULL);
        CHECK(historical->confirming_pow_height == 102);
    }

    {
        const std::string parent(64, 'b');
        const std::string wrong_parent(64, 'f');
        const std::string block_h(64, 'c');

        const std::string response =
            "{\"jsonrpc\":\"2.0\",\"id\":0,\"result\":{"
            "\"status\":\"OK\",\"blocks\":["
            "{\"height\":99,\"id\":\"" + parent +
            "\",\"prev_id\":\"" + std::string(64, 'a') +
            "\",\"type\":1,\"difficulty\":\"4700000\","
            "\"base_reward\":1000000000000},"
            "{\"height\":100,\"id\":\"" + block_h +
            "\",\"prev_id\":\"" + wrong_parent +
            "\",\"type\":1,\"difficulty\":\"4744728\","
            "\"base_reward\":1000000000000}"
            "]}}";

        OneShotRpcServer server(response);

        bool failed = false;
        try {
            static_cast<void>(
                zano_p2pool::RpcClient(server.url())
                    .get_historical_pow_context(100, 8));
        } catch (const std::runtime_error&) {
            failed = true;
        }

        CHECK(failed);
    }

    {
        const std::string parent(64, 'b');
        const std::string block_h(64, 'c');

        const std::string response =
            "{\"jsonrpc\":\"2.0\",\"id\":0,\"result\":{"
            "\"status\":\"OK\",\"blocks\":["
            "{\"height\":99,\"id\":\"" + parent +
            "\",\"prev_id\":\"" + std::string(64, 'a') +
            "\",\"type\":1,\"difficulty\":\"4700000\","
            "\"base_reward\":1000000000000},"
            "{\"height\":100,\"id\":\"" + block_h +
            "\",\"prev_id\":\"" + parent +
            "\",\"type\":0,\"difficulty\":\"900000000\","
            "\"base_reward\":1000000000000}"
            "]}}";

        OneShotRpcServer server(response);

        CHECK(
            !zano_p2pool::RpcClient(server.url())
                 .get_historical_pow_context(100, 8)
                 .has_value());
    }

    {
        bool failed = false;
        try {
            static_cast<void>(
                zano_p2pool::RpcClient(
                    "http://127.0.0.1:1")
                    .get_historical_pow_context(0));
        } catch (const std::invalid_argument&) {
            failed = true;
        }
        CHECK(failed);
    }

    using namespace zano_p2pool;

    CHECK(submit_with_response(
              R"({"jsonrpc":"2.0","id":0,"result":{"status":"OK"}})") ==
          RpcBlockSubmissionResult::Accepted);

    CHECK(submit_with_response(
              R"({"jsonrpc":"2.0","id":0,"error":{"code":-13,"message":"Block added as alternative"}})") ==
          RpcBlockSubmissionResult::AlternativeAccepted);

    bool rejected_threw = false;
    try {
        static_cast<void>(submit_with_response(
            R"({"jsonrpc":"2.0","id":0,"error":{"code":-7,"message":"Block rejected"}})"));
    } catch (const RpcError& error) {
        rejected_threw = error.code() == -7 &&
                         error.rpc_message() == "Block rejected";
    }
    CHECK(rejected_threw);

    bool status_threw = false;
    try {
        static_cast<void>(submit_with_response(
            R"({"jsonrpc":"2.0","id":0,"result":{"status":"FAILED"}})"));
    } catch (const std::runtime_error& error) {
        status_threw =
            std::string(error.what()) == "submitblock returned non-OK status: FAILED";
    }
    CHECK(status_threw);

    return 0;
}
