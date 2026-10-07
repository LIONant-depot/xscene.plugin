#ifndef XSCENE_PREFAB_OVERRIDES_H
#define XSCENE_PREFAB_OVERRIDES_H
#pragma once

// Extracted from xscene_entity_inspector_bridge.h (mechanical move, phase 2 of the kit split - see the
// umbrella file's own top comment). Prefab-instance lookup, override-entry bookkeeping, and the
// entity-reference-component resolver the Entity Properties inspector uses for both (adjacent,
// small, and used by the same override-rendering code path - kept here rather than split out
// further, per "don't explode into too many files"). Meant to be included via the umbrella only.

namespace xscene
{
    // Returns the entity's live prefab_instance component, or nullptr if it isn't a prefab instance
    // (a plain entity never has this component).
    xecs::editor::prefab_instance* FindPrefabInstance(xecs::game_mgr::instance& GameMgr, xecs::component::entity Entity) noexcept
    {
        if (Entity.isValid() == false) return nullptr;
        return xlioncore::ComponentOf<xecs::editor::prefab_instance>(xlioncore::Ecs(GameMgr), Entity);
    }

    // The prefab instance an entity is part of (prefabs_plan.md, phase 3: an instance is a recipe): itself when it carries the recipe (and is
    // not a member of another instance), else the instance whose member it is, with the member's address in it (xecs::editor::member_address:
    // the member's id in its prefab, one per nested instance crossed). An entity of the scene under a member is not part of the instance: it is
    // an ordinary entity of the scene (it has its own file). m_pScene/m_RootId say where the instance lives (null/invalid for an entity of no
    // scene).
    struct prefab_instance_context
    {
        xecs::editor::prefab_instance* m_pPI = nullptr;
        xecs::component::entity        m_RootEntity{};
        xecs::editor::member_address   m_Member;
        xecs::scene::instance*         m_pScene = nullptr;
        xecs::scene::permanent_id      m_RootId = xecs::scene::invalid_permanent_id_v;
    };

    prefab_instance_context FindContainingPrefabInstance(xecs::game_mgr::instance& GameMgr, xecs::component::entity Entity) noexcept
    {
        prefab_instance_context Ctx;
        if (Entity.isValid() == false) return Ctx;

        for (auto& pScene : GameMgr.m_SceneMgr.m_SceneInstances)
        {
            auto It = pScene->m_RuntimeToLocal.find(Entity.m_Value);
            if (It == pScene->m_RuntimeToLocal.end()) continue;

            Ctx.m_pScene = pScene.get();
            if (auto M = pScene->m_InstanceMembers.find(It->second); M != pScene->m_InstanceMembers.end())
            {
                auto RootIt = pScene->m_LocalToRuntime.find(M->second.m_Root);
                if (RootIt == pScene->m_LocalToRuntime.end()) return {};
                Ctx.m_pPI        = FindPrefabInstance(GameMgr, RootIt->second);
                Ctx.m_RootEntity = RootIt->second;
                Ctx.m_RootId     = M->second.m_Root;
                Ctx.m_Member     = M->second.m_Address;
                if (Ctx.m_pPI == nullptr) return {};
                return Ctx;
            }
            if (auto* pPI = FindPrefabInstance(GameMgr, Entity))
            {
                Ctx.m_pPI        = pPI;
                Ctx.m_RootEntity = Entity;
                Ctx.m_RootId     = It->second;
                return Ctx;
            }
            return {};
        }

        if (auto* pPI = FindPrefabInstance(GameMgr, Entity)) { Ctx.m_pPI = pPI; Ctx.m_RootEntity = Entity; }
        return Ctx;
    }

    // The address of an override entry or a diff (as the inspector and the commands compare them).
    inline bool SameMember(std::span<const std::uint64_t> A, std::span<const std::uint64_t> B) noexcept
    {
        return std::ranges::equal(A, B);
    }

    // Resolves a live entity handle of UNKNOWN owning scene (all the inspector ever has for an
    // xecs::component::entity_reference field) into a friendly label + which open scene owns it, by
    // scanning every currently-open scene's m_RuntimeToLocal - same label logic the Level tree's own
    // entity rows already use (Name component if present, else "Entity #Id"), just without a SceneGuid
    // known up front the way a tree row already has one. Only scenes the user has actually opened are
    // searched - a reference into a scene nobody opened this session simply can't be resolved to a
    // live handle yet (matches how the reference itself only round-trips through save/load, not
    // through any live lookup that would need every scene loaded just to inspect one entity).
    bool ResolveEntityReference(xecs::game_mgr::instance& GameMgr, scene_state& State, xecs::component::entity Entity, std::string& OutLabel, xecs::scene::guid& OutSceneGuid) noexcept
    {
        if (Entity.isValid() == false) return false;

        for (auto& SceneGuid : State.m_OpenScenes)
        {
            auto* pScene = GameMgr.m_SceneMgr.Find(SceneGuid);
            if (pScene == nullptr) continue;

            auto It = pScene->m_RuntimeToLocal.find(Entity.m_Value);
            if (It == pScene->m_RuntimeToLocal.end()) continue;

            const auto Id = It->second;
            OutLabel = EntityDisplayName(*pScene, Id);
            std::string SceneLabel;
            xresource_editor::RemapGUIDToString(SceneLabel, xresource::full_guid{ SceneGuid.m_Instance, SceneGuid.m_Type });
            OutLabel += std::format(" ({})", SceneLabel);
            OutSceneGuid = SceneGuid;
            return true;
        }
        return false;
    }

