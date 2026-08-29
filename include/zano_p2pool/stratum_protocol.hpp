#pragma once

#include "zano_p2pool/pow_target.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace zano_p2pool {

inline constexpr std::int64_t kStratumErrorDefault = -32000;
inline constexpr std::int64_t kStratumErrorParse = -32700;
inline constexpr std::int64_t kStratumErrorMethodNotFound = -32601;

using StratumId = std::variant<std::monostate, std::int64_t, std::string>;

enum class StratumMethod {
    SubmitLogin,
    GetWork,
    SubmitHashrate,
    SubmitWork,
    Unknown,
};

struct StratumRequest {
    StratumId id{};
    std::string method;
    std::vector<std::string> params;
    std::string worker;
};

struct StratumLogin {
    std::string username;
    std::string password;
    std::string worker;
    std::optional<std::uint64_t> requested_difficulty;
};

struct StratumSubmission {
    std::uint64_t nonce{0};
    Hash256 header_hash{};
    Hash256 mix_hash{};
    std::string worker;
};

struct StratumWork {
    Hash256 header_hash{};
    Hash256 seed_hash{};
    DifficultyTarget share_target{};
    std::uint64_t height{0};
};

[[nodiscard]] StratumRequest parse_stratum_request(std::string_view json);
[[nodiscard]] StratumMethod stratum_method(const StratumRequest& request) noexcept;
[[nodiscard]] StratumLogin parse_stratum_login(const StratumRequest& request);
[[nodiscard]] StratumSubmission parse_stratum_submission(const StratumRequest& request);

[[nodiscard]] std::string stratum_success_json(const StratumId& id);
[[nodiscard]] std::string stratum_error_json(
    const StratumId& id,
    std::int64_t code,
    std::string_view message);
[[nodiscard]] std::string stratum_work_json(
    const StratumId& id,
    const StratumWork& work);
[[nodiscard]] std::string stratum_work_notification_json(const StratumWork& work);

[[nodiscard]] std::string stratum_uint64_hex(std::uint64_t value);

}  // namespace zano_p2pool
