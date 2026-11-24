#include "IPlugin.hpp"
#include "Logger.hpp"
#include <onnxruntime_cxx_api.h>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr int kInputSize = 640;
constexpr float kMeanB = 104.0f, kMeanG = 117.0f, kMeanR = 123.0f;
constexpr float kVariance0 = 0.1f, kVariance1 = 0.2f;
const std::array<int, 3> kStrides{8, 16, 32};
const std::array<std::array<int, 2>, 3> kMinSizes{{{16, 32}, {64, 128}, {256, 512}}};

std::string environment(const char* name, const char* fallback)
{
    const auto* value = std::getenv(name);
    return value && *value ? value : fallback;
}

struct Prior { float cx, cy, sx, sy; };

std::vector<Prior> generate_priors()
{
    std::vector<Prior> priors;
    for (std::size_t k = 0; k < kStrides.size(); ++k) {
        const int step = kStrides[k];
        const int grid = (kInputSize + step - 1) / step;
        for (int i = 0; i < grid; ++i) {
            for (int j = 0; j < grid; ++j) {
                for (const int min_size : kMinSizes[k]) {
                    priors.push_back({(j + 0.5f) * step / kInputSize,
                                      (i + 0.5f) * step / kInputSize,
                                      static_cast<float>(min_size) / kInputSize,
                                      static_cast<float>(min_size) / kInputSize});
                }
            }
        }
    }
    return priors;
}

struct Candidate { float x1, y1, x2, y2, score; };

float iou(const Candidate& a, const Candidate& b)
{
    const float x1 = std::max(a.x1, b.x1);
    const float y1 = std::max(a.y1, b.y1);
    const float x2 = std::min(a.x2, b.x2);
    const float y2 = std::min(a.y2, b.y2);
    const float inter = std::max(0.0f, x2 - x1 + 1.0f) * std::max(0.0f, y2 - y1 + 1.0f);
    const float area_a = std::max(0.0f, a.x2 - a.x1 + 1.0f) * std::max(0.0f, a.y2 - a.y1 + 1.0f);
    const float area_b = std::max(0.0f, b.x2 - b.x1 + 1.0f) * std::max(0.0f, b.y2 - b.y1 + 1.0f);
    const float union_area = area_a + area_b - inter;
    return union_area > 0.0f ? inter / union_area : 0.0f;
}

class RetinaFacePlugin final : public IPlugin {
public:
    explicit RetinaFacePlugin(Logger* logger) : logger_(logger), priors_(generate_priors()) {}
    std::string get_name() const override { return "face_detection"; }

    void init(const nlohmann::json& config) override
    {
        update_settings(config.is_null() ? nlohmann::json::object() : config);
    }

    void update_settings(const nlohmann::json& config) override
    {
        std::lock_guard lock(mutex_);
        const float threshold = config.value("confidence_threshold", threshold_);
        const float nms = config.value("nms_threshold", nms_threshold_);
        if (!std::isfinite(threshold) || threshold < 0.0f || threshold > 1.0f)
            throw std::invalid_argument("confidence_threshold must be in [0,1]");
        if (!std::isfinite(nms) || nms < 0.0f || nms > 1.0f)
            throw std::invalid_argument("nms_threshold must be in [0,1]");
        const auto model = config.value("model_path", model_path_);
        const bool enabled = config.value("enabled", enabled_);
        const bool show = config.value("show_labels", show_labels_);
        const bool reload = config.value("reload", false) || !session_ || model != model_path_;
        threshold_ = threshold;
        nms_threshold_ = nms;
        enabled_ = enabled;
        show_labels_ = show;
        model_path_ = model;
        if (reload) load_model();
    }

    nlohmann::json get_settings() const override
    {
        std::lock_guard lock(mutex_);
        return {{"model_path", model_path_}, {"confidence_threshold", threshold_},
                {"nms_threshold", nms_threshold_}, {"show_labels", show_labels_},
                {"enabled", enabled_}, {"ready", session_ != nullptr},
                {"last_error", error_}, {"provider", "CPU"},
                {"input_width", kInputSize}, {"input_height", kInputSize}};
    }

