#include "zano_p2pool/stratum_protocol.hpp"

#include "zano_p2pool/crypto_hash.hpp"

#include <json-c/json.h>

#include <charconv>
#include <cctype>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <type_traits>

namespace zano_p2pool {
namespace {

struct JsonObjectDeleter {
    void operator()(json_object* object) const noexcept {
        if (object != nullptr) {
            json_object_put(object);
        }
    }
};

struct JsonTokenerDeleter {
    void operator()(json_tokener* tokener) const noexcept {
        if (tokener != nullptr) {
            json_tokener_free(tokener);
        }
    }
};

using JsonObjectPtr = std::unique_ptr<json_object, JsonObjectDeleter>;
using JsonTokenerPtr = std::unique_ptr<json_tokener, JsonTokenerDeleter>;

[[nodiscard]] std::string_view strip_hex_prefix(std::string_view text) {
    if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        return text.substr(2);
    }
    return text;
}

[[nodiscard]] std::uint64_t parse_hex_u64(std::string_view text) {
    const std::string_view digits = strip_hex_prefix(text);
    if (digits.empty() || digits.size() > 16) {
        throw std::runtime_error("Stratum nonce must contain 1..16 hex digits");
    }

    std::uint64_t value = 0;
    const auto [ptr, ec] = std::from_chars(
        digits.data(), digits.data() + digits.size(), value, 16);
    if (ec != std::errc{} || ptr != digits.data() + digits.size()) {
        throw std::runtime_error("invalid Stratum nonce hex");
    }
    return value;
}

[[nodiscard]] Hash256 parse_hash_hex(std::string_view text, const char* field) {
    const std::string_view digits = strip_hex_prefix(text);
    if (digits.size() != 64) {
        throw std::runtime_error(std::string(field) + " must be exactly 32 bytes");
    }

    const auto bytes = hex_to_bytes(digits);
    Hash256 result{};
    std::copy(bytes.begin(), bytes.end(), result.begin());
    return result;
}

[[nodiscard]] std::optional<std::uint64_t> parse_requested_difficulty(
    std::string& username) {
    const std::size_t delimiter = username.find('-');
    if (delimiter == std::string::npos) {
        return std::nullopt;
    }

    const std::string suffix = username.substr(delimiter + 1);
    if (suffix.empty()) {
        throw std::runtime_error("empty requested Stratum difficulty");
    }

    std::uint64_t value = 0;
    const auto [ptr, ec] = std::from_chars(
        suffix.data(), suffix.data() + suffix.size(), value, 10);
    if (ec != std::errc{} || ptr != suffix.data() + suffix.size()) {
        throw std::runtime_error("invalid requested Stratum difficulty");
    }

    username.resize(delimiter);
    if (username.empty()) {
        throw std::runtime_error("empty Stratum username");
    }

    if (value == 0) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] json_object* json_id(const StratumId& id) {
    return std::visit(
        [](const auto& value) -> json_object* {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                return json_object_new_null();
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                return json_object_new_int64(value);
            } else {
                return json_object_new_string_len(
                    value.data(), static_cast<int>(value.size()));
            }
        },
        id);
}

[[nodiscard]] std::string finalize_json(json_object* root) {
    const char* text = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN);
    if (text == nullptr) {
        throw std::runtime_error("failed to serialize Stratum JSON");
    }
    std::string result(text);
    result.push_back('\n');
    return result;
}

[[nodiscard]] JsonObjectPtr make_response_root(const StratumId& id) {
    JsonObjectPtr root(json_object_new_object());
    if (!root) {
        throw std::bad_alloc();
    }
    json_object_object_add(root.get(), "jsonrpc", json_object_new_string("2.0"));
    json_object_object_add(root.get(), "id", json_id(id));
    return root;
}

[[nodiscard]] json_object* make_work_array(const StratumWork& work) {
    json_object* array = json_object_new_array();
    if (array == nullptr) {
        throw std::bad_alloc();
    }

    const std::string header = "0x" + hash_to_hex(work.header_hash);
    const std::string seed = "0x" + hash_to_hex(work.seed_hash);
    const std::string target = "0x" + work.share_target.hex();
    const std::string height = stratum_uint64_hex(work.height);

    json_object_array_add(array, json_object_new_string(header.c_str()));
    json_object_array_add(array, json_object_new_string(seed.c_str()));
    json_object_array_add(array, json_object_new_string(target.c_str()));
    json_object_array_add(array, json_object_new_string(height.c_str()));
    return array;
}

}  // namespace

