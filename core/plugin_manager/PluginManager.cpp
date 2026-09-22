#include "PluginManager.hpp"

#include <dlfcn.h>

PluginInstance::~PluginInstance()
{
    if (plugin && destroy_fn) {
        destroy_fn(plugin);
    }
    if (handle) {
        dlclose(handle);
    }
}

PluginManager::PluginManager(Logger& logger)
    : logger_(logger), processing_thread_(&PluginManager::processing_loop, this) {}

PluginManager::~PluginManager()
{
    frame_queue_.close();
    if (processing_thread_.joinable()) {
        processing_thread_.join();
    }
}


bool PluginManager::load_plugin(const std::filesystem::path& plugin_path)
{
    std::lock_guard lifecycle_lock(lifecycle_mutex_);
    if (!std::filesystem::exists(plugin_path)) {
        logger_.log(LogLevel::ERROR, "PLUGIN_MGR", "Plugin file not found: " + plugin_path.string());
        return false;
    }

    void* handle = dlopen(plugin_path.c_str(), RTLD_LAZY);
    if (!handle) {
        logger_.log(LogLevel::ERROR, "PLUGIN_MGR", "Failed to load library: " + std::string(dlerror()));
        return false;
    }

    dlerror();
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

    plugin_ptr->init({});
    plugin_ptr->set_enabled(false);

    const std::string name = plugin_path.stem().string();
    if (name.empty()) {
        destroy_fn(plugin_ptr);
        dlclose(handle);
        logger_.log(LogLevel::ERROR, "PLUGIN_MGR", "Plugin file has no valid stem: " + plugin_path.string());
        return false;
    }
    auto loaded = std::make_shared<PluginInstance>();
    loaded->handle = handle;
    loaded->plugin = plugin_ptr;
    loaded->destroy_fn = destroy_fn;

    {
        std::lock_guard lock(mutex_);
        if (plugins_.count(name)) {
            logger_.log(LogLevel::WARN, "PLUGIN_MGR", "Plugin already loaded: " + name);
            loaded.reset();
            return true;
        }
        plugins_[name] = std::move(loaded);
    }

    logger_.log(LogLevel::INFO, "PLUGIN_MGR", "Plugin successfully loaded: " + name);
    return true;
}

bool PluginManager::load_plugin_by_name(const std::string& plugin_name)
{
    std::filesystem::path plugin_path;
    {
        std::lock_guard lock(mutex_);
        if (plugins_.contains(plugin_name)) {
            return true;
        }
        auto it = plugin_paths_.find(plugin_name);
        if (it == plugin_paths_.end()) {
            return false;
        }
        plugin_path = it->second;
    }

    return load_plugin(plugin_path);
}

bool PluginManager::unload_plugin(const std::string& plugin_name)
{
    std::lock_guard lifecycle_lock(lifecycle_mutex_);
    frame_queue_.clear();
    std::shared_ptr<PluginInstance> removed;
    {
        std::lock_guard lock(mutex_);
        auto it = plugins_.find(plugin_name);
        if (it == plugins_.end()) {
            return plugin_paths_.contains(plugin_name);
        }
        removed = std::move(it->second);
        plugins_.erase(it);
        if (active_plugin_name_ == plugin_name) {
            active_plugin_name_.clear();
        }
        if (removed) {
            removed->state = PluginState::Unloaded;
            if (removed->plugin) {
                removed->plugin->set_enabled(false);
            }
        }
    }

    logger_.log(LogLevel::INFO, "PLUGIN_MGR", "Plugin unloaded: " + plugin_name);
    removed.reset();
    return true;
}

bool PluginManager::unload_all_plugins()
{
    std::lock_guard lifecycle_lock(lifecycle_mutex_);
    std::vector<std::shared_ptr<PluginInstance>> removed;
    frame_queue_.clear();
    {
        std::lock_guard lock(mutex_);
        active_plugin_name_.clear();
        for (auto it = plugins_.begin(); it != plugins_.end(); ) {
            if (it->second) {
                it->second->state = PluginState::Unloaded;
                if (it->second->plugin) {
                    it->second->plugin->set_enabled(false);
                }
            }
            removed.push_back(std::move(it->second));
            it = plugins_.erase(it);
        }
    }

    for (auto& instance : removed) {
        instance.reset();
    }

    logger_.log(LogLevel::INFO, "PLUGIN_MGR", "All plugins unloaded");
    return true;
}

