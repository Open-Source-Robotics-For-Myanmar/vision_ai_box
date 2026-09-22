#pragma once

#include "FrameContext.hpp"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <utility>

class FrameQueue
{
public:
    explicit FrameQueue(std::size_t capacity)
        : capacity_(capacity) {}

    bool push(const FrameContext& frame)
    {
        if (!frame.color || frame.color->empty()) {
            return false;
        }

        {
            std::lock_guard lock(mutex_);
            if (closed_) {
                return false;
            }
            bool dropped = false;
            if (frames_.size() >= capacity_) {
                frames_.pop_front();
                dropped = true;
            }
            frames_.push_back(frame);
            condition_.notify_one();
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

private:
    const std::size_t capacity_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<FrameContext> frames_;
    bool closed_{false};
};
