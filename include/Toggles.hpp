#pragma once

#include <atomic>
#include <cstdint>
#include <opencv2/core.hpp>

struct FrameContext
{
    std::uint64_t sequence{0};
    cv::Mat color;
    cv::Mat depth;
};

struct ServiceToggles
{
    std::atomic<bool> camera_enabled{true};
    std::atomic<bool> processing_enabled{false};
    std::atomic<bool> running{true};
};