    // Components that xECS itself attaches/manages internally (never meaningful to add/remove/edit
    // by hand): entity is the identity itself; parent/children carry raw xecs::component::entity
    // references, which the shared xproperty inspector has no rendering style for at all (asserts -
    // "UNHANDLED ATOMIC STYLE: TypeName='entity'" - the moment one is appended); ref_count/
    // share_filter/share_as_data_exclusive_tag are share-component bookkeeping; prefab::tag/root only
    // ever live on a prefab's own root entity (in mgr::m_PrefabList), never on a scene entity; and
    // editor::prefab_instance is this editor's own override-tracking bookkeeping, edited only through
    // the dedicated prefab-override UI. Centralized here so Add/Remove Component and the inspector
    // rebuild loop can't independently drift out of sync on this list.
    // IsComponentType<T>() (xecs_component_type.h) compares by GUID, not by info_v<T> address - safe
    // regardless of which binary ends up owning each of these 9 built-ins' registration, unlike a raw
    // `pInfo == &info_v<T>` (which happens to hold today only because everything below is compiled
    // into this one host exe - see IsComponentType's own comment for the two live bugs elsewhere in
    // this codebase where that assumption silently broke instead).
    // The two components that are the hierarchy itself: SHOWN in the inspector (a component is a type the systems query on: the person must see every one an entity has) but only
    // read: who the parent is and who the children are is changed by making and deleting entities (a child list edited by hand would not agree with the parents), never by hand;
    // and they are not in the Add Component list and have no x.
    bool IsStructuralComponent(const xecs::component::type::info* pInfo) noexcept
    {
        using xecs::component::type::IsComponentType;
        return IsComponentType<xecs::component::parent>(pInfo) || IsComponentType<xecs::component::children>(pInfo);
    }

    // The state of an entity in the editor (editor_disable, editor_no_render: xecs_editor.h): shown like any component, but not in the Add Component list - the Level Tree's power and eye are what set it.
    bool IsEditorStateComponent(const xecs::component::type::info* pInfo) noexcept
    {
        using xecs::component::type::IsComponentType;
        return IsComponentType<xecs::editor::disable_tag>(pInfo) || IsComponentType<xecs::editor::no_render_tag>(pInfo);
    }

    bool IsInternalComponent(const xecs::component::type::info* pInfo) noexcept
    {
        using xecs::component::type::IsComponentType;
        return IsComponentType<xecs::component::entity>(pInfo)
            || IsComponentType<xecs::component::parent>(pInfo)
            || IsComponentType<xecs::component::children>(pInfo)
            || IsComponentType<xecs::component::ref_count>(pInfo)
            || IsComponentType<xecs::component::share_filter>(pInfo)
            || IsComponentType<xecs::component::share_as_data_exclusive_tag>(pInfo)
            || IsComponentType<xecs::prefab::tag>(pInfo)
            || IsComponentType<xecs::prefab::root>(pInfo)
            || IsComponentType<xecs::editor::prefab_instance>(pInfo);
    }


    // A member of an instance was deleted (its subtree goes with it): the instance's recipe changes (the member is removed), so the instance's
    // root is written at the next save. The recipe itself is refreshed from the live instance then (xecs::prefab::recipe::RefreshRecipe).
    inline void MarkContainingInstanceDirty(xecs::game_mgr::instance& GameMgr, xecs::scene::guid SceneGuid, xecs::component::entity Entity) noexcept
    {
        auto Ctx = FindContainingPrefabInstance(GameMgr, Entity);
        if (Ctx.m_pPI == nullptr || Ctx.m_Member.empty() || Ctx.m_RootId == xecs::scene::invalid_permanent_id_v) return;
        GameMgr.m_SceneMgr.MarkEntityDirty(SceneGuid, Ctx.m_RootId);
    }

    // Finds the override-tracking entry for a given (component type, member) pair on a prefab instance, creating one if none exists yet. Member
    // empty means the entity that carries the prefab_instance; otherwise the member's address (xecs::editor::member_address).
    xecs::editor::prefab_component_override& FindOrCreateOverrideEntry(xecs::editor::prefab_instance& PI, std::uint64_t ComponentTypeGuidValue, std::span<const std::uint64_t> Member) noexcept
    {
        for (auto& C : PI.m_lComponents)
            if (C.m_ComponentTypeGuid == ComponentTypeGuidValue && SameMember(C.m_Member, Member)) return C;

        PI.m_lComponents.push_back(xecs::editor::prefab_component_override
        { .m_ComponentTypeGuid = ComponentTypeGuidValue
        , .m_Member            = xecs::editor::member_address(Member.begin(), Member.end())
        , .m_PropertyOverrides = {}
        });
        return PI.m_lComponents.back();
    }

} // namespace xscene

#endif // XSCENE_PREFAB_OVERRIDES_H
