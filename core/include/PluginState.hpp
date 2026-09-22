#pragma once

#include <string_view>

enum class PluginState {
    Unloaded,
    Loaded,
    Active
};

constexpr std::string_view plugin_state_name(PluginState state) noexcept
{
    switch (state) {
    case PluginState::Unloaded:
        return "unloaded";
    case PluginState::Loaded:
        return "loaded";
    case PluginState::Active:
        return "active";
    }
    return "unloaded";
}