#include "UsbCamera.hpp"

#include "Logger.hpp"

#include <chrono>
#include <string>

UsbCamera::UsbCamera(Logger& logger) : logger_(logger) {}

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
    int selected_device = -1;
    constexpr int candidate_indices[] = {1, 2, 3, 0, 4, 5, 6, 7};
    for (const int device_index : candidate_indices) {
        if (capture_.open(device_index, cv::CAP_V4L2)) {
            selected_device = device_index;
            break;
        }
        capture_.release();
    }
    if (selected_device < 0) {
        logger_.log(LogLevel::ERROR, "CAMERA", "Unable to open any USB camera device from /dev/video0 through /dev/video7");
        return false;
    }
    logger_.log(LogLevel::INFO, "CAMERA", "Opened USB camera device index " + std::to_string(selected_device));
    return true;
}

void UsbCamera::configure()
{
    capture_.set(cv::CAP_PROP_FRAME_WIDTH, 640);
    capture_.set(cv::CAP_PROP_FRAME_HEIGHT, 480);
    capture_.set(cv::CAP_PROP_FPS, 30);
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
    if (running_) return true;
    if (!initialize()) return false;
    return start_worker();
}

bool UsbCamera::start_worker()
{
    running_ = true;
    worker_ = std::thread(&UsbCamera::acquisition_loop, this);
    return true;
}

void UsbCamera::stop() noexcept
{
    running_ = false;
    if (worker_.joinable()) worker_.join();

    std::lock_guard lock(capture_mutex_);
    capture_.release();
    initialized_ = false;
}

void UsbCamera::set_processing_enabled(bool enabled) noexcept { processing_enabled_ = enabled; }

void UsbCamera::register_frame_callback(std::function<void(const core::FrameContext&)> callback)
{
    if (!callback) {
        return;
    }
    std::lock_guard lock(callbacks_mutex_);
    frame_callbacks_.push_back(std::move(callback));
}

bool UsbCamera::latest_frame(core::FrameContext& frame)
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
                    logger_.log(LogLevel::WARN, "CAMERA", "USB camera frame read failed");
                    last_read_warning_ = now;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
        }

        core::FrameContext next;
        next.sequence = ++sequence;
        next.color = std::make_shared<cv::Mat>(frame);

        {
            std::lock_guard lock(latest_frame_mutex_);
            latest_frame_ = next.color;
            latest_sequence_.store(next.sequence, std::memory_order_release);
        }

        std::vector<std::function<void(const core::FrameContext&)>> callbacks;
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

