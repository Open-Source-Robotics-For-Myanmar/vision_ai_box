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
    return load_plugin_unlocked(plugin_path);
}

bool PluginManager::load_plugin_unlocked(const std::filesystem::path& plugin_path)
{
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
    loaded->state = PluginState::Loading;

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

std::vector<std::string> PluginManager::loaded_plugin_names_except(const std::string& keep_name) const
{
    std::vector<std::string> names;
    std::lock_guard lock(mutex_);
    names.reserve(plugins_.size());
    for (const auto& [name, instance] : plugins_) {
        if (name != keep_name) {
            names.push_back(name);
        }
    }
    return names;
}

bool PluginManager::unload_plugin_unlocked(const std::string& plugin_name)
{
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
            removed->state = PluginState::Unloading;
            if (removed->plugin) {
                removed->plugin->set_enabled(false);
            }
        }
    }

    // Stop feeding detections from a plugin that is about to be destroyed.
    clear_result();
    logger_.log(LogLevel::INFO, "PLUGIN_MGR", "Unloading plugin: " + plugin_name);
    removed.reset();
    logger_.log(LogLevel::INFO, "PLUGIN_MGR", "Plugin unloaded: " + plugin_name);
    return true;
}

bool PluginManager::unload_plugin(const std::string& plugin_name)
{
    std::lock_guard lifecycle_lock(lifecycle_mutex_);
    return unload_plugin_unlocked(plugin_name);
}

bool PluginManager::unload_all_plugins()
{
    std::lock_guard lifecycle_lock(lifecycle_mutex_);
    const std::vector<std::string> names = loaded_plugin_names_except({});
    bool success = true;
    for (const auto& name : names) {
        success = unload_plugin_unlocked(name) && success;
    }
    return success;
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
    // Take a reference to the active plugin, then release the registry lock
    // before running inference. Holding it across process() would block every
    // other plugin call -- including the web UI reading plugin state -- for
    // the full duration of a detector pass. The shared_ptr keeps the instance
    // alive if it is unloaded mid-frame, so dlclose waits for us to finish.
    std::shared_ptr<PluginInstance> active;
    std::string active_name;
    {
        std::lock_guard lock(mutex_);
        auto it = plugins_.find(active_plugin_name_);
        if (it != plugins_.end() && it->second && it->second->state == PluginState::Active) {
            active = it->second;
            active_name = active_plugin_name_;
        }
    }

    if (!active || !active->plugin) {
        return;
    }

    PluginResult result = active->plugin->process(frame);
    // The plugin only has to fill in detections; the frame it was given and
    // its own identity are known here.
    result.sequence = frame.sequence;
    result.plugin = active_name;

    std::lock_guard lock(result_mutex_);
    latest_result_ = std::move(result);
    has_result_ = true;
}

bool PluginManager::latest_result(PluginResult& result) const
{
    std::lock_guard lock(result_mutex_);
    if (!has_result_) {
        return false;
    }
    result = latest_result_;
    return true;
}

void PluginManager::clear_result()
{
    std::lock_guard lock(result_mutex_);
    latest_result_ = {};
    has_result_ = false;
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

bool PluginManager::activate_plugin_unlocked(const std::string& name)
{
    std::lock_guard lock(mutex_);
    auto it = plugins_.find(name);
    if (it == plugins_.end() || !it->second || !it->second->plugin) {
        return false;
    }

    it->second->plugin->set_enabled(true);
    it->second->state = PluginState::Active;
    active_plugin_name_ = name;
    return true;
}

bool PluginManager::select_plugin(const std::string& name)
{
    std::lock_guard lifecycle_lock(lifecycle_mutex_);

    {
        std::lock_guard lock(mutex_);
        const auto it = plugins_.find(name);
        if (it != plugins_.end() && it->second && it->second->state == PluginState::Active &&
            it->second->plugin && it->second->plugin->is_enabled() &&
            active_plugin_name_ == name && plugins_.size() == 1) {
            return true;
        }
        if (!plugins_.contains(name) && !plugin_paths_.contains(name)) {
            logger_.log(LogLevel::ERROR, "PLUGIN_MGR", "Cannot switch to unknown plugin: " + name);
            return false;
        }
        loading_plugin_name_ = name;
        if (it != plugins_.end() && it->second && it->second->state != PluginState::Active) {
            it->second->state = PluginState::Loading;
        }
    }

    logger_.log(LogLevel::INFO, "PLUGIN_MGR", "Loading plugin: " + name);

    // Exclusive mode: the outgoing plugin must leave memory before the next
    // one is created. Leaving it Loaded kept its threads and .so mapped, and
    // the UI never saw an Unloaded state between modes.
    const std::vector<std::string> outgoing = loaded_plugin_names_except(name);
    for (const auto& outgoing_name : outgoing) {
        if (!unload_plugin_unlocked(outgoing_name)) {
            std::lock_guard lock(mutex_);
            loading_plugin_name_.clear();
            logger_.log(LogLevel::ERROR, "PLUGIN_MGR", "Failed to unload " + outgoing_name + " before switching");
            return false;
        }
    }

    bool already_loaded = false;
    std::filesystem::path plugin_path;
    {
        std::lock_guard lock(mutex_);
        already_loaded = plugins_.contains(name);
        const auto path_it = plugin_paths_.find(name);
        if (path_it != plugin_paths_.end()) {
            plugin_path = path_it->second;
        }
    }

    if (!already_loaded) {
        if (plugin_path.empty() || !load_plugin_unlocked(plugin_path)) {
            std::lock_guard lock(mutex_);
            loading_plugin_name_.clear();
            logger_.log(LogLevel::ERROR, "PLUGIN_MGR", "Failed to load plugin after unload: " + name);
            return false;
        }
    }

    clear_result();
    if (!activate_plugin_unlocked(name)) {
        std::lock_guard lock(mutex_);
        loading_plugin_name_.clear();
        return false;
    }

    {
        std::lock_guard lock(mutex_);
        loading_plugin_name_.clear();
    }
    logger_.log(LogLevel::INFO, "PLUGIN_MGR", "Active plugin switched to: " + name);
    return true;
}

nlohmann::json PluginManager::get_all_plugin_info() const
{
    std::lock_guard lock(mutex_);
    nlohmann::json result = nlohmann::json::array();
    for (const auto& [name, path] : plugin_paths_) {
        const auto loaded = plugins_.find(name);
        if (loaded != plugins_.end() && loaded->second && loaded->second->plugin) {
            const PluginState state = name == loading_plugin_name_
                ? PluginState::Loading
                : loaded->second->state;
            result.push_back({
                {"name", name},
                {"loaded", true},
                {"enabled", state == PluginState::Active},
                {"state", plugin_state_name(state)},
                {"settings", loaded->second->plugin->get_settings()}
            });
        } else {
            result.push_back({
                {"name", name},
                {"loaded", false},
                {"enabled", false},
                {"state", plugin_state_name(name == loading_plugin_name_
                    ? PluginState::Loading
                    : PluginState::Unloaded)},
                {"settings", nlohmann::json::object()}
            });
        }
    }
    return result;
}
