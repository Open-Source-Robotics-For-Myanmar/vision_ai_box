#include "IPlugin.hpp"
#include "Logger.hpp"
#include <iostream>

class FaceRecognitionPlugin : public IPlugin {
public:
    explicit FaceRecognitionPlugin(Logger* logger) : logger_(logger) {
        if (logger_) logger_->log(LogLevel::INFO, "FACE_PLUGIN", "Face Recognition Plugin Constructed");
    }

    ~FaceRecognitionPlugin() override {
        if (logger_) logger_->log(LogLevel::INFO, "FACE_PLUGIN", "Face Recognition Plugin Destroyed");
    }

    std::string get_name() const override { return "face_recognition"; }

    void init(const nlohmann::json& config) override {
        update_settings(config);
    }

    void process(const FrameContext& frame) override {
        if (!enabled_ || !frame.color || frame.color->empty()) return;

        // ROI/Face Detection & Vector Recognition Logic Here
    }

    void update_settings(const nlohmann::json& config) override {
        if (config.contains("enabled")) enabled_ = config["enabled"];
    }

    nlohmann::json get_settings() const override {
        return {{"enabled", enabled_}};
    }

    void set_enabled(bool enabled) override { enabled_ = enabled; }
    bool is_enabled() const override { return enabled_; }

private:
    Logger* logger_{nullptr};
    bool enabled_{true};
};

// Runtime Loading အတွက် Factory Functions များ Export ထုတ်ခြင်း
extern "C" {
    IPlugin* create_plugin(Logger* logger) {
        return new FaceRecognitionPlugin(logger);
    }

    void destroy_plugin(IPlugin* plugin) {
        delete plugin;
    }
}