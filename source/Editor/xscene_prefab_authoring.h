#ifndef XSCENE_PREFAB_AUTHORING_H
#define XSCENE_PREFAB_AUTHORING_H
#pragma once
#include "dependencies/xeditor/include/xeditor/host.h"
#include "dependencies/xeditor/include/xeditor/session.h"

#include "dependencies/xundo/source/xundo_system.h"
#include "dependencies/xLIONCore/src/game/xlioncore_editor.h"

// Extracted from xscene_entity_inspector_bridge.h (mechanical move, phase 2 of the kit split - see the
// umbrella file's own top comment). Prefab creation/instancing/deletion (RegisterInstantiatedSubtree
// through CreatePrefabVariantFromInstance), plus the drag-payload + drop registration that turns a
// Level-tree entity into a Prefab asset (entity_to_prefab_drop) - kept together rather than split
// further since the drop handler directly calls the authoring functions above it and shares their
// the editor context, not a separately-reusable concern on its own. Meant to be
// included via the umbrella only, after xscene_prefab_overrides.h (AttachPrefabInstanceComponent).

namespace xscene
{
    // Recursively registers every entity in a freshly-instantiated prefab subtree (Entity itself,
    // plus - if it has children - every descendant) into Scene's bookkeeping under a freshly minted
    // permanent_id each, marking each new. Shared by InstantiatePrefabIntoScene (the whole returned
    // group needs registering) and CreatePrefabFromGroupRoot (only the NEW group's children need fresh
    // ids - its root keeps a preserved one, registered separately by the caller).
    void RegisterInstantiatedSubtree(xecs::game_mgr::instance& GameMgr, xecs::scene::instance& Scene, xecs::scene::guid SceneGuid, xecs::component::entity Entity) noexcept
    {
        const auto Id = NextFreeEntityId(Scene);
        Scene.m_LocalToRuntime[Id]              = Entity;
        Scene.m_RuntimeToLocal[Entity.m_Value]  = Id;
        GameMgr.m_SceneMgr.MarkEntityNew(SceneGuid, Id);

        auto* pChildren = xlioncore::Ecs(GameMgr).ChildrenOf(Entity);
        if (pChildren == nullptr) return;

        // Snapshot - registering a child only ever touches Scene's own maps, never this entity's OWN
        // children list, so a plain copy is enough (no in-place-mutation hazard to guard against here).
        auto ChildEntities = pChildren->m_List;
        for (auto Child : ChildEntities)
            RegisterInstantiatedSubtree(GameMgr, Scene, SceneGuid, Child);
    }

    // Loads PrefabGuid (if not already resident) and instantiates it into Scene under a fresh
    // permanent_id - the shared tail of both the drag-a-prefab-onto-the-scene-tree flow and (until it
    // existed) the old "+ Instantiate Prefab" button. TargetFolder (invalid = loose, rendered directly
    // at scene root) lets a drop directly onto a specific folder row land the new instance there
    // instead of always landing loose regardless of where the user actually dropped it.
    void InstantiatePrefabIntoScene(xecs::game_mgr::instance& GameMgr, xecs::scene::instance& Scene, xecs::prefab::guid PrefabGuid, xecs::scene::folder_id TargetFolder = xecs::scene::invalid_folder_id_v) noexcept
    {
        if (auto Err = xlioncore::Ecs(GameMgr).EnsureLoadedPrefab(PrefabGuid); Err)
        {
            xeditor::NotifyToast(std::format("Failed to load Prefab: {}", Err.getMessage()));
            return;
        }

        auto RootIt = GameMgr.m_PrefabMgr.m_PrefabList.find(PrefabGuid.m_Instance.m_Value);
        if (RootIt == GameMgr.m_PrefabMgr.m_PrefabList.end()) return;

        // bRemoveRoot=false - a multi-entity ("Scene-Prefab") root must survive instancing so it comes
        // back as a real, independent entity here; bRemoveRoot=true (the default) is for splicing a
        // prefab's CHILDREN directly onto a caller-supplied existing entity, discarding the prefab's
        // own root - not what this wants (this needs one standalone instantiated group, root included).
        auto NewRoot = xlioncore::Ecs(GameMgr).CreatePrefabInstance(RootIt->second, /*bRemoveRoot=*/false);

        // Registers the whole group (root + every descendant, each under a freshly minted id).
        RegisterInstantiatedSubtree(GameMgr, Scene, Scene.m_Guid, NewRoot);

        const auto RootId = Scene.m_RuntimeToLocal.at(NewRoot.m_Value);
        if (TargetFolder != xecs::scene::invalid_folder_id_v)
            ReparentEntityIntoFolder(Scene, RootId, TargetFolder);
        AttachPrefabInstanceComponent(GameMgr, Scene, RootId, NewRoot, PrefabGuid, nullptr); // brand-new entity, can't already be selected
    }

