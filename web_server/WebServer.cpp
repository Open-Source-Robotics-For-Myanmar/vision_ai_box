#include "WebServer.hpp"

#include "Logger.hpp"

#include <arpa/inet.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <netinet/in.h>
#include <nlohmann/json.hpp>
#include <opencv2/imgcodecs.hpp>
#include <random>
#include <sys/socket.h>
#include <unistd.h>

namespace
{
using json = nlohmann::json;

json serialize_resolution_options(const std::vector<ResolutionOption>& options)
{
    json payload = json::array();
    for (const auto& option : options) {
        payload.push_back({
            {"width", option.width},
            {"height", option.height},
            {"label", option.label}
        });
    }
    return payload;
}

json serialize_available_settings()
{
    json payload = {
        {"color_resolutions", json::array()},
        {"color_fps_options", json::array()}
    };

#ifdef CAMERA_USB
    payload["color_resolutions"] = serialize_resolution_options(AvailableCameraSettings::color_resolutions());
    for (const int fps : AvailableCameraSettings::color_fps_options()) {
        payload["color_fps_options"].push_back(fps);
    }
#elif defined(CAMERA_REALSENSE)
    payload["color_resolutions"] = serialize_resolution_options(AvailableCameraSettings::color_resolutions());
    for (const int fps : AvailableCameraSettings::color_fps_options()) {
        payload["color_fps_options"].push_back(fps);
    }
    payload["depth_resolutions"] = serialize_resolution_options(AvailableCameraSettings::depth_resolutions());
    payload["depth_fps_options"] = json::array();
    for (const int fps : AvailableCameraSettings::depth_fps_options()) {
        payload["depth_fps_options"].push_back(fps);
    }
#endif

    return payload;
}

json serialize_camera_settings(const CameraSettings& settings)
{
    json payload = {
        {"color_width", settings.color_width},
        {"color_height", settings.color_height},
        {"color_fps", settings.color_fps},
        {"auto_exposure", settings.auto_exposure},
        {"auto_white_balance", settings.auto_white_balance}
    };

#ifdef CAMERA_USB
    payload["usb_device_index"] = settings.usb_device_index;
#elif defined(CAMERA_REALSENSE)
    payload["depth_enabled"] = settings.depth_enabled;
    payload["depth_width"] = settings.depth_width;
    payload["depth_height"] = settings.depth_height;
    payload["depth_fps"] = settings.depth_fps;
#endif

    return payload;
}

json serialize_camera_settings_response(const CameraSettings& settings)
{
    json payload = {
        {"camera_type",
#ifdef CAMERA_REALSENSE
            "realsense"
#elif defined(CAMERA_USB)
            "usb"
#endif
        },
        {"settings", serialize_camera_settings(settings)},
        {"available_settings", serialize_available_settings()}
    };

    const auto& active = payload["settings"];
    payload["color_width"] = active.value("color_width", settings.color_width);
    payload["color_height"] = active.value("color_height", settings.color_height);
    payload["color_fps"] = active.value("color_fps", settings.color_fps);
    payload["auto_exposure"] = active.value("auto_exposure", settings.auto_exposure);
    payload["auto_white_balance"] = active.value("auto_white_balance", settings.auto_white_balance);
#ifdef CAMERA_USB
    payload["usb_device_index"] = active.value("usb_device_index", settings.usb_device_index);
#elif defined(CAMERA_REALSENSE)
    payload["depth_enabled"] = active.value("depth_enabled", settings.depth_enabled);
    payload["depth_width"] = active.value("depth_width", settings.depth_width);
    payload["depth_height"] = active.value("depth_height", settings.depth_height);
    payload["depth_fps"] = active.value("depth_fps", settings.depth_fps);
#endif

    return payload;
}

std::string json_response(const json& body)
{
    return body.dump();
}

std::string http_response(int status, const std::string& content_type,
                          const std::string& body, const std::string& extra_headers = {})
{
    const char* reason = status == 200 ? "OK" : status == 400 ? "Bad Request" :
        status == 401 ? "Unauthorized" : status == 404 ? "Not Found" : "Internal Server Error";
    return "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n"
        "Content-Type: " + content_type + "\r\nContent-Length: " + std::to_string(body.size()) +
        "\r\nConnection: close\r\n" + extra_headers + "\r\n" + body;
}

bool send_all(int socket, const std::string& data)
{
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t count = send(socket, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (count <= 0) return false;
        sent += static_cast<std::size_t>(count);
    }
    return true;
}

std::string header_value(const std::string& request, const std::string& name)
{
    const std::string prefix = name + ": ";
    const std::size_t begin = request.find(prefix);
    if (begin == std::string::npos) return {};
    const std::size_t value_begin = begin + prefix.size();
    const std::size_t end = request.find("\r\n", value_begin);
    return request.substr(value_begin, end == std::string::npos ? std::string::npos : end - value_begin);
}

std::string make_session_id()
{
    std::random_device device;
    std::mt19937_64 generator(device());
    return std::to_string(generator()) + std::to_string(generator());
}

std::string configured_value(const char* name, const char* fallback)
{
    const char* value = std::getenv(name);
    return value && *value ? value : fallback;
}

std::string read_web_page()
{
    const std::string paths[] = {
        std::string(VISION_WEB_ROOT) + "/index.html",
        "/home/ghost/core3/web_server/index.html",
        "web_server/index.html"
    };
    for (const auto& path : paths) {
        std::ifstream file(path);
        if (file) return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }
    return {};
}

std::string read_static_file(const std::string& file_name)
{
    const std::string paths[] = {
        std::string(VISION_WEB_ROOT) + "/" + file_name,
        "/home/ghost/core3/web_server/" + file_name,
        "web_server/" + file_name
    };
    for (const auto& path : paths) {
        std::ifstream file(path, std::ios::binary);
        if (file) return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }
    return {};
}
}

