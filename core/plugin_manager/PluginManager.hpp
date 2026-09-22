#pragma once

#include "IPlugin.hpp"
#include "FrameQueue.hpp"
#include "Logger.hpp"
#include "PluginState.hpp"

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <thread>
#include <vector>

struct PluginInstance {
    void* handle{nullptr};
    IPlugin* plugin{nullptr};
    DestroyPluginFn destroy_fn{nullptr};
    PluginState state{PluginState::Loaded};
    ~PluginInstance();
    PluginInstance() = default;
    PluginInstance(const PluginInstance&) = delete;
    PluginInstance& operator=(const PluginInstance&) = delete;
};

class PluginManager {
public:
    explicit PluginManager(Logger& logger);
    ~PluginManager();

    bool load_plugin(const std::filesystem::path& plugin_path);
    bool load_plugin_by_name(const std::string& plugin_name);
    bool unload_plugin(const std::string& plugin_name);
    bool unload_all_plugins();
    void discover_plugins(const std::filesystem::path& plugins_directory);
    void scan_plugins();
    void load_all_plugins(const std::filesystem::path& plugins_directory);

    void process_frame(const FrameContext& frame);
    void update_plugin_settings(const std::string& name, const nlohmann::json& config);
    nlohmann::json get_all_plugin_info() const;
    bool set_plugin_enabled(const std::string& name, bool enabled);
    bool select_plugin(const std::string& name);

private:
    void processing_loop();
    void process_frame_now(const FrameContext& frame);

    Logger& logger_;
    mutable std::mutex mutex_;
    std::mutex lifecycle_mutex_;
    std::unordered_map<std::string, std::shared_ptr<PluginInstance>> plugins_;
    std::unordered_map<std::string, std::filesystem::path> plugin_paths_;
    std::string active_plugin_name_;
    std::filesystem::path plugin_directory_;
    FrameQueue frame_queue_{2};
    std::thread processing_thread_;
};