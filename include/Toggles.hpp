#pragma once

#include <atomic>

struct ServiceToggles
{
    std::atomic<bool> camera_enabled{false};
    std::atomic<bool> processing_enabled{false};
    std::atomic<bool> running{true};
    std::atomic<bool> camera_error{false};
};


