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
