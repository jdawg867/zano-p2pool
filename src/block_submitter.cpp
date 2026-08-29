#include "zano_p2pool/block_submitter.hpp"

#include "zano_p2pool/crypto_hash.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace zano_p2pool {

std::string block_blob_with_nonce(
    std::string_view block_blob_hex,
    std::uint64_t nonce) {
    auto bytes = hex_to_bytes(block_blob_hex);
    if (bytes.size() < 9) {
        throw std::runtime_error("block template is too short to contain nonce");
    }

    for (std::size_t i = 0; i < 8; ++i) {
        bytes[1 + i] = static_cast<std::uint8_t>((nonce >> (i * 8)) & 0xffU);
    }
    return bytes_to_hex(bytes);
}

BlockCandidateSubmitter::BlockCandidateSubmitter(
    BlockSubmitFunction submit,
    BlockSubmitEventHandler handler,
    std::size_t max_templates,
    std::size_t max_queue)
    : submit_(std::move(submit)),
      handler_(std::move(handler)),
      max_templates_(max_templates),
      max_queue_(max_queue) {
    if (!submit_) {
        throw std::invalid_argument("block submit function is required");
    }
    if (max_templates_ == 0 || max_queue_ == 0) {
        throw std::invalid_argument("block submitter bounds must be nonzero");
    }
}

BlockCandidateSubmitter::~BlockCandidateSubmitter() {
    stop();
}

void BlockCandidateSubmitter::start() {
    std::lock_guard lock(mutex_);
    if (running_) {
        throw std::runtime_error("block submitter is already running");
    }
    stop_requested_ = false;
    running_ = true;
    try {
        worker_ = std::thread(&BlockCandidateSubmitter::worker_loop, this);
    } catch (...) {
        running_ = false;
        throw;
    }
}

void BlockCandidateSubmitter::stop() noexcept {
    {
        std::lock_guard lock(mutex_);
        if (!running_) {
            return;
        }
        stop_requested_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    std::lock_guard lock(mutex_);
    running_ = false;
    queue_.clear();
}

void BlockCandidateSubmitter::remember_template(
    const Hash256& mining_header_hash,
    std::string block_blob_hex) {
    static_cast<void>(block_blob_with_nonce(block_blob_hex, 0));

    std::lock_guard lock(mutex_);
    if (!templates_.contains(mining_header_hash)) {
        template_order_.push_back(mining_header_hash);
    }
    templates_[mining_header_hash] = std::move(block_blob_hex);

    while (templates_.size() > max_templates_ && !template_order_.empty()) {
        const Hash256 oldest = template_order_.front();
        template_order_.pop_front();
        if (oldest == mining_header_hash) {
            template_order_.push_back(oldest);
            continue;
        }
        templates_.erase(oldest);
    }
}

BlockCandidateQueueStatus BlockCandidateSubmitter::enqueue(
    const BlockCandidate& candidate) noexcept {
    {
        std::lock_guard lock(mutex_);
        if (!running_ || stop_requested_) {
            return BlockCandidateQueueStatus::Stopped;
        }

        const auto template_it = templates_.find(candidate.mining_header_hash);
        if (template_it == templates_.end()) {
            return BlockCandidateQueueStatus::StaleTemplate;
        }

        if (queue_.size() >= max_queue_) {
            return BlockCandidateQueueStatus::QueueFull;
        }

        queue_.push_back(QueuedCandidate{
            candidate,
            template_it->second,
        });
    }
    cv_.notify_one();
    return BlockCandidateQueueStatus::Queued;
}

bool BlockCandidateSubmitter::running() const noexcept {
    std::lock_guard lock(mutex_);
    return running_ && !stop_requested_;
}

std::size_t BlockCandidateSubmitter::queued() const noexcept {
    std::lock_guard lock(mutex_);
    return queue_.size();
}

std::size_t BlockCandidateSubmitter::template_count() const noexcept {
    std::lock_guard lock(mutex_);
    return templates_.size();
}

void BlockCandidateSubmitter::worker_loop() noexcept {
    while (true) {
        QueuedCandidate queued;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] { return stop_requested_ || !queue_.empty(); });
            if (stop_requested_) {
                break;
            }
            queued = std::move(queue_.front());
            queue_.pop_front();
        }

        try {
            submit_(block_blob_with_nonce(
                queued.block_blob_hex,
                queued.candidate.nonce));

            {
                std::lock_guard lock(mutex_);
                std::erase_if(queue_, [&](const QueuedCandidate& sibling) {
                    return sibling.candidate.mining_header_hash ==
                           queued.candidate.mining_header_hash;
                });
                templates_.erase(queued.candidate.mining_header_hash);
                std::erase(
                    template_order_,
                    queued.candidate.mining_header_hash);
            }

            emit(BlockSubmitEvent{
                BlockSubmitStatus::Submitted,
                queued.candidate,
                {},
            });
        } catch (const std::exception& e) {
            emit(BlockSubmitEvent{
                BlockSubmitStatus::SubmissionFailed,
                queued.candidate,
                e.what(),
            });
        } catch (...) {
            emit(BlockSubmitEvent{
                BlockSubmitStatus::SubmissionFailed,
                queued.candidate,
                "unknown block submission failure",
            });
        }
    }
}

void BlockCandidateSubmitter::emit(BlockSubmitEvent event) noexcept {
    if (!handler_) {
        return;
    }
    try {
        handler_(event);
    } catch (...) {
    }
}

const char* block_submit_status_name(BlockSubmitStatus status) noexcept {
    switch (status) {
    case BlockSubmitStatus::Submitted:
        return "submitted";
    case BlockSubmitStatus::SubmissionFailed:
        return "submission-failed";
    }
    return "submission-failed";
}

const char* block_candidate_queue_status_name(
    BlockCandidateQueueStatus status) noexcept {
    switch (status) {
    case BlockCandidateQueueStatus::Queued:
        return "queued";
    case BlockCandidateQueueStatus::StaleTemplate:
        return "stale-template";
    case BlockCandidateQueueStatus::QueueFull:
        return "queue-full";
    case BlockCandidateQueueStatus::Stopped:
        return "stopped";
    }
    return "stopped";
}

}  // namespace zano_p2pool
