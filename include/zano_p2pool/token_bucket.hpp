#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>

namespace zano_p2pool {

struct TokenBucketConfig {
    std::size_t burst{1};
    double refill_per_second{1.0};
};

inline void validate_token_bucket_config(
    const TokenBucketConfig& config,
    std::string_view label) {
    if (config.burst == 0) {
        throw std::runtime_error(
            std::string(label) + " burst must be nonzero");
    }
    if (!std::isfinite(config.refill_per_second) ||
        config.refill_per_second <= 0.0) {
        throw std::runtime_error(
            std::string(label) + " refill rate must be finite and positive");
    }
}

// Small single-owner token bucket used at connection boundaries. Runtime
// connection/peer loops each own their limiter, so no internal synchronization
// is required. A steady clock prevents wall-clock adjustments from refilling or
// draining the bucket unexpectedly.
class TokenBucket {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    explicit TokenBucket(
        TokenBucketConfig config,
        TimePoint now = Clock::now())
        : config_(config),
          tokens_(static_cast<double>(config.burst)),
          last_refill_(now) {
        validate_token_bucket_config(config_, "token bucket");
    }

    [[nodiscard]] bool consume(TimePoint now = Clock::now()) noexcept {
        if (now > last_refill_) {
            const double elapsed_seconds =
                std::chrono::duration<double>(now - last_refill_).count();
            tokens_ = std::min(
                static_cast<double>(config_.burst),
                tokens_ + elapsed_seconds * config_.refill_per_second);
            last_refill_ = now;
        }

        if (tokens_ < 1.0) {
            return false;
        }
        tokens_ -= 1.0;
        return true;
    }

private:
    TokenBucketConfig config_{};
    double tokens_{0.0};
    TimePoint last_refill_{};
};

}  // namespace zano_p2pool
