#include "zano_p2pool/block_template.hpp"

#include <json-c/json.h>

#include <memory>
#include <stdexcept>
#include <string>

namespace zano_p2pool {
namespace {

using JsonPtr = std::unique_ptr<json_object, decltype(&json_object_put)>;

json_object* required_field(json_object* object, const char* name) {
    json_object* value = nullptr;
    if (!json_object_object_get_ex(object, name, &value) || value == nullptr) {
        throw std::runtime_error(std::string("missing JSON field: ") + name);
    }
    return value;
}

std::string required_string(json_object* object, const char* name) {
    auto* value = required_field(object, name);
    if (json_object_get_type(value) != json_type_string) {
        throw std::runtime_error(std::string("JSON field is not a string: ") + name);
    }
    return json_object_get_string(value);
}

std::string required_integer_string(json_object* object, const char* name) {
    auto* value = required_field(object, name);
    const auto type = json_object_get_type(value);

    if (type == json_type_string) {
        return json_object_get_string(value);
    }
    if (type == json_type_int) {
        return std::to_string(json_object_get_uint64(value));
    }

    throw std::runtime_error(
        std::string("JSON field is not an integer/string: ") + name);
}

std::uint64_t required_u64(json_object* object, const char* name) {
    auto* value = required_field(object, name);
    const auto type = json_object_get_type(value);

    if (type == json_type_int) {
        return json_object_get_uint64(value);
    }
    if (type == json_type_string) {
        try {
            return std::stoull(json_object_get_string(value));
        } catch (const std::exception&) {
            throw std::runtime_error(
                std::string("invalid uint64 JSON field: ") + name);
        }
    }

    throw std::runtime_error(std::string("JSON field is not uint64: ") + name);
}

}  // namespace

std::size_t BlockTemplate::blob_bytes() const {
    return blocktemplate_blob.size() / 2;
}

BlockTemplate parse_block_template_json(std::string_view json_text) {
    json_tokener* tokener = json_tokener_new();
    if (!tokener) {
        throw std::runtime_error("json_tokener_new failed");
    }

    json_object* parsed = json_tokener_parse_ex(
        tokener,
        json_text.data(),
        static_cast<int>(json_text.size()));
    const auto error = json_tokener_get_error(tokener);
    json_tokener_free(tokener);

    JsonPtr root(parsed, &json_object_put);
    if (error != json_tokener_success || !root) {
        throw std::runtime_error(
            std::string("invalid block template JSON: ") +
            json_tokener_error_desc(error));
    }

    if (json_object_get_type(root.get()) != json_type_object) {
        throw std::runtime_error("block template result is not a JSON object");
    }

    BlockTemplate out;
    out.block_reward = required_u64(root.get(), "block_reward");
    out.block_reward_without_fee =
        required_u64(root.get(), "block_reward_without_fee");
    out.blocktemplate_blob = required_string(root.get(), "blocktemplate_blob");
    out.difficulty = required_integer_string(root.get(), "difficulty");
    out.height = required_u64(root.get(), "height");
    out.prev_hash = required_string(root.get(), "prev_hash");
    out.seed = required_string(root.get(), "seed");
    out.status = required_string(root.get(), "status");
    out.txs_fee = required_u64(root.get(), "txs_fee");

    if (out.status != "OK") {
        throw std::runtime_error(
            "getblocktemplate returned non-OK status: " + out.status);
    }

    if (out.blocktemplate_blob.empty() ||
        (out.blocktemplate_blob.size() % 2) != 0) {
        throw std::runtime_error("invalid blocktemplate_blob hex length");
    }

    if (out.prev_hash.size() != 64) {
        throw std::runtime_error("invalid prev_hash length");
    }

    if (out.seed.size() != 64) {
        throw std::runtime_error("invalid ProgPoWZ seed length");
    }

    return out;
}

}  // namespace zano_p2pool