    // Recursively deletes Entity and (if it has children) its whole live descendant subtree, scrubbing
    // scene bookkeeping/folder membership for each - the "whole group" analog of a single-entity
    // delete action.
    void DeleteEntitySubtree(xecs::game_mgr::instance& GameMgr, xecs::scene::instance& Scene, xecs::scene::guid SceneGuid, xecs::component::entity Entity, bool bRecordPrefabOverride = true) noexcept
    {
        // Only the top-level delete records a hierarchy diff - recursive child deletes are covered
        // by that one removed path (and would scramble MemberPath bookkeeping mid-teardown).
        if (bRecordPrefabOverride)
            RecordRemovedChildOverride(GameMgr, Scene, SceneGuid, Entity);

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

    // Step 2: given a resolved group root (a real, live entity - either the single dragged/clicked
    // entity, an existing subtree's own root, or DetermineGroupRoot's synthetic one), creates the
    // Prefab asset (at LibraryGUID/ParentGUID - the caller's own drop target) and converts the
    // original live group into an instance of it, generalizing the single-entity "drag out becomes an
    // instance" behavior.
    xresource::full_guid CreatePrefabFromGroupRoot(xecs::game_mgr::instance& GameMgr, xecs::scene::instance& Scene, xecs::scene::guid SceneGuid, scene_state* pState, xresource_editor::library_mgr& AssetMgr, xresource_editor::library::guid LibraryGUID, xresource::full_guid ParentGUID, xecs::component::entity Root) noexcept
    {
        // If Root already had a parent in the live scene (e.g. a single child entity that's part of
        // some OTHER, unrelated hierarchy, or a whole existing subtree being grouped), that positional
        // link is NOT part of what gets persisted (a prefab root never carries its own parent) -
        // captured here so the freshly-instantiated root can be spliced back into the exact same
        // position afterward, rather than unexpectedly falling out to scene-root.
        xecs::component::entity OriginalParent;
        if (auto* pRootParent = xlioncore::Ecs(GameMgr).ParentOf(Root))
            OriginalParent = pRootParent->m_Value;

        const auto RootId           = Scene.m_RuntimeToLocal.at(Root.m_Value);
        const bool bRootWasSelected = pState && (pState->m_SelectedEntityId == RootId);
        const auto StaleRootValue   = Root.m_Value; // Root's own OLD live handle - about to be deleted; only ever compared, never dereferenced, below

        // Folder membership is keyed by RootId (a permanent_id, preserved across this whole
        // conversion) rather than by live entity handle, so in principle it wouldn't need capturing -
        // except DeleteEntitySubtree (below) explicitly scrubs it as part of deleting the OLD live
        // root (ReparentEntityIntoFolder(..., invalid_folder_id_v)), since from ITS point of view the
        // entity is simply being removed. Without capturing and restoring it here, RootId would render
        // loose at scene root after re-registration instead of back in its original folder.
        const auto OriginalFolderId = FindFolderContaining(Scene, RootId);

        // A multi-select group gets a synthetic root with no name of its own.
        std::string Name = "Prefab Root";
        if (auto* pName = FindEntityName(Scene, RootId)) Name = *pName;

        const xresource::full_guid NewGuid   = AssetMgr.NewAsset(LibraryGUID, xresource::full_guid{ {}, xecs::prefab::type_guid_v }, ParentGUID, Name);
        const xecs::prefab::guid   PrefabGuid = NewGuid;

        std::printf("[MakePrefab] CreatePrefabFromGroupRoot: RootId=%u Name='%s' - cloning into prefab\n", RootId, Name.c_str());
        std::fflush(stdout);

        xlioncore::Ecs(GameMgr).CreatePrefabFromEntity(Root, PrefabGuid);
        if (auto Err = xlioncore::Ecs(GameMgr).SavePrefab(PrefabGuid); Err)
        {
            xeditor::NotifyToast(std::format("Failed to save new Prefab: {}", Err.getMessage()));
            return {};
        }

        // Convert the original live group into an instance of the new prefab: delete the original
        // root+descendants, instantiate a fresh copy, splice it back into whatever OriginalParent
        // held, then register it under RootId's preserved permanent_id (so scene bookkeeping/
        // selection keep referencing "the same" entity) - every child gets a freshly minted id
        // instead (they're new scene entities, never existed as "an instance" before).
        DeleteEntitySubtree(GameMgr, Scene, SceneGuid, Root);

        auto NewRoot = xlioncore::Ecs(GameMgr).CreatePrefabInstance(GameMgr.m_PrefabMgr.m_PrefabList.at(PrefabGuid.m_Instance.m_Value), /*bRemoveRoot=*/false);
        std::printf("[MakePrefab] CreatePrefabFromGroupRoot: instantiated fresh copy, NewRoot.isValid=%d NewRoot.isZombie=%d\n", NewRoot.isValid(), NewRoot.isZombie());
        std::fflush(stdout);

        if (OriginalParent.isValid())
        {
            auto& Ecs = xlioncore::Ecs(GameMgr);
            NewRoot = xlioncore::AddComponentsOf<xecs::component::parent>(Ecs, NewRoot);
            Ecs.ParentOf(NewRoot)->m_Value = OriginalParent;

            if (auto* pOriginalChildren = Ecs.ChildrenOf(OriginalParent))
            {
                auto& OPChildren = pOriginalChildren->m_List;
                for (auto& C : OPChildren)
                    if (C.m_Value == StaleRootValue) { C = NewRoot; break; }
            }
        }

        if (auto* pNewChildren = xlioncore::Ecs(GameMgr).ChildrenOf(NewRoot))
        {
            auto ChildEntities = pNewChildren->m_List;
            std::printf("[MakePrefab] CreatePrefabFromGroupRoot: NewRoot has %zu child(ren) to register\n", ChildEntities.size());
            std::fflush(stdout);
            for (auto Child : ChildEntities)
                RegisterInstantiatedSubtree(GameMgr, Scene, SceneGuid, Child);
        }

        Scene.m_LocalToRuntime[RootId]           = NewRoot;
        Scene.m_RuntimeToLocal[NewRoot.m_Value]  = RootId;

        // Restore RootId's folder membership, scrubbed by DeleteEntitySubtree above - but only when
        // the root did NOT get a parent restored (an entity with a parent is never ALSO placed via
        // folder membership - the parent becomes the folder, per this whole tree's own convention).
        if (false == OriginalParent.isValid())
            ReparentEntityIntoFolder(Scene, RootId, OriginalFolderId);

        AttachPrefabInstanceComponent(GameMgr, Scene, RootId, NewRoot, PrefabGuid, pState);

        // RootId lives on as the new instance: cancel the delete DeleteEntitySubtree recorded for it,
        // or SaveScene (where Deleted wins over Dirty) removes the root's entity file.
        Scene.m_PendingChanges[RootId].m_Deleted -= 1;
        GameMgr.m_SceneMgr.MarkEntityDirty(SceneGuid, RootId);

        if (pState)
        {
            pState->m_MultiSelectedEntityIds.clear();
            pState->m_MultiSelectOrder.clear();
            if (bRootWasSelected)
            {
                if (auto It = Scene.m_LocalToRuntime.find(RootId); It != Scene.m_LocalToRuntime.end())
                {
                    pState->m_SelectedEntity        = It->second;
                    pState->m_SelectedEntityScene   = SceneGuid;
                    pState->m_bEntityInspectorDirty = true;
                }
            }
            else if (pState->m_SelectedEntityScene == SceneGuid && pState->m_SelectedEntityId != xecs::scene::invalid_permanent_id_v)
            {
                // The previously-selected entity might have been one of the OTHER group members (a
                // non-root one). Non-root members get deleted and replaced with a FRESH entity under
                // a FRESH id (RegisterInstantiatedSubtree), so there's no principled "same identity"
                // to preserve for them the way the root's own preserved RootId gives one - if the
                // id/handle pairing no longer matches what's actually live, the safe move is to clear
                // the selection rather than leave pState->m_SelectedEntity holding a stale handle into
                // an entity that DeleteEntitySubtree already destroyed (a stale handle used later
                // trips xECS's own generation/validation assert).
                auto It = Scene.m_LocalToRuntime.find(pState->m_SelectedEntityId);
                if (It == Scene.m_LocalToRuntime.end() || It->second.m_Value != pState->m_SelectedEntity.m_Value)
                {
                    pState->m_SelectedEntityId      = xecs::scene::invalid_permanent_id_v;
                    pState->m_SelectedEntity        = {};
                    pState->m_SelectedEntityScene   = {};
                    pState->m_bEntityInspectorDirty = true;
                }
            }
        }

        std::printf("[MakePrefab] CreatePrefabFromGroupRoot: done, RootId=%u still resident=%d\n", RootId, Scene.m_LocalToRuntime.contains(RootId));
        std::fflush(stdout);

        return NewGuid;
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

    // Unity's own "Prefab Variant" fast path: dragging a SINGLE existing prefab instance (no other
    // entity in the active selection) into the asset browser creates a variant WITHOUT touching the
    // scene object's own live identity - Unity re-points that same GameObject's prefab connection at
    // the new variant rather than deleting and recreating it. Deliberately narrower than
    // CreatePrefabFromGroupRoot (which always deletes+recreates): a multi-select group has no single
    // existing identity to preserve in the first place (a brand-new synthetic root is minted either
    // way), and a PLAIN entity (never instanced) has no existing prefab connection to re-point - both
    // of those keep going through the general path unchanged.
    xresource::full_guid CreatePrefabVariantFromInstance(xecs::game_mgr::instance& GameMgr, xecs::scene::instance& Scene, xecs::scene::permanent_id Id, xecs::component::entity Entity, xresource_editor::library_mgr& AssetMgr, xresource_editor::library::guid LibraryGUID, xresource::full_guid ParentGUID) noexcept
    {
        std::string Name = "Prefab";
        if (auto* pName = FindEntityName(Scene, Id)) Name = *pName;

        const xresource::full_guid NewGuid   = AssetMgr.NewAsset(LibraryGUID, xresource::full_guid{ {}, xecs::prefab::type_guid_v }, ParentGUID, Name);
        const xecs::prefab::guid   PrefabGuid = NewGuid;

        std::printf("[MakePrefab] CreatePrefabVariantFromInstance: Id=%u Name='%s' - capturing into a variant, live entity untouched\n", Id, Name.c_str());
        std::fflush(stdout);

        xlioncore::Ecs(GameMgr).CreatePrefabFromEntity(Entity, PrefabGuid);
        if (auto Err = xlioncore::Ecs(GameMgr).SavePrefab(PrefabGuid); Err)
        {
            xeditor::NotifyToast(std::format("Failed to save new Prefab: {}", Err.getMessage()));
            return {};
        }

        // Re-point the SAME live entity's own bookkeeping at the new variant - no deletion, no fresh
        // instantiation needed: this entity's current data IS already exactly what a fresh instance of
        // the new variant looks like, since it's what the variant was just captured FROM. Overrides are
        // cleared (matching AttachPrefabInstanceComponent's own reasoning) since they were computed
        // relative to whatever this entity pointed at BEFORE - a save recomputes them fresh regardless.
        auto& PI = *xlioncore::ComponentOf<xecs::editor::prefab_instance>(xlioncore::Ecs(GameMgr), Entity);
        PI.m_PrefabInstance = PrefabGuid;
        PI.m_lComponents.clear();
        PI.m_ComponentDiffs.clear();
        PI.m_HierarchyDiffs.clear();
        GameMgr.m_SceneMgr.MarkEntityDirty(Scene.m_Guid, Id);

        return NewGuid;
    }

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
