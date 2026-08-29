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
    std::uint64_t configured_share_difficulty{0};
    std::optional<StratumIssuedWork> current_work;
    std::optional<StratumIssuedWork> previous_work;
};

class StratumSessionRegistry {
public:
    explicit StratumSessionRegistry(StratumSessionConfig config = {});

    [[nodiscard]] std::uint64_t create_session();
    void remove_session(std::uint64_t session_id) noexcept;

    // Applies the parsed Zano-compatible login. A missing/zero requested
    // difficulty selects the configured default; non-zero requests are clamped
    // to the configured Stratum range. Wallet/address validation is a later
    // server-policy layer and is intentionally not performed here.
    void login(std::uint64_t session_id, const StratumLogin& login);

    // Publishes locally trusted template data. The version increments only when
    // the actual template context changes, making repeated publication
    // deterministic and idempotent.
    [[nodiscard]] std::uint64_t publish_template(
        const Hash256& header_hash,
        const Hash256& seed_hash,
        std::uint64_t height,
        const Difficulty128& network_difficulty);

    // Issues current work for a logged-in session. The effective P2Pool share
    // difficulty is capped at network difficulty so it can never be harder than
    // a full Zano block under the Milestone 0.3 admission rules.
    [[nodiscard]] StratumIssuedWork issue_work(std::uint64_t session_id);

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
