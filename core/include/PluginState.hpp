#pragma once

#include <string_view>

enum class PluginState {
    Unloaded,
    Unloading,
    Loading,
    Loaded,
    Active
};

constexpr std::string_view plugin_state_name(PluginState state) noexcept
{
    switch (state) {
    case PluginState::Unloaded:
        return "unloaded";
    case PluginState::Unloading:
        return "unloading";
    case PluginState::Loading:
        return "loading";
    case PluginState::Loaded:
        return "loaded";
    case PluginState::Active:
        return "active";
    }
    return "unloaded";
}