WebServer::WebServer(Logger& logger, SelectedCamera& camera, ServiceToggles& toggles)
    : logger_(logger), camera_(camera), toggles_(toggles), base_system_(logger, camera)
{
    camera_.register_frame_callback([this](const FrameContext& frame) {
        if (!frame.color || frame.color->empty()) {
            return;
        }
        std::lock_guard lock(stream_mutex_);
        latest_stream_frame_ = frame.color;
    });
}

WebServer::~WebServer()
{
    stop();
}

bool WebServer::start(std::uint16_t port)
{
    if (running_.exchange(true)) return true;

    listen_socket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_socket_ < 0) {
        running_ = false;
        logger_.log(LogLevel::ERROR, "WEB_SERVER", "Unable to create listening socket");
        return false;
    }

    int reuse = 1;
    setsockopt(listen_socket_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (bind(listen_socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 ||
        listen(listen_socket_, 16) < 0) {
        logger_.log(LogLevel::ERROR, "WEB_SERVER", "Unable to bind or listen on port " + std::to_string(port));
        close(listen_socket_);
        listen_socket_ = -1;
        running_ = false;
        return false;
    }

    port_ = port;
    server_thread_ = std::thread(&WebServer::accept_loop, this);
    logger_.log(LogLevel::INFO, "WEB_SERVER", "Web server listening on port " + std::to_string(port));
    return true;
}

void WebServer::stop() noexcept
{
    if (!running_.exchange(false)) return;
    stream_generation_.fetch_add(1, std::memory_order_release);
    base_system_.stop_recording();
    if (listen_socket_ >= 0) {
        shutdown(listen_socket_, SHUT_RDWR);
        close(listen_socket_);
        listen_socket_ = -1;
    }
    {
        std::lock_guard lock(clients_mutex_);
        for (const int socket : client_sockets_) shutdown(socket, SHUT_RDWR);
    }
    stop_camera();
    if (server_thread_.joinable()) server_thread_.join();
    std::vector<std::thread> client_threads;
    {
        std::lock_guard lock(clients_mutex_);
        client_threads.swap(client_threads_);
        client_sockets_.clear();
    }
    for (auto& thread : client_threads) {
        if (thread.joinable()) thread.join();
    }
    logger_.log(LogLevel::INFO, "WEB_SERVER", "Web server stopped");
}

void WebServer::accept_loop()
{
    while (running_) {
        const int client = accept(listen_socket_, nullptr, nullptr);
        if (client < 0) {
            if (running_) logger_.log(LogLevel::WARN, "WEB_SERVER", "Accept failed: " + std::string(std::strerror(errno)));
            continue;
        }
        {
            std::lock_guard lock(clients_mutex_);
            client_sockets_.push_back(client);
            client_threads_.emplace_back(&WebServer::handle_client, this, client);
        }
    }
}

bool WebServer::is_authenticated(const std::string& request) const
{
    const std::string cookie = header_value(request, "Cookie");
    const std::string prefix = "VISION_SESSION=";
    const std::size_t begin = cookie.find(prefix);
    if (begin == std::string::npos) return false;
    const std::size_t value_begin = begin + prefix.size();
    const std::size_t end = cookie.find(';', value_begin);
    const std::string session = cookie.substr(value_begin, end == std::string::npos ? std::string::npos : end - value_begin);
    std::lock_guard lock(sessions_mutex_);
    for (const auto& active : sessions_) if (active == session) return true;
    return false;
}

bool WebServer::start_camera()
{
    std::lock_guard lock(camera_control_mutex_);
    if (camera_.is_running()) {
        toggles_.camera_error = false;
        toggles_.camera_enabled = true;
        toggles_.processing_enabled = true;
        camera_.set_processing_enabled(true);
        return true;
    }
    if (!camera_.start()) {
        toggles_.camera_error = true;
        toggles_.camera_enabled = false;
        toggles_.processing_enabled = false;
        return false;
    }
    toggles_.camera_error = false;
    toggles_.camera_enabled = true;
    toggles_.processing_enabled = true;
    camera_.set_processing_enabled(true);
    return true;
}

void WebServer::stop_camera()
{
    std::lock_guard lock(camera_control_mutex_);
    stream_generation_.fetch_add(1, std::memory_order_release);
    if (!camera_.is_running()) {
        toggles_.camera_enabled = false;
        toggles_.processing_enabled = false;
        toggles_.camera_error = false;
        return;
    }
    camera_.set_processing_enabled(false);
    camera_.stop();
    toggles_.camera_enabled = false;
    toggles_.processing_enabled = false;
    toggles_.camera_error = false;
}

CameraSettings WebServer::camera_settings() const
{
    return camera_.settings();
}

bool WebServer::apply_camera_settings(const CameraSettings& settings)
{
    const bool was_recording = base_system_.is_recording();
    if (was_recording) {
        base_system_.stop_recording();
        {
            std::lock_guard stream_lock(stream_mutex_);
            latest_stream_frame_.reset();
        }
    }

    std::lock_guard lock(camera_control_mutex_);
    const bool was_running = camera_.is_running();
    if (was_running) {
        camera_.set_processing_enabled(false);
        camera_.stop();
    }

    const bool applied = camera_.apply_settings(settings);
    if (!applied) {
        if (was_recording) {
            base_system_.start_recording();
        }
        return false;
    }

    if (was_running || was_recording) {
        if (!camera_.start()) {
            if (was_recording) {
                base_system_.start_recording();
            }
            return false;
        }
        toggles_.camera_enabled = true;
        toggles_.processing_enabled = true;
        camera_.set_processing_enabled(true);
        stream_generation_.fetch_add(1, std::memory_order_release);
    }

    if (was_recording) {
        base_system_.start_recording();
    }
    return true;
}

void WebServer::handle_client(int client_socket)
{
    timeval send_timeout{1, 0};
    setsockopt(client_socket, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
    std::string request;
    char buffer[4096];
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (request.find("\r\n\r\n") == std::string::npos && request.size() < 65536 &&
           std::chrono::steady_clock::now() < deadline) {
        const ssize_t count = recv(client_socket, buffer, sizeof(buffer), 0);
        if (count <= 0) break;
        request.append(buffer, static_cast<std::size_t>(count));
    }
    const std::size_t header_end = request.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        close(client_socket);
        return;
    }

    const std::size_t request_line_end = request.find("\r\n");
    const std::string request_line = request.substr(0, request_line_end);
    const std::size_t first_space = request_line.find(' ');
    const std::size_t second_space = request_line.find(' ', first_space + 1);
    const std::string method = request_line.substr(0, first_space);
    const std::string target = request_line.substr(first_space + 1, second_space - first_space - 1);
    const std::string path = target.substr(0, target.find('?'));
    const std::size_t content_length = header_value(request, "Content-Length").empty() ? 0 :
        std::stoul(header_value(request, "Content-Length"));
    while (request.size() < header_end + 4 + content_length) {
        const ssize_t count = recv(client_socket, buffer, sizeof(buffer), 0);
        if (count <= 0) break;
        request.append(buffer, static_cast<std::size_t>(count));
    }
    const std::string body = request.substr(header_end + 4, content_length);

    if (method == "GET" && path == "/") {
        const std::string html = read_web_page();
        if (html.empty()) {
            send_all(client_socket, http_response(500, "text/plain", "Web UI is unavailable"));
        } else {
            send_all(client_socket, http_response(200, "text/html; charset=utf-8", html));
        }
    } else if (method == "GET" && path == "/app.js") {
        const std::string script = read_static_file("app.js");
        if (script.empty()) {
            send_all(client_socket, http_response(404, "text/plain", "app.js not found"));
        } else {
            send_all(client_socket, http_response(200, "application/javascript; charset=utf-8", script));
        }
    } else if (method == "POST" && path == "/api/login") {
        try {
            const json credentials = json::parse(body);
            const bool valid = credentials.value("username", "") == configured_value("VISION_AI_BOX_USERNAME", "admin") &&
                credentials.value("password", "") == configured_value("VISION_AI_BOX_PASSWORD", "change-me");
            if (!valid) {
                send_all(client_socket, http_response(401, "application/json", json_response({{"error", "invalid credentials"}})));
            } else {
                const std::string session = make_session_id();
                { std::lock_guard lock(sessions_mutex_); sessions_.push_back(session); }
                send_all(client_socket, http_response(200, "application/json", json_response({{"authenticated", true}}),
                    "Set-Cookie: VISION_SESSION=" + session + "; Path=/; HttpOnly; SameSite=Strict\r\n"));
                logger_.log(LogLevel::INFO, "AUTH", "Authenticated web session created");
            }
        } catch (const std::exception&) {
            send_all(client_socket, http_response(400, "application/json", json_response({{"error", "invalid JSON"}})));
        }
    } else if (!is_authenticated(request)) {
        send_all(client_socket, http_response(401, "application/json", json_response({{"error", "authentication required"}})));
    } else if (method == "GET" && path == "/dashboard") {
        const std::string html = read_web_page();
        send_all(client_socket, http_response(html.empty() ? 500 : 200,
            "text/html; charset=utf-8", html.empty() ? "Web UI is unavailable" : html));
    } else if (method == "GET" && path == "/api/logs") {
        const auto lines = logger_.read_all_logs();
        json payload = json::array();
        for (const auto& line : lines) payload.push_back(line);
        send_all(client_socket, http_response(200, "application/json", payload.dump()));
    } else if (method == "GET" && path == "/api/record/status") {
        send_all(client_socket, http_response(200, "application/json", json_response({
            {"recording", base_system_.is_recording()},
            {"current_file", base_system_.current_recording_file()}
        })));
    } else if (method == "POST" && path == "/api/record/start") {
        const bool started = base_system_.start_recording();
        send_all(client_socket, http_response(started ? 200 : 500, "application/json", json_response({
            {"recording", base_system_.is_recording()},
            {"current_file", base_system_.current_recording_file()}
        })));
    } else if (method == "POST" && path == "/api/record/stop") {
        const bool stopped = base_system_.stop_recording();
        send_all(client_socket, http_response(stopped ? 200 : 500, "application/json", json_response({
            {"recording", base_system_.is_recording()},
            {"current_file", base_system_.current_recording_file()}
        })));
    } else if (method == "POST" && path == "/api/record/restart") {
        const bool restarted = base_system_.restart_recording();
        send_all(client_socket, http_response(restarted ? 200 : 500, "application/json", json_response({
            {"recording", base_system_.is_recording()},
            {"current_file", base_system_.current_recording_file()}
        })));
    } else if (method == "POST" && path == "/api/record/capture") {
        const bool captured = base_system_.capture_frame();
        send_all(client_socket, http_response(captured ? 200 : 500, "application/json", json_response({
            {"captured", captured}
        })));
    } else if (method == "GET" && path == "/api/camera/settings") {
        send_all(client_socket, http_response(200, "application/json", serialize_camera_settings_response(camera_settings()).dump()));
    } else if (method == "POST" && path == "/api/camera/settings") {
        try {
            const json payload = json::parse(body);
            CameraSettings settings = camera_settings();
            settings.color_width = payload.value("color_width", settings.color_width);
            settings.color_height = payload.value("color_height", settings.color_height);
            settings.color_fps = payload.value("color_fps", settings.color_fps);
            settings.auto_exposure = payload.value("auto_exposure", settings.auto_exposure);
            settings.auto_white_balance = payload.value("auto_white_balance", settings.auto_white_balance);
#ifdef CAMERA_USB
            settings.usb_device_index = payload.value("usb_device_index", settings.usb_device_index);
#elif defined(CAMERA_REALSENSE)
            settings.depth_enabled = payload.value("depth_enabled", settings.depth_enabled);
            settings.depth_width = payload.value("depth_width", settings.depth_width);
            settings.depth_height = payload.value("depth_height", settings.depth_height);
            settings.depth_fps = payload.value("depth_fps", settings.depth_fps);
#endif
            const bool applied = apply_camera_settings(settings);
            send_all(client_socket, http_response(applied ? 200 : 500, "application/json", json_response({
                {"applied", applied},
                {"camera_type",
#ifdef CAMERA_REALSENSE
                    "realsense"
#elif defined(CAMERA_USB)
                    "usb"
#endif
                },
                {"settings", serialize_camera_settings(settings)},
                {"available_settings", serialize_available_settings()}
            })));
        } catch (const std::exception&) {
            send_all(client_socket, http_response(400, "application/json", json_response({{"error", "invalid JSON"}})));
        }
    } else if (method == "GET" && path == "/api/camera/status") {
        const bool running = camera_.is_running();
        const std::string status = running ? "connected" : (toggles_.camera_error.load() ? "error" : "disconnected");
        send_all(client_socket, http_response(200, "application/json", json_response({
            {"status", status},
            {"running", running},
            {"processing", toggles_.processing_enabled.load()}
        })));
    } else if (method == "POST" && path == "/api/camera/start") {
        const bool started = start_camera();
        if (started) stream_generation_.fetch_add(1, std::memory_order_release);
        send_all(client_socket, http_response(started ? 200 : 500, "application/json", json_response({{"running", camera_.is_running()}})));
    } else if (method == "POST" && path == "/api/camera/stop") {
        stop_camera();
        send_all(client_socket, http_response(200, "application/json", json_response({{"running", false}})));
    } else if (method == "GET" && path == "/api/camera/stream") {
        const std::uint64_t stream_generation = stream_generation_.load(std::memory_order_acquire);
        const std::string header = "HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=frame\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n";
        if (send_all(client_socket, header)) {
            while (running_ && camera_.is_running() &&
                   stream_generation == stream_generation_.load(std::memory_order_acquire)) {
                std::shared_ptr<cv::Mat> frame_ptr;
                {
                    std::lock_guard lock(stream_mutex_);
                    frame_ptr = latest_stream_frame_;
                }
                if (!frame_ptr || frame_ptr->empty()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }

                std::vector<uchar> encoded;
                if (!cv::imencode(".jpg", *frame_ptr, encoded, {cv::IMWRITE_JPEG_QUALITY, 80})) break;

                const std::string prefix = "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: " +
                    std::to_string(encoded.size()) + "\r\n\r\n";
                const std::string jpeg(reinterpret_cast<const char*>(encoded.data()), encoded.size());
                if (!send_all(client_socket, prefix) || !send_all(client_socket, jpeg) ||
                    !send_all(client_socket, "\r\n")) break;

                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
    } else {
        send_all(client_socket, http_response(404, "application/json", json_response({{"error", "not found"}})));
    }
    close(client_socket);
    std::lock_guard lock(clients_mutex_);
    client_sockets_.erase(std::remove(client_sockets_.begin(), client_sockets_.end(), client_socket), client_sockets_.end());
}
