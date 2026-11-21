#pragma once
#include <chrono>
#include <cstdint>
inline constexpr int kCaptureFps = 30;
// ceil(elapsed * FPS), without floating-point boundary drift.
inline std::uint64_t recording_frame_count(std::chrono::steady_clock::duration elapsed)
{
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    if (ns <= 0) return 0;
    return static_cast<std::uint64_t>(ns / 1'000'000'000) * kCaptureFps +
        (static_cast<std::uint64_t>(ns % 1'000'000'000) * kCaptureFps + 999'999'999) / 1'000'000'000;
}
