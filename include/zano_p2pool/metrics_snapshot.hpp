#pragma once

#include <cstdint>
#include <string>

namespace zano_p2pool {

struct MetricsSnapshot {
    std::uint64_t zano_height{0};
    std::uint64_t sidechain_connected_shares{0};
    std::uint64_t sidechain_orphan_shares{0};
    std::uint64_t sidechain_tip_height{0};
    std::uint64_t p2p_peers{0};
    std::uint64_t p2p_trusted_work_contexts{0};
    std::uint64_t stratum_connections{0};
    std::uint64_t stratum_template_version{0};
    std::uint64_t stratum_accepted_shares_total{0};
    std::uint64_t p2p_admitted_shares_total{0};
    std::uint64_t block_candidates_total{0};
    std::uint64_t blocks_submitted_total{0};
    std::uint64_t block_submission_failures_total{0};
    std::uint64_t template_refresh_failures_total{0};
    bool persistence_ok{true};
};

namespace detail {

inline void append_metric(
    std::string& out,
    const char* name,
    const char* type,
    const char* help,
    std::uint64_t value) {
    out += "# HELP ";
    out += name;
    out += ' ';
    out += help;
    out += '\n';
    out += "# TYPE ";
    out += name;
    out += ' ';
    out += type;
    out += '\n';
    out += name;
    out += ' ';
    out += std::to_string(value);
    out += '\n';
}

}  // namespace detail

// Stable, label-free Prometheus exposition for the node runtime. Intentionally
// excludes wallet addresses, node IDs, share IDs and peer identities so the
// endpoint cannot leak high-cardinality or payout-identifying data.
[[nodiscard]] inline std::string render_prometheus_metrics(
    const MetricsSnapshot& snapshot) {
    std::string out;
    out.reserve(4096);

    detail::append_metric(
        out,
        "zano_p2pool_up",
        "gauge",
        "Whether the zano-p2pool runtime is serving metrics.",
        1);
    detail::append_metric(
        out,
        "zano_p2pool_zano_height",
        "gauge",
        "Latest Zano block-template height observed by the pool.",
        snapshot.zano_height);
    detail::append_metric(
        out,
        "zano_p2pool_sidechain_connected_shares",
        "gauge",
        "Verified connected shares currently retained in the sidechain.",
        snapshot.sidechain_connected_shares);
    detail::append_metric(
        out,
        "zano_p2pool_sidechain_orphan_shares",
        "gauge",
        "Verified orphan shares currently retained in the sidechain.",
        snapshot.sidechain_orphan_shares);
    detail::append_metric(
        out,
        "zano_p2pool_sidechain_tip_height",
        "gauge",
        "Best verified sidechain tip height, or zero for an empty chain.",
        snapshot.sidechain_tip_height);
    detail::append_metric(
        out,
        "zano_p2pool_p2p_peers",
        "gauge",
        "Currently connected P2P peers.",
        snapshot.p2p_peers);
    detail::append_metric(
        out,
        "zano_p2pool_p2p_trusted_work_contexts",
        "gauge",
        "Locally trusted mining work contexts available to P2P share validation.",
        snapshot.p2p_trusted_work_contexts);
    detail::append_metric(
        out,
        "zano_p2pool_stratum_connections",
        "gauge",
        "Currently connected Stratum clients.",
        snapshot.stratum_connections);
    detail::append_metric(
        out,
        "zano_p2pool_stratum_template_version",
        "gauge",
        "Current locally published Stratum template version.",
        snapshot.stratum_template_version);
    detail::append_metric(
        out,
        "zano_p2pool_stratum_accepted_shares_total",
        "counter",
        "Locally accepted Stratum shares since process start.",
        snapshot.stratum_accepted_shares_total);
    detail::append_metric(
        out,
        "zano_p2pool_p2p_admitted_shares_total",
        "counter",
        "P2P shares admitted as connected or orphan since process start.",
        snapshot.p2p_admitted_shares_total);
    detail::append_metric(
        out,
        "zano_p2pool_block_candidates_total",
        "counter",
        "Full-network-difficulty block candidates found by Stratum miners since process start.",
        snapshot.block_candidates_total);
    detail::append_metric(
        out,
        "zano_p2pool_blocks_submitted_total",
        "counter",
        "Block candidates accepted by the Zano submitblock RPC since process start.",
        snapshot.blocks_submitted_total);
    detail::append_metric(
        out,
        "zano_p2pool_block_submission_failures_total",
        "counter",
        "Block submitter events not reported as successful submitblock results since process start.",
        snapshot.block_submission_failures_total);
    detail::append_metric(
        out,
        "zano_p2pool_template_refresh_failures_total",
        "counter",
        "Template refresh attempts that raised an error since process start.",
        snapshot.template_refresh_failures_total);
    detail::append_metric(
        out,
        "zano_p2pool_persistence_ok",
        "gauge",
        "Whether sidechain persistence has remained healthy for this process.",
        snapshot.persistence_ok ? 1 : 0);

    return out;
}

}  // namespace zano_p2pool
