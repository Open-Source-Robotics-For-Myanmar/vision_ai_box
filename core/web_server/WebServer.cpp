#include "WebServer.hpp"

#include "Logger.hpp"
#include "PluginManager.hpp"
#include "RateMeter.hpp"

#include <arpa/inet.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <opencv2/imgproc.hpp>
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

bool send_all(int socket, const std::vector<uchar>& data)
{
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t count = send(socket, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (count <= 0) return false;
        sent += static_cast<std::size_t>(count);
    }
    return true;
}

cv::Mat resize_for_stream(const cv::Mat& source, const StreamProfile& profile)
{
    const int width_limit = std::max(1, profile.max_width);
    const int height_limit = std::max(1, profile.max_height);
    const double scale = std::min(
        static_cast<double>(width_limit) / source.cols,
        static_cast<double>(height_limit) / source.rows);

    if (scale >= 1.0) {
        return source;
    }

    const cv::Size size(
        std::max(1, static_cast<int>(std::lround(source.cols * scale))),
        std::max(1, static_cast<int>(std::lround(source.rows * scale))));
    cv::Mat resized;
    cv::resize(source, resized, size, 0.0, 0.0, cv::INTER_AREA);
    return resized;
}

int configured_stream_fps()
{
    const char* value = std::getenv("VISION_AI_BOX_STREAM_FPS");
    if (!value || !*value) {
        return kDefaultStreamFps;
    }

    try {
        return std::clamp(std::stoi(value), 1, 60);
    } catch (const std::exception&) {
        return kDefaultStreamFps;
    }
}

struct StreamController
{
    explicit StreamController(int fps) : target_fps(fps) {}

    int step{0};
    int jpeg_quality{75};
    int target_fps{kDefaultStreamFps};
    int poor_samples{0};
    int good_samples{0};
    std::chrono::steady_clock::time_point last_change{};
    StreamMetrics metrics{};

    StreamProfile profile(const cv::Size& source_size) const
    {
        return StreamProfile::for_step(step, source_size, jpeg_quality, target_fps);
    }

    void observe(std::size_t bytes, double send_ms, const StreamProfile& current)
    {
        metrics.frames_sent++;
        metrics.bytes_sent += bytes;
        metrics.last_send_ms = send_ms;

        const double frame_interval_ms = 1000.0 / current.target_fps;
        const auto now = std::chrono::steady_clock::now();

        // Comparing observed against required bits per second reduces to this:
        // the encoded size sits on both sides of that inequality and cancels,
        // leaving a plain threshold on how long the send took.
        const bool congested = send_ms > frame_interval_ms * 0.90;

        if (congested) {
            good_samples = 0;
            // Overrunning the frame budget outright counts double, so a badly
            // congested link still reaches the threshold in two samples.
            poor_samples += send_ms > frame_interval_ms * 1.25 ? 2 : 1;
            if (poor_samples >= 3 && now - last_change >= kSettleInterval) {
                // Size the correction to the overrun. A send that took five
                // frame intervals needs roughly a fivefold cut in bytes, and
                // one notch per settling period would spend ten seconds
                // converging on a link that is already visibly failing.
                const int severity = static_cast<int>(send_ms / frame_interval_ms);
                step_down(std::clamp(severity, 1, 4));
                poor_samples = 0;
                last_change = now;
            }
            return;
        }

        poor_samples = 0;
        if (send_ms < frame_interval_ms * 0.50) {
            ++good_samples;
        } else {
            good_samples = 0;
        }

        if (good_samples >= 30 && now - last_change >= kRecoveryInterval) {
            step_up();
            good_samples = 0;
            last_change = now;
        }
    }

private:
    // A profile change needs time to take effect before its result can be
    // judged. Without this the controller re-reacts to sends that were already
    // in flight and walks the whole ladder down in a fraction of a second.
    static constexpr auto kSettleInterval = std::chrono::milliseconds(1000);
    static constexpr auto kRecoveryInterval = std::chrono::seconds(10);

