#include "PluginManager.hpp"

#include <dlfcn.h>
#include <stdexcept>

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
    : logger_(logger)
{
    // Start only after every mutex and result field has been constructed.
    processing_thread_ = std::thread(&PluginManager::processing_loop, this);
}

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

    void* handle = dlopen(plugin_path.c_str(), RTLD_NOW);
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

    auto loaded = std::make_shared<PluginInstance>();
    loaded->handle = handle;
    loaded->destroy_fn = destroy_fn;
    IPlugin* plugin_ptr = nullptr;
    try {
        loaded->plugin = create_fn(&logger_);
        plugin_ptr = loaded->plugin;
        if (!plugin_ptr) throw std::runtime_error("plugin factory returned null");
        plugin_ptr->init({});
        plugin_ptr->set_enabled(false);
    } catch (const std::exception& error) {
        logger_.log(LogLevel::ERROR, "PLUGIN_MGR", std::string("Plugin initialization failed: ") + error.what());
        return false;
    } catch (...) {
        logger_.log(LogLevel::ERROR, "PLUGIN_MGR", "Plugin initialization failed with unknown exception");
        return false;
    }
    const std::string name = plugin_path.stem().string();
    if (name.empty()) return false;
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
                std::lock_guard call_lock(removed->call_mutex);
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
    if (!frame_queue_.push(frame)) ++queue_drops_;
}

void PluginManager::processing_loop()
{
    FrameContext frame;
    while (frame_queue_.pop(frame)) {
        try {
            process_frame_now(frame);
        } catch (const std::exception& error) {
            ++processing_errors_;
            clear_result();
            logger_.log(LogLevel::ERROR, "PIPELINE", error.what());
        } catch (...) {
            ++processing_errors_;
            clear_result();
            logger_.log(LogLevel::ERROR, "PIPELINE", "Unknown preprocessing/inference exception");
        }
    }
}

void PluginManager::process_frame_now(const FrameContext& frame)
{
    // Take a reference to the active plugin, then release the registry lock
    // before running inference. Individual plugin calls are serialized by
    // call_mutex; the registry is not held by inference. The shared_ptr keeps the instance
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

    PipelineSettings settings;
    {
        std::lock_guard lock(pipeline_mutex_);
        settings = pipeline_settings_;
    }
    // First inner stage: optional preparation of an owned raw-frame copy.
    const FrameContext prepared = preprocess_frame(frame, settings);
    PluginResult result;
    {
        // Second inner stage: the selected dynamically linked detector.
        // Serialize inference with settings and enable/disable operations.
        std::lock_guard call_lock(active->call_mutex);
        if (!active->plugin->is_enabled()) return;
        result = active->plugin->process(prepared);
    }
    // The plugin only has to fill in detections; the frame it was given and
    // its own identity are known here.
    result.sequence = frame.sequence;
    result.plugin = active_name;

    // Never publish a result from an instance removed during this inference.
    // Hold registry -> result locks so unload cannot clear then get overwritten.
    std::lock_guard registry_lock(mutex_);
    const auto current = plugins_.find(active_name);
    if (active_plugin_name_ != active_name || current == plugins_.end() ||
        current->second != active || active->state != PluginState::Active) return;
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
        std::lock_guard call_lock(it->second->call_mutex);
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

    std::lock_guard call_lock(it->second->call_mutex);
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
            it->second->plugin && active_plugin_name_ == name && plugins_.size() == 1) {
            std::lock_guard call_lock(it->second->call_mutex);
            if (it->second->plugin->is_enabled()) return true;
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

    // Only one registry entry remains selected. In-flight references can keep
    // the outgoing library mapped until the processing worker releases it.
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
            std::lock_guard call_lock(loaded->second->call_mutex);
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

void PluginManager::stop_accepting_frames()
{
    frame_queue_.close();
}

nlohmann::json PluginManager::pipeline_info() const
{
    std::lock_guard lock(pipeline_mutex_);
    return {{"settings", pipeline_settings_.json()},
            {"queue_drops_or_rejections", queue_drops_.load()},
            {"processing_errors", processing_errors_.load()}};
}

void PluginManager::update_pipeline_settings(const nlohmann::json& settings)
{
    std::lock_guard lock(pipeline_mutex_);
    const auto next = pipeline_settings_.updated(settings);
    frame_queue_.configure(static_cast<std::size_t>(next.queue_capacity),
        next.delivery == "reliable" ? FrameDelivery::Reliable : FrameDelivery::BestEffort);
    pipeline_settings_ = next;
}
