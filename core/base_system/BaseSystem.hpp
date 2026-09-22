#pragma once

#include "FrameContext.hpp"
#include "FrameQueue.hpp"

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
    void on_frame_received(const FrameContext& frame);
    void open_writer_if_needed(const cv::Mat& frame);
    void close_writer();
    void recording_loop();
    static std::string make_recording_stamp();
    static int resolve_fourcc();
    static bool try_open_writer(cv::VideoWriter& writer, const std::string& file_path, const cv::Size& frame_size);

    Logger& logger_;
    SelectedCamera& camera_;
    mutable std::mutex mutex_;
    std::mutex writer_mutex_;
    std::thread recording_thread_;
    std::atomic<bool> recording_active_{false};
    std::atomic<std::uint64_t> last_written_sequence_{0};
    std::chrono::steady_clock::time_point recording_start_time_{};
    std::string current_recording_file_;
    cv::VideoWriter writer_;
    FrameQueue recording_queue_{8};
};

