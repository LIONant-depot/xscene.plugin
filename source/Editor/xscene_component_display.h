#pragma once

// The editor's copy of each loaded component's display category and priority: filled from the game module every time a
// generation loads (LoadGameComponentDisplayInfo), read by the Entity Properties panel to filter and order components.
// Components that never announced one (the engine's own) sort before every categorized one.
#include <string>
#include <unordered_map>

namespace xscene
{
    struct component_display_info { std::string m_Category; int m_Priority = 0; };
    inline std::unordered_map<std::string, component_display_info> g_ComponentDisplayInfo;
}
