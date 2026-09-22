#pragma once

#include "BaseSystem.hpp"
#include "CameraSettings.hpp"
#include "FrameContext.hpp"
#include "StreamProfile.hpp"
#include "Toggles.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class Logger;

class FrameBuffer
{
public:
    void push(const FrameContext& frame)
    {
        if (!frame.color || frame.color->empty()) {
            return;
        }
        std::lock_guard lock(mutex_);
        latest_ = frame.color;
        latest_sequence_ = frame.sequence;
        condition_.notify_all();
    }

    bool wait_for_newer(std::uint64_t sequence, std::shared_ptr<cv::Mat>& frame,
                        std::uint64_t& next_sequence)
    {
        std::unique_lock lock(mutex_);
        condition_.wait_for(lock, std::chrono::milliseconds(100), [this, sequence] {
            return stopped_ || (latest_ && latest_sequence_ > sequence);
        });
        if (stopped_ || !latest_ || latest_sequence_ <= sequence) {
            return false;
        }
        frame = latest_;
        next_sequence = latest_sequence_;
        return true;
    }

    void clear()
    {
        std::lock_guard lock(mutex_);
        latest_.reset();
        latest_sequence_ = 0;
    }

    void start()
    {
        std::lock_guard lock(mutex_);
        stopped_ = false;
    }

    void stop()
    {
        {
            std::lock_guard lock(mutex_);
            stopped_ = true;
        }
        condition_.notify_all();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::shared_ptr<cv::Mat> latest_;
    std::uint64_t latest_sequence_{0};
    bool stopped_{false};
};

class PluginManager;

// Published by each MJPEG client so /api/camera/status can report what the
// browser is actually receiving, which is not the camera capture rate.
struct StreamClientStats
{
    std::atomic<double> delivered_fps{0.0};
    std::atomic<int> profile_level{2};
    std::atomic<int> jpeg_quality{65};
    std::atomic<std::uint64_t> frames_skipped{0};
    // Steady-clock nanoseconds of the last delivered frame. A client that
    // stalls stops publishing, so the reader needs this to decay the rate
    // instead of reporting the last good value forever.
    std::atomic<std::int64_t> last_frame_ns{0};
};

struct StreamSummary
{
    int clients{0};
    double delivered_fps{0.0};
    int profile_level{-1};
    int jpeg_quality{0};
    std::uint64_t frames_skipped{0};
};

class WebServer
{
public:
    WebServer(Logger& logger, SelectedCamera& camera, ServiceToggles& toggles, PluginManager* plugin_manager = nullptr);
    ~WebServer();

    bool start(std::uint16_t port);
    void stop() noexcept;

private:
    // HTTP request handling methods
    void accept_loop();
    void handle_client(int client_socket);
    bool is_authenticated(const std::string& request) const;

    // Camera control methods
    bool start_camera();
    void stop_camera();
    CameraSettings camera_settings() const;
    bool apply_camera_settings(const CameraSettings& settings);
    std::shared_ptr<std::vector<uchar>> encoded_frame(
        std::uint64_t sequence, const cv::Mat& source, const StreamProfile& profile,
        double& encode_ms);

    // Stream telemetry
    std::shared_ptr<StreamClientStats> register_stream_client();
    void unregister_stream_client(const std::shared_ptr<StreamClientStats>& stats);
    StreamSummary stream_summary() const;


    // Member variables
    Logger& logger_;
    SelectedCamera& camera_;
    ServiceToggles& toggles_;
    PluginManager* plugin_manager_;
    BaseSystem base_system_;

    std::atomic<bool> running_{false};

    int listen_socket_{-1};
    std::uint16_t port_{0};
    std::thread server_thread_;

    std::mutex camera_control_mutex_;
    std::atomic<std::uint64_t> stream_generation_{0};

    mutable std::mutex clients_mutex_;
    std::vector<int> client_sockets_;
    std::vector<std::thread> client_threads_;
    mutable std::mutex sessions_mutex_;
    FrameBuffer latest_stream_frame_;
    mutable std::mutex stream_stats_mutex_;
    std::vector<std::shared_ptr<StreamClientStats>> stream_stats_;
    mutable std::mutex encoded_cache_mutex_;
    std::uint64_t encoded_cache_sequence_{0};
    StreamProfile encoded_cache_profile_{};
    std::shared_ptr<std::vector<uchar>> encoded_cache_data_;

    // Browsers commonly open several simultaneous HTTP connections for the UI,
    // polling, and the MJPEG stream, so this must be higher than the single-tab case.
    static constexpr std::size_t kMaxClientConnections = 32;
    std::vector<std::string> sessions_;
};
