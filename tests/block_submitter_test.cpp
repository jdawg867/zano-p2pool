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
        },
        [&](const BlockSubmitEvent& event) {
            events.push(event);
        },
        2,
        8);

    const Hash256 header_a = hash_from_byte(0x11);
    const Hash256 header_b = hash_from_byte(0x22);
    const Hash256 header_missing = hash_from_byte(0x33);

    submitter.remember_template(header_a, base);
    submitter.remember_template(header_b, base);
    CHECK(submitter.template_count() == 2);

    submitter.start();
    CHECK(submitter.running());

    CHECK(submitter.enqueue(BlockCandidate{100, header_a, 1}));
    CHECK(submitter.enqueue(BlockCandidate{100, header_a, 2}));
    CHECK(submitter.enqueue(BlockCandidate{100, header_a, 3}));
    CHECK(events.wait_for(1));

    // First success for header A purges all queued siblings for that template.
    {
        std::lock_guard lock(submitted_mutex);
        CHECK(submitted.size() == 1);
        CHECK(submitted.front() == block_blob_with_nonce(base, 1));
    }
    {
        std::lock_guard lock(events.mutex);
        CHECK(events.values.front().status == BlockSubmitStatus::Submitted);
        CHECK(events.values.front().candidate.nonce == 1);
    }

    CHECK(submitter.enqueue(BlockCandidate{101, header_missing, 7}));
    CHECK(events.wait_for(2));
    {
        std::lock_guard lock(events.mutex);
        CHECK(events.values[1].status == BlockSubmitStatus::TemplateMissing);
    }

    // A submission failure is surfaced but leaves the worker alive.
    submitter.stop();
    CHECK(!submitter.running());

    Events failing_events;
    BlockCandidateSubmitter failing(
        [](const std::string&) {
            throw std::runtime_error("daemon rejected block");
        },
        [&](const BlockSubmitEvent& event) {
            failing_events.push(event);
        });
    failing.remember_template(header_b, base);
    failing.start();
    CHECK(failing.enqueue(BlockCandidate{102, header_b, 9}));
    CHECK(failing_events.wait_for(1));
    {
        std::lock_guard lock(failing_events.mutex);
        CHECK(failing_events.values[0].status == BlockSubmitStatus::SubmissionFailed);
        CHECK(failing_events.values[0].error == "daemon rejected block");
    }
    failing.stop();

    CHECK(std::string(block_submit_status_name(BlockSubmitStatus::Submitted)) ==
          "submitted");
    return 0;
}
