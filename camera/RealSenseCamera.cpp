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

void apply_sensor_defaults(rs2::sensor& sensor)
{
    if (sensor.supports(RS2_OPTION_ENABLE_AUTO_EXPOSURE)) {
        sensor.set_option(RS2_OPTION_ENABLE_AUTO_EXPOSURE, 1.0f);
    }
    if (sensor.supports(RS2_OPTION_ENABLE_AUTO_WHITE_BALANCE)) {
        sensor.set_option(RS2_OPTION_ENABLE_AUTO_WHITE_BALANCE, 1.0f);
    }
}
}

RealSenseCamera::RealSenseCamera(Logger& logger, bool enable_depth)
    : logger_(logger), depth_enabled_(enable_depth) {}

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
    config_.enable_stream(RS2_STREAM_COLOR, 640, 480, RS2_FORMAT_BGR8, 30);
}

void RealSenseCamera::configure_depth()
{
    if (depth_enabled_) {
        config_.enable_stream(RS2_STREAM_DEPTH, 640, 480, RS2_FORMAT_Z16, 30);
    }
}

void RealSenseCamera::configure_alignment()
{
    if (depth_enabled_) {
        align_to_color_ = std::make_unique<rs2::align>(RS2_STREAM_COLOR);
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

bool RealSenseCamera::latest_frame(core::FrameContext& frame)
{
    const auto snapshot = frame_buffer_.consume_latest();
    if (!snapshot) return false;

    // cv::Mat uses reference-counted storage, so a value copy is cheap.
    frame = *snapshot;
    return true;
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

            core::FrameContext next;
            next.sequence = ++sequence;

            // Request BGR8 natively to avoid an extra RGB->BGR CPU conversion.
            next.color = raw_color.clone();

            if (depth_enabled_) {
                const cv::Mat depth = frame_view(frames.get_depth_frame(), CV_16UC1);
                if (!depth.empty()) {
                    next.depth = depth.clone();
                }
            }

            // Publish a refcounted snapshot; no mutex is held while the web
            // stream reads it and no DMA-backed frame pointers are retained.
            frame_buffer_.publish(std::move(next));
        } catch (const rs2::error& error) {
            logger_.log(LogLevel::WARN, "CAMERA", std::string("RealSense frame read failed: ") + error.what());
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        } catch (const std::exception& error) {
            logger_.log(LogLevel::WARN, "CAMERA", std::string("RealSense frame processing exception: ") + error.what());
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}
