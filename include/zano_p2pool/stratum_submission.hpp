#pragma once

#include "zano_p2pool/share_chain.hpp"
#include "zano_p2pool/stratum_protocol.hpp"
#include "zano_p2pool/stratum_session.hpp"

#include <cstdint>
#include <map>
#include <set>
#include <utility>

namespace zano_p2pool {

enum class StratumSubmissionDisposition {
    AcceptedShare,
    BlockCandidate,
    Stale,
    Duplicate,
    UnknownWork,
    Rejected,
};

struct StratumSubmissionResult {
    StratumSubmissionDisposition disposition{StratumSubmissionDisposition::Rejected};
    ShareRejectReason reject_reason{ShareRejectReason::None};
    ShareId share_id{};
    std::uint64_t job_version{0};
};

// Bridges Zano-compatible eth_submitWork into the verified local share-chain
// admission path. Duplicate protection is keyed by (job_version, nonce), because
// the canonical ShareId also includes timestamp and is therefore not sufficient
// to recognize a repeated miner submission by itself.
class StratumSubmissionRouter {
public:
    StratumSubmissionRouter(
        StratumSessionRegistry& sessions,
        ShareChain& share_chain)
        : sessions_(sessions), share_chain_(share_chain) {}

    [[nodiscard]] StratumSubmissionResult submit(
        std::uint64_t session_id,
        const StratumSubmission& submission,
        std::uint64_t now,
        ProgPowZContextMode mode = ProgPowZContextMode::Light);

    [[nodiscard]] bool was_submitted(
        std::uint64_t session_id,
        std::uint64_t job_version,
        std::uint64_t nonce) const noexcept;

private:
    [[nodiscard]] MinerId miner_id_for_session(
        const StratumSession& session) const;

    StratumSessionRegistry& sessions_;
    ShareChain& share_chain_;
    std::map<std::uint64_t, std::set<std::pair<std::uint64_t, std::uint64_t>>>
        submitted_nonces_;
};

[[nodiscard]] const char* stratum_submission_disposition_name(
    StratumSubmissionDisposition disposition) noexcept;

}  // namespace zano_p2pool
