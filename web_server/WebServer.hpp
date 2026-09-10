#pragma once

#include "BaseSystem.hpp"
#include "Toggles.hpp"
#include "core/FrameContext.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class Logger;

class WebServer
{
public:
    WebServer(Logger& logger, SelectedCamera& camera, ServiceToggles& toggles);
    ~WebServer();

    bool start(std::uint16_t port);
    void stop() noexcept;

private:
    void accept_loop();
    void handle_client(int client_socket);
    bool is_authenticated(const std::string& request) const;
    bool start_camera();
    void stop_camera();

    Logger& logger_;
    SelectedCamera& camera_;
    ServiceToggles& toggles_;
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
    std::shared_ptr<cv::Mat> latest_stream_frame_;
    std::vector<std::string> sessions_;
};