    // Walk quality down inside the current rung first, then cut resolution.
    // Quality carries over into the new rung rather than resetting to its top:
    // a 0.75 scale change removes about 44% of the pixels, which a quality
    // jump would hand straight back, making the step counterproductive.
    void step_down(int notches)
    {
        for (int applied = 0; applied < notches; ++applied) {
            const StreamStep& rung = kStreamSteps[std::clamp(step, 0, kStreamStepCount - 1)];
            if (jpeg_quality > rung.min_quality) {
                jpeg_quality = std::max(rung.min_quality, jpeg_quality - 10);
                continue;
            }

            if (step >= kStreamStepCount - 1) {
                return;
            }
            ++step;
            const StreamStep& next = kStreamSteps[step];
            jpeg_quality = std::clamp(jpeg_quality, next.min_quality, next.max_quality);
        }
    }

    // The mirror image: raise quality inside the rung, and when moving to a
    // richer one drop to its floor so the pixel increase is not compounded by
    // a quality increase in the same move.
    void step_up()
    {
        const StreamStep& rung = kStreamSteps[std::clamp(step, 0, kStreamStepCount - 1)];
        if (jpeg_quality < rung.max_quality) {
            jpeg_quality = std::min(rung.max_quality, jpeg_quality + 5);
            return;
        }

        if (step > 0) {
            --step;
            jpeg_quality = kStreamSteps[step].min_quality;
        }
    }
};

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

std::string url_decode(const std::string& value)
{
    std::string decoded;
    decoded.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const std::string hex = value.substr(i + 1, 2);
            try {
                const unsigned char byte = static_cast<unsigned char>(std::stoul(hex, nullptr, 16));
                decoded.push_back(static_cast<char>(byte));
                i += 2;
            } catch (...) {
                decoded.push_back(value[i]);
            }
        } else if (value[i] == '+') {
            decoded.push_back(' ');
        } else {
            decoded.push_back(value[i]);
        }
    }
    return decoded;
}

std::string query_param(const std::string& query, const std::string& name)
{
    const std::string prefix = name + "=";
    const std::size_t begin = query.find(prefix);
    if (begin == std::string::npos) return {};
    std::size_t start = begin + prefix.size();
    std::size_t end = query.find('&', start);
    if (end == std::string::npos) end = query.size();
    return url_decode(query.substr(start, end - start));
}

std::string content_type_for_path(const std::string& file_path)
{
    const std::string lower = [&]() {
        std::string copy = file_path;
        std::transform(copy.begin(), copy.end(), copy.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return copy;
    }();

    if (lower.ends_with(".mp4")) return "video/mp4";
    if (lower.ends_with(".jpg") || lower.ends_with(".jpeg")) return "image/jpeg";
    if (lower.ends_with(".png")) return "image/png";
    if (lower.ends_with(".webp")) return "image/webp";
    if (lower.ends_with(".bmp")) return "image/bmp";
    if (lower.ends_with(".mkv")) return "video/x-matroska";
    if (lower.ends_with(".avi")) return "video/x-msvideo";
    if (lower.ends_with(".mov")) return "video/quicktime";
    return "application/octet-stream";
}
}

WebServer::WebServer(Logger& logger, SelectedCamera& camera, ServiceToggles& toggles, PluginManager* plugin_manager)
    : logger_(logger), camera_(camera), toggles_(toggles), plugin_manager_(plugin_manager), base_system_(logger, camera)
{
    camera_.register_frame_callback([this](const FrameContext& frame) {
        latest_stream_frame_.push(frame);
    });
}

std::shared_ptr<std::vector<uchar>> WebServer::encoded_frame(
    std::uint64_t sequence, const cv::Mat& source, const StreamProfile& profile,
    double& encode_ms)
{
    std::lock_guard lock(encoded_cache_mutex_);
    if (encoded_cache_data_ && encoded_cache_sequence_ == sequence &&
        encoded_cache_profile_.max_width == profile.max_width &&
        encoded_cache_profile_.max_height == profile.max_height &&
        encoded_cache_profile_.jpeg_quality == profile.jpeg_quality) {
        encode_ms = 0.0;
        return encoded_cache_data_;
    }

    const auto encode_start = std::chrono::steady_clock::now();
    const cv::Mat stream_frame = resize_for_stream(source, profile);
    auto encoded = std::make_shared<std::vector<uchar>>();
    if (!cv::imencode(".jpg", stream_frame, *encoded,
                      {cv::IMWRITE_JPEG_QUALITY, profile.jpeg_quality})) {
        return {};
    }

    encode_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - encode_start).count();
    encoded_cache_sequence_ = sequence;
    encoded_cache_profile_ = profile;
    encoded_cache_data_ = encoded;
    return encoded;
}

