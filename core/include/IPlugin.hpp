#pragma once

#include "FrameContext.hpp"
#include "PluginResult.hpp"
#include <nlohmann/json.hpp>
#include <string>

class Logger;

class IPlugin {
public:
    virtual ~IPlugin() = default;

    virtual std::string get_name() const = 0;
    virtual void init(const nlohmann::json& config) = 0;
    // Returns what was found in this frame. Coordinates must be normalised to
    // [0,1] against frame.color->size(); see PluginResult.hpp. Returning an
    // empty result is how a plugin says "nothing here", which the UI needs in
    // order to clear the previous overlay.
    virtual PluginResult process(const FrameContext& frame) = 0;
    virtual void update_settings(const nlohmann::json& config) = 0;
    virtual nlohmann::json get_settings() const = 0;
    virtual void set_enabled(bool enabled) = 0;
    virtual bool is_enabled() const = 0;
};

// Plugin dynamically load/unload ပြုလုပ်ရန် C-Factory Function Signature များ
using CreatePluginFn = IPlugin* (*)(Logger*);
using DestroyPluginFn = void (*)(IPlugin*);