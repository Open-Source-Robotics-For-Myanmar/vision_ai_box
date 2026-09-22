#include "IPlugin.hpp"
#include "Logger.hpp"

#include <atomic>
#include <chrono>
#include <thread>

class ExamplePlugin : public IPlugin {
public:
    explicit ExamplePlugin(Logger* logger) : logger_(logger) {
        if (logger_) {
            logger_->log(LogLevel::INFO, "EXAMPLE_PLUGIN", "Example Plugin Constructed");
            logger_->log(LogLevel::INFO, "EXAMPLE_PLUGIN", "I am Example Plugin");
        }
        heartbeat_thread_ = std::thread(&ExamplePlugin::heartbeat_loop, this);
    }

    ~ExamplePlugin() override {
        heartbeat_running_.store(false);
        if (heartbeat_thread_.joinable()) {
            heartbeat_thread_.join();
        }
        if (logger_) {
            logger_->log(LogLevel::INFO, "EXAMPLE_PLUGIN", "Example Plugin Destroyed");
        }
    }

    std::string get_name() const override { return "example"; }

    void init(const nlohmann::json& config) override {
        update_settings(config);
        if (logger_) {
            logger_->log(LogLevel::INFO, "EXAMPLE_PLUGIN", "Example Plugin initialized");
        }
    }

    // Emits one box that drifts across the view. It detects nothing real; it
    // exists so the whole result path -- plugin to manager to SSE to canvas --
    // can be verified without a model, and as a worked example of the
    // normalised coordinate contract in PluginResult.hpp.
    PluginResult process(const FrameContext& frame) override {
        PluginResult result;
        if (!enabled_.load() || !frame.color || frame.color->empty()) {
            return result;
        }

        const float phase = static_cast<float>(frame.sequence % 240) / 240.0f;
        const float width = 0.25f;
        const float height = 0.35f;

        Detection detection;
        detection.x = phase * (1.0f - width);
        detection.y = 0.30f;
        detection.width = width;
        detection.height = height;
        detection.confidence = 0.75f;
        detection.class_id = 0;
        detection.label = "example";
        result.detections.push_back(std::move(detection));

        return result;
    }

    void update_settings(const nlohmann::json& config) override {
        if (config.contains("enabled")) {
            enabled_.store(config["enabled"].get<bool>());
        }
        if (enabled_.load() && logger_) {
            logger_->log(LogLevel::INFO, "EXAMPLE_PLUGIN", "I am Example Plugin");
        }
    }

    nlohmann::json get_settings() const override {
        return {{"enabled", enabled_.load()}, {"type", "prototype"}, {"message", "I am Example Plugin"}};
    }

    void set_enabled(bool enabled) override {
        enabled_.store(enabled);
        if (enabled_.load() && logger_) {
            logger_->log(LogLevel::INFO, "EXAMPLE_PLUGIN", "I am Example Plugin");
        }
    }

    bool is_enabled() const override { return enabled_.load(); }

private:
    void heartbeat_loop() {
        while (heartbeat_running_.load()) {
            if (enabled_.load() && logger_) {
                logger_->log(LogLevel::INFO, "EXAMPLE_PLUGIN", "I am Example Plugin");
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
        return new ExamplePlugin(logger);
    }

    void destroy_plugin(IPlugin* plugin) {
        delete plugin;
    }
}