std::shared_ptr<StreamClientStats> WebServer::register_stream_client()
{
    auto stats = std::make_shared<StreamClientStats>();
    std::lock_guard lock(stream_stats_mutex_);
    stream_stats_.push_back(stats);
    return stats;
}

void WebServer::unregister_stream_client(const std::shared_ptr<StreamClientStats>& stats)
{
    std::lock_guard lock(stream_stats_mutex_);
    stream_stats_.erase(std::remove(stream_stats_.begin(), stream_stats_.end(), stats),
                        stream_stats_.end());
}

StreamSummary WebServer::stream_summary() const
{
    // Matches the RateMeter window, so a stalled client reads as zero rather
    // than holding the rate it managed before it stopped keeping up.
    constexpr std::int64_t stale_after_ns = 2'000'000'000;
    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    StreamSummary summary;
    std::lock_guard lock(stream_stats_mutex_);
    for (const auto& stats : stream_stats_) {
        if (!stats) {
            continue;
        }
        // Report the worst-off viewer, since that is the one that reveals a
        // problem; with a single dashboard open it is simply that viewer.
        const std::int64_t last_ns = stats->last_frame_ns.load(std::memory_order_acquire);
        const bool stale = last_ns == 0 || now_ns - last_ns > stale_after_ns;
        const double fps = stale ? 0.0 : stats->delivered_fps.load(std::memory_order_acquire);
        if (summary.clients == 0 || fps < summary.delivered_fps) {
            summary.delivered_fps = fps;
            summary.frame_width = stats->frame_width.load(std::memory_order_acquire);
            summary.frame_height = stats->frame_height.load(std::memory_order_acquire);
            summary.jpeg_quality = stats->jpeg_quality.load(std::memory_order_acquire);
            summary.frames_skipped = stats->frames_skipped.load(std::memory_order_acquire);
        }
        ++summary.clients;
    }
    return summary;
}

WebServer::~WebServer()
{
    stop();
}

bool WebServer::start(std::uint16_t port)
{
    if (running_.exchange(true)) return true;
    latest_stream_frame_.start();

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
    latest_stream_frame_.stop();
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
            if (client_sockets_.size() >= kMaxClientConnections) {
                logger_.log(LogLevel::WARN, "WEB_SERVER", "Rejecting client connection: limit reached");
                shutdown(client, SHUT_RDWR);
                close(client);
                continue;
            }
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
    if (!toggles_.request_start_camera()) {
        logger_.log(LogLevel::WARN, "CAMERA", "Rejected camera start request due to invalid system state");
        return false;
    }

    if (camera_.is_running()) {
        toggles_.camera_error.store(false, std::memory_order_release);
        toggles_.processing_enabled.store(true, std::memory_order_release);
        camera_.set_processing_enabled(true);
        logger_.log(LogLevel::INFO, "CAMERA", "Camera Connected");
        return true;
    }

    if (!camera_.check_device_state()) {
        toggles_.camera_error.store(true, std::memory_order_release);
        toggles_.processing_enabled.store(false, std::memory_order_release);
        camera_.set_processing_enabled(false);
        logger_.log(LogLevel::WARN, "CAMERA", "Camera Unplugged");
        return false;
    }

    if (!camera_.start()) {
        toggles_.camera_error.store(true, std::memory_order_release);
        toggles_.processing_enabled.store(false, std::memory_order_release);
        camera_.set_processing_enabled(false);
        logger_.log(LogLevel::WARN, "CAMERA", "Camera Unplugged");
        return false;
    }

    toggles_.camera_error.store(false, std::memory_order_release);
    toggles_.processing_enabled.store(true, std::memory_order_release);
    camera_.set_processing_enabled(true);
    logger_.log(LogLevel::INFO, "CAMERA", "Camera Connected");
    return true;
}

