#include "zano_p2pool/crypto_hash.hpp"
#include "zano_p2pool/share.hpp"
#include "zano_p2pool/share_chain.hpp"
#include "zano_p2pool/stratum_session.hpp"
#include "zano_p2pool/stratum_submission.hpp"
#include "test_check.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace {

zano_p2pool::Hash256 hash_from_hex(std::string_view hex) {
    const auto bytes = zano_p2pool::hex_to_bytes(hex);
    CHECK(bytes.size() == 32);
    zano_p2pool::Hash256 hash{};
    std::copy(bytes.begin(), bytes.end(), hash.begin());
    return hash;
}

zano_p2pool::StratumLogin login_request(
    std::string username,
    std::uint64_t difficulty) {
    zano_p2pool::StratumLogin login;
    login.username = std::move(username);
    login.worker = "worker-1";
    login.requested_difficulty = difficulty;
    return login;
}

}  // namespace

int main() {
    using namespace zano_p2pool;

    const Hash256 header = hash_from_hex(
        "ffeeddccbbaa9988776655443322110000112233445566778899aabbccddeeff");
    const Hash256 next_header = hash_from_hex(
        "00112233445566778899aabbccddeeffffeeddccbbaa99887766554433221100");
    Hash256 seed{};
    seed[31] = 1;

    StratumSessionConfig config;
    config.default_share_difficulty = 3;
    config.minimum_share_difficulty = 1;
    config.maximum_share_difficulty = 10;

    StratumSessionRegistry sessions(config);
    const std::uint64_t session_id = sessions.create_session();
    sessions.login(session_id, login_request("public-miner-id", 3));
    CHECK(sessions.publish_template(
              header,
              seed,
              0,
              difficulty128_from_decimal("4")) == 1);
    const StratumIssuedWork work1 = sessions.issue_work(session_id);
    CHECK(work1.job_version == 1);
    CHECK(difficulty128_to_decimal(work1.share_difficulty) == "3");

    ShareChain chain;
    StratumSubmissionRouter router(sessions, chain);

    StratumSubmission submission;
    submission.nonce = UINT64_C(0x123456789abcdef0);
    submission.header_hash = header;

#ifdef ZANO_P2POOL_HAVE_PROGPOWZ
    // Exact pinned Zano vector meets difficulty 3 but not difficulty 4.
    const StratumSubmissionResult accepted = router.submit(
        session_id,
        submission,
        1'700'200'000,
        ProgPowZContextMode::Light);
    CHECK(accepted.disposition == StratumSubmissionDisposition::AcceptedShare);
    CHECK(accepted.job_version == 1);
    CHECK(chain.connected_size() == 1);
    CHECK(chain.best_tip() != nullptr);
    CHECK(chain.best_tip()->id == accepted.share_id);
    CHECK(chain.best_tip()->pow_validation.has_value());
    CHECK(chain.best_tip()->pow_validation->classification ==
          CandidateClassification::Share);
    CHECK(router.was_submitted(session_id, 1, submission.nonce));

    // Same job/nonce is rejected before a second ProgPoWZ evaluation, even
    // though rebuilding a Share later would otherwise change its timestamp/ID.
    const StratumSubmissionResult duplicate = router.submit(
        session_id,
        submission,
        1'700'200'001,
        ProgPowZContextMode::Light);
    CHECK(duplicate.disposition == StratumSubmissionDisposition::Duplicate);
    CHECK(chain.connected_size() == 1);

    // Publishing a new template does not make old work stale until replacement
    // work is actually issued to this session.
    CHECK(sessions.publish_template(
              next_header,
              seed,
              1,
              difficulty128_from_decimal("4")) == 2);
    StratumSubmission old_before_issue = submission;
    old_before_issue.nonce = UINT64_C(0x123456789abcdef1);
    CHECK(sessions.match_submission(session_id, header) ==
          StratumWorkMatch::Current);

    // Avoid hashing the deliberately unrelated header; issue replacement work
    // first, then prove old header classification is stale and non-PoW.
    const StratumIssuedWork work2 = sessions.issue_work(session_id);
    CHECK(work2.job_version == 2);
    CHECK(sessions.match_submission(session_id, header) == StratumWorkMatch::Stale);
    const StratumSubmissionResult stale = router.submit(
        session_id,
        old_before_issue,
        1'700'200'002,
        ProgPowZContextMode::Light);
    CHECK(stale.disposition == StratumSubmissionDisposition::Stale);
    CHECK(stale.job_version == 1);
    CHECK(!router.was_submitted(session_id, 1, old_before_issue.nonce));

    StratumSubmission unknown = submission;
    unknown.nonce = UINT64_C(0x123456789abcdef2);
    unknown.header_hash.fill(0x55);
    const StratumSubmissionResult unknown_result = router.submit(
        session_id,
        unknown,
        1'700'200'003,
        ProgPowZContextMode::Light);
    CHECK(unknown_result.disposition ==
          StratumSubmissionDisposition::UnknownWork);

    // Restore the known ProgPoWZ header with network difficulty 3. The same
    // pinned nonce now satisfies both share and full-network difficulty and is
    // surfaced as a block candidate without any daemon submission side effect.
    CHECK(sessions.publish_template(
              header,
              seed,
              0,
              difficulty128_from_decimal("3")) == 3);
    const StratumIssuedWork work3 = sessions.issue_work(session_id);
    CHECK(work3.job_version == 3);
    CHECK(difficulty128_to_decimal(work3.share_difficulty) == "3");

    StratumSubmission block_submission = submission;
    block_submission.nonce = UINT64_C(0x123456789abcdef0);
    const StratumSubmissionResult block = router.submit(
        session_id,
        block_submission,
        1'700'200'004,
        ProgPowZContextMode::Light);
    CHECK(block.disposition == StratumSubmissionDisposition::BlockCandidate);
    CHECK(block.job_version == 3);
    CHECK(chain.connected_size() == 2);
    const ConnectedShare* block_share = chain.find(block.share_id);
    CHECK(block_share != nullptr);
    CHECK(block_share->pow_validation.has_value());
    CHECK(block_share->pow_validation->classification ==
          CandidateClassification::Block);

    // A fresh session/job with difficulty 4 rejects the same deterministic PoW
    // and then suppresses an identical repeat as a duplicate nonce submission.
    StratumSessionRegistry invalid_sessions(config);
    const std::uint64_t invalid_id = invalid_sessions.create_session();
    invalid_sessions.login(invalid_id, login_request("invalid-miner", 4));
    CHECK(invalid_sessions.publish_template(
              header,
              seed,
              0,
              difficulty128_from_decimal("5")) == 1);
    CHECK(difficulty128_to_decimal(
              invalid_sessions.issue_work(invalid_id).share_difficulty) == "4");
    ShareChain invalid_chain;
    StratumSubmissionRouter invalid_router(invalid_sessions, invalid_chain);
    const StratumSubmissionResult invalid = invalid_router.submit(
        invalid_id,
        submission,
        1'700'300'000,
        ProgPowZContextMode::Light);
    CHECK(invalid.disposition == StratumSubmissionDisposition::Rejected);
    CHECK(invalid.reject_reason == ShareRejectReason::InvalidPow);
    CHECK(invalid_chain.connected_size() == 0);
    CHECK(invalid_router.submit(
              invalid_id,
              submission,
              1'700'300'001,
              ProgPowZContextMode::Light).disposition ==
          StratumSubmissionDisposition::Duplicate);
#else
    // Lightweight builds fail closed through ShareChain::submit_share().
    const StratumSubmissionResult unavailable = router.submit(
        session_id,
        submission,
        1'700'200'000,
        ProgPowZContextMode::Light);
    CHECK(unavailable.disposition == StratumSubmissionDisposition::Rejected);
    CHECK(unavailable.reject_reason == ShareRejectReason::PowBackendUnavailable);
    CHECK(chain.connected_size() == 0);
    CHECK(router.was_submitted(session_id, 1, submission.nonce));
    CHECK(router.submit(
              session_id,
              submission,
              1'700'200'001,
              ProgPowZContextMode::Light).disposition ==
          StratumSubmissionDisposition::Duplicate);
#endif

    CHECK(std::string(stratum_submission_disposition_name(
              StratumSubmissionDisposition::AcceptedShare)) ==
          "accepted-share");
    CHECK(std::string(stratum_submission_disposition_name(
              StratumSubmissionDisposition::BlockCandidate)) ==
          "block-candidate");
    CHECK(std::string(stratum_submission_disposition_name(
              StratumSubmissionDisposition::Stale)) ==
          "stale");

    return 0;
}
