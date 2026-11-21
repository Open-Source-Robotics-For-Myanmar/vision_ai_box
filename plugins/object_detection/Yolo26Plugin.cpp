#include "IPlugin.hpp"
#include "Logger.hpp"
#include <onnxruntime_cxx_api.h>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace {
std::string environment(const char* name, const char* fallback)
{
    const auto* value = std::getenv(name);
    return value && *value ? value : fallback;
}

class Yolo26Plugin final : public IPlugin {
public:
    explicit Yolo26Plugin(Logger* logger) : logger_(logger) {}
    std::string get_name() const override { return "object_detection"; }
    void init(const nlohmann::json& config) override
    {
        // PluginManager's init({}) supplies JSON null.
        update_settings(config.is_null() ? nlohmann::json::object() : config);
    }
    void update_settings(const nlohmann::json& config) override
    {
        std::lock_guard lock(mutex_);
        const float threshold = config.value("confidence_threshold", threshold_);
        if (!std::isfinite(threshold) || threshold < 0 || threshold > 1)
            throw std::invalid_argument("confidence_threshold must be in [0,1]");
        const auto model = config.value("model_path", model_path_);
        const auto labels = config.value("labels_path", labels_path_);
        const bool enabled = config.value("enabled", enabled_);
        const bool show = config.value("show_labels", show_labels_);
        const bool reload = config.value("reload", false) || !session_ || model != model_path_ || labels != labels_path_;
        threshold_ = threshold;
        enabled_ = enabled;
        show_labels_ = show;
        model_path_ = model;
        labels_path_ = labels;
        if (reload) load_model();
    }
    nlohmann::json get_settings() const override
    {
        std::lock_guard lock(mutex_);
        return {{"model_path", model_path_}, {"labels_path", labels_path_},
                {"confidence_threshold", threshold_}, {"show_labels", show_labels_},
                {"enabled", enabled_}, {"ready", session_ != nullptr},
                {"last_error", error_}, {"provider", "CPU"},
                {"input_width", width_}, {"input_height", height_}};
    }
    void set_enabled(bool enabled) override { std::lock_guard lock(mutex_); enabled_ = enabled; }
    bool is_enabled() const override { std::lock_guard lock(mutex_); return enabled_; }

