#include "zano_p2pool/rpc_client.hpp"

#include <curl/curl.h>
#include <json-c/json.h>

#include <memory>
#include <algorithm>
#include <charconv>
#include <stdexcept>
#include <string>
#include <utility>

namespace zano_p2pool {
namespace {

using JsonPtr = std::unique_ptr<json_object, decltype(&json_object_put)>;

std::size_t write_callback(
    char* ptr,
    std::size_t size,
    std::size_t nmemb,
    void* userdata) {

    const auto bytes = size * nmemb;
    auto* output = static_cast<std::string*>(userdata);
    output->append(ptr, bytes);
    return bytes;
}

struct CurlHandle {
    CURL* handle{curl_easy_init()};

    CurlHandle() {
        if (!handle) {
            throw std::runtime_error("curl_easy_init failed");
        }
    }

    ~CurlHandle() {
        if (handle) {
            curl_easy_cleanup(handle);
        }
    }

    CurlHandle(const CurlHandle&) = delete;
    CurlHandle& operator=(const CurlHandle&) = delete;
};

struct CurlHeaders {
    curl_slist* list{nullptr};

    ~CurlHeaders() {
        if (list) {
            curl_slist_free_all(list);
        }
    }

    void append(const char* value) {
        auto* updated = curl_slist_append(list, value);
        if (!updated) {
            throw std::runtime_error("curl_slist_append failed");
        }
        list = updated;
    }

