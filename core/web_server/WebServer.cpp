#define CPPHTTPLIB_THREAD_POOL_COUNT 64

#include "WebServer.hpp"

#include "H264Stream.hpp"
#include "Logger.hpp"
#include "PluginManager.hpp"
#include "RateMeter.hpp"
#include "httplib.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <opencv2/imgproc.hpp>
#include <random>
#include <thread>

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

cv::Mat resize_for_stream(const cv::Mat& source, const StreamProfile& profile)
{
    const int width_limit = std::max(2, H264Stream::even(profile.max_width));
    const int height_limit = std::max(2, H264Stream::even(profile.max_height));
    const double scale = std::min(
        static_cast<double>(width_limit) / source.cols,
        static_cast<double>(height_limit) / source.rows);

    cv::Size size = source.size();
    if (scale < 1.0) {
        size = {
            std::max(2, H264Stream::even(static_cast<int>(std::lround(source.cols * scale)))),
            std::max(2, H264Stream::even(static_cast<int>(std::lround(source.rows * scale))))
        };
    } else {
        size = {H264Stream::even(source.cols), H264Stream::even(source.rows)};
    }

    if (size == source.size() && source.isContinuous()) {
        return source;
    }

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
        const bool congested = send_ms > frame_interval_ms * 0.90;

        if (congested) {
            good_samples = 0;
            poor_samples += send_ms > frame_interval_ms * 1.25 ? 2 : 1;
            if (poor_samples >= 3 && now - last_change >= kSettleInterval) {
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
    static constexpr auto kSettleInterval = std::chrono::milliseconds(1000);
    static constexpr auto kRecoveryInterval = std::chrono::seconds(10);

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
            jpeg_quality = std::clamp(jpeg_quality, kStreamSteps[step].min_quality, kStreamSteps[step].max_quality);
        }
    }

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
        "web_server/" + file_name
    };
    for (const auto& path : paths) {
        std::ifstream file(path, std::ios::binary);
        if (file) return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }
    return {};
}

std::string content_type_for_path(const std::string& file_path)
{
    std::string lower = file_path;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
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

std::string session_from_cookie(const std::string& cookie)
{
    const std::string prefix = "VISION_SESSION=";
    const std::size_t begin = cookie.find(prefix);
    if (begin == std::string::npos) return {};
    const std::size_t value_begin = begin + prefix.size();
    const std::size_t end = cookie.find(';', value_begin);
    return cookie.substr(value_begin, end == std::string::npos ? std::string::npos : end - value_begin);
}

bool send_sse(httplib::DataSink& sink, const std::string& event, const json& payload)
{
    const std::string body = "event: " + event + "\ndata: " + payload.dump() + "\n\n";
    return sink.write(body.data(), body.size());
}

void send_json(httplib::Response& response, int status, const json& body)
{
    response.status = status;
    response.set_content(body.dump(), "application/json");
}

bool plugin_manager_ready(PluginManager* plugin_manager, httplib::Response& response)
{
    if (plugin_manager) {
        return true;
    }
    send_json(response, 500, {{"error", "plugin manager unavailable"}});
    return false;
}
}

WebServer::WebServer(Logger& logger, SelectedCamera& camera, ServiceToggles& toggles, PluginManager* plugin_manager)
    : logger_(logger), camera_(camera), toggles_(toggles), plugin_manager_(plugin_manager),
      base_system_(logger, camera), server_(std::make_unique<httplib::Server>())
{
    camera_.register_frame_callback([this](const FrameContext& frame) {
        latest_stream_frame_.push(frame);
    });
}

WebServer::~WebServer()
{
    stop();
}