void WebServer::stop_camera()
{
    std::lock_guard lock(camera_control_mutex_);
    stream_generation_.fetch_add(1, std::memory_order_release);
    if (!toggles_.request_stop_camera()) {
        logger_.log(LogLevel::WARN, "CAMERA", "Camera stop rejected while shutting down");
        return;
    }

    if (camera_.is_running()) {
        camera_.set_processing_enabled(false);
        camera_.stop();
    }

    logger_.log(LogLevel::INFO, "CAMERA", "Camera turned off");
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
        latest_stream_frame_.clear();
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
        toggles_.camera_enabled.store(true, std::memory_order_release);
        toggles_.processing_enabled.store(true, std::memory_order_release);
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
    timeval send_timeout{5, 0};
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
    } else if (method == "GET" && path == "/api/plugins") {
        if (!plugin_manager_) {
            send_all(client_socket, http_response(500, "application/json", json_response({{"error", "plugin manager unavailable"}})));
        } else {
            const json plugins = plugin_manager_->get_all_plugin_info();
            send_all(client_socket, http_response(200, "application/json", plugins.dump()));
        }
    } else if (method == "POST" && path == "/api/plugins/scan") {
        if (!plugin_manager_) {
            send_all(client_socket, http_response(500, "application/json", json_response({{"error", "plugin manager unavailable"}})));
        } else {
            plugin_manager_->scan_plugins();
            send_all(client_socket, http_response(200, "application/json", json_response({
                {"success", true},
                {"plugins", plugin_manager_->get_all_plugin_info()}
            })));
        }
    } else if (method == "POST" && path == "/api/plugins") {
        if (!plugin_manager_) {
            send_all(client_socket, http_response(500, "application/json", json_response({{"error", "plugin manager unavailable"}})));
        } else {
            try {
                const json payload = json::parse(body);
                const std::string name = payload.value("name", "");
                if (name.empty()) {
                    send_all(client_socket, http_response(400, "application/json", json_response({{"error", "plugin name required"}})));
                    return;
                }

                const bool enabled = payload.value("enabled", false);
                const bool success = enabled
                    ? plugin_manager_->select_plugin(name)
                    : plugin_manager_->unload_plugin(name);
                const json plugin_info = plugin_manager_->get_all_plugin_info();
                send_all(client_socket, http_response(success ? 200 : 404, "application/json", json_response({
                    {"success", success},
                    {"name", name},
                    {"enabled", enabled},
                    {"plugins", plugin_info}
                })));
            } catch (const std::exception&) {
                send_all(client_socket, http_response(400, "application/json", json_response({{"error", "invalid JSON"}})));
            }
        }
    } else if (method == "POST" && path == "/api/plugins/unload") {
        if (!plugin_manager_) {
            send_all(client_socket, http_response(500, "application/json", json_response({{"error", "plugin manager unavailable"}})));
        } else {
            try {
                const json payload = json::parse(body);
                const std::string name = payload.value("name", "");
                const bool success = name.empty()
                    ? plugin_manager_->unload_all_plugins()
                    : plugin_manager_->unload_plugin(name);
                send_all(client_socket, http_response(success ? 200 : 404, "application/json", json_response({
                    {"success", success},
                    {"name", name},
                    {"plugins", plugin_manager_->get_all_plugin_info()}
                })));
            } catch (const std::exception&) {
                send_all(client_socket, http_response(400, "application/json", json_response({{"error", "invalid JSON"}})));
            }
        }
    } else if (method == "GET" && path == "/api/plugins/events") {
        // Detections travel on their own channel rather than being drawn into
        // the JPEG: that keeps inference rate and stream rate independent, and
        // keeps the overlay crisp when the ladder degrades the video.
        const std::string header =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: close\r\n"
            "X-Accel-Buffering: no\r\n\r\n";
        if (send_all(client_socket, header)) {
            std::uint64_t last_sent_sequence = 0;
            bool sent_any = false;
            auto last_activity = std::chrono::steady_clock::now();
            while (running_) {
                PluginResult result;
                const bool have_result = plugin_manager_ && plugin_manager_->latest_result(result);
                const auto now = std::chrono::steady_clock::now();

                if (have_result && (!sent_any || result.sequence != last_sent_sequence)) {
                    last_sent_sequence = result.sequence;
                    sent_any = true;
                    const json payload = result;
                    if (!send_all(client_socket, "data: " + payload.dump() + "\n\n")) {
                        break;
                    }
                    last_activity = now;
                } else if (now - last_activity >= std::chrono::seconds(15)) {
                    // A comment frame keeps the idle connection from being
                    // dropped while no plugin is producing anything.
                    if (!send_all(client_socket, ": keep-alive\n\n")) {
                        break;
                    }
                    last_activity = now;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(33));
            }
        }
    } else if (method == "POST" && path == "/api/plugins/settings") {
        if (!plugin_manager_) {
            send_all(client_socket, http_response(500, "application/json", json_response({{"error", "plugin manager unavailable"}})));
        } else {
            try {
                const json payload = json::parse(body);
                const std::string name = payload.value("name", "");
                if (name.empty() || !payload.contains("settings")) {
                    send_all(client_socket, http_response(400, "application/json", json_response({{"error", "plugin name and settings required"}})));
                } else {
                    plugin_manager_->update_plugin_settings(name, payload.at("settings"));
                    send_all(client_socket, http_response(200, "application/json", json_response({
                        {"success", true},
                        {"plugins", plugin_manager_->get_all_plugin_info()}
                    })));
                }
            } catch (const std::exception&) {
                send_all(client_socket, http_response(400, "application/json", json_response({{"error", "invalid JSON"}})));
            }
        }
    } else if (method == "GET" && path == "/api/query/media") {
        const std::string raw_query = target.find('?') == std::string::npos ? "" : target.substr(target.find('?') + 1);
        const std::string request_path = query_param(raw_query, "path");

        if (request_path.empty()) {
            const json media = base_system_.discover_media_library();
            send_all(client_socket, http_response(200, "application/json", media.dump()));
            return;
        }

        const std::filesystem::path resolved = std::filesystem::weakly_canonical(std::filesystem::path(request_path));
        const std::filesystem::path root = std::filesystem::current_path() / "media";
        const std::string canonical = resolved.string();
        const bool is_within_media = canonical.rfind(root.string(), 0) == 0 || canonical.rfind("media", 0) == 0;
        if (!std::filesystem::exists(resolved) || !std::filesystem::is_regular_file(resolved) || !is_within_media) {
            send_all(client_socket, http_response(404, "application/json", json_response({{"error", "media not found"}})));
        } else {
            std::ifstream input(resolved, std::ios::binary);
            if (!input) {
                send_all(client_socket, http_response(500, "application/json", json_response({{"error", "unable to read file"}})));
            } else {
                std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
                const std::string headers =
                    "Content-Type: " + content_type_for_path(canonical) + "\r\n"
                    + "Content-Length: " + std::to_string(content.size()) + "\r\n"
                    + "Content-Disposition: inline; filename=\"" + resolved.filename().string() + "\"\r\n"
                    + "Cache-Control: no-cache\r\n"
                    + "Connection: close\r\n\r\n";
                send_all(client_socket, "HTTP/1.1 200 OK\r\n" + headers + content);
            }
        }
    } else if (method == "GET" && path == "/api/record/status") {
        const bool recording = base_system_.is_recording();
        send_all(client_socket, http_response(200, "application/json", json_response({
            {"recording", recording},
            {"elapsed_seconds", base_system_.get_elapsed_seconds()},
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
        const bool enabled = toggles_.camera_enabled.load(std::memory_order_acquire);
        const bool connected = camera_.check_device_state();
        const bool running = camera_.is_running();
        const bool error = toggles_.camera_error.load(std::memory_order_acquire);
        const double fps = camera_.measured_fps();
        const StreamSummary stream = stream_summary();

        send_all(client_socket, http_response(200, "application/json", json_response({
            {"enabled", enabled},
            {"connected", connected},
            {"running", running},
            {"error", error},
            {"fps", fps},
            {"stream_clients", stream.clients},
            {"stream_fps", stream.delivered_fps},
            {"stream_width", stream.frame_width},
            {"stream_height", stream.frame_height},
            {"stream_quality", stream.jpeg_quality},
            {"stream_frames_skipped", stream.frames_skipped}
        })));
    } else if (method == "POST" && path == "/api/camera/start") {
        const bool started = start_camera();
        if (started) stream_generation_.fetch_add(1, std::memory_order_release);
        send_all(client_socket, http_response(started ? 200 : 500, "application/json", json_response({{"running", camera_.is_running()}})));
    } else if (method == "POST" && path == "/api/camera/stop") {
        stop_camera();
        send_all(client_socket, http_response(200, "application/json", json_response({{"running", false}})));
    } else if (method == "GET" && path == "/api/camera/stream") {
        // A default send buffer holds seconds of video, so send() returns long
        // before the bytes reach the wire and a weakening link stays invisible
        // until the buffer finally fills, at which point the delay arrives as a
        // cliff. Sizing it to a few frames makes send duration a live reading of
        // what the link can absorb. Linux doubles the requested value.
        int stream_send_buffer = 64 * 1024;
        setsockopt(client_socket, SOL_SOCKET, SO_SNDBUF,
                   &stream_send_buffer, sizeof(stream_send_buffer));

        const std::uint64_t stream_generation = stream_generation_.load(std::memory_order_acquire);
        const std::string header = "HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=frame\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n";
        if (send_all(client_socket, header)) {
            std::uint64_t last_sequence = 0;
            bool first_frame = true;
            StreamController controller(configured_stream_fps());
            RateMeter delivered_meter;
            const std::shared_ptr<StreamClientStats> stats = register_stream_client();
            auto next_frame_deadline = std::chrono::steady_clock::now();
            while (running_ && camera_.is_running() &&
                   stream_generation == stream_generation_.load(std::memory_order_acquire)) {
                std::shared_ptr<cv::Mat> frame_ptr;
                std::uint64_t sequence = 0;
                if (!latest_stream_frame_.wait_for_newer(last_sequence, frame_ptr, sequence)) {
                    continue;
                }
                // The camera sequence is already well past zero by the time a
                // client attaches, so the first frame is not a skip.
                if (!first_frame && sequence > last_sequence + 1) {
                    controller.metrics.frames_skipped += sequence - last_sequence - 1;
                }
                first_frame = false;
                last_sequence = sequence;

                const StreamProfile profile = controller.profile(frame_ptr->size());
                double encode_ms = 0.0;
                const std::shared_ptr<std::vector<uchar>> encoded =
                    encoded_frame(sequence, *frame_ptr, profile, encode_ms);
                if (!encoded) break;
                controller.metrics.last_encode_ms = encode_ms;

                const std::string prefix = "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: " +
                    std::to_string(encoded->size()) + "\r\n\r\n";
                const auto send_start = std::chrono::steady_clock::now();
                if (!send_all(client_socket, prefix) || !send_all(client_socket, *encoded) ||
                    !send_all(client_socket, "\r\n")) break;
                const double send_ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - send_start).count();
                controller.observe(encoded->size(), send_ms, profile);

                const auto delivered_at = std::chrono::steady_clock::now();
                delivered_meter.record(delivered_at);
                stats->delivered_fps.store(delivered_meter.rate(delivered_at), std::memory_order_release);
                stats->frame_width.store(profile.max_width, std::memory_order_release);
                stats->frame_height.store(profile.max_height, std::memory_order_release);
                stats->jpeg_quality.store(profile.jpeg_quality, std::memory_order_release);
                stats->frames_skipped.store(controller.metrics.frames_skipped, std::memory_order_release);
                stats->last_frame_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    delivered_at.time_since_epoch()).count(), std::memory_order_release);

                // Microseconds rather than milliseconds: integer division of
                // 1000 by a 15 fps target yields 66 ms, pacing at 15.15 fps.
                next_frame_deadline = std::max(next_frame_deadline, delivered_at);
                next_frame_deadline += std::chrono::microseconds(1000000 / profile.target_fps);
                std::this_thread::sleep_until(next_frame_deadline);
            }
            unregister_stream_client(stats);
            logger_.log(LogLevel::INFO, "WEB_SERVER",
                "Stream ended: frames=" + std::to_string(controller.metrics.frames_sent) +
                ", skipped=" + std::to_string(controller.metrics.frames_skipped) +
                ", bytes=" + std::to_string(controller.metrics.bytes_sent) +
                ", quality=" + std::to_string(controller.jpeg_quality) +
                ", step=" + std::to_string(controller.step) +
                ", encode_ms=" + std::to_string(controller.metrics.last_encode_ms) +
                ", send_ms=" + std::to_string(controller.metrics.last_send_ms));
        }
    } else {
        send_all(client_socket, http_response(404, "application/json", json_response({{"error", "not found"}})));
    }

    {
        std::lock_guard lock(clients_mutex_);
        client_sockets_.erase(std::remove(client_sockets_.begin(), client_sockets_.end(), client_socket), client_sockets_.end());
    }

    close(client_socket);
}
