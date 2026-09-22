#include "UsbCamera.hpp"

#include "Logger.hpp"

#include <chrono>
#include <filesystem>
#include <string>

namespace
{
bool device_exists_for_index(int device_index)
{
    if (device_index < 0) {
        return false;
    }
    return std::filesystem::exists("/dev/video" + std::to_string(device_index));
}
}

UsbCamera::UsbCamera(Logger& logger, CameraSettings settings)
    : logger_(logger), settings_(std::move(settings)) {}

CameraSettings UsbCamera::settings() const
{
    return settings_;
}

bool UsbCamera::apply_settings(const CameraSettings& settings)
{
    settings_ = settings;
    return true;
}

UsbCamera::~UsbCamera() { stop(); }

bool UsbCamera::initialize()
{
    if (initialized_) return true;

    {
        std::lock_guard lock(capture_mutex_);
        if (!open_device()) return false;
        configure();
        if (!verify_frame()) {
            capture_.release();
            return false;
        }
    }

    initialized_ = true;
    return true;
}

bool UsbCamera::open_device()
{
    const int preferred = settings_.usb_device_index;
    if (preferred >= 0 && capture_.open(preferred, cv::CAP_V4L2)) {
        logger_.log(LogLevel::INFO, "CAMERA", "Opened USB camera device index " + std::to_string(preferred));
        return true;
    }

    if (preferred >= 0) {
        capture_.release();
    }

    const int selected_device = USB_CAMERA_DEVICE_INDEX;
    if (!capture_.open(selected_device, cv::CAP_V4L2)) {
        logger_.log(LogLevel::ERROR, "CAMERA", "Unable to open USB camera device index " + std::to_string(selected_device));
        return false;
    }
    logger_.log(LogLevel::INFO, "CAMERA", "Opened USB camera device index " + std::to_string(selected_device));
    return true;
}

void UsbCamera::configure()
{
    capture_.set(cv::CAP_PROP_FRAME_WIDTH, settings_.color_width);
    capture_.set(cv::CAP_PROP_FRAME_HEIGHT, settings_.color_height);
    capture_.set(cv::CAP_PROP_FPS, settings_.color_fps);
}

bool UsbCamera::verify_frame()
{
    cv::Mat probe;
    if (!capture_.read(probe) || probe.empty()) {
        logger_.log(LogLevel::ERROR, "CAMERA", "USB camera opened but did not provide a frame");
        return false;
    }
    return true;
}

bool UsbCamera::start()
{
    std::lock_guard lock(lifecycle_mutex_);
    if (running_) return true;
    if (!initialize()) return false;
    running_ = true;
    worker_ = std::thread(&UsbCamera::acquisition_loop, this);
    return true;
}

void UsbCamera::stop() noexcept
{
    std::lock_guard lock(lifecycle_mutex_);
    running_ = false;
    if (worker_.joinable()) worker_.join();

    std::lock_guard capture_lock(capture_mutex_);
    capture_.release();
    initialized_ = false;
}

void UsbCamera::set_processing_enabled(bool enabled) noexcept { processing_enabled_ = enabled; }

void UsbCamera::register_frame_callback(std::function<void(const FrameContext&)> callback)
{
    if (!callback) {
        return;
    }
    std::lock_guard lock(callbacks_mutex_);
    frame_callbacks_.push_back(std::move(callback));
}

bool UsbCamera::latest_frame(FrameContext& frame)
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

bool UsbCamera::is_running() const noexcept { return running_; }

double UsbCamera::measured_fps() const noexcept { return measured_fps_.load(std::memory_order_acquire); }

bool UsbCamera::check_device_state() const noexcept
{
    const int preferred = settings_.usb_device_index;
    if (preferred >= 0) {
        return device_exists_for_index(preferred);
    }

    return device_exists_for_index(USB_CAMERA_DEVICE_INDEX);
}

void UsbCamera::update_measured_fps()
{
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(fps_mutex_);
    frame_timestamps_.push_back(now);

    while (!frame_timestamps_.empty() && now - frame_timestamps_.front() > std::chrono::seconds(2)) {
        frame_timestamps_.pop_front();
    }

    if (frame_timestamps_.size() < 2) {
        measured_fps_.store(0.0, std::memory_order_release);
        return;
    }

    const double elapsed = std::chrono::duration<double>(now - frame_timestamps_.front()).count();
    if (elapsed > 0.0) {
        measured_fps_.store(static_cast<double>(frame_timestamps_.size()) / elapsed, std::memory_order_release);
    } else {
        measured_fps_.store(0.0, std::memory_order_release);
    }
}

void UsbCamera::acquisition_loop()
{
    std::uint64_t sequence = 0;
    while (running_) {
        if (!processing_enabled_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        cv::Mat frame;
        {
            std::lock_guard lock(capture_mutex_);
            if (!capture_.read(frame) || frame.empty()) {
                const auto now = std::chrono::steady_clock::now();
                if (now - last_read_warning_ >= std::chrono::seconds(1)) {
                    logger_.log(LogLevel::WARN, "CAMERA", "Hot unplug detected: camera disconnected");
                    last_read_warning_ = now;
                }
                running_ = false;
                capture_.release();
                initialized_ = false;
                break;
            }
        }

        FrameContext next;
        next.sequence = ++sequence;
        next.color = std::make_shared<cv::Mat>(frame);
        update_measured_fps();

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
    }
}