void PluginManager::discover_plugins(const std::filesystem::path& plugins_directory)
{
    if (!std::filesystem::exists(plugins_directory) || !std::filesystem::is_directory(plugins_directory)) {
        logger_.log(LogLevel::WARN, "PLUGIN_MGR", "Directory does not exist: " + plugins_directory.string());
        return;
    }

    {
        std::lock_guard lock(mutex_);
        plugin_directory_ = plugins_directory;
    }

    for (const auto& entry : std::filesystem::directory_iterator(plugins_directory)) {
        if (entry.is_regular_file() && entry.path().extension() == ".so") {
            const std::string plugin_name = entry.path().stem().string();
            std::lock_guard lock(mutex_);
            plugin_paths_[plugin_name] = entry.path();
            logger_.log(LogLevel::INFO, "PLUGIN_MGR", "Plugin discovered: " + plugin_name);
        }
    }
}

void PluginManager::scan_plugins()
{
    std::filesystem::path plugin_directory;
    {
        std::lock_guard lock(mutex_);
        plugin_directory = plugin_directory_;
    }

    if (!plugin_directory.empty()) {
        discover_plugins(plugin_directory);
    }
}

void PluginManager::process_frame(const FrameContext& frame)
{
    frame_queue_.push(frame);
}

void PluginManager::processing_loop()
{
    FrameContext frame;
    while (frame_queue_.pop(frame)) {
        process_frame_now(frame);
    }
}

void PluginManager::process_frame_now(const FrameContext& frame)
{
    std::lock_guard lock(mutex_);
    for (const auto& [name, instance] : plugins_) {
        if (instance && instance->plugin && instance->state == PluginState::Active) {
            instance->plugin->process(frame);
        }
    }
}

void PluginManager::update_plugin_settings(const std::string& name, const nlohmann::json& config)
{
    std::lock_guard lock(mutex_);
    auto it = plugins_.find(name);
    if (it != plugins_.end() && it->second && it->second->plugin) {
        it->second->plugin->update_settings(config);
    }
}

bool PluginManager::set_plugin_enabled(const std::string& name, bool enabled)
{
    if (enabled) {
        return select_plugin(name);
    }

    return unload_plugin(name);
}

bool PluginManager::select_plugin(const std::string& name)
{
    std::lock_guard lifecycle_lock(lifecycle_mutex_);
    {
        std::lock_guard lock(mutex_);
        if (!plugins_.contains(name)) {
            return false;
        }

        if (active_plugin_name_ == name && plugins_.size() == 1) {
            const auto& active = plugins_.find(name)->second;
            if (active && active->state == PluginState::Active && active->plugin && active->plugin->is_enabled()) {
                return true;
            }
        }

        for (auto& [plugin_name, instance] : plugins_) {
            if (!instance || !instance->plugin) {
                continue;
            }
            if (plugin_name == name) {
                instance->plugin->set_enabled(true);
                instance->state = PluginState::Active;
            } else {
                instance->plugin->set_enabled(false);
                instance->state = PluginState::Loaded;
            }
        }
        active_plugin_name_ = name;
    }
    return true;
}

nlohmann::json PluginManager::get_all_plugin_info() const
{
    std::lock_guard lock(mutex_);
    nlohmann::json result = nlohmann::json::array();
    for (const auto& [name, path] : plugin_paths_) {
        const auto loaded = plugins_.find(name);
        if (loaded != plugins_.end() && loaded->second && loaded->second->plugin) {
            result.push_back({
                {"name", name},
                {"loaded", true},
                {"enabled", loaded->second->state == PluginState::Active},
                {"state", plugin_state_name(loaded->second->state)},
                {"settings", loaded->second->plugin->get_settings()}
            });
        } else {
            result.push_back({
                {"name", name},
                {"loaded", false},
                {"enabled", false},
                {"state", plugin_state_name(PluginState::Unloaded)},
                {"settings", nlohmann::json::object()}
            });
        }
    }
    return result;
}
