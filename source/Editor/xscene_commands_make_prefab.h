#ifndef XSCENE_COMMANDS_MAKE_PREFAB_H
#define XSCENE_COMMANDS_MAKE_PREFAB_H
#pragma once

// MakePrefab / MakePrefabVariant - the gap deliberately deferred from both the prior gap-closing
// session (the command/undo known-gaps list) and the Asset Browser command-layer session
// (the asset browser command layer notes): "Make Prefab" creates a real Prefab ASSET on disk
// (AssetMgr.NewAsset) AND converts a live entity/group into an instance of it - a genuine composition
// of what CreateAsset (xresource_editor_commands_assets.h) and the entity-subtree snapshot/restore machinery
// (SnapshotSubtreeForRestore/RestoreSubtreeFromSnapshot, xscene_commands_entity_lifecycle.h) each already
// solve on their own. This file is that composition, not a third reimplementation.
//
// Two distinct existing functions in xscene_prefab_authoring.h, two distinct commands here, matching
// that file's own split:
//   CreatePrefabFromGroupRoot - the general path: creates the asset, then DELETES the original live
//   root+descendants and INSTANTIATES a fresh copy under the prefab, splicing back into the original
//   parent/folder position. Wrapped below as MakePrefab.
//   CreatePrefabVariantFromInstance - the fast path for a SINGLE entity that's already a prefab
//   instance (no multi-select): creates the asset and just RE-POINTS that same live entity's own
//   prefab_instance component at it (clearing overrides) - no delete/recreate at all. Wrapped below
//   as MakePrefabVariant.
//
// DELIBERATELY NOT WRAPPED, same "flag rather than rush" precedent as EmptyTrashcan/Duplicate before
// it: DetermineGroupRoot's own synthetic-root-creation path (xscene_prefab_authoring.h) - multi-
// selecting 2+ DISJOINT top-level entities (no single existing subtree already covers the whole
// selection) synthesizes a brand-new "Prefab Root" entity and reparents each selected entity under
// it, BEFORE either MakePrefab/MakePrefabVariant below ever runs. That synthesis step is a real,
// separate mutation of its own (creates an entity, moves several others) that neither command here
// captures for undo - both commands below assume the ROOT they're given is already fully resolved
// (a single existing entity, root of a real subtree already, or from that synthesis step run
// separately/manually) exactly as DetermineGroupRoot itself already hands back today. Making THAT
// step undo-routed too is a distinct, smaller follow-up, not folded in here.
#include "dependencies/xresource_pipeline_v2/source/editor/xresource_editor_commands_assets.h"
#include "plugins/xscene.plugin/source/Editor/xscene_commands_entity_lifecycle.h"

namespace xscene::commands
{
    // The recipes of the prefab instances in Root's subtree (Root included), refreshed from what their members are now: a clone into a prefab
    // takes an instance as one opaque member with its recipe (its members are not cloned), so the recipe must say everything first.
    inline void RefreshRecipesInSubtree(xecs::game_mgr::instance& GameMgr, xecs::scene::instance& Scene, xecs::component::entity Root) noexcept
    {
        auto& Ecs = xlioncore::Ecs(GameMgr);
        std::function<void(xecs::component::entity)> Walk = [&](xecs::component::entity E) noexcept
        {
            auto It = Scene.m_RuntimeToLocal.find(E.m_Value);
            if (It == Scene.m_RuntimeToLocal.end()) return;
            if (xlioncore::ComponentOf<xecs::editor::prefab_instance>(Ecs, E) && !Scene.m_InstanceMembers.contains(It->second))
            {
                Ecs.RefreshPrefabRecipe(Scene, It->second);
                return;                                 // its members are its recipe's
            }
            if (auto* pKids = Ecs.ChildrenOf(E))
            {
                auto Kids = pKids->m_List;
                for (auto K : Kids) Walk(K);
            }
        };
        Walk(Root);
    }