    void set_enabled(bool enabled) override { std::lock_guard lock(mutex_); enabled_ = enabled; }
    bool is_enabled() const override { std::lock_guard lock(mutex_); return enabled_; }

    PluginResult process(const FrameContext& frame) override
    {
        std::lock_guard lock(mutex_);
        PluginResult result;
        result.sequence = frame.sequence;
        result.plugin = get_name();
        if (!enabled_ || !session_ || !frame.color || frame.color->empty()) return result;
        try {
            const auto& source = *frame.color;
            if (source.type() != CV_8UC3) throw std::runtime_error("Expected CV_8UC3 BGR frame");
            const float scale = std::min(float(kInputSize) / source.cols,
                                         float(kInputSize) / source.rows);
            const int w = std::clamp(int(std::round(source.cols * scale)), 1, kInputSize);
            const int h = std::clamp(int(std::round(source.rows * scale)), 1, kInputSize);
            cv::Mat resized;
            cv::resize(source, resized, {w, h});
            // biubug6 RetinaFace: BGR raw 0..255 minus (104,117,123), no /255,
            // no channel swap; remaining canvas stays zero-padded (top-left).
            const std::size_t plane = std::size_t(kInputSize) * kInputSize;
            input_.assign(3 * plane, 0.0f);
            for (int y = 0; y < h; ++y) {
                const auto* row = resized.ptr<cv::Vec3b>(y);
                for (int x = 0; x < w; ++x) {
                    const std::size_t idx = std::size_t(y) * kInputSize + x;
                    input_[idx] = static_cast<float>(row[x][0]) - kMeanB;
                    input_[plane + idx] = static_cast<float>(row[x][1]) - kMeanG;
                    input_[2 * plane + idx] = static_cast<float>(row[x][2]) - kMeanR;
                }
            }
            const std::array<std::int64_t, 4> shape{1, 3, kInputSize, kInputSize};
            auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
            auto tensor = Ort::Value::CreateTensor<float>(
                memory, input_.data(), input_.size(), shape.data(), shape.size());
            const char* inputs[] = {input_name_.c_str()};
            auto values = session_->Run(Ort::RunOptions{nullptr}, inputs, &tensor, 1,
                                        output_names_.data(), output_names_.size());
            if (values.size() != 3) throw std::runtime_error("Expected loc/conf/landms outputs");
            const float* loc = values[loc_index_].GetTensorData<float>();
            const float* conf = values[conf_index_].GetTensorData<float>();
            std::vector<Candidate> candidates;
            for (std::size_t i = 0; i < priors_.size(); ++i) {
                const float score = conf[i * 2 + 1];
                if (!std::isfinite(score) || score <= threshold_ || score > 1.0f) continue;
                const auto& p = priors_[i];
                const float* box = loc + i * 4;
                if (!std::all_of(box, box + 4, [](float v) { return std::isfinite(v); })) continue;
                const float cx = p.cx + box[0] * kVariance0 * p.sx;
                const float cy = p.cy + box[1] * kVariance0 * p.sy;
                const float bw = p.sx * std::exp(box[2] * kVariance1);
                const float bh = p.sy * std::exp(box[3] * kVariance1);
                candidates.push_back({(cx - bw / 2.0f) * kInputSize,
                                      (cy - bh / 2.0f) * kInputSize,
                                      (cx + bw / 2.0f) * kInputSize,
                                      (cy + bh / 2.0f) * kInputSize, score});
            }
            std::sort(candidates.begin(), candidates.end(),
                      [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
            std::vector<Candidate> kept;
            for (const auto& candidate : candidates) {
                bool suppressed = false;
                for (const auto& other : kept) {
                    if (iou(candidate, other) > nms_threshold_) { suppressed = true; break; }
                }
                if (!suppressed) kept.push_back(candidate);
            }
            for (const auto& candidate : kept) {
                const float x1 = std::clamp(candidate.x1 / scale / source.cols, 0.0f, 1.0f);
                const float y1 = std::clamp(candidate.y1 / scale / source.rows, 0.0f, 1.0f);
                const float x2 = std::clamp(candidate.x2 / scale / source.cols, 0.0f, 1.0f);
                const float y2 = std::clamp(candidate.y2 / scale / source.rows, 0.0f, 1.0f);
                if (x2 <= x1 || y2 <= y1) continue;
                result.detections.push_back({x1, y1, x2 - x1, y2 - y1,
                                             candidate.score, 0, show_labels_ ? "face" : ""});
            }
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
        if (logger_) logger_->log(LogLevel::ERROR, "face_detection", message);
    }

    void load_model()
    {
        session_.reset();
        error_.clear();
        try {
            if (!std::filesystem::is_regular_file(model_path_))
                throw std::runtime_error("Model not found: " + model_path_);
            Ort::SessionOptions options;
            options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
            options.SetIntraOpNumThreads(2);
            auto session = std::make_unique<Ort::Session>(env_, model_path_.c_str(), options);
            if (session->GetInputCount() != 1 || session->GetOutputCount() != 3)
                throw std::runtime_error("Expected one input and loc/conf/landms outputs");
            auto input_type = session->GetInputTypeInfo(0);
            const auto input_info = input_type.GetTensorTypeAndShapeInfo();
            const auto dims = input_info.GetShape();
            if (input_info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
                dims.size() != 4 || dims[0] != 1 || dims[1] != 3 ||
                dims[2] != kInputSize || dims[3] != kInputSize)
                throw std::runtime_error("Expected FP32 [1,3,640,640] input");
            Ort::AllocatorWithDefaultOptions allocator;
            input_name_ = session->GetInputNameAllocated(0, allocator).get();
            output_names_.clear();
            output_name_storage_.clear();
            int loc = -1, conf = -1, landms = -1;
            for (std::size_t i = 0; i < session->GetOutputCount(); ++i) {
                auto name_ptr = session->GetOutputNameAllocated(i, allocator);
                std::string name = name_ptr.get();
                auto type = session->GetOutputTypeInfo(i);
                const auto info = type.GetTensorTypeAndShapeInfo();
                const auto shape = info.GetShape();
                if (info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
                    shape.size() != 3 || shape[0] != 1)
                    throw std::runtime_error("Expected FP32 [1,N,K] outputs");
                if (name == "loc" || (shape[2] == 4 && loc < 0)) loc = static_cast<int>(i);
                else if (name == "conf" || (shape[2] == 2 && conf < 0)) conf = static_cast<int>(i);
                else if (name == "landms" || (shape[2] == 10 && landms < 0)) landms = static_cast<int>(i);
                else throw std::runtime_error("Unrecognized RetinaFace output: " + name);
                output_name_storage_.push_back(std::move(name));
            }
            if (loc < 0 || conf < 0 || landms < 0)
                throw std::runtime_error("Missing loc/conf/landms output");
            output_names_.clear();
            for (const auto& name : output_name_storage_) output_names_.push_back(name.c_str());
            loc_index_ = static_cast<std::size_t>(loc);
            conf_index_ = static_cast<std::size_t>(conf);
            session_ = std::move(session);
            if (logger_) logger_->log(LogLevel::INFO, "face_detection",
                                      "Loaded CPU model: " + model_path_);
        } catch (const std::exception& error) { fail(error.what()); }
    }

    Logger* logger_;
    const std::vector<Prior> priors_;
    mutable std::mutex mutex_;
    Ort::Env env_{ORT_LOGGING_LEVEL_WARNING, "vision_ai_box_retinaface"};
    std::unique_ptr<Ort::Session> session_;
    std::string model_path_ = environment("VISION_FACE_MODEL", "plugins/face_detection/models/retinaface.onnx");
    std::string input_name_, error_;
    std::vector<std::string> output_name_storage_;
    std::vector<const char*> output_names_;
    std::vector<float> input_;
    std::size_t loc_index_{0}, conf_index_{1};
    float threshold_{0.5f}, nms_threshold_{0.4f};
    bool enabled_{true}, show_labels_{true};
};
} // namespace

extern "C" IPlugin* create_plugin(Logger* logger)
{
    try { return new RetinaFacePlugin(logger); }
    catch (const std::exception& error) {
        if (logger) logger->log(LogLevel::ERROR, "face_detection", error.what());
        return nullptr;
    }
}
extern "C" void destroy_plugin(IPlugin* plugin) { delete plugin; }
