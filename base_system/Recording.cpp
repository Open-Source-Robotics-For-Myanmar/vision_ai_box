#include "BaseSystem.hpp"

#include "Logger.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <opencv2/imgcodecs.hpp>
#include <thread>

BaseSystem::BaseSystem(Logger& logger, SelectedCamera& camera)
    : logger_(logger), camera_(camera)
{
    camera_.register_frame_callback([this](const FrameContext& frame) {
        on_frame_received(frame);
    });
}

BaseSystem::~BaseSystem()
{
    stop_recording();
}

bool BaseSystem::start_recording()
{
    std::lock_guard lock(mutex_);
    if (recording_active_.load(std::memory_order_acquire)) {
        logger_.log(LogLevel::WARN, "BASE_SYSTEM", "Recording already active; ignoring start request");
        return true;
    }

    try {
        std::filesystem::create_directories("media/videos");
        current_recording_file_ = "media/videos/record_" + make_recording_stamp() + ".mp4";
        logger_.log(LogLevel::INFO, "BASE_SYSTEM", "Starting recording: " + current_recording_file_);
    } catch (const std::exception& ex) {
        logger_.log(LogLevel::ERROR, "BASE_SYSTEM", "Failed to prepare recording directory: " + std::string(ex.what()));
        return false;
    }

    recording_active_.store(true, std::memory_order_release);
    last_written_sequence_.store(0, std::memory_order_release);
    logger_.log(LogLevel::INFO, "BASE_SYSTEM", "Recording callback enabled for: " + current_recording_file_);
    return true;
}

bool BaseSystem::stop_recording()
{
    recording_active_.store(false, std::memory_order_release);
    close_writer();
    logger_.log(LogLevel::INFO, "BASE_SYSTEM", "Recording stopped for: " + current_recording_file_);
    return true;
}

bool BaseSystem::restart_recording()
{
    logger_.log(LogLevel::INFO, "BASE_SYSTEM", "Restarting recording session");
    stop_recording();
    return start_recording();
}

bool BaseSystem::capture_frame()
{
    try {
        std::filesystem::create_directories("media/image");
    } catch (const std::exception& ex) {
        logger_.log(LogLevel::ERROR, "BASE_SYSTEM", "Failed to create snapshot directory: " + std::string(ex.what()));
        return false;
    }

    FrameContext frame;
    if (!camera_.latest_frame(frame) || !frame.color || frame.color->empty()) {
        logger_.log(LogLevel::WARN, "BASE_SYSTEM", "Snapshot capture failed: no valid frame available");
        return false;
    }

    const std::shared_ptr<cv::Mat> frame_copy = frame.color;
    const std::string capture_path = "media/image/record_" + make_recording_stamp() + ".jpeg";

    std::thread([this, frame_copy, capture_path]() mutable {
        try {
            const bool ok = cv::imwrite(capture_path, *frame_copy);
            if (ok) {
                logger_.log(LogLevel::INFO, "BASE_SYSTEM", "Snapshot saved: " + capture_path);
            } else {
                logger_.log(LogLevel::ERROR, "BASE_SYSTEM", "Snapshot write failed: " + capture_path);
            }
        } catch (const std::exception& ex) {
            logger_.log(LogLevel::ERROR, "BASE_SYSTEM", "Snapshot worker failed: " + std::string(ex.what()));
        }
    }).detach();

    return true;
}

void BaseSystem::on_frame_received(const FrameContext& frame)
{
    if (!recording_active_.load(std::memory_order_acquire) || !frame.color || frame.color->empty()) {
        return;
    }

    const std::uint64_t sequence = frame.sequence;
    if (sequence != 0 && sequence <= last_written_sequence_.load(std::memory_order_acquire)) {
        return;
    }

    std::lock_guard lock(writer_mutex_);
    open_writer_if_needed(*frame.color);
    if (!writer_.isOpened()) {
        return;
    }

    writer_.write(*frame.color);
    last_written_sequence_.store(sequence, std::memory_order_release);
}

bool BaseSystem::is_recording() const noexcept
{
    return recording_active_.load(std::memory_order_acquire);
}

std::string BaseSystem::current_recording_file() const
{
    std::lock_guard lock(mutex_);
    return current_recording_file_;
}

void BaseSystem::open_writer_if_needed(const cv::Mat& frame)
{
    if (writer_.isOpened()) {
        return;
    }

    if (!try_open_writer(writer_, current_recording_file_, frame.size())) {
        recording_active_.store(false, std::memory_order_release);
        logger_.log(LogLevel::ERROR, "BASE_SYSTEM", "Unable to open video writer for: " + current_recording_file_);
        return;
    }

    logger_.log(LogLevel::INFO, "BASE_SYSTEM", "Video writer opened: " + current_recording_file_);
}

void BaseSystem::close_writer()
{
    std::lock_guard lock(writer_mutex_);
    if (writer_.isOpened()) {
        logger_.log(LogLevel::INFO, "BASE_SYSTEM", "Closing video writer: " + current_recording_file_);
        writer_.release();
    }
}

std::string BaseSystem::make_recording_stamp()
{
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &time);
#else
    localtime_r(&time, &tm);
#endif

    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y%m%d_%H%M%S", &tm);
    return std::string(buffer);
}

int BaseSystem::resolve_fourcc()
{
    const char* configured_fourcc = std::getenv("VISION_AI_BOX_FOURCC");
    if (configured_fourcc && std::strlen(configured_fourcc) == 4) {
        return cv::VideoWriter::fourcc(
            configured_fourcc[0],
            configured_fourcc[1],
            configured_fourcc[2],
            configured_fourcc[3]
        );
    }

    return cv::VideoWriter::fourcc('m', 'p', '4', 'v');
}

bool BaseSystem::try_open_writer(cv::VideoWriter& writer, const std::string& file_path, const cv::Size& frame_size)
{
    const int fourcc = resolve_fourcc();
    const double fps = 30.0;

    const char* gst_pipeline = std::getenv("VISION_AI_BOX_GSTREAMER_PIPELINE");
    if (gst_pipeline && *gst_pipeline) {
        if (writer.open(gst_pipeline, cv::CAP_GSTREAMER, fourcc, fps, frame_size, true)) {
            return true;
        }
    }

    if (writer.open(file_path, fourcc, fps, frame_size, true)) {
        return true;
    }

    return false;
}

std::string BaseSystem::make_timestamp()
{
    return make_recording_stamp();
}

