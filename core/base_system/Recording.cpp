#include "BaseSystem.hpp"

#include "Logger.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <opencv2/imgcodecs.hpp>
#include <regex>
#include <thread>
#include <vector>

namespace
{
std::string to_lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string normalize_media_kind(const std::string& file_name)
{
    const std::string lower = to_lower(file_name);
    const bool is_video = lower.ends_with(".mp4") || lower.ends_with(".avi") || lower.ends_with(".mov") || lower.ends_with(".mkv") || lower.ends_with(".mjpeg");
    const bool is_captured_photo = lower.ends_with(".jpg") || lower.ends_with(".jpeg") || lower.ends_with(".png") || lower.ends_with(".bmp") || lower.ends_with(".webp");
    if (is_video) return "video";
    if (is_captured_photo) return "captured_photo";
    return "image";
}

std::string extract_date_value(const std::string& value)
{
    std::string text = value;
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });

    const std::regex date_pattern(R"((\d{1,2})-(\d{1,2})-(\d{4}))");
    std::smatch match;
    if (std::regex_search(text, match, date_pattern)) {
        const std::string day = match[1].str();
        const std::string month = match[2].str();
        const std::string year = match[3].str();
        return year + "-" + month + "-" + day;
    }

    return {};
}

nlohmann::json build_media_tree(const std::filesystem::path& path)
{
    nlohmann::json node = {
        {"name", path.filename().string()},
        {"path", path.string()},
        {"type", std::filesystem::is_directory(path) ? "folder" : "file"},
        {"children", nlohmann::json::array()}
    };

    if (std::filesystem::is_directory(path)) {
        std::vector<std::filesystem::path> children;
        for (const auto& entry : std::filesystem::directory_iterator(path)) {
            if (entry.is_directory() || entry.is_regular_file()) {
                children.push_back(entry.path());
            }
        }
        std::sort(children.begin(), children.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.filename().string() < rhs.filename().string();
        });

        for (const auto& child : children) {
            node["children"].push_back(build_media_tree(child));
        }
    } else if (std::filesystem::is_regular_file(path)) {
        node["size"] = static_cast<std::uint64_t>(std::filesystem::file_size(path));
        node["extension"] = path.extension().string();
        node["kind"] = normalize_media_kind(path.filename().string());
    }

    return node;
}

std::vector<nlohmann::json> collect_media_files(const nlohmann::json& node)
{
    std::vector<nlohmann::json> files;
    if (node["type"] == "file") {
        if (node.contains("kind") && !node["kind"].empty()) {
            files.push_back(node);
        }
        return files;
    }

    if (!node.contains("children") || !node["children"].is_array()) {
        return files;
    }

    for (const auto& child : node["children"]) {
        const auto nested = collect_media_files(child);
        files.insert(files.end(), nested.begin(), nested.end());
    }
    return files;
}

nlohmann::json make_media_group(const std::filesystem::path& directory_path, const std::string& group_name)
{
    nlohmann::json entries = nlohmann::json::array();
    if (!std::filesystem::exists(directory_path) || !std::filesystem::is_directory(directory_path)) {
        return {
            {"id", group_name},
            {"name", group_name},
            {"type", group_name == "capture_photos" ? "capture" : "recording"},
            {"path", directory_path.string()},
            {"date", ""},
            {"files", entries},
            {"tree", build_media_tree(directory_path)}
        };
    }

    const auto tree = build_media_tree(directory_path);
    const std::vector<nlohmann::json> files = collect_media_files(tree);
    for (const auto& file : files) {
        entries.push_back(file);
    }

    std::string date_value;
    for (const auto& file : entries) {
        const std::string extracted = extract_date_value(file.value("name", ""));
        if (!extracted.empty()) {
            date_value = extracted;
            break;
        }
    }
    if (date_value.empty()) {
        date_value = directory_path.filename().string();
    }

    return {
        {"id", group_name},
        {"name", group_name},
        {"type", group_name == "capture_photos" ? "capture" : "recording"},
        {"path", directory_path.string()},
        {"date", date_value},
        {"files", entries},
        {"tree", tree},
        {"counts", {
            {"video", static_cast<int>(std::count_if(entries.begin(), entries.end(), [](const nlohmann::json& item) { return item.value("kind", "") == "video"; }))},
            {"capture", static_cast<int>(std::count_if(entries.begin(), entries.end(), [](const nlohmann::json& item) { return item.value("kind", "") == "captured_photo" || item.value("kind", "") == "image"; }))},
            {"total", static_cast<int>(entries.size())}
        }}
    };
}
}

