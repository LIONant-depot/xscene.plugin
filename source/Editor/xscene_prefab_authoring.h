#ifndef XSCENE_PREFAB_AUTHORING_H
#define XSCENE_PREFAB_AUTHORING_H
#pragma once
#include "dependencies/xeditor/include/xeditor/host.h"
#include "dependencies/xeditor/include/xeditor/session.h"

#include "dependencies/xundo/source/xundo_system.h"
#include "dependencies/xLIONCore/src/game/xlioncore_editor.h"

// Extracted from xscene_entity_inspector_bridge.h (mechanical move, phase 2 of the kit split - see the
// umbrella file's own top comment). The deletion of a subtree, the group root of a multi-selection, and
// the drag-payload + drop registration that turns a Level-tree entity into a Prefab asset
// (entity_to_prefab_drop, routed through the MakePrefab / MakePrefabVariant commands). Placing an
// instance is the engine's (xECSEditor::InstantiatePrefabInScene: prefabs_plan.md phase 3, an instance
// is a recipe). Meant to be included via the umbrella only, after xscene_prefab_overrides.h.

namespace xscene
{
    // Recursively deletes Entity and (if it has children) its whole live descendant subtree, scrubbing
    // scene bookkeeping/folder membership for each - the "whole group" analog of a single-entity
    // delete action.
    void DeleteEntitySubtree(xecs::game_mgr::instance& GameMgr, xecs::scene::instance& Scene, xecs::scene::guid SceneGuid, xecs::component::entity Entity, bool bRecordPrefabOverride = true) noexcept
    {
        // A member of a prefab instance taken away: the instance's recipe removes it (computed from the live instance when it is saved), so the
        // instance is written again. Only the top-level delete: the subtree goes with it.
        if (bRecordPrefabOverride)
            MarkContainingInstanceDirty(GameMgr, SceneGuid, Entity);

        auto& Ecs = xlioncore::Ecs(GameMgr);

        // Scrub Entity out of its own parent's children list, if it has one - otherwise the parent
        // keeps holding a dangling handle to an entity that's about to stop existing, rendering as a
        // broken/empty expandable row. Only meaningful on the TOP-LEVEL call (the row actually
        // clicked) - a recursive call's own parent is itself being deleted this same pass, so
        // scrubbing it is harmless but moot; doing it unconditionally here is simpler than threading a
        // "is this the top call" flag through the recursion.
        if (auto* pParent = Ecs.ParentOf(Entity))
        {
            const auto ParentEntity = pParent->m_Value;
            if (ParentEntity.isValid())
            {
                if (auto* pParentChildren = Ecs.ChildrenOf(ParentEntity))
                {
                    auto& List = pParentChildren->m_List;
                    std::erase_if(List, [&](auto& E) noexcept { return E.m_Value == Entity.m_Value; });
                    if (auto ParentIt = Scene.m_RuntimeToLocal.find(ParentEntity.m_Value); ParentIt != Scene.m_RuntimeToLocal.end())
                        GameMgr.m_SceneMgr.MarkEntityDirty(SceneGuid, ParentIt->second);
                }
            }
        }

        if (auto* pChildren = Ecs.ChildrenOf(Entity))
        {
            auto ChildEntities = pChildren->m_List;
            for (auto Child : ChildEntities)
                DeleteEntitySubtree(GameMgr, Scene, SceneGuid, Child, /*bRecordPrefabOverride*/ false);
        }

        if (auto It = Scene.m_RuntimeToLocal.find(Entity.m_Value); It != Scene.m_RuntimeToLocal.end())
        {
            const auto Id = It->second;
            Scene.m_RuntimeToLocal.erase(It);
            Scene.m_LocalToRuntime.erase(Id);
            Scene.m_InstanceMembers.erase(Id);
            GameMgr.m_SceneMgr.MarkEntityDeleted(SceneGuid, Id);
            ReparentEntityIntoFolder(Scene, Id, xecs::scene::invalid_folder_id_v);
        }

        Ecs.DeleteEntity(Entity);
    }

