#pragma once

#include "CameraSettings.hpp"
#include "FrameContext.hpp"
#include "Toggles.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <opencv2/videoio.hpp>
#include <thread>
#include <vector>

class Logger;

class UsbCamera final
{
public:
    explicit UsbCamera(Logger& logger, CameraSettings settings = {});
    ~UsbCamera();

    bool start();
    void stop() noexcept;
    void set_processing_enabled(bool enabled) noexcept;
    void register_frame_callback(std::function<void(const FrameContext&)> callback);
    bool latest_frame(FrameContext& frame);
    bool is_running() const noexcept;

private:
    bool initialize();
    bool open_device();
    void configure();
    bool verify_frame();
    bool start_worker();
    void acquisition_loop();

    Logger& logger_;
    CameraSettings settings_;
    cv::VideoCapture capture_;
    mutable std::mutex capture_mutex_;
    std::mutex callbacks_mutex_;
    std::mutex latest_frame_mutex_;
    std::vector<std::function<void(const FrameContext&)>> frame_callbacks_;
    std::shared_ptr<cv::Mat> latest_frame_;
    std::atomic<std::uint64_t> latest_sequence_{0};
    std::thread worker_;
    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> processing_enabled_{false};
    std::chrono::steady_clock::time_point last_read_warning_{};
};
