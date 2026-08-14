#pragma once

#include <condition_variable>
#include <mutex>
#include <optional>
#include <utility>

namespace eggvision {

// A bounded queue with capacity one. A slow consumer always receives the latest
// frame and cannot create unbounded latency or exhaust camera requests.
template <typename T>
class LatestFrameQueue {
public:
    bool push(T value) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) {
            return false;
        }
        const bool replaced = value_.has_value();
        value_ = std::move(value);
        cv_.notify_one();
        return replaced;
    }

    bool waitPop(T &value) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return closed_ || value_.has_value(); });
        if (!value_) {
            return false;
        }
        value = std::move(*value_);
        value_.reset();
        return true;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        value_.reset();
    }

    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        value_.reset();
        cv_.notify_all();
    }

    // Reopen only after the previous consumer has stopped. This makes the
    // capacity-one queue reusable by a new lifecycle epoch without allowing a
    // frame retained by the previous epoch to cross the boundary.
    void reopen() {
        std::lock_guard<std::mutex> lock(mutex_);
        value_.reset();
        closed_ = false;
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::optional<T> value_;
    bool closed_ = false;
};

}  // namespace eggvision
