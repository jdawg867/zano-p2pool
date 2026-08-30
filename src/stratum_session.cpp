#include "zano_p2pool/stratum_session.hpp"

#include "zano_p2pool/pow_target.hpp"
#include "zano_p2pool/zano_address.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace zano_p2pool {
namespace {

[[nodiscard]] bool difficulty_less(
    const Difficulty128& left,
    const Difficulty128& right) noexcept {
    return std::lexicographical_compare(
        left.begin(), left.end(), right.begin(), right.end());
}

[[nodiscard]] bool difficulty_equal(
    const Difficulty128& left,
    const Difficulty128& right) noexcept {
    return left == right;
}

[[nodiscard]] std::uint64_t clamp_requested_difficulty(
    const StratumSessionConfig& config,
    std::optional<std::uint64_t> requested) {
    const std::uint64_t chosen =
        !requested.has_value() || *requested == 0
            ? config.default_share_difficulty
            : *requested;
    return std::clamp(
        chosen,
        config.minimum_share_difficulty,
        config.maximum_share_difficulty);
}

}  // namespace

StratumSessionRegistry::StratumSessionRegistry(StratumSessionConfig config)
    : config_(config) {
    if (config_.minimum_share_difficulty == 0) {
        throw std::runtime_error("Stratum minimum share difficulty must be nonzero");
    }
    if (config_.maximum_share_difficulty < config_.minimum_share_difficulty) {
        throw std::runtime_error(
            "Stratum maximum share difficulty is below minimum");
    }
    if (config_.default_share_difficulty < config_.minimum_share_difficulty ||
        config_.default_share_difficulty > config_.maximum_share_difficulty) {
        throw std::runtime_error(
            "Stratum default share difficulty is outside configured range");
    }
}

std::uint64_t StratumSessionRegistry::create_session() {
    if (next_session_id_ == 0) {
        throw std::runtime_error("Stratum session id space exhausted");
    }

    const std::uint64_t id = next_session_id_++;
    StratumSession session;
    session.id = id;
    session.configured_share_difficulty = config_.default_share_difficulty;
    sessions_.emplace(id, std::move(session));
    return id;
}

void StratumSessionRegistry::remove_session(std::uint64_t session_id) noexcept {
    sessions_.erase(session_id);
}

void StratumSessionRegistry::login(
    std::uint64_t session_id,
    const StratumLogin& login_request) {
    StratumSession& session = require_session(session_id);
    if (login_request.username.empty()) {
        throw std::runtime_error("Stratum login username must not be empty");
    }

    session.username = login_request.username;
    session.worker = login_request.worker.empty()
        ? std::to_string(session.id)
        : login_request.worker;
    session.payout.reset();

    const ZanoAddressDecodeResult decoded =
        decode_zano_standard_address(login_request.username);
    if (decoded.status == ZanoAddressDecodeStatus::Valid) {
        session.payout = PayoutPublicKeys{
            decoded.payout.spend_public_key,
            decoded.payout.view_public_key,
        };
    }

    session.configured_share_difficulty = clamp_requested_difficulty(
        config_, login_request.requested_difficulty);
    session.logged_in = true;
    session.current_work.reset();
    session.previous_work.reset();
}

std::uint64_t StratumSessionRegistry::publish_template(
    const Hash256& header_hash,
    const Hash256& seed_hash,
    std::uint64_t height,
    const Difficulty128& network_difficulty) {
    if (difficulty128_is_zero(network_difficulty)) {
        throw std::runtime_error("Stratum template network difficulty must be nonzero");
    }

    if (current_template_.has_value() &&
        current_template_->header_hash == header_hash &&
        current_template_->seed_hash == seed_hash &&
        current_template_->height == height &&
        difficulty_equal(current_template_->network_difficulty, network_difficulty)) {
        return current_template_->version;
    }

    if (next_template_version_ == 0) {
        throw std::runtime_error("Stratum template version space exhausted");
    }

    StratumTemplate work_template;
    work_template.version = next_template_version_++;
    work_template.header_hash = header_hash;
    work_template.seed_hash = seed_hash;
    work_template.height = height;
    work_template.network_difficulty = network_difficulty;
    current_template_ = work_template;
    return work_template.version;
}

