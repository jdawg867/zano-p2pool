#include "zano_p2pool/block_submitter.hpp"
#include "zano_p2pool/crypto_hash.hpp"
#include "test_check.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace zano_p2pool;
using namespace std::chrono_literals;

Hash256 hash_from_byte(std::uint8_t value) {
    Hash256 h{};
    h.fill(value);
    return h;
}

struct Events {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<BlockSubmitEvent> values;

    void push(const BlockSubmitEvent& event) {
        {
            std::lock_guard lock(mutex);
            values.push_back(event);
        }
        cv.notify_all();
    }

    bool wait_for(std::size_t count) {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, 2s, [&] { return values.size() >= count; });
    }
};

}  // namespace

int main() {
    using namespace zano_p2pool;

    // major_version + 8-byte nonce + 3 bytes of arbitrary suffix
    const std::string base = "03" "0000000000000000" "aabbcc";
    const std::string patched = block_blob_with_nonce(
        base, UINT64_C(0x123456789abcdef0));
    CHECK(patched == "03f0debc9a78563412aabbcc");

    bool short_threw = false;
    try {
        static_cast<void>(block_blob_with_nonce("00", 1));
    } catch (const std::runtime_error&) {
        short_threw = true;
    }
    CHECK(short_threw);

    Events events;
    std::mutex submitted_mutex;
    std::vector<std::string> submitted;

    BlockCandidateSubmitter submitter(
        [&](const std::string& blob) {
            std::lock_guard lock(submitted_mutex);
            submitted.push_back(blob);
            return BlockSubmissionResult::Submitted;
        },
        [&](const BlockSubmitEvent& event) {
            events.push(event);
        },
        2,
        8);

    const Hash256 header_a = hash_from_byte(0x11);
    const Hash256 header_b = hash_from_byte(0x22);
    const Hash256 header_missing = hash_from_byte(0x33);
    const Hash256 header_alternative = hash_from_byte(0x44);

    submitter.remember_template(header_a, base);
    submitter.remember_template(header_b, base);
    CHECK(submitter.template_count() == 2);

    submitter.start();
    CHECK(submitter.running());

    CHECK(submitter.enqueue(BlockCandidate{100, header_a, 1}) ==
          BlockCandidateQueueStatus::Queued);
    CHECK(submitter.enqueue(BlockCandidate{100, header_a, 2}) ==
          BlockCandidateQueueStatus::Queued);
    CHECK(submitter.enqueue(BlockCandidate{100, header_a, 3}) ==
          BlockCandidateQueueStatus::Queued);
    CHECK(events.wait_for(1));

    // First success for header A purges all queued siblings and retires the
    // exact template. Already-in-flight Stratum submissions for that solved
    // header must now fail admission synchronously instead of reaching the
    // worker and generating a template-missing log storm.
    {
        std::lock_guard lock(submitted_mutex);
        CHECK(submitted.size() == 1);
        CHECK(submitted.front() == block_blob_with_nonce(base, 1));
    }
    {
        std::lock_guard lock(events.mutex);
        CHECK(events.values.size() == 1);
        CHECK(events.values.front().status == BlockSubmitStatus::Submitted);
        CHECK(events.values.front().candidate.nonce == 1);
    }

    CHECK(submitter.enqueue(BlockCandidate{100, header_a, 4}) ==
          BlockCandidateQueueStatus::StaleTemplate);
    CHECK(submitter.enqueue(BlockCandidate{101, header_missing, 7}) ==
          BlockCandidateQueueStatus::StaleTemplate);
    CHECK(submitter.queued() == 0);

    // An alternative-chain acceptance is a handled block submission, not an
    // invalid block. It retires the solved template and queued siblings just
    // like a main-chain acceptance, but is surfaced with its own status.
    Events alternative_events;
    BlockCandidateSubmitter alternative(
        [](const std::string&) {
            return BlockSubmissionResult::AlternativeAccepted;
        },
        [&](const BlockSubmitEvent& event) {
            alternative_events.push(event);
        });
    alternative.remember_template(header_alternative, base);
    alternative.start();
    CHECK(alternative.enqueue(BlockCandidate{101, header_alternative, 5}) ==
          BlockCandidateQueueStatus::Queued);
    CHECK(alternative.enqueue(BlockCandidate{101, header_alternative, 6}) ==
          BlockCandidateQueueStatus::Queued);
    CHECK(alternative_events.wait_for(1));
    {
        std::lock_guard lock(alternative_events.mutex);
        CHECK(alternative_events.values.size() == 1);
        CHECK(alternative_events.values[0].status ==
              BlockSubmitStatus::AlternativeAccepted);
        CHECK(alternative_events.values[0].error.empty());
    }
    CHECK(alternative.template_count() == 0);
    CHECK(alternative.queued() == 0);
    CHECK(alternative.enqueue(BlockCandidate{101, header_alternative, 7}) ==
          BlockCandidateQueueStatus::StaleTemplate);
    alternative.stop();

    // A submission failure is surfaced but leaves the worker alive.
    submitter.stop();
    CHECK(!submitter.running());
    CHECK(submitter.enqueue(BlockCandidate{101, header_b, 8}) ==
          BlockCandidateQueueStatus::Stopped);

    Events failing_events;
    BlockCandidateSubmitter failing(
        [](const std::string&) -> BlockSubmissionResult {
            throw std::runtime_error("daemon rejected block");
        },
        [&](const BlockSubmitEvent& event) {
            failing_events.push(event);
        });
    failing.remember_template(header_b, base);
    failing.start();
    CHECK(failing.enqueue(BlockCandidate{102, header_b, 9}) ==
          BlockCandidateQueueStatus::Queued);
    CHECK(failing_events.wait_for(1));
    {
        std::lock_guard lock(failing_events.mutex);
        CHECK(failing_events.values[0].status == BlockSubmitStatus::SubmissionFailed);
        CHECK(failing_events.values[0].error == "daemon rejected block");
    }
    failing.stop();

    CHECK(std::string(block_submit_status_name(BlockSubmitStatus::Submitted)) ==
          "submitted");
    CHECK(std::string(block_submit_status_name(
              BlockSubmitStatus::AlternativeAccepted)) ==
          "alternative-accepted");
    CHECK(std::string(block_candidate_queue_status_name(
              BlockCandidateQueueStatus::StaleTemplate)) ==
          "stale-template");
    return 0;
}
