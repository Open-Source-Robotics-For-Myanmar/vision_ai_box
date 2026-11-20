#pragma once

#include "FrameContext.hpp"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <utility>

enum class FrameDelivery { BestEffort, Reliable };

class FrameQueue
{
public:
    explicit FrameQueue(std::size_t capacity)
        : capacity_(capacity) {
        if (capacity == 0) throw std::invalid_argument("queue capacity must be positive");
    }

    bool push(const FrameContext& frame)
    {
        if (!frame.color || frame.color->empty()) {
            return false;
        }

        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [this] {
                return closed_ || delivery_ == FrameDelivery::BestEffort || frames_.size() < capacity_;
            });
            if (closed_) {
                return false;
            }
            bool dropped = false;
            if (frames_.size() >= capacity_) {
                frames_.pop_front();
                dropped = true;
            }
            frames_.push_back(frame);
            condition_.notify_all();
            return !dropped;
        }
    }

    bool pop(FrameContext& frame)
    {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [this] {
            return closed_ || !frames_.empty();
        });
        if (frames_.empty()) {
            return false;
        }
        frame = std::move(frames_.front());
        frames_.pop_front();
        condition_.notify_all();
        return true;
    }

    void close()
    {
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
            frames_.clear();
        }
        condition_.notify_all();
    }

    void clear()
    {
        std::lock_guard lock(mutex_);
        frames_.clear();
        condition_.notify_all();
    }

    // Configuration is a boundary: discard pending frames from the old policy.
    void configure(std::size_t capacity, FrameDelivery delivery)
    {
        if (capacity == 0) throw std::invalid_argument("queue capacity must be positive");
        std::lock_guard lock(mutex_);
        capacity_ = capacity;
        delivery_ = delivery;
        frames_.clear();
        condition_.notify_all();
    }

private:
    std::size_t capacity_;
    FrameDelivery delivery_{FrameDelivery::BestEffort};
    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<FrameContext> frames_;
    bool closed_{false};
};
