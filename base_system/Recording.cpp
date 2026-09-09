#include "BaseSystem.hpp"

#include "Logger.hpp"

#include <chrono>
#include <filesystem>
#include <opencv2/imgcodecs.hpp>
#include <thread>

namespace
{
std::string make_recording_stamp()
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
} // namespace

BaseSystem::BaseSystem(Logger& logger, SelectedCamera& camera)
    : logger_(logger), camera_(camera)
{
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
    recording_thread_ = std::thread(&BaseSystem::recording_loop, this);
    return true;
}

bool BaseSystem::stop_recording()
{
    recording_active_.store(false, std::memory_order_release);

    if (recording_thread_.joinable()) {
        logger_.log(LogLevel::INFO, "BASE_SYSTEM", "Stopping recording thread for: " + current_recording_file_);
        recording_thread_.join();
    }

    std::lock_guard lock(writer_mutex_);
    if (writer_.isOpened()) {
        logger_.log(LogLevel::INFO, "BASE_SYSTEM", "Releasing video writer for: " + current_recording_file_);
        writer_.release();
    }
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

    core::FrameContext frame;
    if (!camera_.latest_frame(frame) || frame.color.empty()) {
        logger_.log(LogLevel::WARN, "BASE_SYSTEM", "Snapshot capture failed: no valid frame available");
        return false;
    }

    const std::string capture_path = "media/image/record_" + make_recording_stamp() + ".jpeg";
    const bool ok = cv::imwrite(capture_path, frame.color);
    if (ok) {
        logger_.log(LogLevel::INFO, "BASE_SYSTEM", "Snapshot saved: " + capture_path);
    } else {
        logger_.log(LogLevel::ERROR, "BASE_SYSTEM", "Snapshot write failed: " + capture_path);
    }
    return ok;
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

void BaseSystem::recording_loop()
{
    while (recording_active_.load(std::memory_order_acquire)) {
        core::FrameContext frame;
        if (!camera_.latest_frame(frame) || frame.color.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            continue;
        }

        std::lock_guard lock(writer_mutex_);
        if (!writer_.isOpened()) {
            const cv::Size frame_size = frame.color.size();
            const int fourcc = cv::VideoWriter::fourcc('m', 'p', '4', 'v');
            writer_.open(current_recording_file_, fourcc, 30.0, frame_size, true);
            if (!writer_.isOpened()) {
                recording_active_.store(false, std::memory_order_release);
                logger_.log(LogLevel::ERROR, "BASE_SYSTEM", "Unable to open video writer for: " + current_recording_file_);
                break;
            }
            logger_.log(LogLevel::INFO, "BASE_SYSTEM", "Video writer opened: " + current_recording_file_);
        }

        writer_.write(frame.color);
    }

    std::lock_guard lock(writer_mutex_);
    if (writer_.isOpened()) {
        logger_.log(LogLevel::INFO, "BASE_SYSTEM", "Closing video writer: " + current_recording_file_);
        writer_.release();
    }
}

std::string BaseSystem::make_timestamp()
{
    return make_recording_stamp();
}