    PluginResult process(const FrameContext& frame) override
    {
        std::lock_guard lock(mutex_);
        PluginResult result;
        if (!enabled_ || !session_ || !frame.color || frame.color->empty()) return result;
        try {
            const auto& source = *frame.color;
            if (source.type() != CV_8UC3) throw std::runtime_error("Expected CV_8UC3 BGR frame");
            const float gain = std::min(float(width_) / source.cols, float(height_) / source.rows);
            const int w = std::clamp(int(std::round(source.cols * gain)), 1, width_);
            const int h = std::clamp(int(std::round(source.rows * gain)), 1, height_);
            const int left = (width_ - w) / 2, top = (height_ - h) / 2;
            cv::Mat resized, padded(height_, width_, CV_8UC3, cv::Scalar(114, 114, 114));
            cv::resize(source, resized, {w, h});
            resized.copyTo(padded(cv::Rect(left, top, w, h)));
            const std::size_t plane = std::size_t(width_) * height_;
            input_.resize(3 * plane);
            for (int y = 0; y < height_; ++y) {
                const auto* row = padded.ptr<cv::Vec3b>(y);
                for (int x = 0; x < width_; ++x)
                    for (int c = 0; c < 3; ++c)
                        input_[c * plane + std::size_t(y) * width_ + x] = row[x][2 - c] / 255.0f;
            }
            const std::array<std::int64_t, 4> shape{1, 3, height_, width_};
            auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
            auto tensor = Ort::Value::CreateTensor<float>(memory, input_.data(), input_.size(), shape.data(), shape.size());
            const char* inputs[] = {input_name_.c_str()};
            const char* outputs[] = {output_name_.c_str()};
            auto values = session_->Run(Ort::RunOptions{nullptr}, inputs, &tensor, 1, outputs, 1);
            if (!values[0].IsTensor()) throw std::runtime_error("Expected output tensor");
            const auto info = values[0].GetTensorTypeAndShapeInfo();
            const auto dims = info.GetShape();
            if (info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
                dims.size() != 3 || dims[0] != 1 || dims[1] < 1 || dims[2] != 6)
                throw std::runtime_error("Expected FP32 [1,N,6]; export end2end=True nms=False");
            const float* rows = values[0].GetTensorData<float>();
            for (std::int64_t i = 0; i < dims[1]; ++i) {
                const float* box = rows + i * 6;
                if (!std::all_of(box, box + 6, [](float v) { return std::isfinite(v); }) ||
                    box[4] < threshold_ || box[4] <= 0 || box[4] > 1 || box[5] < 0 ||
                    box[5] >= float(labels_.size()) || std::floor(box[5]) != box[5]) continue;
                const float x1 = std::clamp((box[0] - left) / gain / source.cols, 0.0f, 1.0f);
                const float y1 = std::clamp((box[1] - top) / gain / source.rows, 0.0f, 1.0f);
                const float x2 = std::clamp((box[2] - left) / gain / source.cols, 0.0f, 1.0f);
                const float y2 = std::clamp((box[3] - top) / gain / source.rows, 0.0f, 1.0f);
                if (x2 <= x1 || y2 <= y1) continue;
                const int id = static_cast<int>(box[5]);
                result.detections.push_back({x1, y1, x2-x1, y2-y1, box[4], id, show_labels_ ? labels_[id] : ""});
            }
            // YOLO26 end-to-end outputs need no additional NMS.
        } catch (const std::exception& error) {
            fail(error.what());
            result.detections.clear();
        }
        return result;
    }
private:
    void fail(const std::string& message)
    {
        session_.reset();
        error_ = message;
        if (logger_) logger_->log(LogLevel::ERROR, "YOLO26", message);
    }
    void load_model()
    {
        session_.reset();
        error_.clear();
        width_ = height_ = 0;
        try {
            if (!std::filesystem::is_regular_file(model_path_)) throw std::runtime_error("Model not found: " + model_path_);
            std::ifstream file(labels_path_);
            if (!file) throw std::runtime_error("Labels not found: " + labels_path_);
            labels_ = nlohmann::json::parse(file).get<std::vector<std::string>>();
            if (labels_.empty()) throw std::runtime_error("Labels must be a non-empty JSON string array");
            Ort::SessionOptions options;
            options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
            options.SetIntraOpNumThreads(2);
            auto session = std::make_unique<Ort::Session>(env_, model_path_.c_str(), options);
            if (session->GetInputCount() != 1 || session->GetOutputCount() != 1)
                throw std::runtime_error("Expected one detection input/output");
            // TypeInfo must outlive the borrowed TensorTypeAndShapeInfo.
            auto input_type = session->GetInputTypeInfo(0);
            const auto input_info = input_type.GetTensorTypeAndShapeInfo();
            const auto dims = input_info.GetShape();
            if (input_info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT || dims.size() != 4 ||
                dims[0] != 1 || dims[1] != 3 || dims[2] < 32 || dims[3] < 32 || dims[2] > 4096 || dims[3] > 4096)
                throw std::runtime_error("Expected static FP32 [1,3,H,W]; export dynamic=False half=False batch=1");
            auto output_type = session->GetOutputTypeInfo(0);
            const auto output_info = output_type.GetTensorTypeAndShapeInfo();
            const auto out = output_info.GetShape();
            if (output_info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
                out.size() != 3 || out[0] != 1 || out[2] != 6)
                throw std::runtime_error("Expected [1,N,6]; export end2end=True nms=False");
            Ort::AllocatorWithDefaultOptions allocator;
            input_name_ = session->GetInputNameAllocated(0, allocator).get();
            output_name_ = session->GetOutputNameAllocated(0, allocator).get();
            height_ = static_cast<int>(dims[2]);
            width_ = static_cast<int>(dims[3]);
            session_ = std::move(session);
            if (logger_) logger_->log(LogLevel::INFO, "YOLO26", "Loaded CPU model: " + model_path_);
        } catch (const std::exception& error) { fail(error.what()); }
    }
    Logger* logger_;
    mutable std::mutex mutex_;
    Ort::Env env_{ORT_LOGGING_LEVEL_WARNING, "vision_ai_box_yolo26"};
    std::unique_ptr<Ort::Session> session_; // destroyed before env_
    std::string model_path_ = environment("VISION_YOLO26_MODEL", "plugins/object_detection/models/yolo26l.onnx");
    std::string labels_path_ = environment("VISION_YOLO26_LABELS", "plugins/object_detection/models/yolo26l.names.json");
    std::string input_name_, output_name_, error_;
    std::vector<std::string> labels_;
    std::vector<float> input_;
    int width_{0}, height_{0};
    float threshold_{0.25f};
    bool enabled_{true}, show_labels_{true};
};
}
extern "C" IPlugin* create_plugin(Logger* logger)
{
    try { return new Yolo26Plugin(logger); }
    catch (const std::exception& error) {
        if (logger) logger->log(LogLevel::ERROR, "YOLO26", error.what());
        return nullptr;
    }
}
extern "C" void destroy_plugin(IPlugin* plugin) { delete plugin; }
