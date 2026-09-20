#pragma once

#include "BaseSystem.hpp"
#include "CameraSettings.hpp"
#include "FrameContext.hpp"
#include "Toggles.hpp"

#include <atomic>
#include <cstdint>
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
    }

    std::shared_ptr<cv::Mat> latest() const
    {
        std::lock_guard lock(mutex_);
        return latest_;
    }

    void pop()
    {
        std::lock_guard lock(mutex_);
        latest_.reset();
    }

    void clear()
    {
        pop();
    }

private:
    mutable std::mutex mutex_;
    std::shared_ptr<cv::Mat> latest_;
};

class PluginManager;

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
    mutable std::mutex stream_mutex_;
    FrameBuffer latest_stream_frame_;

    // Browsers commonly open several simultaneous HTTP connections for the UI,
    // polling, and the MJPEG stream, so this must be higher than the single-tab case.
    static constexpr std::size_t kMaxClientConnections = 32;
    std::vector<std::string> sessions_;
};
