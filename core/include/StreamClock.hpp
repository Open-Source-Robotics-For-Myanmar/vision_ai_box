#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>

// Live-stream timing helpers.
//
// H.264 packets carry timestamps, not wall-clock times. Deriving those
// timestamps from the encoded-frame count makes the media timeline run slower
// whenever the encoder misses its deadline, so the browser plays progressively
// older frames. Derive PTS from a steady clock instead: skipped frames become
// gaps in the timeline, not lost time.
class StreamClock
{
public:
    void reset(int fps, std::chrono::steady_clock::time_point start =
                            std::chrono::steady_clock::now())
    {
        fps_ = fps > 0 ? fps : 1;
        start_ = start;
        last_ = -1;
    }

    std::int64_t pts_for(std::chrono::steady_clock::time_point now)
    {
        const double elapsed = std::chrono::duration<double>(now - start_).count();
        std::int64_t pts = static_cast<std::int64_t>(std::llround(elapsed * fps_));
        if (pts < 0) pts = 0;
        if (pts <= last_) pts = last_ + 1;
        last_ = pts;
        return pts;
    }

    int fps() const noexcept { return fps_; }
    std::int64_t last() const noexcept { return last_; }

private:
    int fps_{1};
    std::chrono::steady_clock::time_point start_{};
    std::int64_t last_{-1};
};

// Schedule the next frame against the original slot, not against the actual
// completion time. A late frame is skipped; it must not push every following
// slot further into the past.
inline std::chrono::steady_clock::time_point schedule_next_frame(
    std::chrono::steady_clock::time_point deadline,
    std::chrono::steady_clock::time_point delivered,
    std::chrono::microseconds interval)
{
    deadline += interval;
    return std::max(deadline, delivered);
}
