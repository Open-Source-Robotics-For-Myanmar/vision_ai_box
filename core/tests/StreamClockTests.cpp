#include "StreamClock.hpp"

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>

using Clock = std::chrono::steady_clock;
using Ms = std::chrono::milliseconds;
using Us = std::chrono::microseconds;

int main()
{
    const auto t0 = Clock::time_point{} + Ms(1000);

    // Timestamps stay strictly increasing even inside one frame slot.
    StreamClock clock;
    clock.reset(15, t0);
    const std::int64_t first = clock.pts_for(t0);
    const std::int64_t same_slot = clock.pts_for(t0);
    assert(first == 0);
    assert(same_slot == 1);

    // A stalled producer yields timestamp gaps, not lost media time.
    clock.reset(15, t0);
    const std::int64_t p0 = clock.pts_for(t0);
    const std::int64_t p1 = clock.pts_for(t0 + Ms(120));
    const std::int64_t p2 = clock.pts_for(t0 + Ms(240));
    const std::int64_t p3 = clock.pts_for(t0 + Ms(300));
    assert(p0 == 0);
    assert(p1 >= 2);
    assert(p2 >= 4);
    assert(p3 >= p2);

    // Over ten seconds of intermittent 120 ms stalls the media timeline must
    // still track wall time within two frame slots.
    clock.reset(15, t0);
    int elapsed_ms = 0;
    std::int64_t last = 0;
    for (int step = 0; step < 100; ++step) {
        elapsed_ms += (step % 5 == 4) ? 120 : 80;
        last = clock.pts_for(t0 + Ms(elapsed_ms));
    }
    const double wall_s = elapsed_ms / 1000.0;
    const double media_s = static_cast<double>(last) / 15.0;
    assert(std::fabs(wall_s - media_s) <= 2.0 / 15.0);

    // A late frame skips its slot; it does not push every later slot back.
    const auto slot = t0 + Ms(66);
    const auto interval = Us(66666);
    assert(schedule_next_frame(slot, t0 + Ms(200), interval) == t0 + Ms(200));
    assert(schedule_next_frame(slot, t0 + Ms(10), interval) == slot + interval);

    std::printf("stream clock: pts gaps preserve wall time (wall=%.3f media=%.3f)\n",
                wall_s, media_s);
    return 0;
}
