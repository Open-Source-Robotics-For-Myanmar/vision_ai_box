#pragma once

#include <opencv2/core.hpp>

#include <cstdint>

namespace core {

struct FrameContext
{
    std::uint64_t sequence{0};
    cv::Mat color;
    cv::Mat depth;
};

} // namespace core
