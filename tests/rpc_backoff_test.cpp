#include "zano_p2pool/rpc_backoff.hpp"
#include "test_check.hpp"

#include <chrono>
#include <stdexcept>

int main() {
    using namespace std::chrono_literals;
    using zano_p2pool::RpcRetryBackoff;

    RpcRetryBackoff backoff(100ms, 750ms);
    CHECK(backoff.next_delay() == 100ms);
    CHECK(backoff.record_failure() == 100ms);
    CHECK(backoff.record_failure() == 200ms);
    CHECK(backoff.record_failure() == 400ms);
    CHECK(backoff.record_failure() == 750ms);
    CHECK(backoff.record_failure() == 750ms);

    backoff.record_success();
    CHECK(backoff.next_delay() == 100ms);
    CHECK(backoff.record_failure() == 100ms);

    bool zero_initial_threw = false;
    try {
        static_cast<void>(RpcRetryBackoff(0ms, 1s));
    } catch (const std::invalid_argument&) {
        zero_initial_threw = true;
    }
    CHECK(zero_initial_threw);

    bool inverted_bounds_threw = false;
    try {
        static_cast<void>(RpcRetryBackoff(2s, 1s));
    } catch (const std::invalid_argument&) {
        inverted_bounds_threw = true;
    }
    CHECK(inverted_bounds_threw);

    return 0;
}
