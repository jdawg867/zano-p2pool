#include "zano_p2pool/crypto_hash.hpp"
#include "zano_p2pool/pow_target.hpp"
#include "zano_p2pool/stratum_protocol.hpp"
#include "test_check.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>

namespace {

template <typename Fn>
void expect_runtime_error(Fn&& fn) {
    bool threw = false;
    try {
        fn();
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
}

zano_p2pool::Hash256 hash_from_hex(std::string_view hex) {
    const auto bytes = zano_p2pool::hex_to_bytes(hex);
    CHECK(bytes.size() == 32);
    zano_p2pool::Hash256 hash{};
    for (std::size_t i = 0; i < hash.size(); ++i) {
        hash[i] = bytes[i];
    }
    return hash;
}

}  // namespace

int main() {
    using namespace zano_p2pool;

    const auto login_request = parse_stratum_request(
        R"({"id":1,"jsonrpc":"2.0","method":"eth_submitLogin","worker":"p2pool-probe","params":["miner","x"]})");
    CHECK(stratum_method(login_request) == StratumMethod::SubmitLogin);
    CHECK(std::get<std::int64_t>(login_request.id) == 1);
    const auto login = parse_stratum_login(login_request);
    CHECK(login.username == "miner");
    CHECK(login.password == "x");
    CHECK(login.worker == "p2pool-probe");
    CHECK(!login.requested_difficulty.has_value());

    const auto login_with_diff = parse_stratum_login(parse_stratum_request(
        R"({"id":"abc","jsonrpc":"2.0","method":"eth_submitLogin","params":["miner-250000000","pw"]})"));
    CHECK(login_with_diff.username == "miner");
    CHECK(login_with_diff.password == "pw");
    CHECK(login_with_diff.requested_difficulty.has_value());
    CHECK(*login_with_diff.requested_difficulty == UINT64_C(250000000));

    const auto get_work = parse_stratum_request(
        R"({"worker":"","jsonrpc":"2.0","params":[],"id":3,"method":"eth_getWork"})");
    CHECK(stratum_method(get_work) == StratumMethod::GetWork);
    CHECK(get_work.params.empty());

    const auto submission_request = parse_stratum_request(
        R"({"id":10,"jsonrpc":"2.0","method":"eth_submitWork","worker":"rig-1","params":["0x899624c0078e824f","0xb98617aec14a2872fb45ab755f6273a1ae719d0ff0d441a25bb8235d82cb4123","0x30f599f39276df17656727f16c3230c072dd8f2dd780161625479d352e8b2a97"]})");
    CHECK(stratum_method(submission_request) == StratumMethod::SubmitWork);
    const auto submission = parse_stratum_submission(submission_request);
    CHECK(submission.nonce == UINT64_C(0x899624c0078e824f));
    CHECK(hash_to_hex(submission.header_hash) ==
          "b98617aec14a2872fb45ab755f6273a1ae719d0ff0d441a25bb8235d82cb4123");
    CHECK(hash_to_hex(submission.mix_hash) ==
          "30f599f39276df17656727f16c3230c072dd8f2dd780161625479d352e8b2a97");
    CHECK(submission.worker == "rig-1");

    // Captured from current Zano testnet Stratum at height 165006. The target
    // is exactly difficulty 100,000,000, Zano's current default vdiff floor.
    StratumWork work;
    work.header_hash = hash_from_hex(
        "fa31238872c1e7a6efefcbd2e0d838970f99457adb4e4203766210bac18e625e");
    work.seed_hash = hash_from_hex(
        "f2e59013a0a379837166b59f871b20a8a0d101d1c355ea85d35329360e69c000");
    work.share_target = difficulty_to_target("100000000");
    work.height = 165006;

    CHECK(work.share_target.hex() ==
          "0000002af31dc4611873bf3f70834acdae9f0f4f534f5d60585a5f1c1a3ced1b");
    CHECK(stratum_uint64_hex(work.height) == "0x000000000002848e");

    const std::string expected_work =
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":["
        "\"0xfa31238872c1e7a6efefcbd2e0d838970f99457adb4e4203766210bac18e625e\","
        "\"0xf2e59013a0a379837166b59f871b20a8a0d101d1c355ea85d35329360e69c000\","
        "\"0x0000002af31dc4611873bf3f70834acdae9f0f4f534f5d60585a5f1c1a3ced1b\","
        "\"0x000000000002848e\"]}\n";
    CHECK(stratum_work_json(StratumId{std::int64_t{2}}, work) == expected_work);

    const std::string expected_notification =
        "{\"jsonrpc\":\"2.0\",\"result\":["
        "\"0xfa31238872c1e7a6efefcbd2e0d838970f99457adb4e4203766210bac18e625e\","
        "\"0xf2e59013a0a379837166b59f871b20a8a0d101d1c355ea85d35329360e69c000\","
        "\"0x0000002af31dc4611873bf3f70834acdae9f0f4f534f5d60585a5f1c1a3ced1b\","
        "\"0x000000000002848e\"]}\n";
    CHECK(stratum_work_notification_json(work) == expected_notification);

    CHECK(stratum_success_json(StratumId{std::int64_t{1}}) ==
          "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":true}\n");
    CHECK(stratum_error_json(
              StratumId{std::string{"req-7"}},
              kStratumErrorMethodNotFound,
              "unknown method") ==
          "{\"jsonrpc\":\"2.0\",\"id\":\"req-7\",\"error\":{""
          "\"code\":-32601,\"message\":\"unknown method\"}}\n");

    const auto unknown = parse_stratum_request(
        R"({"id":11,"jsonrpc":"2.0","method":"no_such_method","params":[]})");
    CHECK(stratum_method(unknown) == StratumMethod::Unknown);

    expect_runtime_error([] {
        (void)parse_stratum_request("{not-json}");
    });
    expect_runtime_error([] {
        (void)parse_stratum_request(
            R"({"jsonrpc":"1.0","method":"eth_getWork","params":[]})");
    });
    expect_runtime_error([] {
        (void)parse_stratum_request(
            R"({"jsonrpc":"2.0","method":"eth_getWork","params":[1]})");
    });
    expect_runtime_error([] {
        (void)parse_stratum_login(parse_stratum_request(
            R"({"jsonrpc":"2.0","method":"eth_submitLogin","params":[]})"));
    });
    expect_runtime_error([] {
        (void)parse_stratum_submission(parse_stratum_request(
            R"({"jsonrpc":"2.0","method":"eth_submitWork","params":["0x10000000000000000","0xb98617aec14a2872fb45ab755f6273a1ae719d0ff0d441a25bb8235d82cb4123","0x30f599f39276df17656727f16c3230c072dd8f2dd780161625479d352e8b2a97"]})"));
    });
    expect_runtime_error([] {
        (void)parse_stratum_submission(parse_stratum_request(
            R"({"jsonrpc":"2.0","method":"eth_submitWork","params":["0x1","0x1234","0x30f599f39276df17656727f16c3230c072dd8f2dd780161625479d352e8b2a97"]})"));
    });

    return 0;
}
