#pragma once

#include "Toggles.hpp"
#include "core/FrameContext.hpp"
#include "core/LatestFrameBuffer.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <opencv2/videoio.hpp>
#include <thread>

class Logger;

class UsbCamera final
{
public:
    explicit UsbCamera(Logger& logger);
    ~UsbCamera();

    bool start();
    void stop() noexcept;
    void set_processing_enabled(bool enabled) noexcept;
    bool latest_frame(core::FrameContext& frame);
    bool is_running() const noexcept;

private:
    bool initialize();
    bool open_device();
    void configure();
    bool verify_frame();
    bool start_worker();
    void acquisition_loop();

    Logger& logger_;
    cv::VideoCapture capture_;
    mutable std::mutex capture_mutex_;
    core::LatestFrameBuffer<core::FrameContext> frame_buffer_;
    std::thread worker_;
    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> processing_enabled_{false};
    std::chrono::steady_clock::time_point last_read_warning_{};
};
