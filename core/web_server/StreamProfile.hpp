#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <opencv2/core.hpp>

// One rung of the degradation ladder.
//
// Resolution is a fraction of the source frame rather than a fixed box, so the
// aspect ratio is preserved by construction and the steps stay proportional
// whatever the camera is configured to capture.
struct StreamStep
{
    double scale;
    int max_quality;
    int min_quality;
};

inline constexpr StreamStep kStreamSteps[] = {
    {1.00, 85, 50},
    {0.75, 80, 45},
    {0.50, 75, 40},
    {0.33, 70, 35},
    {0.25, 65, 30},
};

inline constexpr int kStreamStepCount =
    static_cast<int>(sizeof(kStreamSteps) / sizeof(kStreamSteps[0]));

inline constexpr int kDefaultStreamFps = 15;

struct StreamProfile
{
    int max_width{640};
    int max_height{480};
    int jpeg_quality{70};
    int target_fps{kDefaultStreamFps};

    // Frame rate is deliberately not part of the ladder. Degrading it would
    // defeat the point of a steady stream, and it would also loosen the
    // congestion threshold, which is expressed relative to the frame interval
    // -- the controller would convince itself the link had recovered.
    static StreamProfile for_step(int step, const cv::Size& source_size,
                                  int jpeg_quality, int target_fps)
    {
        const StreamStep& rung = kStreamSteps[std::clamp(step, 0, kStreamStepCount - 1)];

        StreamProfile profile;
        profile.max_width = std::max(160,
            static_cast<int>(std::lround(source_size.width * rung.scale)));
        profile.max_height = std::max(120,
            static_cast<int>(std::lround(source_size.height * rung.scale)));
        profile.jpeg_quality = std::clamp(jpeg_quality, rung.min_quality, rung.max_quality);
        profile.target_fps = std::clamp(target_fps, 1, 60);
        return profile;
    }
};

struct StreamMetrics
{
    std::uint64_t frames_sent{0};
    std::uint64_t frames_skipped{0};
    std::uint64_t bytes_sent{0};
    double last_encode_ms{0.0};
    double last_send_ms{0.0};
};
