#pragma once

#include "zano_p2pool/pow_target.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace zano_p2pool {

struct BlockCandidate {
    std::uint64_t zano_height{};
    Hash256 mining_header_hash{};
    std::uint64_t nonce{};
};

enum class BlockSubmitStatus : std::uint8_t {
    Submitted,
    AlternativeAccepted,
    SubmissionFailed,
};

enum class BlockSubmissionResult : std::uint8_t {
    Submitted,
    AlternativeAccepted,
};

struct BlockSubmitEvent {
    BlockSubmitStatus status{BlockSubmitStatus::SubmissionFailed};
    BlockCandidate candidate{};
    std::string error;
};

enum class BlockCandidateQueueStatus : std::uint8_t {
    Queued,
    StaleTemplate,
    QueueFull,
    Stopped,
};

struct BlockCandidateQueueResult {
    BlockCandidateQueueStatus status{BlockCandidateQueueStatus::Stopped};

    // Existing callers historically treated enqueue() as a bool. A candidate
    // for a retired template is intentionally considered handled: it must be
    // discarded silently rather than producing a warning for every old-header
    // submission already in flight after a block win.
    [[nodiscard]] operator bool() const noexcept {
        return status == BlockCandidateQueueStatus::Queued ||
               status == BlockCandidateQueueStatus::StaleTemplate;
    }

    friend bool operator==(
        const BlockCandidateQueueResult& left,
        BlockCandidateQueueStatus right) noexcept {
        return left.status == right;
    }
};

using BlockSubmitFunction =
    std::function<BlockSubmissionResult(const std::string& block_blob_hex)>;
using BlockSubmitEventHandler = std::function<void(const BlockSubmitEvent&)>;

// Return a copy of the exact RPC block template with block_header.nonce patched
// in place. Zano serializes the fixed-width uint64 nonce little-endian at bytes
// 1..8 immediately after major_version.
[[nodiscard]] std::string block_blob_with_nonce(
    std::string_view block_blob_hex,
    std::uint64_t nonce);

// A single-worker submit queue keeps daemon submitblock RPC off Stratum client
// threads. Templates are keyed by the independently derived mining-header hash,
// so a candidate can only be reconstructed from the exact work it solved.
class BlockCandidateSubmitter {
public:
    explicit BlockCandidateSubmitter(
        BlockSubmitFunction submit,
        BlockSubmitEventHandler handler = {},
        std::size_t max_templates = 8,
        std::size_t max_queue = 64);
    ~BlockCandidateSubmitter();

    BlockCandidateSubmitter(const BlockCandidateSubmitter&) = delete;
    BlockCandidateSubmitter& operator=(const BlockCandidateSubmitter&) = delete;

    void start();
    void stop() noexcept;

    void remember_template(
        const Hash256& mining_header_hash,
        std::string block_blob_hex);

    // Admission snapshots the exact template bytes while holding the submitter
    // mutex. A candidate for an already-solved/evicted header is rejected as
    // StaleTemplate before it can reach the worker. This is important on fast
    // testnets where miners may have many old-header submissions already in
    // flight when the first sibling block is accepted.
    [[nodiscard]] BlockCandidateQueueResult enqueue(
        const BlockCandidate& candidate) noexcept;

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] std::size_t queued() const noexcept;
    [[nodiscard]] std::size_t template_count() const noexcept;

private:
    struct QueuedCandidate {
        BlockCandidate candidate{};
        std::string block_blob_hex;
    };

    void worker_loop() noexcept;
    void emit(BlockSubmitEvent event) noexcept;

    BlockSubmitFunction submit_;
    BlockSubmitEventHandler handler_;
    std::size_t max_templates_{8};
    std::size_t max_queue_{64};

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool running_{false};
    bool stop_requested_{false};
    std::thread worker_;

    std::map<Hash256, std::string> templates_;
    std::deque<Hash256> template_order_;
    std::deque<QueuedCandidate> queue_;
};

[[nodiscard]] const char* block_submit_status_name(
    BlockSubmitStatus status) noexcept;
[[nodiscard]] const char* block_candidate_queue_status_name(
    BlockCandidateQueueStatus status) noexcept;

}  // namespace zano_p2pool
