#include "BaseSystem.hpp"

#include "Logger.hpp"

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
    camera_.register_frame_callback([this](const FrameContext& frame) {
        recording_queue_.push(frame);
    });
}

BaseSystem::~BaseSystem()
{
    stop_recording();
    recording_queue_.close();
    if (recording_thread_.joinable()) {
        recording_thread_.join();
    }
}

bool BaseSystem::start_recording()
{
    std::lock_guard lock(mutex_);
    if (recording_active_.load(std::memory_order_acquire)) {
        logger_.log(LogLevel::WARN, "BASE_SYSTEM", "Background recording already active; continuing without restart");
        return true;
    }

    try {
        const std::string stamp = make_recording_stamp();
        std::filesystem::create_directories("media/recorded_videos");
        current_recording_file_ = "media/recorded_videos/" + stamp + ".mp4";
        logger_.log(LogLevel::INFO, "BASE_SYSTEM", "Starting recording: " + current_recording_file_);
    } catch (const std::exception& ex) {
        logger_.log(LogLevel::ERROR, "BASE_SYSTEM", "Failed to prepare recording directory: " + std::string(ex.what()));
        return false;
    }

    recording_start_time_ = std::chrono::steady_clock::now();
    recording_active_.store(true, std::memory_order_release);
    last_written_sequence_.store(0, std::memory_order_release);
    logger_.log(LogLevel::INFO, "BASE_SYSTEM", "Recording callback enabled for: " + current_recording_file_);
    return true;
}

bool BaseSystem::stop_recording()
{
    recording_active_.store(false, std::memory_order_release);
    close_writer();
    logger_.log(LogLevel::INFO, "BASE_SYSTEM", "Recording stopped for: " + current_recording_file_);
    return true;
}

bool BaseSystem::restart_recording()
{
    logger_.log(LogLevel::INFO, "BASE_SYSTEM", "Restarting recording session");
    stop_recording();
    return start_recording();
}

bool BaseSystem::capture_frame()
{
    try {
        std::filesystem::create_directories("media/capture_photos");
    } catch (const std::exception& ex) {
        logger_.log(LogLevel::ERROR, "BASE_SYSTEM", "Failed to create captured photo directory: " + std::string(ex.what()));
        return false;
    }

    FrameContext frame;
    if (!camera_.latest_frame(frame) || !frame.color || frame.color->empty()) {
        logger_.log(LogLevel::WARN, "BASE_SYSTEM", "Captured photo request failed: no valid frame available");
        return false;
    }

    const std::shared_ptr<cv::Mat> frame_copy = frame.color;
    const std::string stamp = make_recording_stamp();
    const std::string capture_path = "media/capture_photos/captured_" + stamp + ".jpeg";

    std::thread([this, frame_copy, capture_path]() mutable {
        try {
            const bool ok = cv::imwrite(capture_path, *frame_copy);
            if (ok) {
                logger_.log(LogLevel::INFO, "BASE_SYSTEM", "Captured photo saved: " + capture_path);
            } else {
                logger_.log(LogLevel::ERROR, "BASE_SYSTEM", "Captured photo write failed: " + capture_path);
            }
        } catch (const std::exception& ex) {
            logger_.log(LogLevel::ERROR, "BASE_SYSTEM", "Captured photo worker failed: " + std::string(ex.what()));
        }
    }).detach();

    return true;
}

void BaseSystem::on_frame_received(const FrameContext& frame)
{
    if (!recording_active_.load(std::memory_order_acquire) || !frame.color || frame.color->empty()) {
        return;
    }

    const std::uint64_t sequence = frame.sequence;
    if (sequence != 0 && sequence <= last_written_sequence_.load(std::memory_order_acquire)) {
        return;
    }

    std::lock_guard lock(writer_mutex_);
    if (!recording_active_.load(std::memory_order_acquire)) {
        return;
    }
    open_writer_if_needed(*frame.color);
    if (!writer_.isOpened()) {
        return;
    }

    writer_.write(*frame.color);
    last_written_sequence_.store(sequence, std::memory_order_release);
}

void BaseSystem::recording_loop()
{
    FrameContext frame;
    while (recording_queue_.pop(frame)) {
        on_frame_received(frame);
    }
}

bool BaseSystem::is_recording() const noexcept
{
    return recording_active_.load(std::memory_order_acquire);
}

std::uint64_t BaseSystem::get_elapsed_seconds() const
{
    if (!recording_active_.load(std::memory_order_acquire)) {
        return 0;
    }

    const auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::seconds>(now - recording_start_time_).count();
}

std::string BaseSystem::current_recording_file() const
{
    std::lock_guard lock(mutex_);
    return current_recording_file_;
}

void BaseSystem::open_writer_if_needed(const cv::Mat& frame)
{
    if (writer_.isOpened()) {
        return;
    }

    if (!try_open_writer(writer_, current_recording_file_, frame.size())) {
        recording_active_.store(false, std::memory_order_release);
        logger_.log(LogLevel::ERROR, "BASE_SYSTEM", "Unable to open video writer for: " + current_recording_file_);
        return;
    }

    logger_.log(LogLevel::INFO, "BASE_SYSTEM", "Video writer opened: " + current_recording_file_);
}

void BaseSystem::close_writer()
{
    std::lock_guard lock(writer_mutex_);
    if (writer_.isOpened()) {
        logger_.log(LogLevel::INFO, "BASE_SYSTEM", "Closing video writer: " + current_recording_file_);
        writer_.release();
    }
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
    return std::string(buffer);
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
    const double fps = 30.0;

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

