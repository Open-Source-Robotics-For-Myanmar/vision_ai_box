#pragma once

#include <opencv2/core.hpp>

#include <cstdint>
#include <memory>

namespace core {

struct FrameContext
{
    std::uint64_t sequence{0};
    std::shared_ptr<cv::Mat> color;
    std::shared_ptr<cv::Mat> depth;
};

} // namespace core
