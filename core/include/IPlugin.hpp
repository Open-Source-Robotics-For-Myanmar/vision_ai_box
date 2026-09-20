#pragma once

#include "FrameContext.hpp"
#include <nlohmann/json.hpp>
#include <string>

class Logger;

class IPlugin {
public:
    virtual ~IPlugin() = default;

    virtual std::string get_name() const = 0;
    virtual void init(const nlohmann::json& config) = 0;
    virtual void process(const FrameContext& frame) = 0;
    virtual void update_settings(const nlohmann::json& config) = 0;
    virtual nlohmann::json get_settings() const = 0;
    virtual void set_enabled(bool enabled) = 0;
    virtual bool is_enabled() const = 0;
};

// Plugin dynamically load/unload ပြုလုပ်ရန် C-Factory Function Signature များ
using CreatePluginFn = IPlugin* (*)(Logger*);
using DestroyPluginFn = void (*)(IPlugin*);