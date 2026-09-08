#pragma once

#include "Toggles.hpp"
#include "core/FrameContext.hpp"
#include "core/LatestFrameBuffer.hpp"

#include <librealsense2/rs.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

class Logger;

class RealSenseCamera final
{
public:
    explicit RealSenseCamera(Logger& logger, bool enable_depth = false);
    ~RealSenseCamera();

    bool start();
    void stop() noexcept;
    void set_processing_enabled(bool enabled) noexcept;
    bool latest_frame(core::FrameContext& frame);
    bool is_running() const noexcept;

private:
    bool initialize();
    void configure_color();
    void configure_depth();
    void configure_alignment();
    void configure_sensor_defaults(const rs2::device& device);
    bool start_worker();
    void acquisition_loop();

    Logger& logger_;
    rs2::pipeline pipeline_;
    rs2::config config_;
    std::unique_ptr<rs2::align> align_to_color_;
    mutable std::mutex pipeline_mutex_;
    core::LatestFrameBuffer<core::FrameContext> frame_buffer_;
    std::thread worker_;
    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> processing_enabled_{true};
    bool depth_enabled_{false};
};