StratumIssuedWork StratumSessionRegistry::issue_work(
    std::uint64_t session_id,
    std::optional<Difficulty128> consensus_share_difficulty) {
    StratumSession& session = require_session(session_id);
    if (!session.logged_in) {
        throw std::runtime_error("Stratum session is not logged in");
    }
    if (!current_template_.has_value()) {
        throw std::runtime_error("no Stratum template is available");
    }

    Difficulty128 share_difficulty{};
    if (consensus_share_difficulty.has_value()) {
        if (difficulty128_is_zero(*consensus_share_difficulty)) {
            throw std::runtime_error(
                "consensus Stratum share difficulty must be nonzero");
        }
        if (difficulty_less(
                current_template_->network_difficulty,
                *consensus_share_difficulty)) {
            throw std::runtime_error(
                "consensus Stratum share difficulty exceeds network difficulty");
        }
        share_difficulty = *consensus_share_difficulty;
    } else {
        share_difficulty = effective_share_difficulty(
            session,
            *current_template_);
    }

    if (session.current_work.has_value() &&
        session.current_work->job_version == current_template_->version &&
        session.current_work->share_difficulty == share_difficulty) {
        return *session.current_work;
    }

    StratumIssuedWork issued;
    issued.job_version = current_template_->version;
    issued.session_id = session.id;
    issued.share_difficulty = share_difficulty;
    issued.wire_work.header_hash = current_template_->header_hash;
    issued.wire_work.seed_hash = current_template_->seed_hash;
    issued.wire_work.share_target = difficulty_to_target(
        difficulty128_to_decimal(share_difficulty));
    issued.wire_work.height = current_template_->height;
    issued.trusted_context.zano_height = current_template_->height;
    issued.trusted_context.mining_header_hash = current_template_->header_hash;
    issued.trusted_context.network_difficulty =
        current_template_->network_difficulty;

    session.previous_work = session.current_work;
    session.current_work = issued;
    return issued;
}

StratumWorkMatch StratumSessionRegistry::match_submission(
    std::uint64_t session_id,
    const Hash256& header_hash) const {
    const StratumSession& session = require_session(session_id);
    if (!session.current_work.has_value()) {
        return StratumWorkMatch::Unknown;
    }

    if (session.current_work->wire_work.header_hash == header_hash) {
        return StratumWorkMatch::Current;
    }

    if (session.previous_work.has_value() &&
        session.previous_work->wire_work.header_hash == header_hash) {
        return StratumWorkMatch::Stale;
    }

    return StratumWorkMatch::Unknown;
}

const StratumSession* StratumSessionRegistry::find_session(
    std::uint64_t session_id) const noexcept {
    const auto it = sessions_.find(session_id);
    return it == sessions_.end() ? nullptr : &it->second;
}

const StratumTemplate* StratumSessionRegistry::current_template() const noexcept {
    return current_template_.has_value() ? &*current_template_ : nullptr;
}

StratumSession& StratumSessionRegistry::require_session(
    std::uint64_t session_id) {
    const auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
        throw std::runtime_error("unknown Stratum session");
    }
    return it->second;
}

const StratumSession& StratumSessionRegistry::require_session(
    std::uint64_t session_id) const {
    const auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
        throw std::runtime_error("unknown Stratum session");
    }
    return it->second;
}

Difficulty128 StratumSessionRegistry::effective_share_difficulty(
    const StratumSession& session,
    const StratumTemplate& work_template) const {
    const Difficulty128 configured = difficulty128_from_decimal(
        std::to_string(session.configured_share_difficulty));

    if (difficulty_less(work_template.network_difficulty, configured)) {
        return work_template.network_difficulty;
    }
    return configured;
}

const char* stratum_work_match_name(StratumWorkMatch match) noexcept {
    switch (match) {
    case StratumWorkMatch::Current:
        return "current";
    case StratumWorkMatch::Stale:
        return "stale";
    case StratumWorkMatch::Unknown:
        return "unknown";
    }
    return "unknown";
}

}  // namespace zano_p2pool
