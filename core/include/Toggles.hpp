#pragma once

#include <atomic>

enum class SystemState
{
    BOOT,
    CAMERA_INIT,
    CAMERA_READY,
    STREAMING,
    RECORDING,
    AI_PROCESSING,
    ERROR_RECOVER,
    SHUTDOWN
};

struct ServiceToggles
{
    std::atomic<bool> camera_enabled{false};
    std::atomic<bool> processing_enabled{false};
    std::atomic<bool> running{true};
    std::atomic<bool> camera_error{false};
    std::atomic<SystemState> state{SystemState::BOOT};

    static bool is_valid_transition(SystemState from, SystemState to) noexcept
    {
        switch (from) {
            case SystemState::BOOT:
                return to == SystemState::CAMERA_INIT || to == SystemState::SHUTDOWN;
            case SystemState::CAMERA_INIT:
                return to == SystemState::CAMERA_READY || to == SystemState::ERROR_RECOVER || to == SystemState::SHUTDOWN;
            case SystemState::CAMERA_READY:
                return to == SystemState::STREAMING || to == SystemState::RECORDING || to == SystemState::AI_PROCESSING ||
                       to == SystemState::ERROR_RECOVER || to == SystemState::SHUTDOWN;
            case SystemState::STREAMING:
                return to == SystemState::CAMERA_READY || to == SystemState::RECORDING || to == SystemState::AI_PROCESSING ||
                       to == SystemState::ERROR_RECOVER || to == SystemState::SHUTDOWN;
            case SystemState::RECORDING:
                return to == SystemState::STREAMING || to == SystemState::CAMERA_READY || to == SystemState::AI_PROCESSING ||
                       to == SystemState::ERROR_RECOVER || to == SystemState::SHUTDOWN;
            case SystemState::AI_PROCESSING:
                return to == SystemState::STREAMING || to == SystemState::RECORDING || to == SystemState::CAMERA_READY ||
                       to == SystemState::ERROR_RECOVER || to == SystemState::SHUTDOWN;
            case SystemState::ERROR_RECOVER:
                return to == SystemState::CAMERA_INIT || to == SystemState::CAMERA_READY || to == SystemState::SHUTDOWN;
            case SystemState::SHUTDOWN:
                return false;
        }
        return false;
    }

    bool request_transition(SystemState next) noexcept
    {
        const SystemState current = state.load(std::memory_order_acquire);
        if (current == next) {
            return true;
        }
        if (current == SystemState::SHUTDOWN || !is_valid_transition(current, next)) {
            return false;
        }
        state.store(next, std::memory_order_release);
        return true;
    }

    bool request_start_camera() noexcept
    {
        const SystemState current = state.load(std::memory_order_acquire);
        if (current == SystemState::SHUTDOWN) {
            return false;
        }
        camera_enabled.store(true, std::memory_order_release);
        if (current == SystemState::BOOT) {
            return request_transition(SystemState::CAMERA_INIT);
        }
        if (current == SystemState::ERROR_RECOVER) {
            return request_transition(SystemState::CAMERA_INIT);
        }
        return request_transition(SystemState::CAMERA_READY);
    }

    bool request_stop_camera() noexcept
    {
        const SystemState current = state.load(std::memory_order_acquire);
        if (current == SystemState::SHUTDOWN) {
            return false;
        }
        camera_enabled.store(false, std::memory_order_release);
        processing_enabled.store(false, std::memory_order_release);
        camera_error.store(false, std::memory_order_release);
        if (current == SystemState::STREAMING || current == SystemState::RECORDING || current == SystemState::AI_PROCESSING) {
            return request_transition(SystemState::CAMERA_READY);
        }
        if (current == SystemState::CAMERA_INIT || current == SystemState::CAMERA_READY) {
            return request_transition(SystemState::BOOT);
        }
        return request_transition(SystemState::BOOT);
    }

    bool request_recording_start() noexcept
    {
        if (state.load(std::memory_order_acquire) == SystemState::SHUTDOWN) {
            return false;
        }
        return request_transition(SystemState::RECORDING);
    }

    bool request_shutdown() noexcept
    {
        if (state.load(std::memory_order_acquire) == SystemState::SHUTDOWN) {
            return true;
        }
        running.store(false, std::memory_order_release);
        camera_enabled.store(false, std::memory_order_release);
        processing_enabled.store(false, std::memory_order_release);
        camera_error.store(false, std::memory_order_release);
        state.store(SystemState::SHUTDOWN, std::memory_order_release);
        return true;
    }
};


