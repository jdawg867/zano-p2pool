#include "zano_p2pool/rpc_client.hpp"

#include <curl/curl.h>
#include <json-c/json.h>

#include <memory>
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