nlohmann::json BaseSystem::discover_media_library() const
{
    nlohmann::json media = nlohmann::json::array();
    const std::filesystem::path media_root = "media";
    if (!std::filesystem::exists(media_root) || !std::filesystem::is_directory(media_root)) {
        return media;
    }

    const std::filesystem::path capture_root = media_root / "capture_photos";
    const std::filesystem::path recording_root = media_root / "recorded_videos";
    const auto add_group = [&](const std::filesystem::path& directory_path, const std::string& group_name) {
        if (std::filesystem::exists(directory_path) && std::filesystem::is_directory(directory_path)) {
            media.push_back(make_media_group(directory_path, group_name));
        }
    };

    add_group(capture_root, "capture_photos");
    add_group(recording_root, "recorded_videos");

    std::sort(media.begin(), media.end(), [](const nlohmann::json& lhs, const nlohmann::json& rhs) {
        const std::string left = lhs.value("id", "");
        const std::string right = rhs.value("id", "");
        return left < right;
    });

    return media;
}

BaseSystem::BaseSystem(Logger& logger, SelectedCamera& camera)
    : logger_(logger), camera_(camera)
{
    recording_thread_ = std::thread(&BaseSystem::recording_loop, this);
}

BaseSystem::~BaseSystem()
{
    {
        std::lock_guard lock(mutex_);
        stop_recording_locked();
        stopping_ = true;
    }
    condition_.notify_all();
    if (recording_thread_.joinable()) recording_thread_.join();
}

bool BaseSystem::start_recording()
{
    std::lock_guard lock(mutex_);
    return start_recording_locked();
}

bool BaseSystem::start_recording_locked()
{
    if (recording_active_) return true;
    FrameContext frame;
    if (!camera_.is_running() || !camera_.latest_frame(frame) || !frame.color || frame.color->empty()) return false;
    try {
        std::filesystem::create_directories("media/recorded_videos");
        current_recording_file_ = "media/recorded_videos/" + make_recording_stamp() + ".mp4";
        if (!try_open_writer(writer_, current_recording_file_, frame.color->size())) return false;
        recording_frame_ = frame.color;
        frames_written_ = 0;
        recording_start_time_ = std::chrono::steady_clock::now();
        recording_active_ = true;
        condition_.notify_all();
        logger_.log(LogLevel::INFO, "BASE_SYSTEM", "Recording at fixed 30 FPS: " + current_recording_file_);
        return true;
    } catch (const std::exception& error) {
        writer_.release();
        logger_.log(LogLevel::ERROR, "BASE_SYSTEM", error.what());
        return false;
    }
}

// Each encoded slot represents 1/30 second, regardless of camera callback rate.
// All writer access and metadata are protected by mutex_.
void BaseSystem::write_until(std::chrono::steady_clock::time_point now)
{
    const auto target = recording_frame_count(now - recording_start_time_);
    while (frames_written_ < target) {
        writer_.write(*recording_frame_);
        ++frames_written_;
    }
}

bool BaseSystem::stop_recording_locked()
{
    bool ok = true;
    if (recording_active_.exchange(false)) {
        try { write_until(std::chrono::steady_clock::now()); }
        catch (const std::exception& error) {
            ok = false;
            logger_.log(LogLevel::ERROR, "BASE_SYSTEM", error.what());
        }
    }
    writer_.release();
    recording_frame_.reset();
    condition_.notify_all();
    return ok;
}

