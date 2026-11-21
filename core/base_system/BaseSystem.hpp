#pragma once

#include "FrameContext.hpp"
#include "RecordingClock.hpp"
#include <condition_variable>

#ifdef CAMERA_USB
#include "UsbCamera.hpp"
using SelectedCamera = UsbCamera;
#elif defined(CAMERA_REALSENSE)
#include "RealSenseCamera.hpp"
using SelectedCamera = RealSenseCamera;
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <opencv2/videoio.hpp>
#include <string>
#include <thread>
#include <utility>

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
    std::uint64_t get_elapsed_seconds() const;
    std::string current_recording_file() const;
    nlohmann::json discover_media_library() const;

private:
    bool start_recording_locked();
    bool stop_recording_locked();
    void write_until(std::chrono::steady_clock::time_point now);
    void recording_loop();
    static std::string make_recording_stamp();
    static int resolve_fourcc();
    static bool try_open_writer(cv::VideoWriter& writer, const std::string& file_path, const cv::Size& frame_size);

    Logger& logger_;
    SelectedCamera& camera_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    bool stopping_{false};
    std::thread recording_thread_;
    std::atomic<bool> recording_active_{false};
    std::uint64_t frames_written_{0};
    std::shared_ptr<cv::Mat> recording_frame_;
    std::chrono::steady_clock::time_point recording_start_time_{};
    std::string current_recording_file_;
    cv::VideoWriter writer_;

};