StratumRequest parse_stratum_request(std::string_view json) {
    JsonTokenerPtr tokener(json_tokener_new());
    if (!tokener) {
        throw std::bad_alloc();
    }

    JsonObjectPtr root(json_tokener_parse_ex(
        tokener.get(), json.data(), static_cast<int>(json.size())));
    if (!root || json_tokener_get_error(tokener.get()) != json_tokener_success) {
        throw std::runtime_error("invalid Stratum JSON");
    }

    std::size_t parse_end = json_tokener_get_parse_end(tokener.get());
    while (parse_end < json.size() &&
           std::isspace(static_cast<unsigned char>(json[parse_end])) != 0) {
        ++parse_end;
    }
    if (parse_end != json.size()) {
        throw std::runtime_error("trailing data after Stratum JSON object");
    }

    if (!json_object_is_type(root.get(), json_type_object)) {
        throw std::runtime_error("Stratum request must be a JSON object");
    }

    StratumRequest request;

    json_object* version = nullptr;
    if (json_object_object_get_ex(root.get(), "jsonrpc", &version)) {
        if (!json_object_is_type(version, json_type_string) ||
            std::string_view(json_object_get_string(version)) != "2.0") {
            throw std::runtime_error("unsupported JSON-RPC version");
        }
    }

    json_object* id = nullptr;
    if (json_object_object_get_ex(root.get(), "id", &id)) {
        if (json_object_is_type(id, json_type_null)) {
            request.id = std::monostate{};
        } else if (json_object_is_type(id, json_type_int)) {
            request.id = json_object_get_int64(id);
        } else if (json_object_is_type(id, json_type_string)) {
            request.id = std::string(json_object_get_string(id));
        } else {
            throw std::runtime_error("unsupported Stratum request id type");
        }
    }

    json_object* method = nullptr;
    if (!json_object_object_get_ex(root.get(), "method", &method) ||
        !json_object_is_type(method, json_type_string)) {
        throw std::runtime_error("Stratum request is missing a string method");
    }
    request.method = json_object_get_string(method);

    json_object* worker = nullptr;
    if (json_object_object_get_ex(root.get(), "worker", &worker)) {
        if (!json_object_is_type(worker, json_type_string)) {
            throw std::runtime_error("Stratum worker must be a string");
        }
        request.worker = json_object_get_string(worker);
    }

    json_object* params = nullptr;
    if (json_object_object_get_ex(root.get(), "params", &params)) {
        if (!json_object_is_type(params, json_type_array)) {
            throw std::runtime_error("Stratum params must be an array");
        }
        const std::size_t count = json_object_array_length(params);
        request.params.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            json_object* value = json_object_array_get_idx(params, i);
            if (value == nullptr || !json_object_is_type(value, json_type_string)) {
                throw std::runtime_error("Stratum params must contain strings only");
            }
            request.params.emplace_back(json_object_get_string(value));
        }
    }

    return request;
}

StratumMethod stratum_method(const StratumRequest& request) noexcept {
    if (request.method == "eth_submitLogin") {
        return StratumMethod::SubmitLogin;
    }
    if (request.method == "eth_getWork") {
        return StratumMethod::GetWork;
    }
    if (request.method == "eth_submitHashrate") {
        return StratumMethod::SubmitHashrate;
    }
    if (request.method == "eth_submitWork") {
        return StratumMethod::SubmitWork;
    }
    return StratumMethod::Unknown;
}

StratumLogin parse_stratum_login(const StratumRequest& request) {
    if (stratum_method(request) != StratumMethod::SubmitLogin) {
        throw std::runtime_error("request is not eth_submitLogin");
    }
    if (request.params.empty() || request.params.size() > 2) {
        throw std::runtime_error("eth_submitLogin expects username and optional password");
    }

    StratumLogin login;
    login.username = request.params[0];
    login.password = request.params.size() == 2 ? request.params[1] : std::string{};
    login.worker = request.worker;
    login.requested_difficulty = parse_requested_difficulty(login.username);
    return login;
}

StratumSubmission parse_stratum_submission(const StratumRequest& request) {
    if (stratum_method(request) != StratumMethod::SubmitWork) {
        throw std::runtime_error("request is not eth_submitWork");
    }
    if (request.params.size() != 3) {
        throw std::runtime_error("eth_submitWork expects nonce, header, and mix hash");
    }

    StratumSubmission submission;
    submission.nonce = parse_hex_u64(request.params[0]);
    submission.header_hash = parse_hash_hex(request.params[1], "Stratum header hash");
    submission.mix_hash = parse_hash_hex(request.params[2], "Stratum mix hash");
    submission.worker = request.worker;
    return submission;
}

std::string stratum_success_json(const StratumId& id) {
    JsonObjectPtr root = make_response_root(id);
    json_object_object_add(root.get(), "result", json_object_new_boolean(1));
    return finalize_json(root.get());
}

std::string stratum_error_json(
    const StratumId& id,
    std::int64_t code,
    std::string_view message) {
    JsonObjectPtr root = make_response_root(id);
    json_object* error = json_object_new_object();
    if (error == nullptr) {
        throw std::bad_alloc();
    }
    json_object_object_add(error, "code", json_object_new_int64(code));
    json_object_object_add(
        error,
        "message",
        json_object_new_string_len(message.data(), static_cast<int>(message.size())));
    json_object_object_add(root.get(), "error", error);
    return finalize_json(root.get());
}

std::string stratum_work_json(const StratumId& id, const StratumWork& work) {
    JsonObjectPtr root = make_response_root(id);
    json_object_object_add(root.get(), "result", make_work_array(work));
    return finalize_json(root.get());
}

std::string stratum_work_notification_json(const StratumWork& work) {
    JsonObjectPtr root(json_object_new_object());
    if (!root) {
        throw std::bad_alloc();
    }
    json_object_object_add(root.get(), "jsonrpc", json_object_new_string("2.0"));
    json_object_object_add(root.get(), "result", make_work_array(work));
    return finalize_json(root.get());
}

std::string stratum_uint64_hex(std::uint64_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << std::nouppercase << std::setfill('0')
        << std::setw(16) << value;
    return out.str();
}

}  // namespace zano_p2pool
