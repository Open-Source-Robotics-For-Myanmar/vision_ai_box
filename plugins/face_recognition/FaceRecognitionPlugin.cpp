#include "IPlugin.hpp"
#include "Logger.hpp"

#include <atomic>
#include <chrono>
#include <thread>

class FaceRecognitionPlugin : public IPlugin {
public:
    explicit FaceRecognitionPlugin(Logger* logger) : logger_(logger) {
        if (logger_) {
            logger_->log(LogLevel::INFO, "FACE_PLUGIN", "Face Recognition Plugin Constructed");
            logger_->log(LogLevel::INFO, "FACE_PLUGIN", "I am Face Recognigiton Plugin");
        }
        heartbeat_thread_ = std::thread(&FaceRecognitionPlugin::heartbeat_loop, this);
    }

    ~FaceRecognitionPlugin() override {
        heartbeat_running_ = false;
        if (heartbeat_thread_.joinable()) {
            heartbeat_thread_.join();
        }
        if (logger_) {
            logger_->log(LogLevel::INFO, "FACE_PLUGIN", "Face Recognition Plugin Destroyed");
        }
    }

    std::string get_name() const override { return "face_recognition"; }

    void init(const nlohmann::json& config) override {
        update_settings(config);
        if (logger_) {
            logger_->log(LogLevel::INFO, "FACE_PLUGIN", "Face Recognition Plugin initialized");
        }
    }

    void process(const FrameContext& frame) override {
        if (!enabled_.load() || !frame.color || frame.color->empty()) {
            return;
        }
    }

    void update_settings(const nlohmann::json& config) override {
        if (config.contains("enabled")) {
            enabled_.store(config["enabled"].get<bool>());
        }
        if (enabled_.load() && logger_) {
            logger_->log(LogLevel::INFO, "FACE_PLUGIN", "I am Face Recognigiton Plugin");
        }
    }

    nlohmann::json get_settings() const override {
        return {{"enabled", enabled_.load()}, {"type", "prototype"}, {"message", "I am Face Recognigiton Plugin"}};
    }

    void set_enabled(bool enabled) override {
        enabled_.store(enabled);
        if (enabled_.load() && logger_) {
            logger_->log(LogLevel::INFO, "FACE_PLUGIN", "I am Face Recognigiton Plugin");
        }
    }

    bool is_enabled() const override { return enabled_.load(); }

private:
    void heartbeat_loop()
    {
        while (heartbeat_running_) {
            if (enabled_.load() && logger_) {
                logger_->log(LogLevel::INFO, "FACE_PLUGIN", "I am Face Recognigiton Plugin");
            }
            std::this_thread::sleep_for(std::chrono::seconds(3));
        }
    }

    Logger* logger_{nullptr};
    std::atomic<bool> enabled_{true};
    std::atomic<bool> heartbeat_running_{true};
    std::thread heartbeat_thread_;
};

extern "C" {
    IPlugin* create_plugin(Logger* logger) {
        return new FaceRecognitionPlugin(logger);
    }

    void destroy_plugin(IPlugin* plugin) {
        delete plugin;
    }
}