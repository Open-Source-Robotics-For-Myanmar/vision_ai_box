#pragma once

#include <algorithm>
#include <cstdint>
#include <opencv2/core.hpp>

struct StreamProfile
{
    int max_width{640};
    int max_height{480};
    int jpeg_quality{70};
    int target_fps{15};

    static StreamProfile high(const cv::Size& source_size)
    {
        return {source_size.width, source_size.height, 80, 20};
    }

    static StreamProfile medium()
    {
        return {1280, 720, 65, 15};
    }

    static StreamProfile low()
    {
        return {640, 480, 50, 10};
    }

    static StreamProfile very_low()
    {
        return {320, 240, 40, 8};
    }

    StreamProfile clamped() const
    {
        return {
            std::max(160, max_width),
            std::max(120, max_height),
            std::clamp(jpeg_quality, 30, 90),
            std::clamp(target_fps, 5, 30)
        };
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