    // The Level tree's "Make Prefab" action - the multi-entity-aware counterpart of dragging a single
    // entity onto the asset browser (entity_to_prefab_drop, below). ClickedId is the row the context
    // menu/drag was started on; if it's part of a live multi-selection (2+ entities, all in this same
    // scene), the WHOLE selection becomes the group, otherwise just ClickedId alone.
    //
    // Root selection: a single selected entity becomes the root directly (covers "no children" and
    // "already has children" alike - CreatePrefabFromEntity/CloneEntityIntoPrefabGroup pulls in
    // children automatically). Multiple selected entities compute their "top-level" subset (those
    // whose parent, if any, isn't ALSO selected): exactly one top-level entity means the user
    // multi-selected an existing subtree - use it as the real root directly; otherwise (multiple
    // disjoint top-level entities) a synthetic root (Name + Children only) is created and every
    // top-level entity is reparented under it.
    xecs::component::entity DetermineGroupRoot(xecs::game_mgr::instance& GameMgr, xecs::scene::instance& Scene, xecs::scene::guid SceneGuid, scene_state& State, xecs::scene::permanent_id ClickedId) noexcept
    {
        std::vector<xecs::scene::permanent_id> SelectedIds;
        if (State.m_MultiSelectScene == SceneGuid && State.m_MultiSelectedEntityIds.size() > 1 && State.m_MultiSelectedEntityIds.contains(ClickedId))
            SelectedIds = State.m_MultiSelectOrder; // click order, not m_MultiSelectedEntityIds' own unordered iteration
        else
            SelectedIds.push_back(ClickedId);

        std::vector<xecs::component::entity> SelectedEntities;
        for (auto Id : SelectedIds)
            if (auto It = Scene.m_LocalToRuntime.find(Id); It != Scene.m_LocalToRuntime.end())
                SelectedEntities.push_back(It->second);
        std::printf("[MakePrefab] DetermineGroupRoot: %zu selected id(s), %zu resolved live entity(ies)\n", SelectedIds.size(), SelectedEntities.size());
        std::fflush(stdout);
        if (SelectedEntities.empty()) return {};

        if (SelectedEntities.size() == 1)
            return SelectedEntities.front();

        auto IsSelected = [&](xecs::component::entity E) noexcept
        {
            return std::find_if(SelectedEntities.begin(), SelectedEntities.end(), [&](auto& S) noexcept { return S.m_Value == E.m_Value; }) != SelectedEntities.end();
        };

        std::vector<xecs::component::entity> TopLevel;
        for (auto E : SelectedEntities)
        {
            bool bParentSelected = false;
            if (auto* pParent = xlioncore::Ecs(GameMgr).ParentOf(E))
                bParentSelected = IsSelected(pParent->m_Value);
            if (!bParentSelected) TopLevel.push_back(E);
        }
        std::printf("[MakePrefab] DetermineGroupRoot: %zu top-level entity(ies) among the selection\n", TopLevel.size());
        std::fflush(stdout);

        if (TopLevel.size() == 1)
            return TopLevel.front();

        // The synthetic root is brand new, so it has no history of its own to fall back on - without
        // this, it always starts loose at scene root, even when the entities it's about to wrap all
        // came from the SAME real folder or the SAME real
        // scene-hierarchy parent. A real PARENT wins over folder membership - matching how "an entity
        // with a parent is never ALSO in a folder" already works everywhere else in this tree - falling
        // back to whichever folder (if any) the FIRST top-level entity was in when there's no external
        // parent, and using ITS choice when several top-level entities disagree.
        xecs::component::entity InheritedParent;
        auto& Ecs = xlioncore::Ecs(GameMgr);
        if (auto* pFirstParent = Ecs.ParentOf(TopLevel.front()))
            InheritedParent = pFirstParent->m_Value;
        const auto InheritedFolderId = InheritedParent.isValid() ? xecs::scene::invalid_folder_id_v : FindFolderContaining(Scene, Scene.m_RuntimeToLocal.at(TopLevel.front().m_Value));

        auto Root = xlioncore::CreateEntityOf<xecs::component::children>(Ecs);

        if (InheritedParent.isValid())
        {
            Root = xlioncore::AddComponentsOf<xecs::component::parent>(Ecs, Root);
            Ecs.ParentOf(Root)->m_Value = InheritedParent;

            // Splice Root into whatever position the FIRST top-level entity held in ITS parent's own
            // children list, replacing it - the parent's list otherwise keeps pointing at that entity's
            // stale handle (about to be swapped for a fresh one below) instead of the new wrapper root.
            // Every OTHER top-level entity that ALSO happened to share this same external parent
            // (multi-selecting 2+ disjoint entities that are siblings under one real parent) must be
            // ERASED from this list entirely, not merely left alone - Root already represents the
            // whole group in the one spliced slot, and each of those other entities is ALSO about to
            // be migrated to a new handle by the reparent loop below, so leaving its OLD entry here
            // would be both a duplicate membership (appears under InheritedParent AND under Root) and
            // a dangling one (pointing at a handle the migration is about to invalidate).
            if (auto* pExtChildren = Ecs.ChildrenOf(InheritedParent))
            {
                auto& ExtChildren = pExtChildren->m_List;
                bool bSplicedRoot = false;
                std::erase_if(ExtChildren, [&](auto& C) noexcept
                {
                    const bool bIsTopLevelMember = std::find_if(TopLevel.begin(), TopLevel.end(), [&](auto& T) noexcept { return T.m_Value == C.m_Value; }) != TopLevel.end();
                    if (!bIsTopLevelMember) return false;
                    if (!bSplicedRoot) { C = Root; bSplicedRoot = true; return false; }
                    return true;
                });
            }
        }

        const auto RootId = NextFreeEntityId(Scene);
        Scene.m_LocalToRuntime[RootId]        = Root;
        Scene.m_RuntimeToLocal[Root.m_Value]  = RootId;
        GameMgr.m_SceneMgr.MarkEntityNew(SceneGuid, RootId);
        if (InheritedFolderId != xecs::scene::invalid_folder_id_v)
            ReparentEntityIntoFolder(Scene, RootId, InheritedFolderId);

        for (auto E : TopLevel)
        {
            const auto OldId = Scene.m_RuntimeToLocal.at(E.m_Value);

            auto NewE = xlioncore::AddComponentsOf<xecs::component::parent>(Ecs, E);
            Ecs.ParentOf(NewE)->m_Value = Root;

            Scene.m_RuntimeToLocal.erase(E.m_Value);
            Scene.m_LocalToRuntime[OldId]         = NewE;
            Scene.m_RuntimeToLocal[NewE.m_Value]  = OldId;
            GameMgr.m_SceneMgr.MarkEntityDirty(SceneGuid, OldId);

            Ecs.ChildrenOf(Root)->m_List.push_back(NewE);

            if (State.m_SelectedEntityId == OldId)
            {
                State.m_SelectedEntity        = NewE;
                State.m_bEntityInspectorDirty = true;
            }

            // Entities with a parent are excluded from folder membership entirely (rendered via their
            // parent's own row instead) - scrub whatever folder this entity was in.
            ReparentEntityIntoFolder(Scene, OldId, xecs::scene::invalid_folder_id_v);
        }

        return Root;
    }

