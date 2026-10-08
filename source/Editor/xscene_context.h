#pragma once
#include "plugins/xscene.plugin/source/Editor/xscene_component_display.h"

// What the scene editing code works on: which scenes are open, which entity is selected, and the world and undo it edits.
// Nothing here knows about levels or Play, so the scene code can be used by any editor that opens scenes.
namespace xscene
{
    struct scene_state
    {
        // Every open scene stays resident until the editor removes it; opening one never closes another.
        std::vector<xecs::scene::guid> m_OpenScenes;

        xecs::scene::permanent_id  m_SelectedEntityId    = xecs::scene::invalid_permanent_id_v;
        xecs::component::entity    m_SelectedEntity      = {};    // the live handle: a cache, re-resolved from the id and scene after any world change
        xecs::scene::guid          m_SelectedEntityScene = {};    // which open scene the selection belongs to

        bool m_bEntityInspectorDirty = true;

        // The root of the tree (a Level, for the Level editor) is selected instead of an entity: the Inspector shows the root's own properties. A plain Select ends it.
        bool m_bRootSelected = false;

        // Ctrl-click set, separate from the primary selection above (which alone drives the Properties panel); a plain click
        // clears it. It is scoped to ONE scene at a time, because a prefab's members must all come from the same live scene.
        std::unordered_set<xecs::scene::permanent_id>  m_MultiSelectedEntityIds;
        // The same members in click order (an unordered_set has none): the first one clicked donates its folder and parent to
        // the synthetic root of a group. Kept in lockstep with the set at every mutation.
        std::vector<xecs::scene::permanent_id>         m_MultiSelectOrder;
        xecs::scene::guid                              m_MultiSelectScene;

        // Entity Properties: the category filter (empty = every component) and the Add Component popup's search text and
        // per-category open state (key = category, empty = "Uncategorized").
        std::string                            m_ComponentCategoryFilter;
        std::string                            m_ComponentSelectorSearchString;
        std::unordered_map<std::string, bool>  m_ComponentSelectorCategoryOpen;
    };

    // One editor's working set for scenes: its state, the owner of its world and the undo of its document. Commands reach it
    // through scene_command (World(), State(), SceneContext()); panels and helpers take it explicitly. The world is
    // destroyed and recreated on every Game.dll reload, so the context holds the unique_ptr that owns it and reads through
    // it each time (World()), never a pointer to the world itself.
    struct scene_context
    {
        scene_state&                               m_State;
        std::unique_ptr<xecs::game_mgr::instance>& m_pWorld;
        xundo::system&                             m_Undo;      // every edit of this editor's document goes through it
        component_display*                         m_pDisplay = nullptr;     // what this editor knows about its types (its game module's categories and sources), set by its session
        // A prefab was just made from this editor's world (MakePrefab, MakePrefabVariant): the editor that knows what Game the work is done under gives the prefab that Game to play with (prefabs_plan.md, D2).
        std::function<void(xresource::full_guid)>  m_OnPrefabMade;
        // The editor opens another editor for the prefab of an instance (the prefab row of the Entity Properties: Edit In Context, Edit Alone). A scene editor knows nothing of editors or levels, so the host gives it:
        // m_QueueCommand - a command of this editor run at a clean point of the frame, with the same queue the Level Tree's "Edit in Context" uses (an editor cannot be made while the panels draw);
        // m_WhyNotEditPrefab - empty when the action is allowed, else why not (the entry is disabled and says it): bInContext false is Edit Alone; the prefab's guid value.
        std::function<void(std::string)>           m_QueueCommand;
        std::function<std::string(bool, std::uint64_t)> m_WhyNotEditPrefab;
        std::function<void(std::uint64_t)>         m_QueueOpenPrefab;         // Edit Alone: the prefab (its guid value) opens in an editor of its own at the clean point of the frame, as a double click on it in the Asset Browser does (OpenPrefab is a workspace command, not one of the editor's own)

        xecs::game_mgr::instance& World() noexcept { return *m_pWorld; }
        const component_display& Display() const noexcept { static const component_display s_Nothing; return m_pDisplay ? *m_pDisplay : s_Nothing; }
    };

    // The active editor's scene context, provided to the host at startup. For code that has no session of its own, such as
    // a static drag-drop handler.
    inline scene_context* FindSceneContext() noexcept
    {
        auto* pHost = xeditor::host::current();
        return pHost ? pHost->find<scene_context>() : nullptr;
    }
}
