#include "zano_p2pool/stratum_submission.hpp"

#include "zano_p2pool/crypto_hash.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace zano_p2pool {

StratumSubmissionResult StratumSubmissionRouter::submit(
    std::uint64_t session_id,
    const StratumSubmission& submission,
    std::uint64_t now,
    ProgPowZContextMode mode) {
    const StratumSession* session = sessions_.find_session(session_id);
    if (session == nullptr || !session->logged_in || !session->current_work.has_value()) {
        return {
            StratumSubmissionDisposition::UnknownWork,
            ShareRejectReason::None,
            {},
            0,
        };
    }

    const StratumWorkMatch match =
        sessions_.match_submission(session_id, submission.header_hash);
    if (match == StratumWorkMatch::Stale) {
        const std::uint64_t stale_version = session->previous_work.has_value()
            ? session->previous_work->job_version
            : 0;
        return {
            StratumSubmissionDisposition::Stale,
            ShareRejectReason::None,
            {},
            stale_version,
        };
    }
    if (match == StratumWorkMatch::Unknown) {
        return {
            StratumSubmissionDisposition::UnknownWork,
            ShareRejectReason::None,
            {},
            0,
        };
    }

    const StratumIssuedWork& issued = *session->current_work;
    auto& seen = submitted_nonces_[session_id];
    const auto duplicate_key = std::make_pair(issued.job_version, submission.nonce);
    if (seen.contains(duplicate_key)) {
        return {
            StratumSubmissionDisposition::Duplicate,
            ShareRejectReason::None,
            {},
            issued.job_version,
        };
    }

    // Record before the expensive local PoW check so repeated invalid work is
    // also suppressed. A current job/nonce pair is deterministic: retrying it
    // cannot become valid later.
    seen.insert(duplicate_key);

    Share share;
    if (const ConnectedShare* tip = share_chain_.best_tip(); tip != nullptr) {
        share.parent_id = tip->id;
        share.share_height = tip->share.share_height + 1;
    }
    share.timestamp = now;
    share.zano_height = issued.trusted_context.zano_height;
    share.mining_header_hash = issued.trusted_context.mining_header_hash;
    share.nonce = submission.nonce;
    share.share_difficulty = issued.share_difficulty;
    share.network_difficulty = issued.trusted_context.network_difficulty;
    share.miner_id = miner_id_for_session(*session);

    const ShareId id = share_id(share);
    const AddShareResult result = share_chain_.submit_share(
        share,
        issued.trusted_context,
        now,
        mode);

    if (result.disposition != ShareDisposition::Connected &&
        result.disposition != ShareDisposition::Orphan) {
        return {
            StratumSubmissionDisposition::Rejected,
            result.reject_reason,
            id,
            issued.job_version,
        };
    }

    const ConnectedShare* connected = share_chain_.find(id);
    if (connected == nullptr || !connected->pow_validation.has_value()) {
        // A locally mined share always extends the local best tip, so it should
        // connect immediately. Treat anything else as an internal admission
        // failure rather than claiming miner success.
        return {
            StratumSubmissionDisposition::Rejected,
            ShareRejectReason::None,
            id,
            issued.job_version,
        };
    }

    if (connected->pow_validation->classification ==
        CandidateClassification::Block) {
        return {
            StratumSubmissionDisposition::BlockCandidate,
            ShareRejectReason::None,
            id,
            issued.job_version,
        };
    }

    return {
        StratumSubmissionDisposition::AcceptedShare,
        ShareRejectReason::None,
        id,
        issued.job_version,
    };
}

bool StratumSubmissionRouter::was_submitted(
    std::uint64_t session_id,
    std::uint64_t job_version,
    std::uint64_t nonce) const noexcept {
    const auto session_it = submitted_nonces_.find(session_id);
    if (session_it == submitted_nonces_.end()) {
        return false;
    }
    return session_it->second.contains(std::make_pair(job_version, nonce));
}

MinerId StratumSubmissionRouter::miner_id_for_session(
    const StratumSession& session) const {
    // Domain-separate the public Stratum login identity from arbitrary strings
    // hashed elsewhere in the protocol. This is not a secret and is not yet a
    // payout address representation; later payout accounting can bind a public
    // miner identity to explicit payout policy.
    constexpr std::array<std::uint8_t, 5> kDomain{'Z', 'P', '2', 'M', 0};
    std::vector<std::uint8_t> bytes;
    bytes.reserve(kDomain.size() + session.username.size());
    bytes.insert(bytes.end(), kDomain.begin(), kDomain.end());
    bytes.insert(bytes.end(), session.username.begin(), session.username.end());
    return cn_fast_hash(bytes);
}

const char* stratum_submission_disposition_name(
    StratumSubmissionDisposition disposition) noexcept {
    switch (disposition) {
    case StratumSubmissionDisposition::AcceptedShare:
        return "accepted-share";
    case StratumSubmissionDisposition::BlockCandidate:
        return "block-candidate";
    case StratumSubmissionDisposition::Stale:
        return "stale";
    case StratumSubmissionDisposition::Duplicate:
        return "duplicate";
    case StratumSubmissionDisposition::UnknownWork:
        return "unknown-work";
    case StratumSubmissionDisposition::Rejected:
        return "rejected";
    }
    return "rejected";
}

}  // namespace zano_p2pool
