#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>

namespace RoboGo::app {

// A decoded image is a snapshot: displaying old images later adds latency.
// Keep one pending snapshot; never let a paused UI accumulate pixel buffers.
template <typename Frame>
class LatestVideoFrameMailbox {
public:
    struct Snapshot {
        std::uint64_t submitted = 0;
        std::uint64_t overwritten = 0;
        std::uint64_t taken = 0;
        std::size_t pending = 0;
    };

    void Push(Frame &&frame) {
        std::optional<Frame> displaced;
        {
            std::lock_guard lock(mutex_);
            displaced.swap(latest_);
            latest_.emplace(std::move(frame));
            ++submitted_;
            if (displaced.has_value()) ++overwritten_;
        }
        // Destruction/freeing the replaced image must not hold the mutex.
    }

    [[nodiscard]] std::optional<Frame> Take() {
        std::optional<Frame> result;
        {
            std::lock_guard lock(mutex_);
            result.swap(latest_);
            if (result.has_value()) ++taken_;
        }
        return result;
    }

    void Clear() {
        std::optional<Frame> displaced;
        {
            std::lock_guard lock(mutex_);
            displaced.swap(latest_);
        }
    }

    [[nodiscard]] Snapshot Stats() const {
        std::lock_guard lock(mutex_);
        return {submitted_, overwritten_, taken_, latest_.has_value() ? 1U : 0U};
    }

private:
    mutable std::mutex mutex_;
    std::optional<Frame> latest_;
    std::uint64_t submitted_ = 0;
    std::uint64_t overwritten_ = 0;
    std::uint64_t taken_ = 0;
};

} // namespace RoboGo::app
