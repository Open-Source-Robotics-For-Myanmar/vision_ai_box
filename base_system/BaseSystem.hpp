#pragma once

#include "core/FrameContext.hpp"

#ifdef CAMERA_USB
#include "UsbCamera.hpp"
using SelectedCamera = UsbCamera;
#elif defined(CAMERA_REALSENSE)
#include "RealSenseCamera.hpp"
using SelectedCamera = RealSenseCamera;
#endif

#include <atomic>
#include <mutex>
#include <opencv2/videoio.hpp>
#include <string>
#include <thread>

class Logger;

class BaseSystem
{
public:
    BaseSystem(Logger& logger, SelectedCamera& camera);
    ~BaseSystem();

    bool start_recording();
    bool stop_recording();
    bool restart_recording();
    bool capture_frame();
    bool is_recording() const noexcept;
    std::string current_recording_file() const;

private:
    void recording_loop();
    static std::string make_timestamp();

    Logger& logger_;
    SelectedCamera& camera_;
    mutable std::mutex mutex_;
    std::mutex writer_mutex_;
    std::thread recording_thread_;
    std::atomic<bool> recording_active_{false};
    std::string current_recording_file_;
    cv::VideoWriter writer_;
};