    CurlHeaders(const CurlHeaders&) = delete;
    CurlHeaders& operator=(const CurlHeaders&) = delete;
    CurlHeaders() = default;
};

std::string json_to_string(json_object* object) {
    return json_object_to_json_string_ext(object, JSON_C_TO_STRING_PLAIN);
}

JsonPtr parse_json(std::string_view text, const char* context) {
    json_tokener* tokener = json_tokener_new();
    if (!tokener) {
        throw std::runtime_error("json_tokener_new failed");
    }

    json_object* parsed = json_tokener_parse_ex(
        tokener,
        text.data(),
        static_cast<int>(text.size()));
    const auto error = json_tokener_get_error(tokener);
    json_tokener_free(tokener);

    JsonPtr root(parsed, &json_object_put);
    if (error != json_tokener_success || !root) {
        throw std::runtime_error(
            std::string("invalid JSON in ") + context + ": " +
            json_tokener_error_desc(error));
    }

    return root;
}

}  // namespace

RpcError::RpcError(int code, std::string message)
    : std::runtime_error(
          "Zano RPC error " + std::to_string(code) + ": " + message),
      code_(code),
      rpc_message_(std::move(message)) {}

RpcClient::RpcClient(
    std::string rpc_url,
    std::chrono::milliseconds timeout)
    : rpc_url_(std::move(rpc_url)),
      timeout_(timeout) {

    static const int curl_init_result = [] {
        const auto rc = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (rc != CURLE_OK) {
            throw std::runtime_error(
                std::string("curl_global_init failed: ") +
                curl_easy_strerror(rc));
        }
        return 0;
    }();

    (void)curl_init_result;
}

std::string RpcClient::call(
    const std::string& method,
    std::string_view params_json) const {

    auto params = parse_json(params_json, "RPC params");

    JsonPtr request(json_object_new_object(), &json_object_put);
    if (!request) {
        throw std::runtime_error("json_object_new_object failed");
    }

    json_object_object_add(request.get(), "id", json_object_new_int(0));
    json_object_object_add(
        request.get(), "jsonrpc", json_object_new_string("2.0"));
    json_object_object_add(
        request.get(), "method", json_object_new_string(method.c_str()));
    json_object_object_add(request.get(), "params", json_object_get(params.get()));

    const std::string body = json_to_string(request.get());
    std::string response_body;

    CurlHandle curl;
    CurlHeaders headers;
    headers.append("Content-Type: application/json");
    headers.append("Accept: application/json");

    curl_easy_setopt(curl.handle, CURLOPT_URL, rpc_url_.c_str());
    curl_easy_setopt(curl.handle, CURLOPT_POST, 1L);
    curl_easy_setopt(curl.handle, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(
        curl.handle,
        CURLOPT_POSTFIELDSIZE,
        static_cast<long>(body.size()));
    curl_easy_setopt(curl.handle, CURLOPT_HTTPHEADER, headers.list);
    curl_easy_setopt(curl.handle, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl.handle, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(
        curl.handle,
        CURLOPT_TIMEOUT_MS,
        static_cast<long>(timeout_.count()));
    curl_easy_setopt(curl.handle, CURLOPT_NOSIGNAL, 1L);

    const auto rc = curl_easy_perform(curl.handle);
    if (rc != CURLE_OK) {
        throw std::runtime_error(
            std::string("Zano RPC transport error: ") +
            curl_easy_strerror(rc));
    }

    long http_status = 0;
    curl_easy_getinfo(curl.handle, CURLINFO_RESPONSE_CODE, &http_status);
    if (http_status < 200 || http_status >= 300) {
        throw std::runtime_error(
            "Zano RPC HTTP status " + std::to_string(http_status));
    }

    auto response = parse_json(response_body, "Zano RPC response");
    if (json_object_get_type(response.get()) != json_type_object) {
        throw std::runtime_error("Zano RPC response is not a JSON object");
    }

    json_object* error_object = nullptr;
    if (json_object_object_get_ex(response.get(), "error", &error_object) &&
        error_object != nullptr &&
        json_object_get_type(error_object) != json_type_null) {
        if (json_object_get_type(error_object) == json_type_object) {
            json_object* code_object = nullptr;
            json_object* message_object = nullptr;
            if (json_object_object_get_ex(error_object, "code", &code_object) &&
                code_object != nullptr &&
                json_object_get_type(code_object) == json_type_int &&
                json_object_object_get_ex(
                    error_object,
                    "message",
                    &message_object) &&
                message_object != nullptr &&
                json_object_get_type(message_object) == json_type_string) {
                throw RpcError(
                    json_object_get_int(code_object),
                    json_object_get_string(message_object));
            }
        }
        throw std::runtime_error(
            "Zano RPC error: " + json_to_string(error_object));
    }

    json_object* result = nullptr;
    if (!json_object_object_get_ex(response.get(), "result", &result) ||
        result == nullptr) {
        throw std::runtime_error("Zano RPC response has no result");
    }

    return json_to_string(result);
}

BlockTemplate RpcClient::get_block_template(
    const std::string& wallet_address,
    const std::string& extra_text) const {

    if (wallet_address.empty()) {
        throw std::invalid_argument(
            "wallet address is required for getblocktemplate");
    }

    JsonPtr params(json_object_new_object(), &json_object_put);
    if (!params) {
        throw std::runtime_error("json_object_new_object failed");
    }

    json_object_object_add(
        params.get(), "do_explicit_simulation", json_object_new_boolean(false));
    json_object_object_add(
        params.get(), "explicit_transaction", json_object_new_string(""));
    json_object_object_add(
        params.get(), "extra_text", json_object_new_string(extra_text.c_str()));
    json_object_object_add(
        params.get(), "pos_block", json_object_new_boolean(false));
    json_object_object_add(
        params.get(), "stakeholder_address", json_object_new_string(""));
    json_object_object_add(
        params.get(), "wallet_address", json_object_new_string(wallet_address.c_str()));

    return parse_block_template_json(
        call("getblocktemplate", json_to_string(params.get())));
}

RpcCanonicalHeader RpcClient::get_canonical_header(std::uint64_t height) const {
    const auto text = call("getblockheaderbyheight",
        "{\"height\":" + std::to_string(height) + "}");
    auto root = parse_json(text, "historical header result");
    const auto field = [](json_object* object, const char* name, json_type type) {
        json_object* value = nullptr;
        if (!object || json_object_get_type(object) != json_type_object ||
            !json_object_object_get_ex(object, name, &value) || !value ||
            json_object_get_type(value) != type)
            throw std::runtime_error(std::string("invalid historical header field: ") + name);
        return value;
    };
    const auto string = [](json_object* value) {
        return std::string(json_object_get_string(value), json_object_get_string_len(value));
    };
    if (string(field(root.get(), "status", json_type_string)) != "OK")
        throw std::runtime_error("historical header RPC status is not OK");
    auto* header = field(root.get(), "block_header", json_type_object);
    if (json_object_get_boolean(field(header, "orphan_status", json_type_boolean)))
        throw std::runtime_error("historical header is orphaned");
    const std::string encoded_height = json_to_string(field(header, "height", json_type_int));
    std::uint64_t returned_height = 0;
    const auto parsed = std::from_chars(encoded_height.data(),
        encoded_height.data() + encoded_height.size(), returned_height);
    if (parsed.ec != std::errc{} || parsed.ptr != encoded_height.data() + encoded_height.size() ||
        returned_height != height)
        throw std::runtime_error("historical header height mismatch");
    const auto hex = string(field(header, "hash", json_type_string));
    if (hex.size() != 64) throw std::runtime_error("invalid historical header hash length");
    const auto bytes = hex_to_bytes(hex);
    RpcCanonicalHeader result{height, {}};
    std::copy(bytes.begin(), bytes.end(), result.hash.begin());
    if (result.hash == Hash256{}) throw std::runtime_error("zero historical header hash");
    return result;
}

Hash256 stable_canonical_parent_for_work_height(
    std::uint64_t zano_height,
    const std::function<RpcCanonicalHeader(std::uint64_t)>& lookup) {
    if (zano_height == 0) {
        throw std::invalid_argument(
            "canonical-parent audit requires nonzero Zano work height");
    }

    const std::uint64_t parent_height = zano_height - 1;

    const RpcCanonicalHeader first = lookup(parent_height);
    const RpcCanonicalHeader second = lookup(parent_height);

    if (first.height != parent_height ||
        second.height != parent_height) {
        throw std::runtime_error(
            "canonical-parent audit returned the wrong Zano height");
    }

    if (first.hash != second.hash) {
        throw std::runtime_error(
            "canonical Zano parent changed during stable audit");
    }

    return first.hash;
}

bool stable_canonical_checkpoint_matches(
    const ReplayValidationCheckpoint& checkpoint,
    const std::function<RpcCanonicalHeader(std::uint64_t)>& lookup) {

    if (checkpoint.block_hash == Hash256{}) {
        throw std::invalid_argument(
            "replay-validation checkpoint hash must be nonzero");
    }

    const RpcCanonicalHeader first =
        lookup(checkpoint.zano_height);

    const RpcCanonicalHeader second =
        lookup(checkpoint.zano_height);

    if (first.height != checkpoint.zano_height ||
        second.height != checkpoint.zano_height) {
        throw std::runtime_error(
            "replay-validation checkpoint audit returned "
            "the wrong Zano height");
    }

    if (first.hash == Hash256{} ||
        second.hash == Hash256{}) {
        throw std::runtime_error(
            "replay-validation checkpoint audit returned "
            "a zero canonical hash");
    }

    if (first.hash != second.hash) {
        throw std::runtime_error(
            "canonical Zano checkpoint changed during stable audit");
    }

    return first.hash == checkpoint.block_hash;
}

std::optional<RpcHistoricalPowContext>
RpcClient::get_historical_pow_context(
    std::uint64_t height,
    std::size_t max_pow_lookahead) const {

    if (height == 0) {
        throw std::invalid_argument(
            "historical PoW context requires nonzero height");
    }
    if (max_pow_lookahead == 0 || max_pow_lookahead > 4096) {
        throw std::invalid_argument(
            "historical PoW lookahead must be between 1 and 4096");
    }

    JsonPtr params(json_object_new_object(), &json_object_put);
    if (!params) {
        throw std::runtime_error("json_object_new_object failed");
    }

    json_object_object_add(
        params.get(),
        "height_start",
        json_object_new_int64(
            static_cast<std::int64_t>(height - 1)));
    json_object_object_add(
        params.get(),
        "count",
        json_object_new_int64(
            static_cast<std::int64_t>(max_pow_lookahead + 2)));
    json_object_object_add(
        params.get(),
        "ignore_transactions",
        json_object_new_boolean(true));

    const std::string text = call(
        "get_blocks_details",
        json_to_string(params.get()));

    auto root = parse_json(
        text,
        "historical PoW context result");

    const auto field = [](
        json_object* object,
        const char* name,
        json_type type) -> json_object* {

        json_object* value = nullptr;
        if (!object ||
            json_object_get_type(object) != json_type_object ||
            !json_object_object_get_ex(object, name, &value) ||
            !value ||
            json_object_get_type(value) != type) {
            throw std::runtime_error(
                std::string(
                    "invalid historical PoW context field: ") +
                name);
        }
        return value;
    };

    const auto string_value = [](
        json_object* value) -> std::string {
        return std::string(
            json_object_get_string(value),
            json_object_get_string_len(value));
    };

    const auto uint64_value = [](
        json_object* value,
        const char* name) -> std::uint64_t {

        const std::int64_t parsed =
            json_object_get_int64(value);

        if (parsed < 0) {
            throw std::runtime_error(
                std::string(
                    "negative historical PoW context field: ") +
                name);
        }

        return static_cast<std::uint64_t>(parsed);
    };

    const auto hash_value = [&](
        json_object* object,
        const char* name) -> Hash256 {

        const std::string hex = string_value(
            field(object, name, json_type_string));

        if (hex.size() != 64) {
            throw std::runtime_error(
                std::string(
                    "invalid historical PoW context hash length: ") +
                name);
        }

        const auto bytes = hex_to_bytes(hex);
        Hash256 result{};
        std::copy(
            bytes.begin(),
            bytes.end(),
            result.begin());

        if (result == Hash256{}) {
            throw std::runtime_error(
                std::string(
                    "zero historical PoW context hash: ") +
                name);
        }

        return result;
    };

    if (string_value(
            field(root.get(), "status", json_type_string)) !=
        "OK") {
        throw std::runtime_error(
            "historical PoW context RPC status is not OK");
    }

    json_object* blocks =
        field(root.get(), "blocks", json_type_array);

    const std::size_t count =
        json_object_array_length(blocks);

    std::optional<Hash256> parent_id;
    std::optional<Hash256> height_prev_id;
    std::optional<std::uint64_t> base_reward;
    std::optional<Difficulty128> pow_difficulty;
    std::optional<std::uint64_t> pow_height;

    for (std::size_t i = 0; i < count; ++i) {
        json_object* block =
            json_object_array_get_idx(blocks, i);

        if (!block ||
            json_object_get_type(block) != json_type_object) {
            throw std::runtime_error(
                "invalid historical PoW context block entry");
        }

        const std::uint64_t block_height =
            uint64_value(
                field(block, "height", json_type_int),
                "height");

        if (block_height == height - 1) {
            parent_id = hash_value(block, "id");
        }

        if (block_height == height) {
            height_prev_id =
                hash_value(block, "prev_id");

            base_reward =
                uint64_value(
                    field(
                        block,
                        "base_reward",
                        json_type_int),
                    "base_reward");
        }

        if (block_height >= height &&
            !pow_difficulty.has_value()) {

            const std::int64_t type =
                json_object_get_int64(
                    field(block, "type", json_type_int));

            if (type != 0 && type != 1) {
                throw std::runtime_error(
                    "invalid historical PoW context block type");
            }

            if (type == 1) {
                const std::string difficulty =
                    string_value(
                        field(
                            block,
                            "difficulty",
                            json_type_string));

                pow_difficulty =
                    difficulty128_from_decimal(
                        difficulty);
                pow_height = block_height;
            }
        }
    }

    // Near the current tip it is normal for H or the next PoW block not to
    // exist yet. Historical trust must remain deferred rather than guessing.
    if (!parent_id.has_value() ||
        !height_prev_id.has_value() ||
        !base_reward.has_value() ||
        !pow_difficulty.has_value() ||
        !pow_height.has_value()) {
        return std::nullopt;
    }

    if (*parent_id != *height_prev_id) {
        throw std::runtime_error(
            "historical PoW context canonical parent changed");
    }

    return RpcHistoricalPowContext{
        height,
        *parent_id,
        *pow_difficulty,
        *base_reward,
        *pow_height,
    };
}

RpcBlockSubmissionResult RpcClient::submit_block(
    const std::string& block_blob_hex) const {
    if (block_blob_hex.empty()) {
        throw std::invalid_argument("block blob cannot be empty");
    }

    JsonPtr params(json_object_new_array(), &json_object_put);
    if (!params) {
        throw std::runtime_error("json_object_new_array failed");
    }
    json_object_array_add(params.get(), json_object_new_string(block_blob_hex.c_str()));

    std::string result_text;
    try {
        result_text = call("submitblock", json_to_string(params.get()));
    } catch (const RpcError& error) {
        if (error.code() == kZanoRpcErrorBlockAddedAsAlternative) {
            return RpcBlockSubmissionResult::AlternativeAccepted;
        }
        throw;
    }
    auto result = parse_json(result_text, "submitblock result");

    json_object* status = nullptr;
    if (!json_object_object_get_ex(result.get(), "status", &status) ||
        status == nullptr ||
        json_object_get_type(status) != json_type_string) {
        throw std::runtime_error("submitblock result has no string status");
    }

    const std::string status_text = json_object_get_string(status);
    if (status_text != "OK") {
        throw std::runtime_error(
            "submitblock returned non-OK status: " + status_text);
    }

    return RpcBlockSubmissionResult::Accepted;
}

}  // namespace zano_p2pool
