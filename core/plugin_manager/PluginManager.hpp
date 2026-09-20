#pragma once

#include "IPlugin.hpp"
#include "Logger.hpp"

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

struct PluginInstance {
    void* handle{nullptr};
    IPlugin* plugin{nullptr};
    DestroyPluginFn destroy_fn{nullptr};
};

class PluginManager {
public:
    explicit PluginManager(Logger& logger);
    ~PluginManager();

    bool load_plugin(const std::filesystem::path& plugin_path);
    bool unload_plugin(const std::string& plugin_name);
    void load_all_plugins(const std::filesystem::path& plugins_directory);

    void process_frame(const FrameContext& frame);
    void update_plugin_settings(const std::string& name, const nlohmann::json& config);
    nlohmann::json get_all_plugin_info() const;

private:
    Logger& logger_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, PluginInstance> plugins_;
};