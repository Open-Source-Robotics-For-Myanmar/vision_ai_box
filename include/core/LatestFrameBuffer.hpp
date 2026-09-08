#pragma once

#include <array>
#include <atomic>
#include <optional>
#include <utility>

namespace core {

template <typename T>
class LatestFrameBuffer
{
public:
    LatestFrameBuffer() : write_idx_(0), read_idx_(1), ready_idx_(2) {}

    void publish(T value)
    {
        buffers_[write_idx_] = std::move(value);
        const int prev_ready = ready_idx_.exchange(write_idx_, std::memory_order_acq_rel);
        write_idx_ = prev_ready;
        dirty_.store(true, std::memory_order_release);
        has_value_.store(true, std::memory_order_release);
    }

    std::optional<T> consume_latest()
    {
        if (!has_value_.load(std::memory_order_acquire)) {
            return std::nullopt;
        }

        if (dirty_.exchange(false, std::memory_order_acq_rel)) {
            const int prev_ready = ready_idx_.exchange(read_idx_, std::memory_order_acq_rel);
            read_idx_ = prev_ready;
        }

        return buffers_[read_idx_];
    }

private:
    std::array<T, 3> buffers_{};
    int write_idx_;
    int read_idx_;
    std::atomic<int> ready_idx_;
    std::atomic<bool> dirty_{false};
    std::atomic<bool> has_value_{false};
};

} // namespace core
