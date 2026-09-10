#pragma once

#include <atomic>

struct ServiceToggles
{
    std::atomic<bool> camera_enabled{true};
    std::atomic<bool> processing_enabled{false};
    std::atomic<bool> running{true};
};


