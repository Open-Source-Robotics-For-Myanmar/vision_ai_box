#pragma once

#include <chrono>
#include <deque>

// Sliding-window rate estimate shared by the camera backends and the MJPEG
// stream.
//
// N samples inside the window bound N-1 intervals, so dividing the sample
// count by the window span overstates the rate by 1/span -- about +0.5 fps on
// the two-second default, and proportionally far worse at low rates.
//
// Pruning on read rather than only on write is what makes the estimate decay:
// a producer that stops delivering drains to zero instead of holding its last
// reading forever.
class RateMeter
{
public:
    explicit RateMeter(std::chrono::milliseconds window = std::chrono::seconds(2))
        : window_(window) {}

    void record(std::chrono::steady_clock::time_point now)
    {
        samples_.push_back(now);
        prune(now);
    }

    double rate(std::chrono::steady_clock::time_point now)
    {
        prune(now);
        if (samples_.size() < 2) {
            return 0.0;
        }

        const double elapsed = std::chrono::duration<double>(now - samples_.front()).count();
        if (elapsed <= 0.0) {
            return 0.0;
        }
        return static_cast<double>(samples_.size() - 1) / elapsed;
    }

private:
    void prune(std::chrono::steady_clock::time_point now)
    {
        while (!samples_.empty() && now - samples_.front() > window_) {
            samples_.pop_front();
        }
    }

    std::chrono::milliseconds window_;
    std::deque<std::chrono::steady_clock::time_point> samples_;
};
