#include "PluginManager.hpp"
#include <dlfcn.h>

PluginManager::PluginManager(Logger& logger) : logger_(logger) {}

PluginManager::~PluginManager() {
    std::lock_guard lock(mutex_);
    for (auto& [name, instance] : plugins_) {
        if (instance.plugin && instance.destroy_fn) {
            instance.destroy_fn(instance.plugin);
        }
        if (instance.handle) {
            dlclose(instance.handle);
        }
    }
    plugins_.clear();
}

bool PluginManager::load_plugin(const std::filesystem::path& plugin_path) {
    if (!std::filesystem::exists(plugin_path)) {
        logger_.log(LogLevel::ERROR, "PLUGIN_MGR", "Plugin file not found: " + plugin_path.string());
        return false;
    }

    void* handle = dlopen(plugin_path.c_str(), RTLD_LAZY);
    if (!handle) {
        logger_.log(LogLevel::ERROR, "PLUGIN_MGR", "Failed to load library: " + std::string(dlerror()));
        return false;
    }

    dlerror(); // Clear existing errors
    auto create_fn = reinterpret_cast<CreatePluginFn>(dlsym(handle, "create_plugin"));
    auto destroy_fn = reinterpret_cast<DestroyPluginFn>(dlsym(handle, "destroy_plugin"));

    const char* dlsym_error = dlerror();
    if (dlsym_error || !create_fn || !destroy_fn) {
        logger_.log(LogLevel::ERROR, "PLUGIN_MGR", "Failed to resolve symbols in: " + plugin_path.string());
        dlclose(handle);
        return false;
    }

    IPlugin* plugin_ptr = create_fn(&logger_);
    if (!plugin_ptr) {
        logger_.log(LogLevel::ERROR, "PLUGIN_MGR", "Plugin creation failed: " + plugin_path.string());
        dlclose(handle);
        return false;
    }

    std::string name = plugin_ptr->get_name();
    std::lock_guard lock(mutex_);
    if (plugins_.count(name)) {
        logger_.log(LogLevel::WARN, "PLUGIN_MGR", "Overwriting existing plugin: " + name);
        unload_plugin(name);
    }

    plugins_[name] = PluginInstance{handle, plugin_ptr, destroy_fn};
    logger_.log(LogLevel::INFO, "PLUGIN_MGR", "Plugin successfully loaded: " + name);
    return true;
}

bool PluginManager::unload_plugin(const std::string& plugin_name) {
    auto it = plugins_.find(plugin_name);
    if (it == plugins_.end()) {
        return false;
    }

    if (it->second.plugin && it->second.destroy_fn) {
        it->second.destroy_fn(it->second.plugin);
    }
    if (it->second.handle) {
        dlclose(it->second.handle);
    }

    plugins_.erase(it);
    logger_.log(LogLevel::INFO, "PLUGIN_MGR", "Plugin unloaded: " + plugin_name);
    return true;
}

void PluginManager::load_all_plugins(const std::filesystem::path& plugins_directory) {
    if (!std::filesystem::exists(plugins_directory) || !std::filesystem::is_directory(plugins_directory)) {
        logger_.log(LogLevel::WARN, "PLUGIN_MGR", "Directory does not exist: " + plugins_directory.string());
        return;
    }

    for (const auto& entry : std::filesystem::directory_iterator(plugins_directory)) {
        if (entry.is_regular_file() && entry.path().extension() == ".so") {
            load_plugin(entry.path());
        }
    }
}

void PluginManager::process_frame(const FrameContext& frame) {
    std::lock_guard lock(mutex_);
    for (auto& [name, instance] : plugins_) {
        if (instance.plugin && instance.plugin->is_enabled()) {
            instance.plugin->process(frame);
        }
    }
}

void PluginManager::update_plugin_settings(const std::string& name, const nlohmann::json& config) {
    std::lock_guard lock(mutex_);
    auto it = plugins_.find(name);
    if (it != plugins_.end() && it->second.plugin) {
        it->second.plugin->update_settings(config);
    }
}

nlohmann::json PluginManager::get_all_plugin_info() const {
    std::lock_guard lock(mutex_);
    nlohmann::json result = nlohmann::json::array();
    for (const auto& [name, instance] : plugins_) {
        if (instance.plugin) {
            result.push_back({
                {"name", name},
                {"enabled", instance.plugin->is_enabled()},
                {"settings", instance.plugin->get_settings()}
            });
        }
    }
    return result;
}