bool WebServer::start(std::uint16_t port)
{
    if (running_.exchange(true)) {
        return true;
    }

    latest_stream_frame_.start();
    register_routes();
    port_ = port;
    server_->new_task_queue = [] { return new httplib::ThreadPool(64); };

    server_thread_ = std::thread([this, port] {
        if (!server_->listen("0.0.0.0", port)) {
            logger_.log(LogLevel::ERROR, "WEB_SERVER", "Unable to bind or listen on port " + std::to_string(port));
            running_ = false;
        }
    });

    for (int attempt = 0; attempt < 50 && running_; ++attempt) {
        if (server_->is_running()) {
            logger_.log(LogLevel::INFO, "WEB_SERVER", "Web server listening on port " + std::to_string(port));
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    if (!server_->is_running()) {
        logger_.log(LogLevel::ERROR, "WEB_SERVER", "Web server failed to start on port " + std::to_string(port));
        stop();
        return false;
    }
    return true;
}

void WebServer::stop() noexcept
{
    if (!running_.exchange(false)) {
        return;
    }

    stream_generation_.fetch_add(1, std::memory_order_release);
    latest_stream_frame_.stop();
    base_system_.stop_recording();
    stop_camera();
    if (server_) {
        server_->stop();
    }
    if (server_thread_.joinable()) {
        server_thread_.join();
    }
    logger_.log(LogLevel::INFO, "WEB_SERVER", "Web server stopped");
}

bool WebServer::is_authenticated(const httplib::Request& request) const
{
    const std::string session = session_from_cookie(request.get_header_value("Cookie"));
    if (session.empty()) {
        return false;
    }
    std::lock_guard lock(sessions_mutex_);
    return std::find(sessions_.begin(), sessions_.end(), session) != sessions_.end();
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
    stream_stats_.erase(std::remove(stream_stats_.begin(), stream_stats_.end(), stats), stream_stats_.end());
}

StreamSummary WebServer::stream_summary() const
{
    constexpr std::int64_t stale_after_ns = 2'000'000'000;
    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    StreamSummary summary;
    std::lock_guard lock(stream_stats_mutex_);
    for (const auto& stats : stream_stats_) {
        if (!stats) {
            continue;
        }
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

nlohmann::json WebServer::camera_status_json() const
{
    const StreamSummary stream = stream_summary();
    return {
        {"enabled", toggles_.camera_enabled.load(std::memory_order_acquire)},
        {"connected", camera_.check_device_state()},
        {"running", camera_.is_running()},
        {"error", toggles_.camera_error.load(std::memory_order_acquire)},
        {"fps", camera_.measured_fps()},
        {"stream_clients", stream.clients},
        {"stream_fps", stream.delivered_fps},
        {"stream_width", stream.frame_width},
        {"stream_height", stream.frame_height},
        {"stream_quality", stream.jpeg_quality},
        {"stream_frames_skipped", stream.frames_skipped},
        {"codec", "h264"}
    };
}

nlohmann::json WebServer::recording_status_json() const
{
    return {
        {"recording", base_system_.is_recording()},
        {"elapsed_seconds", base_system_.get_elapsed_seconds()},
        {"current_file", base_system_.current_recording_file()}
    };
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

    if (!camera_.check_device_state() || !camera_.start()) {
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

bool WebServer::stream_h264(httplib::DataSink& sink)
{
    const std::uint64_t stream_generation = stream_generation_.load(std::memory_order_acquire);
    StreamController controller(configured_stream_fps());
    RateMeter delivered_meter;
    const auto stats = register_stream_client();
    H264Stream encoder;

    std::uint64_t last_sequence = 0;
    bool first_frame = true;
    auto next_frame_deadline = std::chrono::steady_clock::now();
    int encode_width = 0;
    int encode_height = 0;

    while (running_ && camera_.is_running() &&
           stream_generation == stream_generation_.load(std::memory_order_acquire) &&
           sink.is_writable()) {
        std::shared_ptr<cv::Mat> frame_ptr;
        std::uint64_t sequence = 0;
        if (!latest_stream_frame_.wait_for_newer(last_sequence, frame_ptr, sequence)) {
            continue;
        }
        if (!first_frame && sequence > last_sequence + 1) {
            controller.metrics.frames_skipped += sequence - last_sequence - 1;
        }
        first_frame = false;
        last_sequence = sequence;

        const StreamProfile profile = controller.profile(frame_ptr->size());
        const cv::Mat stream_frame = resize_for_stream(*frame_ptr, profile);
        if (stream_frame.empty()) {
            break;
        }

        if (encode_width == 0) {
            const int bitrate = H264Stream::bitrate_for(
                stream_frame.cols, stream_frame.rows, profile.target_fps, profile.jpeg_quality);
            if (!encoder.open(stream_frame.cols, stream_frame.rows, profile.target_fps, bitrate)) {
                logger_.log(LogLevel::ERROR, "WEB_SERVER", "H.264 encoder failed to open");
                break;
            }
            encode_width = encoder.width();
            encode_height = encoder.height();
            const auto& init = encoder.init_segment();
            if (init.empty() || !sink.write(reinterpret_cast<const char*>(init.data()), init.size())) {
                break;
            }
        } else if (H264Stream::even(stream_frame.cols) != encode_width ||
                   H264Stream::even(stream_frame.rows) != encode_height) {
            // MSE cannot change SPS/PPS mid-stream; end so the client reconnects.
            break;
        }

        const auto encode_start = std::chrono::steady_clock::now();
        std::vector<std::uint8_t> fragment;
        if (!encoder.encode(stream_frame, fragment)) {
            break;
        }
        controller.metrics.last_encode_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - encode_start).count();
        if (fragment.empty()) {
            continue;
        }

        const auto send_start = std::chrono::steady_clock::now();
        if (!sink.write(reinterpret_cast<const char*>(fragment.data()), fragment.size())) {
            break;
        }
        const double send_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - send_start).count();
        controller.observe(fragment.size(), send_ms, profile);

        const auto delivered_at = std::chrono::steady_clock::now();
        delivered_meter.record(delivered_at);
        stats->delivered_fps.store(delivered_meter.rate(delivered_at), std::memory_order_release);
        stats->frame_width.store(encode_width, std::memory_order_release);
        stats->frame_height.store(encode_height, std::memory_order_release);
        stats->jpeg_quality.store(profile.jpeg_quality, std::memory_order_release);
        stats->frames_skipped.store(controller.metrics.frames_skipped, std::memory_order_release);
        stats->last_frame_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
            delivered_at.time_since_epoch()).count(), std::memory_order_release);

        next_frame_deadline = std::max(next_frame_deadline, delivered_at);
        next_frame_deadline += std::chrono::microseconds(1000000 / profile.target_fps);
        std::this_thread::sleep_until(next_frame_deadline);
    }

    unregister_stream_client(stats);
    logger_.log(LogLevel::INFO, "WEB_SERVER",
        "H.264 stream ended: frames=" + std::to_string(controller.metrics.frames_sent) +
        ", skipped=" + std::to_string(controller.metrics.frames_skipped) +
        ", bytes=" + std::to_string(controller.metrics.bytes_sent) +
        ", quality=" + std::to_string(controller.jpeg_quality) +
        ", step=" + std::to_string(controller.step));
    return true;
}

bool WebServer::write_sse(httplib::DataSink& sink)
{
    std::uint64_t log_cursor = 0;
    std::uint64_t last_detection_sequence = 0;
    std::string last_status;
    std::string last_plugins;
    std::string last_recording;
    auto last_activity = std::chrono::steady_clock::now();
    bool sent_logs = false;

    const auto logs = logger_.read_logs_after(log_cursor);
    if (!send_sse(sink, "logs", json{{"reset", true}, {"lines", logs}})) {
        return false;
    }
    sent_logs = true;

    while (running_ && sink.is_writable()) {
        const json status = camera_status_json();
        const std::string status_dump = status.dump();
        if (status_dump != last_status) {
            if (!send_sse(sink, "status", status)) {
                return false;
            }
            last_status = status_dump;
            last_activity = std::chrono::steady_clock::now();
        }

        const json recording = recording_status_json();
        const std::string recording_dump = recording.dump();
        if (recording_dump != last_recording) {
            if (!send_sse(sink, "recording", recording)) {
                return false;
            }
            last_recording = recording_dump;
            last_activity = std::chrono::steady_clock::now();
        }

        if (plugin_manager_) {
            const json plugins = plugin_manager_->get_all_plugin_info();
            const std::string plugins_dump = plugins.dump();
            if (plugins_dump != last_plugins) {
                if (!send_sse(sink, "plugins", plugins)) {
                    return false;
                }
                last_plugins = plugins_dump;
                last_activity = std::chrono::steady_clock::now();
            }

            PluginResult result;
            if (plugin_manager_->latest_result(result) && result.sequence != last_detection_sequence) {
                last_detection_sequence = result.sequence;
                if (!send_sse(sink, "detections", result)) {
                    return false;
                }
                last_activity = std::chrono::steady_clock::now();
            }
        }

        const auto new_logs = logger_.read_logs_after(log_cursor);
        if (!new_logs.empty()) {
            if (!send_sse(sink, "logs", json{{"reset", !sent_logs}, {"lines", new_logs}})) {
                return false;
            }
            sent_logs = true;
            last_activity = std::chrono::steady_clock::now();
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - last_activity >= std::chrono::seconds(15)) {
            const std::string keep_alive = ": keep-alive\n\n";
            if (!sink.write(keep_alive.data(), keep_alive.size())) {
                return false;
            }
            last_activity = now;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return true;
}

void WebServer::register_routes()
{
    auto& server = *server_;

    server.set_pre_routing_handler([this](const httplib::Request& request, httplib::Response& response) {
        const bool public_path = request.path == "/" || request.path == "/app.js" || request.path == "/api/login";
        if (public_path || is_authenticated(request)) {
            return httplib::Server::HandlerResponse::Unhandled;
        }
        send_json(response, 401, {{"error", "authentication required"}});
        return httplib::Server::HandlerResponse::Handled;
    });

    server.Get("/", [](const httplib::Request&, httplib::Response& response) {
        const std::string html = read_web_page();
        if (html.empty()) {
            response.status = 500;
            response.set_content("Web UI is unavailable", "text/plain");
            return;
        }
        response.set_content(html, "text/html; charset=utf-8");
    });

    server.Get("/dashboard", [](const httplib::Request&, httplib::Response& response) {
        const std::string html = read_web_page();
        if (html.empty()) {
            response.status = 500;
            response.set_content("Web UI is unavailable", "text/plain");
            return;
        }
        response.set_content(html, "text/html; charset=utf-8");
    });

    server.Get("/app.js", [](const httplib::Request&, httplib::Response& response) {
        const std::string script = read_static_file("app.js");
        if (script.empty()) {
            response.status = 404;
            response.set_content("app.js not found", "text/plain");
            return;
        }
        response.set_content(script, "application/javascript; charset=utf-8");
    });

    server.Post("/api/login", [this](const httplib::Request& request, httplib::Response& response) {
        try {
            const json credentials = json::parse(request.body);
            const bool valid = credentials.value("username", "") == configured_value("VISION_AI_BOX_USERNAME", "admin") &&
                credentials.value("password", "") == configured_value("VISION_AI_BOX_PASSWORD", "change-me");
            if (!valid) {
                send_json(response, 401, {{"error", "invalid credentials"}});
                return;
            }
            const std::string session = make_session_id();
            {
                std::lock_guard lock(sessions_mutex_);
                sessions_.push_back(session);
            }
            response.set_header("Set-Cookie", "VISION_SESSION=" + session + "; Path=/; HttpOnly; SameSite=Strict");
            send_json(response, 200, {{"authenticated", true}});
            logger_.log(LogLevel::INFO, "AUTH", "Authenticated web session created");
        } catch (const std::exception&) {
            send_json(response, 400, {{"error", "invalid JSON"}});
        }
    });

    server.Get("/api/logs", [this](const httplib::Request&, httplib::Response& response) {
        send_json(response, 200, logger_.read_all_logs());
    });

    server.Get("/api/events", [this](const httplib::Request&, httplib::Response& response) {
        response.set_header("Cache-Control", "no-cache");
        response.set_header("Connection", "close");
        response.set_header("X-Accel-Buffering", "no");
        response.set_chunked_content_provider("text/event-stream", [this](std::size_t, httplib::DataSink& sink) {
            return write_sse(sink);
        });
    });

    server.Get("/api/plugins", [this](const httplib::Request&, httplib::Response& response) {
        if (!plugin_manager_ready(plugin_manager_, response)) return;
        send_json(response, 200, plugin_manager_->get_all_plugin_info());
    });

    server.Post("/api/plugins/scan", [this](const httplib::Request&, httplib::Response& response) {
        if (!plugin_manager_ready(plugin_manager_, response)) return;
        plugin_manager_->scan_plugins();
        send_json(response, 200, {{"success", true}, {"plugins", plugin_manager_->get_all_plugin_info()}});
    });

    server.Post("/api/plugins", [this](const httplib::Request& request, httplib::Response& response) {
        if (!plugin_manager_ready(plugin_manager_, response)) return;
        try {
            const json payload = json::parse(request.body);
            const std::string name = payload.value("name", "");
            if (name.empty()) {
                send_json(response, 400, {{"error", "plugin name required"}});
                return;
            }
            const bool enabled = payload.value("enabled", false);
            const bool success = enabled ? plugin_manager_->select_plugin(name) : plugin_manager_->unload_plugin(name);
            send_json(response, success ? 200 : 404, {
                {"success", success},
                {"name", name},
                {"enabled", enabled},
                {"plugins", plugin_manager_->get_all_plugin_info()}
            });
        } catch (const std::exception&) {
            send_json(response, 400, {{"error", "invalid JSON"}});
        }
    });

    server.Post("/api/plugins/unload", [this](const httplib::Request& request, httplib::Response& response) {
        if (!plugin_manager_ready(plugin_manager_, response)) return;
        try {
            const json payload = json::parse(request.body);
            const std::string name = payload.value("name", "");
            const bool success = name.empty()
                ? plugin_manager_->unload_all_plugins()
                : plugin_manager_->unload_plugin(name);
            send_json(response, success ? 200 : 404, {
                {"success", success},
                {"name", name},
                {"plugins", plugin_manager_->get_all_plugin_info()}
            });
        } catch (const std::exception&) {
            send_json(response, 400, {{"error", "invalid JSON"}});
        }
    });

    server.Post("/api/plugins/settings", [this](const httplib::Request& request, httplib::Response& response) {
        if (!plugin_manager_ready(plugin_manager_, response)) return;
        try {
            const json payload = json::parse(request.body);
            const std::string name = payload.value("name", "");
            if (name.empty() || !payload.contains("settings")) {
                send_json(response, 400, {{"error", "plugin name and settings required"}});
                return;
            }
            plugin_manager_->update_plugin_settings(name, payload.at("settings"));
            send_json(response, 200, {{"success", true}, {"plugins", plugin_manager_->get_all_plugin_info()}});
        } catch (const std::exception&) {
            send_json(response, 400, {{"error", "invalid JSON"}});
        }
    });

    server.Get("/api/query/media", [this](const httplib::Request& request, httplib::Response& response) {
        const std::string request_path = request.get_param_value("path");
        if (request_path.empty()) {
            send_json(response, 200, base_system_.discover_media_library());
            return;
        }

        const std::filesystem::path resolved = std::filesystem::weakly_canonical(std::filesystem::path(request_path));
        const std::filesystem::path root = std::filesystem::current_path() / "media";
        const std::string canonical = resolved.string();
        const bool is_within_media = canonical.rfind(root.string(), 0) == 0;
        if (!std::filesystem::exists(resolved) || !std::filesystem::is_regular_file(resolved) || !is_within_media) {
            send_json(response, 404, {{"error", "media not found"}});
            return;
        }
        response.set_file_content(canonical, content_type_for_path(canonical));
    });

    server.Get("/api/record/status", [this](const httplib::Request&, httplib::Response& response) {
        send_json(response, 200, recording_status_json());
    });

    server.Post("/api/record/start", [this](const httplib::Request&, httplib::Response& response) {
        const bool started = base_system_.start_recording();
        send_json(response, started ? 200 : 500, recording_status_json());
    });

    server.Post("/api/record/stop", [this](const httplib::Request&, httplib::Response& response) {
        const bool stopped = base_system_.stop_recording();
        send_json(response, stopped ? 200 : 500, recording_status_json());
    });

    server.Post("/api/record/restart", [this](const httplib::Request&, httplib::Response& response) {
        const bool restarted = base_system_.restart_recording();
        send_json(response, restarted ? 200 : 500, recording_status_json());
    });

    server.Post("/api/record/capture", [this](const httplib::Request&, httplib::Response& response) {
        const bool captured = base_system_.capture_frame();
        send_json(response, captured ? 200 : 500, {{"captured", captured}});
    });

    server.Get("/api/camera/settings", [this](const httplib::Request&, httplib::Response& response) {
        send_json(response, 200, serialize_camera_settings_response(camera_settings()));
    });

    server.Post("/api/camera/settings", [this](const httplib::Request& request, httplib::Response& response) {
        try {
            const json payload = json::parse(request.body);
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
            auto body = serialize_camera_settings_response(settings);
            body["applied"] = applied;
            send_json(response, applied ? 200 : 500, body);
        } catch (const std::exception&) {
            send_json(response, 400, {{"error", "invalid JSON"}});
        }
    });

    server.Get("/api/camera/status", [this](const httplib::Request&, httplib::Response& response) {
        send_json(response, 200, camera_status_json());
    });

    server.Post("/api/camera/start", [this](const httplib::Request&, httplib::Response& response) {
        const bool started = start_camera();
        if (started) {
            stream_generation_.fetch_add(1, std::memory_order_release);
        }
        send_json(response, started ? 200 : 500, {{"running", camera_.is_running()}});
    });

    server.Post("/api/camera/stop", [this](const httplib::Request&, httplib::Response& response) {
        stop_camera();
        send_json(response, 200, {{"running", false}});
    });

    server.Get("/api/camera/stream", [this](const httplib::Request&, httplib::Response& response) {
        response.set_header("Cache-Control", "no-cache");
        response.set_header("Connection", "close");
        response.set_chunked_content_provider("video/mp4", [this](std::size_t, httplib::DataSink& sink) {
            return stream_h264(sink);
        });
    });
}
