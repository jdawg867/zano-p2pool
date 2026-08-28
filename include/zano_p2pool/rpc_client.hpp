#pragma once

#include <chrono>
#include <string>
#include <string_view>

#include "zano_p2pool/block_template.hpp"

namespace zano_p2pool {

class RpcClient {
public:
    explicit RpcClient(
        std::string rpc_url,
        std::chrono::milliseconds timeout = std::chrono::seconds(15));

    [[nodiscard]] BlockTemplate get_block_template(
        const std::string& wallet_address,
        const std::string& extra_text = {}) const;

    void submit_block(const std::string& block_blob_hex) const;

private:
    [[nodiscard]] std::string call(
        const std::string& method,
        std::string_view params_json) const;

    std::string rpc_url_;
    std::chrono::milliseconds timeout_;
};

}  // namespace zano_p2pool
