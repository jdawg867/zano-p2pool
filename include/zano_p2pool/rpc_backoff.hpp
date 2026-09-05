#pragma once

#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace zano_p2pool {

class RpcRetryBackoff {
public:
    RpcRetryBackoff(
        std::chrono::milliseconds initial,
        std::chrono::milliseconds maximum)
        : initial_(initial), maximum_(maximum), next_(initial) {
        if (initial_.count() <= 0) {
            throw std::invalid_argument(
                "RPC reconnect initial delay must be positive");
        }
        if (maximum_ < initial_) {
            throw std::invalid_argument(
                "RPC reconnect maximum delay must not be less than initial delay");
        }
    }

    [[nodiscard]] std::chrono::milliseconds record_failure() noexcept {
        const auto delay = next_;
        if (next_ < maximum_) {
            const auto maximum_count = maximum_.count();
            const auto next_count = next_.count();
            next_ = next_count > maximum_count / 2
                        ? maximum_
                        : std::min(next_ * 2, maximum_);
        }
        return delay;
    }

    void record_success() noexcept { next_ = initial_; }

    [[nodiscard]] std::chrono::milliseconds next_delay() const noexcept {
        return next_;
    }

private:
    std::chrono::milliseconds initial_;
    std::chrono::milliseconds maximum_;
    std::chrono::milliseconds next_;
};

}  // namespace zano_p2pool
