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
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

namespace httplib
{
class Server;
class Request;
class Response;
class DataSink;
}

class Logger;
class PluginManager;

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

struct StreamClientStats
{
    std::atomic<double> delivered_fps{0.0};
    std::atomic<int> frame_width{0};
    std::atomic<int> frame_height{0};
    std::atomic<int> jpeg_quality{0};
    std::atomic<std::uint64_t> frames_skipped{0};
    std::atomic<std::int64_t> last_frame_ns{0};
};

struct StreamSummary
{
    int clients{0};
    double delivered_fps{0.0};
    int frame_width{0};
    int frame_height{0};
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
    void register_routes();
    bool is_authenticated(const httplib::Request& request) const;
    bool start_camera();
    void stop_camera();
    CameraSettings camera_settings() const;
    bool apply_camera_settings(const CameraSettings& settings);
    nlohmann::json camera_status_json() const;
    nlohmann::json recording_status_json() const;
    bool stream_h264(httplib::DataSink& sink);
    bool write_sse(httplib::DataSink& sink);

    std::shared_ptr<StreamClientStats> register_stream_client();
    void unregister_stream_client(const std::shared_ptr<StreamClientStats>& stats);
    StreamSummary stream_summary() const;

    Logger& logger_;
    SelectedCamera& camera_;
    ServiceToggles& toggles_;
    PluginManager* plugin_manager_;
    BaseSystem base_system_;

    std::atomic<bool> running_{false};
    std::uint16_t port_{0};
    std::unique_ptr<httplib::Server> server_;
    std::thread server_thread_;

    std::mutex camera_control_mutex_;
    std::atomic<std::uint64_t> stream_generation_{0};

    mutable std::mutex sessions_mutex_;
    std::vector<std::string> sessions_;
    FrameBuffer latest_stream_frame_;
    mutable std::mutex stream_stats_mutex_;
    std::vector<std::shared_ptr<StreamClientStats>> stream_stats_;
};
