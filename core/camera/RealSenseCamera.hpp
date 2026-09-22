#pragma once

#include "CameraSettings.hpp"
#include "FrameContext.hpp"
#include "RateMeter.hpp"
#include "Toggles.hpp"

#include <librealsense2/rs.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class Logger;

class RealSenseCamera final
{
public:
    explicit RealSenseCamera(Logger& logger, CameraSettings settings = {});
    ~RealSenseCamera();

    CameraSettings settings() const;
    bool apply_settings(const CameraSettings& settings);

    bool start();
    void stop() noexcept;
    void set_processing_enabled(bool enabled) noexcept;
    void register_frame_callback(std::function<void(const FrameContext&)> callback);
    bool latest_frame(FrameContext& frame);
    bool is_running() const noexcept;
    bool check_device_state() const noexcept;
    double measured_fps() const;

private:
    bool initialize();
    void configure_color();
    void configure_depth();
    void configure_alignment();
    void apply_sensor_defaults(rs2::sensor& sensor);
    void configure_sensor_defaults(const rs2::device& device);
    void acquisition_loop();

    Logger& logger_;
    CameraSettings settings_;
    rs2::context ctx_;
    rs2::pipeline pipeline_;
    rs2::config config_;
    std::string serial_number_;
    std::unique_ptr<rs2::align> align_to_color_;
    mutable std::mutex pipeline_mutex_;
    std::mutex lifecycle_mutex_;
    std::mutex callbacks_mutex_;
    std::mutex latest_frame_mutex_;
    std::vector<std::function<void(const FrameContext&)>> frame_callbacks_;
    std::shared_ptr<cv::Mat> latest_frame_;
    std::atomic<std::uint64_t> latest_sequence_{0};
    std::thread worker_;
    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> processing_enabled_{true};
    bool depth_enabled_{false};
    mutable std::mutex fps_mutex_;
    mutable RateMeter fps_meter_;
};
