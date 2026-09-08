#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

namespace core {

template <typename T>
class ConcurrentQueue
{
public:
    explicit ConcurrentQueue(std::size_t max_size = 4) : max_size_(max_size) {}

    ConcurrentQueue(const ConcurrentQueue&) = delete;
    ConcurrentQueue& operator=(const ConcurrentQueue&) = delete;

    void push(T item)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.size() >= max_size_) {
                queue_.pop_front();
            }
            queue_.push_back(std::move(item));
        }
        cv_.notify_one();
    }

    std::optional<T> wait_and_pop()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return !queue_.empty() || shutdown_; });

        if (queue_.empty()) {
            return std::nullopt;
        }

        T item = std::move(queue_.front());
        queue_.pop_front();
        return item;
    }

    std::optional<T> try_pop()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return std::nullopt;

        T item = std::move(queue_.front());
        queue_.pop_front();
        return item;
    }

    void shutdown()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            shutdown_ = true;
        }
        cv_.notify_all();
    }

    std::size_t size() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<T> queue_;
    std::size_t max_size_;
    bool shutdown_ = false;
};

} // namespace core
