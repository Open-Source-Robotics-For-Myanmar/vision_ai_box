#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

// A single detection, expressed in normalised [0,1] coordinates relative to
// the source frame.
//
// Normalised rather than pixel coordinates is a hard requirement, not a
// preference: the MJPEG ladder rescales the stream at runtime in response to
// network conditions, so a box measured in capture pixels would be wrong by up
// to a factor of four by the time it reached the browser. Normalised values
// also survive window resizing on the client for free.
struct Detection
{
    float x{0.0f};
    float y{0.0f};
    float width{0.0f};
    float height{0.0f};
    float confidence{0.0f};
    int class_id{-1};
    std::string label;
};

// What a plugin produced for one frame. The sequence identifies the frame the
// detections were computed from, which is generally older than the frame the
// browser is displaying -- inference and streaming run at independent rates.
struct PluginResult
{
    std::uint64_t sequence{0};
    std::string plugin;
    std::vector<Detection> detections;
};

inline void to_json(nlohmann::json& json, const Detection& detection)
{
    json = nlohmann::json{
        {"x", detection.x},
        {"y", detection.y},
        {"width", detection.width},
        {"height", detection.height},
        {"confidence", detection.confidence},
        {"class_id", detection.class_id},
        {"label", detection.label}
    };
}

inline void to_json(nlohmann::json& json, const PluginResult& result)
{
    json = nlohmann::json{
        {"sequence", result.sequence},
        {"plugin", result.plugin},
        {"detections", result.detections}
    };
}
