#include "zano_p2pool/block_template.hpp"

#include <json-c/json.h>

#include <cassert>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

#ifndef ZANO_P2POOL_SOURCE_DIR
#error "ZANO_P2POOL_SOURCE_DIR must be defined"
#endif

namespace {

json_object* require_field(json_object* object, const char* name) {
    json_object* value = nullptr;
    if (!json_object_object_get_ex(object, name, &value) || value == nullptr) {
        throw std::runtime_error(std::string("fixture missing field: ") + name);
    }
    return value;
}

std::string require_string(json_object* object, const char* name) {
    json_object* value = require_field(object, name);
    if (json_object_get_type(value) != json_type_string) {
        throw std::runtime_error(std::string("fixture field is not a string: ") + name);
    }
    return json_object_get_string(value);
}

std::uint64_t require_uint64(json_object* object, const char* name) {
    json_object* value = require_field(object, name);
    if (json_object_get_type(value) != json_type_int) {
        throw std::runtime_error(std::string("fixture field is not an integer: ") + name);
    }
    return json_object_get_uint64(value);
}

}  // namespace

int main() {
    const std::string fixture_path =
        std::string(ZANO_P2POOL_SOURCE_DIR) +
        "/tests/fixtures/zano_testnet_template_164895.json";

    std::ifstream input(fixture_path);
    if (!input) {
        throw std::runtime_error("failed to open testnet fixture: " + fixture_path);
    }

    const std::string fixture_text(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());

    json_object* fixture = json_tokener_parse(fixture_text.c_str());
    if (!fixture) {
        throw std::runtime_error("failed to parse testnet fixture JSON");
    }

    const auto height = require_uint64(fixture, "height");
    const auto difficulty = require_string(fixture, "difficulty");
    const auto block_reward = require_uint64(fixture, "block_reward");
    const auto block_reward_without_fee =
        require_uint64(fixture, "block_reward_without_fee");
    const auto txs_fee = require_uint64(fixture, "txs_fee");
    const auto prev_hash = require_string(fixture, "prev_hash");
    const auto seed = require_string(fixture, "seed");
    const auto blob_bytes = require_uint64(fixture, "blob_bytes");
    const auto status = require_string(fixture, "status");

    json_object* parser_input = json_object_new_object();
    json_object_object_add(
        parser_input, "block_reward", json_object_new_uint64(block_reward));
    json_object_object_add(
        parser_input,
        "block_reward_without_fee",
        json_object_new_uint64(block_reward_without_fee));

    const std::string sanitized_blob(blob_bytes * 2, '0');
    json_object_object_add(
        parser_input,
        "blocktemplate_blob",
        json_object_new_string(sanitized_blob.c_str()));
    json_object_object_add(
        parser_input, "difficulty", json_object_new_string(difficulty.c_str()));
    json_object_object_add(
        parser_input, "height", json_object_new_uint64(height));
    json_object_object_add(
        parser_input, "prev_hash", json_object_new_string(prev_hash.c_str()));
    json_object_object_add(
        parser_input, "seed", json_object_new_string(seed.c_str()));
    json_object_object_add(
        parser_input, "status", json_object_new_string(status.c_str()));
    json_object_object_add(
        parser_input, "txs_fee", json_object_new_uint64(txs_fee));

    const std::string parser_json =
        json_object_to_json_string_ext(parser_input, JSON_C_TO_STRING_PLAIN);

    const auto block = zano_p2pool::parse_block_template_json(parser_json);

    assert(block.height == 164895ULL);
    assert(block.difficulty == "1179735");
    assert(block.block_reward == 1000000000000ULL);
    assert(block.block_reward_without_fee == 1000000000000ULL);
    assert(block.txs_fee == 0ULL);
    assert(block.prev_hash ==
           "db88f5755e69f8c86cf4a279e2b2998fe52d295d937ad44b593753fae695ec41");
    assert(block.seed ==
           "f2e59013a0a379837166b59f871b20a8a0d101d1c355ea85d35329360e69c000");
    assert(block.blob_bytes() == 1388ULL);
    assert(block.status == "OK");

    json_object_put(parser_input);
    json_object_put(fixture);

    return 0;
}