    // Makes a Prefab asset (under an EXPLICIT, caller-pre-minted guid: same "-Asset is pre-minted by the caller" convention
    // CreateAsset/InstantiatePrefab established, so make_prefab_cmd::Redo stays deterministic across an Undo/Redo cycle) from Root's subtree,
    // and turns that subtree into an instance of it: the subtree is deleted and an instance placed under Root's id, in Root's place (its parent,
    // at the same index, or its folder). The instance's members get ids derived from Root's id (prefabs_plan.md, phase 3). A reference a member
    // held to an entity outside the group cannot be kept by the prefab (it is null there): the instance keeps it as an override (Unity does the
    // same) - OutOutside lists them for the caller, which sets them (make_prefab_cmd::Redo).
    inline xresource::full_guid CreatePrefabFromGroupRootWithAssetGuid(xecs::game_mgr::instance& GameMgr, xecs::scene::instance& Scene, xecs::scene::guid SceneGuid, scene_state* pState, xresource_editor::library_mgr& AssetMgr, xresource_editor::library::guid LibraryGUID, xresource::full_guid ParentGUID, xecs::component::entity Root, xresource::full_guid ExplicitPrefabAssetGuid, std::vector<xecs::prefab::outside_reference>& OutOutside) noexcept
    {
        (void)AssetMgr;
        auto& Ecs = xlioncore::Ecs(GameMgr);

        xecs::component::entity OriginalParent;
        std::size_t             OriginalIndex = 0;
        if (auto* pRootParent = Ecs.ParentOf(Root))
        {
            OriginalParent = pRootParent->m_Value;
            if (auto* pSiblings = OriginalParent.isValid() ? Ecs.ChildrenOf(OriginalParent) : nullptr)
                if (auto It = std::ranges::find(pSiblings->m_List, Root.m_Value, &xecs::component::entity::m_Value); It != pSiblings->m_List.end())
                    OriginalIndex = static_cast<std::size_t>(It - pSiblings->m_List.begin());
        }

        const auto RootId           = Scene.m_RuntimeToLocal.at(Root.m_Value);
        const bool bRootWasSelected = pState && (pState->m_SelectedEntityId == RootId);
        const auto OriginalFolderId = xscene::FindFolderContaining(Scene, RootId);

        std::string Name = "Prefab";
        if (auto* pName = xscene::FindEntityName(Scene, RootId)) Name = *pName;

        // CreateOrRestoreAsset (xresource_editor_commands_assets.h), not a plain NewAsset call - a re-Redo
        // (after an Undo trashed this exact prefab asset guid) must restore-from-trash instead of
        // calling NewAsset again, same reasoning/bug CreateAsset's own Redo already had to solve.
        xresource_editor::commands::CreateOrRestoreAsset(LibraryGUID, ExplicitPrefabAssetGuid, ParentGUID, Name);
        const xecs::prefab::guid PrefabGuid = ExplicitPrefabAssetGuid;

        RefreshRecipesInSubtree(GameMgr, Scene, Root);
        std::unordered_map<std::uint64_t, std::uint64_t> MemberIds;          // each entity of the group: its id in the prefab
        Ecs.CreatePrefabFromEntity(Root, PrefabGuid, &OutOutside, &MemberIds);
        if (auto Err = Ecs.SavePrefab(PrefabGuid); Err)
        {
            xeditor::NotifyToast(std::format("Failed to save new Prefab: {}", Err.getMessage()));
            return {};
        }

        xscene::DeleteEntitySubtree(GameMgr, Scene, SceneGuid, Root);

        auto NewRoot = Ecs.InstantiatePrefabInScene(Scene, PrefabGuid, RootId, OriginalParent);
        if (NewRoot.isValid() == false)
        {
            xeditor::NotifyToast("MakePrefab: the new prefab could not be instantiated");
            return {};
        }
        if (OriginalParent.isValid())
        {
            // back to the place the group had among its siblings (the instance was appended)
            if (auto* pSiblings = Ecs.ChildrenOf(Ecs.ParentOf(NewRoot)->m_Value))
            {
                auto& L = pSiblings->m_List;
                std::erase_if(L, [&](auto& E) noexcept { return E.m_Value == NewRoot.m_Value; });
                L.insert(L.begin() + static_cast<std::ptrdiff_t>(std::min(OriginalIndex, L.size())), NewRoot);
            }
        }
        else xscene::ReparentEntityIntoFolder(Scene, RootId, OriginalFolderId);

        // What referenced an entity of the group now references the member that entity became (the root keeps its id; the others' ids are
        // derived from it and their id in the prefab).
        {
            std::unordered_map<std::uint64_t, xecs::component::entity> Moved;
            const auto PRoot = GameMgr.m_PrefabMgr.m_PrefabList.at(PrefabGuid.m_Instance.m_Value);
            const auto RootLocal = GameMgr.m_PrefabMgr.m_PrefabGroups.at(PrefabGuid.m_Instance.m_Value).m_RuntimeToLocal.at(PRoot.m_Value);
            for (auto& [SourceValue, Local] : MemberIds)
            {
                xecs::editor::member_address A;
                if (Local != RootLocal) A.push_back(Local);
                if (auto It = Scene.m_LocalToRuntime.find(xecs::scene::DeriveMemberId(RootId, A)); It != Scene.m_LocalToRuntime.end()) Moved[SourceValue] = It->second;
            }
            for (auto& pOther : GameMgr.m_SceneMgr.m_SceneInstances)
            {
                std::vector<std::pair<xecs::scene::permanent_id, xecs::component::entity>> Entities(pOther->m_LocalToRuntime.begin(), pOther->m_LocalToRuntime.end());
                for (auto& [Id, E] : Entities)
                {
                    if (pOther->m_InstanceMembers.contains(Id) || !Ecs.IsAlive(E)) continue;
                    bool bChanged = false;
                    Ecs.RemapLoadedEntityReferences(E, [&](std::int64_t V) noexcept
                    {
                        xecs::component::entity R; R.m_Value = static_cast<std::uint64_t>(V);
                        if (auto It = Moved.find(R.m_Value); It != Moved.end()) { bChanged = true; return It->second; }
                        return R;
                    });
                    if (bChanged) GameMgr.m_SceneMgr.MarkEntityDirty(pOther->m_Guid, Id);
                }
            }
        }

        // RootId lives on as the new instance: cancel the delete DeleteEntitySubtree recorded for it (SaveScene: Deleted wins over Dirty, which
        // would remove the root's file) and the New the placement recorded (its file exists).
        Scene.m_PendingChanges[RootId].m_Deleted -= 1;
        Scene.m_PendingChanges[RootId].m_New     -= 1;
        GameMgr.m_SceneMgr.MarkEntityDirty(SceneGuid, RootId);

        if (pState)
        {
            pState->m_MultiSelectedEntityIds.clear();
            pState->m_MultiSelectOrder.clear();
            if (bRootWasSelected)
            {
                pState->m_SelectedEntity        = NewRoot;
                pState->m_SelectedEntityScene   = SceneGuid;
                pState->m_bEntityInspectorDirty = true;
            }
            else if (pState->m_SelectedEntityScene == SceneGuid && pState->m_SelectedEntityId != xecs::scene::invalid_permanent_id_v)
            {
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

        return ExplicitPrefabAssetGuid;
    }

    // The references the group held outside itself (CreatePrefabFromGroupRootWithAssetGuid), kept by the instance: set on its members and
    // recorded as overrides of the instance (the prefab has them null).
    inline void KeepOutsideReferencesAsOverrides(xecs::game_mgr::instance& GameMgr, xecs::scene::instance& Scene, xecs::scene::permanent_id RootId, xecs::prefab::guid PrefabGuid, const std::vector<xecs::prefab::outside_reference>& Outside) noexcept
    {
        if (Outside.empty()) return;
        auto& Ecs    = xlioncore::Ecs(GameMgr);
        auto  RootIt = Scene.m_LocalToRuntime.find(RootId);
        if (RootIt == Scene.m_LocalToRuntime.end()) return;
        auto GroupIt = GameMgr.m_PrefabMgr.m_PrefabGroups.find(PrefabGuid.m_Instance.m_Value);
        auto PRootIt = GameMgr.m_PrefabMgr.m_PrefabList.find(PrefabGuid.m_Instance.m_Value);
        if (GroupIt == GameMgr.m_PrefabMgr.m_PrefabGroups.end() || PRootIt == GameMgr.m_PrefabMgr.m_PrefabList.end()) return;
        const auto RootLocal = GroupIt->second.m_RuntimeToLocal.at(PRootIt->second.m_Value);

        for (auto& O : Outside)
        {
            xecs::editor::member_address Member;
            if (O.m_Member != RootLocal) Member.push_back(O.m_Member);
            auto MemberIt = Scene.m_LocalToRuntime.find(xecs::scene::DeriveMemberId(RootId, Member));
            if (MemberIt == Scene.m_LocalToRuntime.end() || !Ecs.IsAlive(O.m_Target)) continue;

            const xecs::component::type::info* pInfo = nullptr;
            auto* pData = Ecs.ResolveComponent(MemberIt->second, xecs::component::type::guid{ O.m_Component }, pInfo);
            if (pData == nullptr || pInfo == nullptr || pInfo->m_pPropertyTable == nullptr) continue;

            xproperty::any Value;
            Value.set<xecs::component::entity>(O.m_Target);
            std::string                  SetError;
            xproperty::settings::context Context;
            xproperty::sprop::setProperty(SetError, pData, *pInfo->m_pPropertyTable, xproperty::sprop::container::prop{ O.m_Path, Value }, Context);

            auto* pPI = xlioncore::ComponentOf<xecs::editor::prefab_instance>(Ecs, RootIt->second);
            if (pPI == nullptr) continue;
            std::array<char, 256> Buffer{};
            const auto Len = FormatPropertyValue(Buffer, Value);
            auto& Entry = xscene::FindOrCreateOverrideEntry(*pPI, O.m_Component, Member);
            std::erase_if(Entry.m_PropertyOverrides, [&](auto& P) noexcept { return P.m_PropertyName == O.m_Path; });
            Entry.m_PropertyOverrides.push_back({ .m_PropertyName = O.m_Path, .m_PropertyValueAsString = std::string(Buffer.data(), Len > 0 ? static_cast<std::size_t>(Len) : 0) });
        }
        GameMgr.m_SceneMgr.MarkEntityDirty(Scene.m_Guid, RootId);
    }


    // MoveToTrash alone is in-memory until a library Save - and its return value used to be ignored
    // here, so a silent miss (bad guid/library) left the Prefab visible in Resources forever after
    // Ctrl+Z. Persist the trashed info.txt immediately so the hide sticks across any reload, and
    // surface failures through xeditor::NotifyError.
    inline void TrashCreatedPrefabAsset(xresource_editor::library::guid LibraryGuid, xresource::full_guid AssetGuid) noexcept
    {
        if (AssetGuid.empty())
        {
            xeditor::NotifyToast("MakePrefab Undo: refusing to trash an empty asset guid");
            return;
        }
        if (auto Err = xresource_editor::g_LibMgr.MoveToTrash(LibraryGuid, AssetGuid); !Err.empty())
        {
            xeditor::NotifyToast(std::format("MakePrefab Undo: MoveToTrash failed: {}", Err));
            return;
        }

        xproperty::settings::context Context;
        const bool bFound = xresource_editor::g_LibMgr.getNodeInfo(LibraryGuid, AssetGuid, [&](xresource_editor::library_db::info_node& Node)
        {
            if (Node.m_Path.empty()) return;
            if (auto SerErr = Node.m_Info.Serialize(false, Node.m_Path.c_str(), Context); SerErr)
            {
                xeditor::NotifyToast(std::format("MakePrefab Undo: failed to persist trashed info.txt: {}", SerErr.getMessage()));
                return;
            }
            Node.m_InfoChangeCount = 0;
        });
        if (!bFound)
            xeditor::NotifyToast("MakePrefab Undo: MoveToTrash succeeded but getNodeInfo missed the asset");
    }

    //================================================================================================
    // MakePrefab - the general path. BackupCurrenState snapshots the ORIGINAL group (same shadow-id
    // machinery delete_entity_cmd's own Undo already relies on) BEFORE Redo converts it. Undo deletes
    // whatever instance is currently registered under Id, restores the original group from that
    // snapshot, and trashes the created asset - same DOCUMENTED ASYMMETRY as CreateAsset's own Undo
    // (MoveToTrash/MoveFromTrashTo is the only reversal primitive this asset system has; the created
    // info.txt is not deleted from disk, only trashed).
    //================================================================================================
    struct make_prefab_cmd : scene_command
    {
        make_prefab_cmd(xundo::system& System, void* pDataBase) noexcept : scene_command(System, "MakePrefab", pDataBase) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override
        {
            return "Converts an entity (and its whole subtree) into a Prefab instance, creating the Prefab asset (undoable - restores the original group and trashes the created asset on Undo). Usage: MakePrefab -Scene hexguid -Id hexid -Library hexguid -Asset assetguid -Parent assetguid";
        }
        void RegisterArguments() noexcept override
        {
            m_hScene   = m_Parser.addOption("Scene",   "Scene guid, 16 hex digits",                                   true, 1);
            m_hId      = m_Parser.addOption("Id",      "Root entity permanent_id, 8 or 16 hex digits",                      true, 1);
            m_hLibrary = m_Parser.addOption("Library", "Asset library guid, 16 hex digits",                           true, 1);
            m_hAsset   = m_Parser.addOption("Asset",   "New Prefab asset's guid, 32 hex digits, pre-minted by the caller", true, 1);
            m_hParent  = m_Parser.addOption("Parent",  "Parent asset guid (where the new Prefab is filed), 32 hex digits", true, 1);
        }

        std::string Redo() noexcept override
        {
            auto SceneArg   = m_Parser.getOptionArgAs<std::string>(m_hScene, 0);
            auto IdArg      = m_Parser.getOptionArgAs<std::string>(m_hId, 0);
            auto LibraryArg = m_Parser.getOptionArgAs<std::string>(m_hLibrary, 0);
            auto AssetArg   = m_Parser.getOptionArgAs<std::string>(m_hAsset, 0);
            auto ParentArg  = m_Parser.getOptionArgAs<std::string>(m_hParent, 0);
            if (std::holds_alternative<xerr>(SceneArg) || std::holds_alternative<xerr>(IdArg) || std::holds_alternative<xerr>(LibraryArg) || std::holds_alternative<xerr>(AssetArg) || std::holds_alternative<xerr>(ParentArg))
                return "MakePrefab: bad arguments";

            const auto SceneGuid   = ParseSceneGuid(std::get<std::string>(SceneArg));
            const auto Id          = ParseEntityId(std::get<std::string>(IdArg));
            const auto LibraryGuid = xresource_editor::commands::ParseLibraryGuid(std::get<std::string>(LibraryArg));
            const auto AssetGuid   = xresource_editor::commands::ParseAssetGuid(std::get<std::string>(AssetArg));
            const auto ParentGuid  = xresource_editor::commands::ParseAssetGuid(std::get<std::string>(ParentArg));

            auto* pScene = World().m_SceneMgr.Find(SceneGuid);
            if (!pScene || !pScene->m_LocalToRuntime.contains(Id)) return "MakePrefab: target not found";
            if (pScene->m_InstanceMembers.contains(Id)) return "MakePrefab: the entity is a member of a prefab instance (its prefab owns it: make the prefab from the instance)";

            const auto Root = pScene->m_LocalToRuntime.at(Id);
            std::vector<xecs::prefab::outside_reference> Outside;
            const auto Result = CreatePrefabFromGroupRootWithAssetGuid(World(), *pScene, SceneGuid, &State(), xresource_editor::g_LibMgr, LibraryGuid, ParentGuid, Root, AssetGuid, Outside);
            if (Result.empty()) return "MakePrefab: failed";
            KeepOutsideReferencesAsOverrides(World(), *pScene, Id, xecs::prefab::guid{ AssetGuid }, Outside);
            if (SceneContext().m_OnPrefabMade) SceneContext().m_OnPrefabMade(Result);
            return {};
        }

        void BackupCurrenState(xundo::undo_file& File) noexcept override
        {
            auto SceneArg   = m_Parser.getOptionArgAs<std::string>(m_hScene, 0);
            auto IdArg      = m_Parser.getOptionArgAs<std::string>(m_hId, 0);
            auto LibraryArg = m_Parser.getOptionArgAs<std::string>(m_hLibrary, 0);
            auto AssetArg   = m_Parser.getOptionArgAs<std::string>(m_hAsset, 0);

            const std::uint64_t Scene   = std::holds_alternative<xerr>(SceneArg) ? 0 : std::strtoull(std::get<std::string>(SceneArg).c_str(), nullptr, 16);
            const xecs::scene::permanent_id Id      = std::holds_alternative<xerr>(IdArg) ? 0 : ParseEntityId(std::get<std::string>(IdArg));
            const std::uint64_t Library = std::holds_alternative<xerr>(LibraryArg) ? 0 : std::strtoull(std::get<std::string>(LibraryArg).c_str(), nullptr, 16);

            File.Write(Scene);
            File.Write(Id);
            File.Write(Library);
            xeditor::WriteString(File, std::holds_alternative<xerr>(AssetArg) ? std::string(32, '0') : std::get<std::string>(AssetArg));

            // BEFORE Redo runs anything - captures the ORIGINAL, pre-conversion group exactly, same
            // machinery delete_entity_cmd's own Undo relies on (xscene_commands_entity_lifecycle.h).
            SnapshotSubtreeForRestore(SceneContext(), File, xecs::scene::guid{ .m_Instance = { Scene } }, static_cast<xecs::scene::permanent_id>(Id));
        }

        void Undo(xundo::undo_file& File) noexcept override
        {
            std::uint64_t Scene = 0;   File.Read(Scene);
            xecs::scene::permanent_id Id = 0;      File.Read(Id);
            std::uint64_t Library = 0; File.Read(Library);
            const std::string Asset = xeditor::ReadString(File);

            const auto SceneGuid = xecs::scene::guid{ .m_Instance = { Scene } };
            const auto RootId    = static_cast<xecs::scene::permanent_id>(Id);

            // Remove whatever instance is currently registered under RootId (the fresh copy Redo
            // created), then restore the original group from the snapshot taken before Redo ever ran.
            DeleteSubtreeByPermanentId(SceneContext(), SceneGuid, RootId);
            RestoreSubtreeFromSnapshot(SceneContext(), File, SceneGuid, RootId);

            // Same documented asymmetry as CreateAsset's own Undo - trash, don't attempt to make the
            // on-disk info.txt vanish (MoveToTrash/MoveFromTrashTo is the only reversal primitive this
            // asset system has). Persist the trash tag to info.txt immediately - see
            // TrashCreatedPrefabAsset's own comment (silent MoveToTrash misses left "Entity" Prefabs
            // visible in Resources after Ctrl+Z).
            const auto LibraryGuid = xresource_editor::commands::ParseLibraryGuid(std::format("{:016X}", Library));
            TrashCreatedPrefabAsset(LibraryGuid, xresource_editor::commands::ParseAssetGuid(Asset));
        }

        xcmdline::parser::handle m_hScene, m_hId, m_hLibrary, m_hAsset, m_hParent;
    };

    //================================================================================================
    // MakePrefabVariant - the fast path for a single entity that's already a prefab instance: creates
    // the asset and re-points that SAME live entity's own prefab_instance component at it (no delete/
    // recreate at all - see CreatePrefabVariantFromInstance's own comment, xscene_prefab_authoring.h,
    // for why this is safe: the entity's current data already IS what a fresh instance of the new
    // variant looks like). Undo restores the old m_PrefabInstance/m_lComponents/m_ComponentDiffs and
    // trashes the created asset.
    //================================================================================================
    struct make_prefab_variant_cmd : scene_command
    {
        make_prefab_variant_cmd(xundo::system& System, void* pDataBase) noexcept : scene_command(System, "MakePrefabVariant", pDataBase) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override
        {
            return "Captures a prefab instance's current overrides into a new Prefab Variant asset, in place (undoable). Usage: MakePrefabVariant -Scene hexguid -Id hexid -Library hexguid -Asset assetguid -Parent assetguid";
        }
        void RegisterArguments() noexcept override
        {
            m_hScene   = m_Parser.addOption("Scene",   "Scene guid, 16 hex digits",                                   true, 1);
            m_hId      = m_Parser.addOption("Id",      "Entity permanent_id, 8 or 16 hex digits",                           true, 1);
            m_hLibrary = m_Parser.addOption("Library", "Asset library guid, 16 hex digits",                           true, 1);
            m_hAsset   = m_Parser.addOption("Asset",   "New Prefab asset's guid, 32 hex digits, pre-minted by the caller", true, 1);
            m_hParent  = m_Parser.addOption("Parent",  "Parent asset guid (where the new Prefab is filed), 32 hex digits", true, 1);
        }

        std::string Redo() noexcept override
        {
            auto SceneArg   = m_Parser.getOptionArgAs<std::string>(m_hScene, 0);
            auto IdArg      = m_Parser.getOptionArgAs<std::string>(m_hId, 0);
            auto LibraryArg = m_Parser.getOptionArgAs<std::string>(m_hLibrary, 0);
            auto AssetArg   = m_Parser.getOptionArgAs<std::string>(m_hAsset, 0);
            auto ParentArg  = m_Parser.getOptionArgAs<std::string>(m_hParent, 0);
            if (std::holds_alternative<xerr>(SceneArg) || std::holds_alternative<xerr>(IdArg) || std::holds_alternative<xerr>(LibraryArg) || std::holds_alternative<xerr>(AssetArg) || std::holds_alternative<xerr>(ParentArg))
                return "MakePrefabVariant: bad arguments";

            const auto SceneGuid   = ParseSceneGuid(std::get<std::string>(SceneArg));
            const auto Id          = ParseEntityId(std::get<std::string>(IdArg));
            const auto LibraryGuid = xresource_editor::commands::ParseLibraryGuid(std::get<std::string>(LibraryArg));
            const auto AssetGuid   = xresource_editor::commands::ParseAssetGuid(std::get<std::string>(AssetArg));
            const auto ParentGuid  = xresource_editor::commands::ParseAssetGuid(std::get<std::string>(ParentArg));

            auto* pScene = World().m_SceneMgr.Find(SceneGuid);
            if (!pScene || !pScene->m_LocalToRuntime.contains(Id)) return "MakePrefabVariant: target not found";
            auto Entity = pScene->m_LocalToRuntime.at(Id);

            auto* pPrefabInstance = xlioncore::ComponentOf<xecs::editor::prefab_instance>(xlioncore::Ecs(World()), Entity);
            if (!pPrefabInstance) return "MakePrefabVariant: entity is not a prefab instance";
            if (pScene->m_InstanceMembers.contains(Id)) return "MakePrefabVariant: the entity is a member of another prefab instance";

            // what the instance's members do differently is in its recipe before the variant captures it
            xlioncore::Ecs(World()).RefreshPrefabRecipe(*pScene, Id);

            std::string Name = "Prefab";
            if (auto* pName = xscene::FindEntityName(*pScene, Id)) Name = *pName;

            xresource_editor::commands::CreateOrRestoreAsset(LibraryGuid, AssetGuid, ParentGuid, Name);
            const xecs::prefab::guid PrefabGuid = AssetGuid;

            xlioncore::Ecs(World()).CreatePrefabFromEntity(Entity, PrefabGuid, nullptr, nullptr);
            if (auto Err = xlioncore::Ecs(World()).SavePrefab(PrefabGuid); Err)
                return std::format("MakePrefabVariant: {}", Err.getMessage());
            if (SceneContext().m_OnPrefabMade) SceneContext().m_OnPrefabMade(AssetGuid);

            // the pools may have moved while the prefab was made: resolved again. The instance is now one of the variant, which holds what it did
            // differently: its own recipe is empty (its members keep their addresses: a variant's root adds no element to them)
            auto& PI = *xlioncore::ComponentOf<xecs::editor::prefab_instance>(xlioncore::Ecs(World()), Entity);
            PI.m_PrefabInstance = PrefabGuid;
            PI.m_lComponents.clear();
            PI.m_ComponentDiffs.clear();
            PI.m_HierarchyDiffs.clear();
            PI.m_Format = xecs::editor::prefab_instance::recipe_format_v;
            World().m_SceneMgr.MarkEntityDirty(SceneGuid, Id);
            return {};
        }

        void BackupCurrenState(xundo::undo_file& File) noexcept override
        {
            auto SceneArg   = m_Parser.getOptionArgAs<std::string>(m_hScene, 0);
            auto IdArg      = m_Parser.getOptionArgAs<std::string>(m_hId, 0);
            auto LibraryArg = m_Parser.getOptionArgAs<std::string>(m_hLibrary, 0);
            auto AssetArg   = m_Parser.getOptionArgAs<std::string>(m_hAsset, 0);

            const std::uint64_t Scene   = std::holds_alternative<xerr>(SceneArg) ? 0 : std::strtoull(std::get<std::string>(SceneArg).c_str(), nullptr, 16);
            const xecs::scene::permanent_id Id      = std::holds_alternative<xerr>(IdArg) ? 0 : ParseEntityId(std::get<std::string>(IdArg));
            const std::uint64_t Library = std::holds_alternative<xerr>(LibraryArg) ? 0 : std::strtoull(std::get<std::string>(LibraryArg).c_str(), nullptr, 16);

            File.Write(Scene);
            File.Write(Id);
            File.Write(Library);
            xeditor::WriteString(File, std::holds_alternative<xerr>(AssetArg) ? std::string(32, '0') : std::get<std::string>(AssetArg));

            const auto SceneGuid = xecs::scene::guid{ .m_Instance = { Scene } };
            xecs::editor::prefab_instance* pOldPI = nullptr;
            if (auto* pScene = World().m_SceneMgr.Find(SceneGuid); pScene && pScene->m_LocalToRuntime.contains(static_cast<xecs::scene::permanent_id>(Id)))
            {
                auto Entity = pScene->m_LocalToRuntime.at(static_cast<xecs::scene::permanent_id>(Id));
                pOldPI = xlioncore::ComponentOf<xecs::editor::prefab_instance>(xlioncore::Ecs(World()), Entity);
            }

            File.Write(pOldPI != nullptr);
            if (!pOldPI) return;

            File.Write(pOldPI->m_PrefabInstance.m_Instance.m_Value);
            File.Write(pOldPI->m_PrefabInstance.m_Type.m_Value);

            File.Write(static_cast<std::uint32_t>(pOldPI->m_lComponents.size()));
            for (auto& C : pOldPI->m_lComponents)
            {
                File.Write(C.m_ComponentTypeGuid);
                File.Write(static_cast<std::uint32_t>(C.m_Member.size()));
                for (auto P : C.m_Member) File.Write(P);
                File.Write(static_cast<std::uint32_t>(C.m_PropertyOverrides.size()));
                for (auto& O : C.m_PropertyOverrides)
                {
                    xeditor::WriteString(File, O.m_PropertyName);
                    xeditor::WriteString(File, O.m_PropertyValueAsString);
                }
            }

            File.Write(static_cast<std::uint32_t>(pOldPI->m_ComponentDiffs.size()));
            for (auto& D : pOldPI->m_ComponentDiffs)
            {
                File.Write(D.m_ComponentTypeGuid);
                File.Write(D.m_bAdded);
                File.Write(static_cast<std::uint32_t>(D.m_Member.size()));
                for (auto P : D.m_Member) File.Write(P);
            }

            File.Write(static_cast<std::uint32_t>(pOldPI->m_HierarchyDiffs.size()));
            for (auto& H : pOldPI->m_HierarchyDiffs)
            {
                File.Write(static_cast<std::uint32_t>(H.m_Member.size()));
                for (auto P : H.m_Member) File.Write(P);
                File.Write(H.m_bAdded);
            }
        }

        void Undo(xundo::undo_file& File) noexcept override
        {
            std::uint64_t Scene = 0;   File.Read(Scene);
            xecs::scene::permanent_id Id = 0;      File.Read(Id);
            std::uint64_t Library = 0; File.Read(Library);
            const std::string Asset = xeditor::ReadString(File);

            bool bHadPI = false; File.Read(bHadPI);

            std::uint64_t OldInstance = 0, OldType = 0;
            std::vector<xecs::editor::prefab_component_override> OldComponents;
            std::vector<xecs::editor::prefab_component_diff>     OldDiffs;
            std::vector<xecs::editor::prefab_hierarchy_diff>     OldHierarchy;

            if (bHadPI)
            {
                File.Read(OldInstance);
                File.Read(OldType);

                std::uint32_t CompCount = 0; File.Read(CompCount);
                OldComponents.resize(CompCount);
                for (auto& C : OldComponents)
                {
                    File.Read(C.m_ComponentTypeGuid);
                    std::uint32_t PathCount = 0; File.Read(PathCount);
                    C.m_Member.resize(PathCount);
                    for (auto& P : C.m_Member) File.Read(P);
                    std::uint32_t OverrideCount = 0; File.Read(OverrideCount);
                    C.m_PropertyOverrides.resize(OverrideCount);
                    for (auto& O : C.m_PropertyOverrides)
                    {
                        O.m_PropertyName          = xeditor::ReadString(File);
                        O.m_PropertyValueAsString = xeditor::ReadString(File);
                    }
                }

                std::uint32_t DiffCount = 0; File.Read(DiffCount);
                OldDiffs.resize(DiffCount);
                for (auto& D : OldDiffs)
                {
                    File.Read(D.m_ComponentTypeGuid);
                    File.Read(D.m_bAdded);
                    std::uint32_t PathCount = 0; File.Read(PathCount);
                    D.m_Member.resize(PathCount);
                    for (auto& P : D.m_Member) File.Read(P);
                }

                std::uint32_t HierCount = 0; File.Read(HierCount);
                OldHierarchy.resize(HierCount);
                for (auto& H : OldHierarchy)
                {
                    std::uint32_t PathCount = 0; File.Read(PathCount);
                    H.m_Member.resize(PathCount);
                    for (auto& P : H.m_Member) File.Read(P);
                    File.Read(H.m_bAdded);
                }
            }

            const auto SceneGuid = xecs::scene::guid{ .m_Instance = { Scene } };
            const auto LibraryGuid = xresource_editor::commands::ParseLibraryGuid(std::format("{:016X}", Library));

            // Trash the created asset first (same asymmetric-Undo shape as CreateAsset/MakePrefab).
            TrashCreatedPrefabAsset(LibraryGuid, xresource_editor::commands::ParseAssetGuid(Asset));

            if (!bHadPI) return;
            auto* pScene = World().m_SceneMgr.Find(SceneGuid);
            if (!pScene || !pScene->m_LocalToRuntime.contains(static_cast<xecs::scene::permanent_id>(Id))) return;
            auto Entity = pScene->m_LocalToRuntime.at(static_cast<xecs::scene::permanent_id>(Id));
            auto* pRestored = xlioncore::ComponentOf<xecs::editor::prefab_instance>(xlioncore::Ecs(World()), Entity);
            if (!pRestored) return;

            auto& PI = *pRestored;
            PI.m_PrefabInstance = xecs::prefab::guid{ .m_Instance = { OldInstance }, .m_Type = { OldType } };
            PI.m_lComponents    = std::move(OldComponents);
            PI.m_ComponentDiffs = std::move(OldDiffs);
            PI.m_HierarchyDiffs = std::move(OldHierarchy);
            World().m_SceneMgr.MarkEntityDirty(SceneGuid, static_cast<xecs::scene::permanent_id>(Id));
        }

        xcmdline::parser::handle m_hScene, m_hId, m_hLibrary, m_hAsset, m_hParent;
    };

    //================================================================================================
    // UpgradeProject - converts the project's files that are still in an old format to the current one,
    // all at once (documentation/Editors/prefabs_plan.md, decision D4; each file is also converted when it
    // is next loaded and saved):
    //   * prefabs stored in one Entity.txt (before phase 1): read with this Level's world (it has the
    //     components of its Game) and saved in the scene format;
    //   * scenes whose prefab instances were saved before recipes (phase 3: every member an entity file of
    //     the scene, overrides addressed by child-index paths): loaded into this Level's world (a scene that is
    //     open is already converted: loading converts), saved (the old member files go, each instance is
    //     written as its recipe), and released again when it was not open.
    // One that cannot be read (a component no module registers, a record the reader does not know) is left as
    // it is and reported. Not undoable: it writes files, as Save does.
    //================================================================================================
    namespace upgrade
    {
        // The entity ids a scene's descriptor lists (its ActiveEntities rows: ;u32 or ;u64, the id after '#').
        inline std::vector<xecs::scene::permanent_id> ActiveEntitiesOf(const std::filesystem::path& Descriptor) noexcept
        {
            std::vector<xecs::scene::permanent_id> Ids;
            std::ifstream F(Descriptor, std::ios::binary);
            std::string   Line;
            while (std::getline(F, Line))
            {
                if (Line.find("\"Scene/ActiveEntities[G:") == std::string::npos) continue;
                if (auto At = Line.find('#'); At != std::string::npos) Ids.push_back(xecs::scene::ParsePermanentId(Line.c_str() + At + 1));
            }
            return Ids;
        }

        // A prefab instance written before recipes: its file has a prefab_instance and no Format row.
        inline bool IsOldInstanceFile(const std::filesystem::path& File) noexcept
        {
            std::ifstream F(File, std::ios::binary);
            const std::string Text((std::istreambuf_iterator<char>(F)), std::istreambuf_iterator<char>());
            return Text.find("\"EditorPrafabInstance/Prefab\"") != std::string::npos && Text.find("\"EditorPrafabInstance/Format\"") == std::string::npos;
        }

        // The scenes of the project with an instance written before recipes among the entities they list.
        inline std::vector<std::uint64_t> ScenesWithOldInstances(const std::wstring& ProjectPath) noexcept
        {
            std::vector<std::uint64_t> Out;
            std::error_code Ec;
            for (auto It = std::filesystem::recursive_directory_iterator(std::filesystem::path(ProjectPath) / L"Descriptors" / L"Scene", std::filesystem::directory_options::skip_permission_denied, Ec)
                ; !Ec && It != std::filesystem::recursive_directory_iterator(); It.increment(Ec))
            {
                if (!It->is_regular_file(Ec) || It->path().filename() != L"Descriptor.txt") continue;
                const auto Folder = It->path().parent_path();
                for (auto Id : ActiveEntitiesOf(It->path()))
                {
                    const auto File = Folder / L"entity_db" / std::format(L"{:02X}", Id & 0xFF) / std::format(L"{:02X}", (Id >> 8) & 0xFF) / (xecs::scene::FormatPermanentIdW(Id) + L".entity");
                    if (IsOldInstanceFile(File)) { Out.push_back(std::wcstoull(Folder.stem().c_str(), nullptr, 16)); break; }       // the folder is <guid>.desc
                }
            }
            std::sort(Out.begin(), Out.end());
            return Out;
        }
    }

    struct upgrade_project_query_cmd : scene_query_command
    {
        upgrade_project_query_cmd(xundo::system& System, void* pDataBase) noexcept : scene_query_command(System, "UpgradeProject", pDataBase) {}
        const char* getCommandHelp() const noexcept override
        {
            return "Converts the project's files that are in an old format to the current one (prefabs stored in one Entity.txt; scenes whose prefab instances were saved before recipes), read with this Level's components; one that cannot be read is left as it is and listed. Not undoable. Usage: UpgradeProject";
        }
        void RegisterArguments() noexcept override {}

        std::string Query() noexcept override
        {
            std::vector<std::uint64_t> OldPrefabs;
            std::error_code Ec;
            for (auto It = std::filesystem::recursive_directory_iterator(std::filesystem::path(World().m_PrefabMgr.m_ProjectPath) / L"Descriptors" / L"Prefab", std::filesystem::directory_options::skip_permission_denied, Ec)
                ; !Ec && It != std::filesystem::recursive_directory_iterator(); It.increment(Ec))
            {
                if (It->is_regular_file(Ec) && It->path().filename() == L"Entity.txt")
                    OldPrefabs.push_back(std::wcstoull(It->path().parent_path().stem().c_str(), nullptr, 16));      // the folder is <guid>.desc
            }
            std::sort(OldPrefabs.begin(), OldPrefabs.end());

            std::string Converted, Left;
            int         nConverted = 0, nLeft = 0;
            for (auto Value : OldPrefabs)
            {
                const xecs::prefab::guid Guid{ .m_Instance = { Value }, .m_Type = xecs::prefab::type_guid_v };
                auto Err = xlioncore::Ecs(World()).EnsureLoadedPrefab(Guid);
                if (!Err) Err = xlioncore::Ecs(World()).SavePrefab(Guid);
                if (Err) { ++nLeft;      Left      += std::format("  {:016X}  {}\n", Value, Err.getMessage()); }
                else     { ++nConverted; Converted += std::format("  {:016X}\n", Value); }
            }

            // the scenes whose instances are not recipes yet
            std::string ScenesConverted, ScenesLeft;
            int         nScenes = 0, nScenesLeft = 0;
            for (auto Value : upgrade::ScenesWithOldInstances(World().m_SceneMgr.m_ProjectPath))
            {
                const xecs::scene::guid SceneGuid{ .m_Instance = { Value } };
                auto*      pOpen  = World().m_SceneMgr.Find(SceneGuid);
                const bool bOpen  = pOpen && pOpen->m_State == xecs::scene::state::Active;
                xerr       Err;
                if (!bOpen) Err = xlioncore::Ecs(World()).RequestLoadScene(SceneGuid);
                if (!Err)   Err = xlioncore::Ecs(World()).SaveScene(SceneGuid);
                if (!bOpen) (void)xlioncore::Ecs(World()).ReleaseLoadScene(SceneGuid);

                const auto Still     = upgrade::ScenesWithOldInstances(World().m_SceneMgr.m_ProjectPath);
                const bool bStillOld = std::ranges::find(Still, Value) != Still.end();
                if (Err || bStillOld) { ++nScenesLeft; ScenesLeft += std::format("  {:016X}  {}\n", Value, Err ? std::string(Err.getMessage()) : std::string("an instance could not be converted (its prefab or one of its entities did not load)")); }
                else                  { ++nScenes;     ScenesConverted += std::format("  {:016X}\n", Value); }
            }

            return std::format("UpgradeProject: {} prefab(s) converted, {} left as they are (they cannot be read)\n{}{}{}{}"
                               "UpgradeProject: {} scene(s) converted, {} left as they are\n{}{}{}{}"
                , nConverted, nLeft, nConverted ? "Converted:\n" : "", Converted, nLeft ? "Left as they are:\n" : "", Left
                , nScenes, nScenesLeft, nScenes ? "Scenes converted:\n" : "", ScenesConverted, nScenesLeft ? "Scenes left as they are:\n" : "", ScenesLeft);
        }
    };
}

// Drop-path entry used by entity_to_prefab_drop via g_MakePrefabDropHandler. Lives here (not in
// PrefabAuthoring.h) so it can call xeditor::Run / Format* without an include cycle. Preserves the
// existing Variant-vs-MakePrefab decision and still runs DetermineGroupRoot for multi-select BEFORE
// MakePrefab (that synthesis step remains a known separate undo gap - see this file's top comment).
namespace xscene
{

    inline xresource::full_guid MakePrefabDropViaCommands(xresource_editor::library_mgr& AssetMgr, xresource_editor::library::guid LibraryGUID, xresource::full_guid ParentGUID, const entity_drag_payload_t& Payload) noexcept
    {
        (void)AssetMgr;
        auto* pEd = FindSceneContext();
        if (pEd == nullptr) return {};
        // Prefab creation mutates the Level document, so it goes through the editor's document undo.
        xundo::system* pDocUndo = &pEd->m_Undo;
        auto*          pWorld   = &pEd->World();
        auto*          pState   = &pEd->m_State;

        auto* pScene = pWorld->m_SceneMgr.Find(Payload.m_SceneGuid);
        if (pScene == nullptr) return {};

        auto SourceIt = pScene->m_LocalToRuntime.find(Payload.m_Id);
        if (SourceIt == pScene->m_LocalToRuntime.end()) return {};

        xresource::instance_guid NewInstance{};
        NewInstance.GenerateGUID();
        const xresource::full_guid NewAsset{ .m_Instance = NewInstance, .m_Type = xecs::prefab::type_guid_v };

        const bool bIsMultiSelect = pState && pState->m_MultiSelectScene == Payload.m_SceneGuid && pState->m_MultiSelectedEntityIds.size() > 1 && pState->m_MultiSelectedEntityIds.contains(Payload.m_Id);
        if (!bIsMultiSelect)
        {
            if (xlioncore::ComponentOf<xecs::editor::prefab_instance>(xlioncore::Ecs(*pWorld), SourceIt->second))
            {
                const auto Cmd = std::format("MakePrefabVariant -Scene {} -Id {} -Library {} -Asset {} -Parent {}"
                    , xscene::commands::FormatSceneGuid(Payload.m_SceneGuid)
                    , xscene::commands::FormatEntityId(Payload.m_Id)
                    , xresource_editor::commands::FormatLibraryGuid(LibraryGUID)
                    , xresource_editor::commands::FormatAssetGuid(NewAsset)
                    , xresource_editor::commands::FormatAssetGuid(ParentGUID));
                if (!xeditor::RunGroup(*pDocUndo, "MakePrefabVariant", { Cmd })) return {};
                return NewAsset;
            }
        }

        auto Root = pState ? DetermineGroupRoot(*pWorld, *pScene, Payload.m_SceneGuid, *pState, Payload.m_Id)
                             : SourceIt->second;
        if (Root.isValid() == false) return {};

        auto RootIdIt = pScene->m_RuntimeToLocal.find(Root.m_Value);
        if (RootIdIt == pScene->m_RuntimeToLocal.end()) return {};

        const auto Cmd = std::format("MakePrefab -Scene {} -Id {} -Library {} -Asset {} -Parent {}"
            , xscene::commands::FormatSceneGuid(Payload.m_SceneGuid)
            , xscene::commands::FormatEntityId(RootIdIt->second)
            , xresource_editor::commands::FormatLibraryGuid(LibraryGUID)
            , xresource_editor::commands::FormatAssetGuid(NewAsset)
            , xresource_editor::commands::FormatAssetGuid(ParentGUID));
        if (!xeditor::RunGroup(*pDocUndo, "MakePrefab", { Cmd })) return {};
        return NewAsset;
    }
}


#endif // XSCENE_COMMANDS_MAKE_PREFAB_H