bool BaseSystem::stop_recording()
{
    std::lock_guard lock(mutex_);
    return stop_recording_locked();
}

bool BaseSystem::restart_recording()
{
    std::lock_guard lock(mutex_);
    return stop_recording_locked() && start_recording_locked();
}

bool BaseSystem::capture_frame()
{
    // Finish on the HTTP worker before returning; no detached lifetime hazards.
    try {
        FrameContext frame;
        if (!camera_.is_running() || !camera_.latest_frame(frame) || !frame.color || frame.color->empty()) return false;
        std::filesystem::create_directories("media/capture_photos");
        const auto path = "media/capture_photos/captured_" + make_recording_stamp() + ".jpeg";
        if (!cv::imwrite(path, *frame.color)) return false;
        logger_.log(LogLevel::INFO, "BASE_SYSTEM", "Captured photo saved: " + path);
        return true;
    } catch (const std::exception& error) {
        logger_.log(LogLevel::ERROR, "BASE_SYSTEM", error.what());
        return false;
    }
}

void BaseSystem::recording_loop()
{
    std::unique_lock lock(mutex_);
    while (!stopping_) {
        condition_.wait(lock, [this] { return stopping_ || recording_active_.load(); });
        if (stopping_) break;
        try {
            // Fill missing slots using the previous frame, then sample the next.
            write_until(std::chrono::steady_clock::now());
            FrameContext frame;
            if (camera_.latest_frame(frame) && frame.color && frame.color->size() == recording_frame_->size())
                recording_frame_ = frame.color;
            const auto deadline = recording_start_time_ + std::chrono::nanoseconds(
                frames_written_ * 1'000'000'000ULL / kCaptureFps);
            condition_.wait_until(lock, deadline);
        } catch (const std::exception& error) {
            recording_active_ = false;
            writer_.release();
            recording_frame_.reset();
            logger_.log(LogLevel::ERROR, "BASE_SYSTEM", error.what());
        }
    }
}

bool BaseSystem::is_recording() const noexcept { return recording_active_.load(); }

std::uint64_t BaseSystem::get_elapsed_seconds() const
{
    std::lock_guard lock(mutex_);
    if (!recording_active_) return 0;
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - recording_start_time_).count();
}

std::string BaseSystem::current_recording_file() const
{
    std::lock_guard lock(mutex_);
    return current_recording_file_;
}

std::string BaseSystem::make_recording_stamp()
{
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &time);
#else
    localtime_r(&time, &tm);
#endif

    char buffer[64];
    std::strftime(buffer, sizeof(buffer), "%d-%m-%Y_%I-%M-%S-%p", &tm);
    static std::atomic<std::uint64_t> counter{0};
    return std::string(buffer) + "_" + std::to_string(counter.fetch_add(1)) + "_" +
        std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count());
}

int BaseSystem::resolve_fourcc()
{
    const char* configured_fourcc = std::getenv("VISION_AI_BOX_FOURCC");
    if (configured_fourcc && std::strlen(configured_fourcc) == 4) {
        return cv::VideoWriter::fourcc(
            configured_fourcc[0],
            configured_fourcc[1],
            configured_fourcc[2],
            configured_fourcc[3]
        );
    }

    return cv::VideoWriter::fourcc('m', 'p', '4', 'v');
}

bool BaseSystem::try_open_writer(cv::VideoWriter& writer, const std::string& file_path, const cv::Size& frame_size)
{
    const int fourcc = resolve_fourcc();
    const double fps = kCaptureFps;

    const char* gst_pipeline = std::getenv("VISION_AI_BOX_GSTREAMER_PIPELINE");
    if (gst_pipeline && *gst_pipeline) {
        if (writer.open(gst_pipeline, cv::CAP_GSTREAMER, fourcc, fps, frame_size, true)) {
            return true;
        }
    }

    if (writer.open(file_path, fourcc, fps, frame_size, true)) {
        return true;
    }

    return false;
}

