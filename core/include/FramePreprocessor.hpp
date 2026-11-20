#pragma once

#include "FrameContext.hpp"
#include <opencv2/imgproc.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

// Independent of capture settings and of each model's tensor preprocessing.
struct PipelineSettings
{
    bool enabled{false};
    std::string mode{"downscale"};
    std::string quality{"medium"};
    std::string delivery{"best_effort"};
    int queue_capacity{1};

    nlohmann::json json() const
    {
        return {{"enabled", enabled}, {"mode", mode}, {"quality", quality},
                {"delivery", delivery}, {"queue_capacity", queue_capacity}};
    }

    PipelineSettings updated(const nlohmann::json& patch) const
    {
        if (!patch.is_object()) throw std::invalid_argument("pipeline settings must be an object");
        const auto known = json();
        for (const auto& item : patch.items()) {
            if (!known.contains(item.key())) throw std::invalid_argument("unknown pipeline setting: " + item.key());
            if (item.key() == "enabled" && !item.value().is_boolean())
                throw std::invalid_argument("enabled must be boolean");
            if (item.key() == "queue_capacity" &&
                (!item.value().is_number_integer() || item.value() < 1 || item.value() > 32))
                throw std::invalid_argument("queue_capacity must be an integer from 1 to 32");
        }
        auto next = *this;
        next.enabled = patch.value("enabled", enabled);
        next.mode = patch.value("mode", mode);
        next.quality = patch.value("quality", quality);
        next.delivery = patch.value("delivery", delivery);
        next.queue_capacity = patch.value("queue_capacity", queue_capacity);
        if (next.mode != "downscale" && next.mode != "filter")
            throw std::invalid_argument("mode must be downscale or filter; compression belongs to output transport");
        if (next.quality != "low" && next.quality != "medium" && next.quality != "high")
            throw std::invalid_argument("quality must be low, medium or high");
        if (next.delivery != "best_effort" && next.delivery != "reliable")
            throw std::invalid_argument("delivery must be best_effort or reliable");
        return next;
    }
};

inline FrameContext preprocess_frame(const FrameContext& raw, const PipelineSettings& settings)
{
    FrameContext result = raw;
    if (!raw.color || raw.color->empty()) return result;

    // Give models owned buffers even in bypass mode: a model must not alter
    // the shared raw recording/preview frame through cv::Mat's shallow const.
    result.color = std::make_shared<cv::Mat>(raw.color->clone());
    if (raw.depth) result.depth = std::make_shared<cv::Mat>(raw.depth->clone());
    if (!settings.enabled) return result;

    const int max_edge = settings.quality == "low" ? 640 :
                         settings.quality == "medium" ? 1280 : 0;
    const int longest = std::max(raw.color->cols, raw.color->rows);
    if (max_edge > 0 && longest > max_edge) {
        const double scale = static_cast<double>(max_edge) / longest;
        const cv::Size size(std::max(1, static_cast<int>(std::lround(raw.color->cols * scale))),
                            std::max(1, static_cast<int>(std::lround(raw.color->rows * scale))));
        cv::resize(*raw.color, *result.color, size, 0, 0, cv::INTER_AREA);
        if (raw.depth && !raw.depth->empty()) {
            // Camera backends must supply depth aligned to color.
            cv::resize(*raw.depth, *result.depth, size, 0, 0, cv::INTER_NEAREST);
        }
    }
    if (settings.mode == "filter") {
        cv::GaussianBlur(*result.color, *result.color, cv::Size(3, 3), 0);
    }
    return result;
}
