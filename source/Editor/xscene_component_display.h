#pragma once

// The editor's copy of each loaded component's display category and priority: filled from the game module every time a
// generation loads (LoadGameComponentDisplayInfo), read by the Entity Properties panel to filter and order components.
// Components that never announced one (the engine's own) sort before every categorized one.
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

namespace xscene
{
    struct component_display_info { std::string m_Category; int m_Priority = 0; };
    inline std::unordered_map<std::string, component_display_info> g_ComponentDisplayInfo;

    // Where a component or a system comes from: the script module that defines it and the file. Every panel that shows a type (the component header of the inspector, the Add Component list,
    // the System Registry, the chips of the systems that run on an entity) asks for it here, so what the editor knows about a type is shown the same way everywhere.
    struct type_source
    {
        bool            m_bKnown     = false;       // the host could say where the type comes from
        bool            m_bBuiltIn   = false;       // it is the engine's or the editor's own: no script module defines it
        std::uint64_t   m_Module     = 0;           // the ScriptModule resource that defines it (0: none)
        std::string     m_ModuleName;               // its name, "SoccerGame"
        std::string     m_File;                     // the file it is defined in, relative to the module's source_db ("soccer_components.h")
        std::string     m_Path;                     // the same, absolute
    };

    // Set by the host that knows the game module (the editor). Null: nothing is known about any type.
    inline std::function<type_source(bool bSystem, std::uint64_t Guid)> g_SourceOfType;
    // Shows the file of a type: the module's editor opens at it (or the system's editor for the file). Set by the application that owns the editors.
    inline std::function<bool(const type_source&)>                      g_OpenTypeSource;

    inline type_source SourceOfType(bool bSystem, std::uint64_t Guid) noexcept { return g_SourceOfType ? g_SourceOfType(bSystem, Guid) : type_source{}; }

    // "SoccerGame . soccer_components.h", "Engine" for a built-in type, "" when nothing is known: the one line every hint shows.
    inline std::string DescribeSource(const type_source& S) noexcept
    {
        if (!S.m_bKnown) return {};
        if (S.m_bBuiltIn) return "Built in (engine)";
        if (S.m_Module == 0) return S.m_File.empty() ? std::string() : S.m_File;
        return S.m_File.empty() ? S.m_ModuleName : S.m_ModuleName + " \xC2\xB7 " + S.m_File;
    }
}
