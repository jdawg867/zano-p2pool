#pragma once

#include "zano_p2pool/share.hpp"
#include "zano_p2pool/share_chain.hpp"
#include "zano_p2pool/stratum_protocol.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace zano_p2pool {

struct StratumSessionConfig {
    std::uint64_t default_share_difficulty{100000000};
    std::uint64_t minimum_share_difficulty{1};
    std::uint64_t maximum_share_difficulty{100000000000ULL};
};

struct StratumTemplate {
    std::uint64_t version{0};
    Hash256 header_hash{};
    Hash256 seed_hash{};
    std::uint64_t height{0};
    Difficulty128 network_difficulty{};
};

struct StratumIssuedWork {
    std::uint64_t job_version{0};
    std::uint64_t session_id{0};
    Difficulty128 share_difficulty{};
    StratumWork wire_work{};
    ShareWorkContext trusted_context{};
};

enum class StratumWorkMatch {
    Current,
    Stale,
    Unknown,
};

struct StratumSession {
    std::uint64_t id{0};
    bool logged_in{false};
    std::string username;
    std::string worker;
    // Present when username is a classic standard Zx address. These are public
    // account keys only and are copied into payout-capable share v2 records.
    std::optional<PayoutPublicKeys> payout;
    std::uint64_t configured_share_difficulty{0};
    std::optional<StratumIssuedWork> current_work;
    std::optional<StratumIssuedWork> previous_work;
};

class StratumSessionRegistry {
public:
    explicit StratumSessionRegistry(StratumSessionConfig config = {});

    [[nodiscard]] std::uint64_t create_session();
    void remove_session(std::uint64_t session_id) noexcept;

    // Applies the parsed Zano-compatible login. Classic standard Zx usernames
    // are decoded to public payout keys. Non-address usernames remain accepted
    // as development v1 identities until payout-capable login becomes mandatory.
    void login(std::uint64_t session_id, const StratumLogin& login);

    [[nodiscard]] std::uint64_t publish_template(
        const Hash256& header_hash,
        const Hash256& seed_hash,
        std::uint64_t height,
        const Difficulty128& network_difficulty);

    // A full sidechain node supplies consensus_share_difficulty so miner work
    // uses the exact branch-derived target required by ShareChain admission.
    // Standalone/session tests may omit it and retain configured vardiff behavior.
    [[nodiscard]] StratumIssuedWork issue_work(
        std::uint64_t session_id,
        std::optional<Difficulty128> consensus_share_difficulty = std::nullopt);

    [[nodiscard]] StratumWorkMatch match_submission(
        std::uint64_t session_id,
        const Hash256& header_hash) const;

    [[nodiscard]] const StratumSession* find_session(
        std::uint64_t session_id) const noexcept;
    [[nodiscard]] const StratumTemplate* current_template() const noexcept;

    [[nodiscard]] const StratumSessionConfig& config() const noexcept {
        return config_;
    }

private:
    [[nodiscard]] StratumSession& require_session(std::uint64_t session_id);
    [[nodiscard]] const StratumSession& require_session(
        std::uint64_t session_id) const;
    [[nodiscard]] Difficulty128 effective_share_difficulty(
        const StratumSession& session,
        const StratumTemplate& work_template) const;

    StratumSessionConfig config_{};
    std::uint64_t next_session_id_{1};
    std::uint64_t next_template_version_{1};
    std::map<std::uint64_t, StratumSession> sessions_;
    std::optional<StratumTemplate> current_template_;
};

[[nodiscard]] const char* stratum_work_match_name(
    StratumWorkMatch match) noexcept;

}  // namespace zano_p2pool
