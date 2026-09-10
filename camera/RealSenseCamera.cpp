#include "RealSenseCamera.hpp"
#include "Logger.hpp"

#include <chrono>
#include <string>
#include <utility>
#include <opencv2/imgproc.hpp>

namespace
{
cv::Mat frame_view(const rs2::video_frame& frame, int type)
{
    if (!frame) return {};
    return {cv::Size(frame.get_width(), frame.get_height()), type,
            const_cast<void*>(frame.get_data()), cv::Mat::AUTO_STEP};
}
}

RealSenseCamera::RealSenseCamera(Logger& logger, CameraSettings settings)
    : logger_(logger), settings_(std::move(settings)), depth_enabled_(settings_.depth_enabled) {}

CameraSettings RealSenseCamera::settings() const
{
    return settings_;
}

bool RealSenseCamera::apply_settings(const CameraSettings& settings)
{
    settings_ = settings;
    depth_enabled_ = settings_.depth_enabled;
    return true;
}

RealSenseCamera::~RealSenseCamera() { stop(); }

bool RealSenseCamera::initialize()
{
    if (initialized_) return true;

    try {
        config_ = rs2::config{};
        configure_color();
        configure_depth();
        configure_alignment();

        rs2::pipeline_profile profile;
        {
            std::lock_guard lock(pipeline_mutex_);
            profile = pipeline_.start(config_);
        }

        const rs2::device opened_device = profile.get_device();
        configure_sensor_defaults(opened_device);

        logger_.log(LogLevel::INFO, "CAMERA",
            std::string("Opened RealSense device: ") +
            opened_device.get_info(RS2_CAMERA_INFO_NAME) + " (serial " +
            opened_device.get_info(RS2_CAMERA_INFO_SERIAL_NUMBER) + ")");

        initialized_ = true;
        processing_enabled_ = true;
        return true;
    } catch (const rs2::error& error) {
        logger_.log(LogLevel::ERROR, "CAMERA", std::string("RealSense initialization failed: ") + error.what());
        return false;
    } catch (const std::exception& error) {
        logger_.log(LogLevel::ERROR, "CAMERA", std::string("RealSense initialization exception: ") + error.what());
        return false;
    }
}

void RealSenseCamera::configure_color()
{
    config_.enable_stream(RS2_STREAM_COLOR,
        settings_.color_width,
        settings_.color_height,
        settings_.color_format,
        settings_.color_fps);
}

void RealSenseCamera::configure_depth()
{
    if (depth_enabled_) {
        config_.enable_stream(RS2_STREAM_DEPTH,
            settings_.depth_width,
            settings_.depth_height,
            settings_.depth_format,
            settings_.depth_fps);
    }
}

void RealSenseCamera::configure_alignment()
{
    if (depth_enabled_) {
        align_to_color_ = std::make_unique<rs2::align>(RS2_STREAM_COLOR);
    }
}

void RealSenseCamera::apply_sensor_defaults(rs2::sensor& sensor)
{
    if (settings_.auto_exposure && sensor.supports(RS2_OPTION_ENABLE_AUTO_EXPOSURE)) {
        sensor.set_option(RS2_OPTION_ENABLE_AUTO_EXPOSURE, settings_.auto_exposure_val);
    }
    if (settings_.auto_white_balance && sensor.supports(RS2_OPTION_ENABLE_AUTO_WHITE_BALANCE)) {
        sensor.set_option(RS2_OPTION_ENABLE_AUTO_WHITE_BALANCE, settings_.auto_white_balance_val);
    }
}

void RealSenseCamera::configure_sensor_defaults(const rs2::device& device)
{
    auto sensors = device.query_sensors();
    for (auto& sensor : sensors) {
        apply_sensor_defaults(sensor);
    }
}

bool RealSenseCamera::start()
{
    if (running_) return true;
    if (!initialize()) return false;
    processing_enabled_ = true;
    return start_worker();
}

bool RealSenseCamera::start_worker()
{
    running_ = true;
    worker_ = std::thread(&RealSenseCamera::acquisition_loop, this);
    return true;
}

void RealSenseCamera::stop() noexcept
{
    running_ = false;
    if (worker_.joinable()) worker_.join();

    std::lock_guard lock(pipeline_mutex_);
    if (initialized_) {
        try {
            pipeline_.stop();
        } catch (const rs2::error&) {
        } catch (const std::exception&) {
        }
    }
    initialized_ = false;
    align_to_color_.reset();
}

void RealSenseCamera::set_processing_enabled(bool enabled) noexcept { processing_enabled_ = enabled; }

bool RealSenseCamera::is_running() const noexcept { return running_; }

bool RealSenseCamera::latest_frame(FrameContext& frame)
{
    std::lock_guard lock(latest_frame_mutex_);
    if (!latest_frame_ || latest_frame_->empty()) {
        frame = {};
        return false;
    }
    frame.sequence = latest_sequence_.load(std::memory_order_acquire);
    frame.color = latest_frame_;
    frame.depth.reset();
    return true;
}

void RealSenseCamera::register_frame_callback(std::function<void(const FrameContext&)> callback)
{
    if (!callback) {
        return;
    }
    std::lock_guard lock(callbacks_mutex_);
    frame_callbacks_.push_back(std::move(callback));
}

void RealSenseCamera::acquisition_loop()
{
    std::uint64_t sequence = 0;
    while (running_) {
        if (!processing_enabled_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        try {
            rs2::frameset frames;

            // Poll the hardware outside the long-lived mutex so stop() can
            // proceed without waiting for camera I/O to finish.
            if (!initialized_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            if (!pipeline_.try_wait_for_frames(&frames, 250)) {
                continue;
            }

            if (depth_enabled_ && align_to_color_) {
                frames = align_to_color_->process(frames);
            }

            const cv::Mat raw_color = frame_view(frames.get_color_frame(), CV_8UC3);
            if (raw_color.empty()) {
                continue;
            }

            FrameContext next;
            next.sequence = ++sequence;
            // Clone here so downstream async consumers never share the raw
            // librealsense buffer after the frameset goes out of scope.
            next.color = std::make_shared<cv::Mat>(raw_color.clone());

            if (depth_enabled_) {
                const cv::Mat depth = frame_view(frames.get_depth_frame(), CV_16UC1);
                if (!depth.empty()) {
                    next.depth = std::make_shared<cv::Mat>(depth.clone());
                }
            }

            {
                std::lock_guard lock(latest_frame_mutex_);
                latest_frame_ = next.color;
                latest_sequence_.store(next.sequence, std::memory_order_release);
            }

            std::vector<std::function<void(const FrameContext&)>> callbacks;
            {
                std::lock_guard lock(callbacks_mutex_);
                callbacks = frame_callbacks_;
            }

            for (const auto& callback : callbacks) {
                if (callback) {
                    callback(next);
                }
            }
        } catch (const rs2::error& error) {
            logger_.log(LogLevel::WARN, "CAMERA", std::string("RealSense frame read failed: ") + error.what());
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        } catch (const std::exception& error) {
            logger_.log(LogLevel::WARN, "CAMERA", std::string("RealSense frame processing exception: ") + error.what());
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}
