#include "zano_p2pool/crypto_hash.hpp"
#include "zano_p2pool/share.hpp"
#include "zano_p2pool/stratum_protocol.hpp"
#include "zano_p2pool/stratum_session.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

bool check(bool condition, const char* expression, int line) {
    if (condition) {
        return true;
    }
    std::cerr << "CHECK failed at line " << line << ": " << expression << '\n';
    return false;
}

#define CHECK(expr) do { if (!check((expr), #expr, __LINE__)) return 1; } while (false)

template <typename Fn>
void expect_runtime_error(Fn&& fn) {
    try {
        fn();
    } catch (const std::runtime_error&) {
        return;
    }
    throw std::runtime_error("expected std::runtime_error");
}

zano_p2pool::Hash256 hash_from_hex(std::string_view hex) {
    const auto bytes = zano_p2pool::hex_to_bytes(hex);
    zano_p2pool::Hash256 result{};
    std::copy(bytes.begin(), bytes.end(), result.begin());
    return result;
}

}  // namespace

int main() {
    using namespace zano_p2pool;

    StratumSessionConfig config;
    config.default_share_difficulty = 100000000;
    config.minimum_share_difficulty = 1000000;
    config.maximum_share_difficulty = 500000000;
    StratumSessionRegistry registry(config);

    const std::uint64_t session1 = registry.create_session();
    const std::uint64_t session2 = registry.create_session();
    CHECK(session1 == 1);
    CHECK(session2 == 2);

    const auto login_request = parse_stratum_request(
        R"({"worker":"rig-a","jsonrpc":"2.0","params":["wallet-200000000","x"],"id":1,"method":"eth_submitLogin"})");
    const auto login = parse_stratum_login(login_request);
    registry.login(session1, login);

    const auto* s1 = registry.find_session(session1);
    CHECK(s1 != nullptr);
    CHECK(s1->logged_in);
    CHECK(s1->username == "wallet");
    CHECK(s1->worker == "rig-a");
    CHECK(s1->configured_share_difficulty == 200000000);
    CHECK(stratum_success_json(login_request.id) ==
          "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":true}\n");

    // A missing worker name is assigned deterministically from the session id.
    StratumLogin default_login;
    default_login.username = "wallet-two";
    registry.login(session2, default_login);
    const auto* s2 = registry.find_session(session2);
    CHECK(s2 != nullptr);
    CHECK(s2->worker == "2");
    CHECK(s2->configured_share_difficulty == config.default_share_difficulty);

    const Hash256 header1 = hash_from_hex(
        "fa31238872c1e7a6efefcbd2e0d838970f99457adb4e4203766210bac18e625e");
    const Hash256 seed1 = hash_from_hex(
        "f2e59013a0a379837166b59f871b20a8a0d101d1c355ea85d35329360e69c000");
    const Difficulty128 network1 = difficulty128_from_decimal("1000000000");

    CHECK(registry.publish_template(header1, seed1, 165006, network1) == 1);
    CHECK(registry.publish_template(header1, seed1, 165006, network1) == 1);
    CHECK(registry.current_template() != nullptr);
    CHECK(registry.current_template()->version == 1);

    const auto work1 = registry.issue_work(session1);
    CHECK(work1.job_version == 1);
    CHECK(work1.session_id == session1);
    CHECK(difficulty128_to_decimal(work1.share_difficulty) == "200000000");
    CHECK(work1.wire_work.header_hash == header1);
    CHECK(work1.wire_work.seed_hash == seed1);
    CHECK(work1.wire_work.height == 165006);
    CHECK(work1.wire_work.share_target.hex() ==
          difficulty_to_target("200000000").hex());
    CHECK(work1.trusted_context.zano_height == 165006);
    CHECK(work1.trusted_context.mining_header_hash == header1);
    CHECK(work1.trusted_context.network_difficulty == network1);
    CHECK(registry.match_submission(session1, header1) ==
          StratumWorkMatch::Current);

    // Consensus sidechain mode overrides a miner's requested/configured vardiff
    // exactly. The wire target and recorded share difficulty must be identical
    // to the branch-derived value supplied by the node.
    const Difficulty128 consensus_override =
        difficulty128_from_decimal("300000000");
    const auto consensus_work =
        registry.issue_work(session2, consensus_override);
    CHECK(consensus_work.share_difficulty == consensus_override);
    CHECK(consensus_work.wire_work.share_target.hex() ==
          difficulty_to_target("300000000").hex());
    s2 = registry.find_session(session2);
    CHECK(s2 != nullptr);
    CHECK(s2->configured_share_difficulty == config.default_share_difficulty);

    // An impossible consensus override still fails closed at the Stratum edge.
    expect_runtime_error([&] {
        (void)registry.issue_work(
            session2,
            difficulty128_from_decimal("1000000001"));
    });

    // Reissuing unchanged work is idempotent and does not create a false stale
    // generation.
    const auto work1_again = registry.issue_work(session1);
    CHECK(work1_again.job_version == work1.job_version);
    s1 = registry.find_session(session1);
    CHECK(s1 != nullptr);
    CHECK(!s1->previous_work.has_value());

    const Hash256 header2 = hash_from_hex(
        "43147bd3560a1385c7359475e8974bbfc7aeac85c328e779e037b2d8eeec604e");
    CHECK(registry.publish_template(header2, seed1, 165014, network1) == 2);

    // Until the new template is issued to this session, the miner's previously
    // issued header remains its current work.
    CHECK(registry.match_submission(session1, header1) ==
          StratumWorkMatch::Current);

    const auto work2 = registry.issue_work(session1);
    CHECK(work2.job_version == 2);
    CHECK(work2.wire_work.header_hash == header2);
    CHECK(registry.match_submission(session1, header2) ==
          StratumWorkMatch::Current);
    CHECK(registry.match_submission(session1, header1) ==
          StratumWorkMatch::Stale);

    Hash256 unknown_header{};
    unknown_header[31] = 1;
    CHECK(registry.match_submission(session1, unknown_header) ==
          StratumWorkMatch::Unknown);
    CHECK(std::string(stratum_work_match_name(StratumWorkMatch::Current)) ==
          "current");
    CHECK(std::string(stratum_work_match_name(StratumWorkMatch::Stale)) ==
          "stale");

    // P2Pool work can never be issued above network difficulty. This matters on
    // low-difficulty testnet even when a worker requested a much harder target.
    const Difficulty128 low_network = difficulty128_from_decimal("50000000");
    CHECK(registry.publish_template(header1, seed1, 165015, low_network) == 3);
    const auto capped = registry.issue_work(session1);
    CHECK(difficulty128_to_decimal(capped.share_difficulty) == "50000000");
    CHECK(capped.wire_work.share_target.hex() ==
          difficulty_to_target("50000000").hex());
    CHECK(capped.trusted_context.network_difficulty == low_network);

    // Requested difficulty is clamped to the configured range.
    const std::uint64_t session3 = registry.create_session();
    StratumLogin high_login;
    high_login.username = "wallet-three";
    high_login.requested_difficulty = 900000000;
    registry.login(session3, high_login);
    const auto* s3 = registry.find_session(session3);
    CHECK(s3 != nullptr);
    CHECK(s3->configured_share_difficulty == config.maximum_share_difficulty);

    const std::uint64_t session4 = registry.create_session();
    StratumLogin low_login;
    low_login.username = "wallet-four";
    low_login.requested_difficulty = 1;
    registry.login(session4, low_login);
    const auto* s4 = registry.find_session(session4);
    CHECK(s4 != nullptr);
    CHECK(s4->configured_share_difficulty == config.minimum_share_difficulty);

    // Session/template misuse fails closed.
    expect_runtime_error([&] {
        (void)registry.issue_work(9999);
    });

    StratumSessionRegistry empty_registry(config);
    const auto empty_session = empty_registry.create_session();
    StratumLogin empty_login;
    empty_login.username = "wallet";
    empty_registry.login(empty_session, empty_login);
    expect_runtime_error([&] {
        (void)empty_registry.issue_work(empty_session);
    });

    StratumSessionRegistry not_logged_in(config);
    const auto unauthed = not_logged_in.create_session();
    CHECK(not_logged_in.publish_template(header1, seed1, 1, network1) == 1);
    expect_runtime_error([&] {
        (void)not_logged_in.issue_work(unauthed);
    });

    expect_runtime_error([&] {
        StratumSessionConfig bad = config;
        bad.minimum_share_difficulty = 0;
        StratumSessionRegistry invalid(bad);
    });

    return 0;
}