    // Payload for dragging a scene entity onto an asset-browser folder to create a Prefab from it -
    // registered against xresource_editor::external_drop_registration_base (see xresource_editor_asset_browser.h) so the browser
    // can accept it without knowing anything about xECS/scenes. Carries the scene guid + the entity's
    // scene-local permanent_id rather than a live xecs::component::entity handle, since the handle
    // itself is only guaranteed valid for the frame it was captured in - re-resolving it through the
    // scene's own maps at drop time is what makes this safe across the drag's lifetime.
    struct entity_drag_payload_t
    {
        xecs::scene::guid          m_SceneGuid;
        xecs::scene::permanent_id  m_Id;
    };



    // Set by the editor once the command/undo system exists.
    // entity_to_prefab_drop::OnDrop cannot include the MakePrefab command headers (include order /
    // cycle with this file), so the drop path calls through this hook instead of CreatePrefab* directly.
    using make_prefab_drop_fn_t = xresource::full_guid(*)(xresource_editor::library_mgr&, xresource_editor::library::guid, xresource::full_guid, const entity_drag_payload_t&) noexcept;
    inline make_prefab_drop_fn_t g_MakePrefabDropHandler = nullptr;

    struct entity_to_prefab_drop final : xresource_editor::external_drop_registration_base
    {
        entity_to_prefab_drop() noexcept : xresource_editor::external_drop_registration_base{ "LEVEL_ENTITY_DRAG" } {}

        xresource::full_guid OnDrop(xresource_editor::library_mgr& AssetMgr, xresource_editor::library::guid LibraryGUID, xresource::full_guid ParentGUID, const void* pData, std::size_t Size) const noexcept override
        {
            if (Size != sizeof(entity_drag_payload_t) || FindSceneContext() == nullptr) return {};
            if (g_MakePrefabDropHandler == nullptr) return {};
            auto& Payload = *reinterpret_cast<const entity_drag_payload_t*>(pData);
            // Routed through MakePrefab / MakePrefabVariant commands (see MakePrefabDropViaCommands) so
            // drag-to-browser Make Prefab is undoable like every other scene/asset mutation.
            return g_MakePrefabDropHandler(AssetMgr, LibraryGUID, ParentGUID, Payload);
        }
    };
    inline static entity_to_prefab_drop g_EntityToPrefabDrop{};

} // namespace xscene

#endif // XSCENE_PREFAB_AUTHORING_H
