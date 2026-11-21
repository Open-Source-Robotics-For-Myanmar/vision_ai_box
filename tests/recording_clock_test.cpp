#include "RecordingClock.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
int main()
{
    using namespace std::chrono;
    assert(recording_frame_count(nanoseconds(-1)) == 0);
    assert(recording_frame_count(nanoseconds(0)) == 0);
    assert(recording_frame_count(nanoseconds(1)) == 1);
    assert(recording_frame_count(seconds(1)) == 30);
    assert(recording_frame_count(seconds(60)) == 1800);
    assert(recording_frame_count(hours(24)) == 2'592'000);
    std::uint64_t written = 0;
    // Missing capture/worker callbacks must not shorten the timeline.
    for (auto elapsed : {milliseconds(5), milliseconds(34), milliseconds(100),
                         milliseconds(5100), milliseconds(10001)}) {
        const auto target = recording_frame_count(elapsed);
        assert(target >= written);
        written = target;
        const double encoded = double(written) / kCaptureFps;
        const double wall = duration<double>(elapsed).count();
        assert(encoded >= wall && encoded - wall < 1.0 / kCaptureFps);
    }
    assert(written == 301);
    for (std::int64_t ns = 0; ns < 2'000'000'000LL; ns += 7919) {
        const auto n = recording_frame_count(nanoseconds(ns));
        assert(n * 1'000'000'000ULL >= static_cast<std::uint64_t>(ns) * kCaptureFps);
        if (n) assert((n - 1) * 1'000'000'000ULL < static_cast<std::uint64_t>(ns) * kCaptureFps);
    }
    std::cout << "Recording timeline tests passed\n";
}
