#ifndef XSCENE_COMMANDS_APPLY_OVERRIDES_H
#define XSCENE_COMMANDS_APPLY_OVERRIDES_H
#pragma once

// ApplyOverrides - Unity-style "Apply Overrides to Prefab": pushes every recorded override on one
// prefab_instance up into the source Prefab asset, saves the prefab, and clears the instance's
// m_lComponents bookkeeping. The previous UI path (xscene_panel_entity_properties.h) called
// ApplyInstanceOverridesToPrefab directly with no undo. BackupCurrenState snapshots (1) the full
// override bookkeeping that will be cleared and (2) each affected Prefab property's BEFORE value so
// Undo can put the Prefab asset AND the instance's override list back.

#include "plugins/xscene.plugin/source/Editor/xscene_commands_property_edit.h"
#include "plugins/xscene.plugin/source/Editor/xscene_commands_entity_lifecycle.h"
#include "plugins/xscene.plugin/source/Editor/xscene_prefab_authoring.h"

namespace xscene::commands
{
    inline void WriteMemberPath(xundo::undo_file& File, const std::vector<std::uint32_t>& Path) noexcept
    {
        File.Write(static_cast<std::uint32_t>(Path.size()));
        for (auto P : Path) File.Write(P);
    }

    inline std::vector<std::uint32_t> ReadMemberPath(xundo::undo_file& File) noexcept
    {
        std::uint32_t Count = 0; File.Read(Count);
        std::vector<std::uint32_t> Path(Count);
        for (auto& P : Path) File.Read(P);
        return Path;
    }

    // Full PI.m_lComponents dump - same length-prefixed shape SnapshotComponentOverrideEntry uses,
    // but for EVERY component-override entry on the instance (Apply clears the whole list).
    inline void SnapshotAllOverrideBookkeeping(xundo::undo_file& File, const xecs::editor::prefab_instance& PI) noexcept
    {
        File.Write(static_cast<std::uint32_t>(PI.m_lComponents.size()));
        for (auto& C : PI.m_lComponents)
        {
            File.Write(C.m_ComponentTypeGuid);
            WriteMemberPath(File, C.m_MemberPath);
            File.Write(static_cast<std::uint32_t>(C.m_PropertyOverrides.size()));
            for (auto& O : C.m_PropertyOverrides)
            {
                xeditor::WriteString(File, O.m_PropertyName);
                xeditor::WriteString(File, O.m_PropertyValueAsString);
            }
        }
        File.Write(static_cast<std::uint32_t>(PI.m_HierarchyDiffs.size()));
        for (auto& H : PI.m_HierarchyDiffs)
        {
            WriteMemberPath(File, H.m_MemberPath);
            File.Write(H.m_bAdded);
        }
    }

    inline void RestoreAllOverrideBookkeeping(xundo::undo_file& File, xecs::editor::prefab_instance& PI) noexcept
    {
        PI.m_lComponents.clear();
        std::uint32_t CompCount = 0; File.Read(CompCount);
        PI.m_lComponents.reserve(CompCount);
        for (std::uint32_t i = 0; i < CompCount; ++i)
        {
            xecs::editor::prefab_component_override Comp{};
            File.Read(Comp.m_ComponentTypeGuid);
            Comp.m_MemberPath = ReadMemberPath(File);
            std::uint32_t OverrideCount = 0; File.Read(OverrideCount);
            Comp.m_PropertyOverrides.reserve(OverrideCount);
            for (std::uint32_t j = 0; j < OverrideCount; ++j)
            {
                xecs::editor::prefab_property_override Prop{};
                Prop.m_PropertyName          = xeditor::ReadString(File);
                Prop.m_PropertyValueAsString = xeditor::ReadString(File);
                Comp.m_PropertyOverrides.push_back(std::move(Prop));
            }
            PI.m_lComponents.push_back(std::move(Comp));
        }
        PI.m_HierarchyDiffs.clear();
        std::uint32_t HierCount = 0; File.Read(HierCount);
        PI.m_HierarchyDiffs.reserve(HierCount);
        for (std::uint32_t i = 0; i < HierCount; ++i)
        {
            xecs::editor::prefab_hierarchy_diff H{};
            H.m_MemberPath = ReadMemberPath(File);
            File.Read(H.m_bAdded);
            PI.m_HierarchyDiffs.push_back(std::move(H));
        }
    }

    // For each property Apply will overwrite on the Prefab, capture the Prefab's current value so
    // Undo can put it back. Mirrors ApplyInstanceOverridesToPrefab's own walk.
    inline void SnapshotPrefabBeforeValues(xundo::undo_file& File, xecs::game_mgr::instance& GameMgr, xecs::component::entity PIRootEntity, const xecs::editor::prefab_instance& PI) noexcept
    {
        struct row
        {
            std::uint64_t              m_ComponentTypeGuid = 0;
            std::vector<std::uint32_t> m_MemberPath;
            std::string                m_PropertyName;
            std::uint32_t              m_TypeGuid = 0;
            std::string                m_Before;
        };
        std::vector<row> Rows;

        if (auto Err = xlioncore::Ecs(GameMgr).EnsureLoadedPrefab(PI.m_PrefabInstance); !Err)
        {
            auto RootIt = GameMgr.m_PrefabMgr.m_PrefabList.find(PI.m_PrefabInstance.m_Instance.m_Value);
            if (RootIt != GameMgr.m_PrefabMgr.m_PrefabList.end())
            {
                for (auto& CompOverride : PI.m_lComponents)
                {
                    auto* pOwnerInfo = xlioncore::Ecs(GameMgr).FindComponentType(xecs::component::type::guid{ CompOverride.m_ComponentTypeGuid });
                    if (pOwnerInfo == nullptr || pOwnerInfo->m_pPropertyTable == nullptr) continue;

                    const auto PrefabEntity = xlioncore::Ecs(GameMgr).ResolveMemberPath(RootIt->second, CompOverride.m_MemberPath);
                    if (PrefabEntity.isValid() == false) continue;

                    const xecs::component::type::info* pPrefInfo = nullptr;
                    auto* pPrefData = xlioncore::Ecs(GameMgr).ResolveComponent(PrefabEntity, pOwnerInfo->m_Guid, pPrefInfo);
                    if (pPrefData == nullptr) continue;

                    for (auto& PropOverride : CompOverride.m_PropertyOverrides)
                    {
                        xproperty::settings::context Context{};
                        xproperty::any               BeforeValue;
                        bool                         bFound = false;
                        xproperty::sprop::collector(pPrefData, *pOwnerInfo->m_pPropertyTable, Context, [&](const char* pPropertyName, xproperty::any&& Value, const xproperty::type::members&, bool, const void*) noexcept
                        {
                            if (PropOverride.m_PropertyName == pPropertyName) { BeforeValue = std::move(Value); bFound = true; }
                        });
                        if (!bFound) continue;

                        std::array<char, 256> Buffer{};
                        const auto Len = FormatPropertyValue(Buffer, BeforeValue);
                        row R;
                        R.m_ComponentTypeGuid = CompOverride.m_ComponentTypeGuid;
                        R.m_MemberPath        = CompOverride.m_MemberPath;
                        R.m_PropertyName      = PropOverride.m_PropertyName;
                        R.m_TypeGuid          = BeforeValue.m_pType ? BeforeValue.m_pType->m_GUID : 0;
                        R.m_Before.assign(Buffer.data(), Len > 0 ? static_cast<std::size_t>(Len) : 0);
                        Rows.push_back(std::move(R));
                    }
                }
            }
        }

        File.Write(static_cast<std::uint32_t>(Rows.size()));
        for (auto& R : Rows)
        {
            File.Write(R.m_ComponentTypeGuid);
            WriteMemberPath(File, R.m_MemberPath);
            xeditor::WriteString(File, R.m_PropertyName);
            File.Write(R.m_TypeGuid);
            xeditor::WriteString(File, R.m_Before);
        }
    }

    inline void RestorePrefabBeforeValues(xundo::undo_file& File, xecs::game_mgr::instance& GameMgr, const xecs::editor::prefab_instance& PI) noexcept
    {
        std::uint32_t Count = 0; File.Read(Count);

        if (auto Err = xlioncore::Ecs(GameMgr).EnsureLoadedPrefab(PI.m_PrefabInstance); Err)
        {
            // Still need to drain the file stream even if restore can't proceed.
            for (std::uint32_t i = 0; i < Count; ++i)
            {
                std::uint64_t Comp = 0; File.Read(Comp);
                (void)ReadMemberPath(File);
                (void)xeditor::ReadString(File);
                std::uint32_t TypeGuid = 0; File.Read(TypeGuid);
                (void)xeditor::ReadString(File);
            }
            return;
        }

        auto RootIt = GameMgr.m_PrefabMgr.m_PrefabList.find(PI.m_PrefabInstance.m_Instance.m_Value);
        if (RootIt == GameMgr.m_PrefabMgr.m_PrefabList.end())
        {
            for (std::uint32_t i = 0; i < Count; ++i)
            {
                std::uint64_t Comp = 0; File.Read(Comp);
                (void)ReadMemberPath(File);
                (void)xeditor::ReadString(File);
                std::uint32_t TypeGuid = 0; File.Read(TypeGuid);
                (void)xeditor::ReadString(File);
            }
            return;
        }

        for (std::uint32_t i = 0; i < Count; ++i)
        {
            std::uint64_t CompGuid = 0; File.Read(CompGuid);
            auto MemberPath = ReadMemberPath(File);
            auto PropertyName = xeditor::ReadString(File);
            std::uint32_t TypeGuid = 0; File.Read(TypeGuid);
            auto Before = xeditor::ReadString(File);

            auto* pOwnerInfo = xlioncore::Ecs(GameMgr).FindComponentType(xecs::component::type::guid{ CompGuid });
            if (pOwnerInfo == nullptr || pOwnerInfo->m_pPropertyTable == nullptr) continue;

            const auto PrefabEntity = xlioncore::Ecs(GameMgr).ResolveMemberPath(RootIt->second, MemberPath);
            if (PrefabEntity.isValid() == false) continue;

            const xecs::component::type::info* pPrefInfo = nullptr;
            auto* pPrefData = xlioncore::Ecs(GameMgr).ResolveComponent(PrefabEntity, pOwnerInfo->m_Guid, pPrefInfo);
            if (pPrefData == nullptr) continue;

            xproperty::any Value;
            std::string    ValueStrMutable = Before;
            xproperty::settings::StringToAny(Value, TypeGuid, std::span<char>(ValueStrMutable.data(), ValueStrMutable.size()));
            xproperty::settings::context Context{};
            std::string SetError;
            xproperty::sprop::setProperty(SetError, pPrefData, *pOwnerInfo->m_pPropertyTable, xproperty::sprop::container::prop{ PropertyName, Value }, Context);
        }
    }

    //================================================================================================
    struct apply_overrides_cmd : scene_command
    {
        apply_overrides_cmd(xundo::system& System, void* pDataBase) noexcept : scene_command(System, "ApplyOverrides", pDataBase) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override
        {
            return "Applies every override on a prefab instance up into the Prefab asset (undoable - restores Prefab property values and the instance override list). Usage: ApplyOverrides -Scene hexguid -Id hexid";
        }
        void RegisterArguments() noexcept override
        {
            m_hScene = m_Parser.addOption("Scene", "Scene guid, 16 hex digits", true, 1);
            m_hId    = m_Parser.addOption("Id",    "Prefab-instance root permanent_id, 8 hex digits", true, 1);
        }

        std::string Redo() noexcept override
        {
            auto SceneArg = m_Parser.getOptionArgAs<std::string>(m_hScene, 0);
            auto IdArg    = m_Parser.getOptionArgAs<std::string>(m_hId, 0);
            if (std::holds_alternative<xerr>(SceneArg) || std::holds_alternative<xerr>(IdArg))
                return "ApplyOverrides: bad arguments";

            const auto SceneGuid = ParseSceneGuid(std::get<std::string>(SceneArg));
            const auto Id        = ParseEntityId(std::get<std::string>(IdArg));

            auto* pScene = World().m_SceneMgr.Find(SceneGuid);
            if (!pScene || !pScene->m_LocalToRuntime.contains(Id)) return "ApplyOverrides: target not found";

            const auto RootEntity = pScene->m_LocalToRuntime.at(Id);
            if (auto Err = xlioncore::Ecs(World()).ApplyInstanceOverridesToPrefab(RootEntity); Err)
                return std::format("ApplyOverrides: {}", Err.getMessage());

            World().m_SceneMgr.MarkEntityDirty(SceneGuid, Id);
            State().m_bEntityInspectorDirty = true;
            return {};
        }

        void BackupCurrenState(xundo::undo_file& File) noexcept override
        {
            auto SceneArg = m_Parser.getOptionArgAs<std::string>(m_hScene, 0);
            auto IdArg    = m_Parser.getOptionArgAs<std::string>(m_hId, 0);
            const std::uint64_t Scene = std::holds_alternative<xerr>(SceneArg) ? 0 : std::strtoull(std::get<std::string>(SceneArg).c_str(), nullptr, 16);
            const std::uint32_t Id    = std::holds_alternative<xerr>(IdArg) ? 0 : ParseEntityId(std::get<std::string>(IdArg));
            File.Write(Scene);
            File.Write(Id);


            const auto SceneGuid = xecs::scene::guid{ .m_Instance = { Scene } };
            auto* pScene = World().m_SceneMgr.Find(SceneGuid);
            if (!pScene || !pScene->m_LocalToRuntime.contains(static_cast<xecs::scene::permanent_id>(Id)))
            {
                File.Write(std::uint32_t{ 0 });
                File.Write(std::uint32_t{ 0 });
                File.Write(std::uint32_t{ 0 });
                return;
            }

            const auto RootEntity = pScene->m_LocalToRuntime.at(static_cast<xecs::scene::permanent_id>(Id));
            auto* pRootPrefabInstance = xlioncore::ComponentOf<xecs::editor::prefab_instance>(xlioncore::Ecs(World()), RootEntity);
            if (pRootPrefabInstance == nullptr)
            {
                File.Write(std::uint32_t{ 0 });
                File.Write(std::uint32_t{ 0 });
                File.Write(std::uint32_t{ 0 });
                return;
            }

            auto& PI = *pRootPrefabInstance;
            SnapshotAllOverrideBookkeeping(File, PI);
            SnapshotPrefabBeforeValues(File, World(), RootEntity, PI);
        }

        void Undo(xundo::undo_file& File) noexcept override
        {
            std::uint64_t Scene = 0; File.Read(Scene);
            std::uint32_t Id = 0;    File.Read(Id);

            const auto SceneGuid = xecs::scene::guid{ .m_Instance = { Scene } };
            auto* pScene = World().m_SceneMgr.Find(SceneGuid);
            if (!pScene || !pScene->m_LocalToRuntime.contains(static_cast<xecs::scene::permanent_id>(Id))) return;

            const auto RootEntity = pScene->m_LocalToRuntime.at(static_cast<xecs::scene::permanent_id>(Id));
            auto* pRootPrefabInstance = xlioncore::ComponentOf<xecs::editor::prefab_instance>(xlioncore::Ecs(World()), RootEntity);
            if (pRootPrefabInstance == nullptr) return;

            auto& PI = *pRootPrefabInstance;

            // File order: override bookkeeping, then prefab-before values. Restore Prefab first (needs
            // EnsureLoaded), then put the instance's override list back, then re-save the Prefab.
            // But bookkeeping is written first - so read bookkeeping into a temp, restore prefab
            // befores, then assign bookkeeping.
            xecs::editor::prefab_instance TempPI;
            TempPI.m_PrefabInstance = PI.m_PrefabInstance;
            RestoreAllOverrideBookkeeping(File, TempPI);
            RestorePrefabBeforeValues(File, World(), PI);

            PI.m_lComponents    = std::move(TempPI.m_lComponents);
            PI.m_HierarchyDiffs = std::move(TempPI.m_HierarchyDiffs);
            if (auto Err = xlioncore::Ecs(World()).SavePrefab(PI.m_PrefabInstance); Err)
                xeditor::NotifyToast(std::format("ApplyOverrides Undo: Prefab Save failed: {}", Err.getMessage()));

            World().m_SceneMgr.MarkEntityDirty(SceneGuid, static_cast<xecs::scene::permanent_id>(Id));
            State().m_bEntityInspectorDirty = true;
        }

        xcmdline::parser::handle m_hScene, m_hId;
    };

    //------------------------------------------------------------------------------------------------
    // RevertHierarchyOverrides - Unity-style discard of hierarchy overrides on one PI root:
    //   Added  -> delete the instance-only child subtree
    //   Removed -> clone the prefab member back under the instance parent
    // Undo restores HierarchyDiffs bookkeeping and reverses those structural edits via the shared
    // subtree snapshot helpers.
    //------------------------------------------------------------------------------------------------
    struct revert_hierarchy_overrides_cmd : scene_command
    {
        revert_hierarchy_overrides_cmd(xundo::system& System, void* pDataBase) noexcept
            : scene_command(System, "RevertHierarchyOverrides", pDataBase) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override
        {
            return "Reverts hierarchy overrides on a prefab instance (deletes Added children; restores Removed children from the Prefab). Usage: RevertHierarchyOverrides -Scene hexguid -Id hexid";
        }
        void RegisterArguments() noexcept override
        {
            m_hScene = m_Parser.addOption("Scene", "Scene guid, 16 hex digits", true, 1);
            m_hId    = m_Parser.addOption("Id",    "Prefab-instance root permanent_id, 8 hex digits", true, 1);
        }

        // Clone a prefab-resident entity (and plain children) into the open scene under Parent.
        static xecs::component::entity ClonePrefabEntityIntoScene(
            xecs::game_mgr::instance& GameMgr,
            xecs::scene::instance& Scene,
            xecs::scene::guid SceneGuid,
            xecs::component::entity Source,
            xecs::component::entity ParentEntity,
            std::uint32_t InsertIndex) noexcept
        {
            auto& Ecs = xlioncore::Ecs(GameMgr);
            if (!Ecs.IsAlive(Source)) return {};

            // a new entity with the same data components (and a parent component when it goes under one): made inside the copy of the core the world belongs to
            auto NewEntity = Ecs.CloneEntity(Source, ParentEntity.isValid());

            const auto Id = xscene::NextFreeEntityId(Scene);
            Scene.m_LocalToRuntime[Id] = NewEntity;
            Scene.m_RuntimeToLocal[NewEntity.m_Value] = Id;
            GameMgr.m_SceneMgr.MarkEntityNew(SceneGuid, Id);

            if (ParentEntity.isValid())
            {
                if (auto* pNewParent = Ecs.ParentOf(NewEntity))
                {
                    pNewParent->m_Value = ParentEntity;
                    if (auto* pParentChildren = Ecs.ChildrenOf(ParentEntity))
                    {
                        auto& List = pParentChildren->m_List;
                        if (InsertIndex <= List.size())
                            List.insert(List.begin() + static_cast<std::ptrdiff_t>(InsertIndex), NewEntity);
                        else
                            List.push_back(NewEntity);
                    }
                    if (auto It = Scene.m_RuntimeToLocal.find(ParentEntity.m_Value); It != Scene.m_RuntimeToLocal.end())
                        GameMgr.m_SceneMgr.MarkEntityDirty(SceneGuid, It->second);
                }
            }

            // Opaque nested prefab instance: do not recurse.
            if (xlioncore::ComponentOf<xecs::editor::prefab_instance>(Ecs, Source))
                return NewEntity;

            if (auto* pSourceChildren = Ecs.ChildrenOf(Source))
            {
                auto ChildList = pSourceChildren->m_List;
                std::uint32_t i = 0;
                for (auto Child : ChildList)
                {
                    ClonePrefabEntityIntoScene(GameMgr, Scene, SceneGuid, Child, NewEntity, i);
                    ++i;
                }
            }
            return NewEntity;
        }

        std::string Redo() noexcept override
        {
            auto SceneArg = m_Parser.getOptionArgAs<std::string>(m_hScene, 0);
            auto IdArg    = m_Parser.getOptionArgAs<std::string>(m_hId, 0);
            if (std::holds_alternative<xerr>(SceneArg) || std::holds_alternative<xerr>(IdArg))
                return "RevertHierarchyOverrides: bad arguments";
            const auto SceneGuid = ParseSceneGuid(std::get<std::string>(SceneArg));
            const auto Id        = ParseEntityId(std::get<std::string>(IdArg));
            auto* pScene = World().m_SceneMgr.Find(SceneGuid);
            if (!pScene || !pScene->m_LocalToRuntime.contains(Id))
                return "RevertHierarchyOverrides: target not found";

            const auto RootEntity = pScene->m_LocalToRuntime.at(Id);
            auto* pPI = xscene::FindPrefabInstance(World(), RootEntity);
            if (!pPI) return "RevertHierarchyOverrides: not a prefab instance";
            if (pPI->m_HierarchyDiffs.empty()) return {};

            if (auto Err = xlioncore::Ecs(World()).EnsureLoadedPrefab(pPI->m_PrefabInstance); Err)
                return std::format("RevertHierarchyOverrides: {}", Err.getMessage());
            auto RootIt = World().m_PrefabMgr.m_PrefabList.find(pPI->m_PrefabInstance.m_Instance.m_Value);
            if (RootIt == World().m_PrefabMgr.m_PrefabList.end())
                return "RevertHierarchyOverrides: prefab root missing";

            auto Diffs = pPI->m_HierarchyDiffs;

            // Added: delete deepest-first
            std::vector<std::vector<std::uint32_t>> Added;
            for (auto& D : Diffs) if (D.m_bAdded && !D.m_MemberPath.empty()) Added.push_back(D.m_MemberPath);
            std::sort(Added.begin(), Added.end(), [](const auto& A, const auto& B) noexcept {
                if (A.size() != B.size()) return A.size() > B.size();
                return std::lexicographical_compare(B.begin(), B.end(), A.begin(), A.end());
            });
            for (auto& Path : Added)
            {
                const auto Target = xlioncore::Ecs(World()).ResolveMemberPath(RootEntity, Path);
                if (!Target.isValid()) continue;
                if (auto It = pScene->m_RuntimeToLocal.find(Target.m_Value); It != pScene->m_RuntimeToLocal.end())
                    xscene::DeleteEntitySubtree(World(), *pScene, SceneGuid, Target, /*bRecordPrefabOverride*/ true);
            }

            // Removed: restore shallowest-first
            std::vector<std::vector<std::uint32_t>> Removed;
            for (auto& D : Diffs) if (!D.m_bAdded && !D.m_MemberPath.empty()) Removed.push_back(D.m_MemberPath);
            std::sort(Removed.begin(), Removed.end(), [](const auto& A, const auto& B) noexcept {
                if (A.size() != B.size()) return A.size() < B.size();
                return std::lexicographical_compare(A.begin(), A.end(), B.begin(), B.end());
            });
            for (auto& Path : Removed)
            {
                const auto PrefabSrc = xlioncore::Ecs(World()).ResolveMemberPath(RootIt->second, Path);
                if (!PrefabSrc.isValid()) continue;
                const auto InsertIndex = Path.back();
                std::vector<std::uint32_t> ParentPath(Path.begin(), Path.end() - 1);
                const auto InstParent = ParentPath.empty()
                    ? RootEntity
                    : xlioncore::Ecs(World()).ResolveMemberPath(RootEntity, ParentPath);
                if (!InstParent.isValid()) continue;
                ClonePrefabEntityIntoScene(World(), *pScene, SceneGuid, PrefabSrc, InstParent, InsertIndex);
            }

            // Cancel any leftover hierarchy diffs that matched (delete-added already cleared Added;
            // strip Remaining Removed entries we restored).
            pPI->m_HierarchyDiffs.clear();
            World().m_SceneMgr.MarkEntityDirty(SceneGuid, Id);
            State().m_bEntityInspectorDirty = true;
            return {};
        }

        void BackupCurrenState(xundo::undo_file& File) noexcept override
        {
            auto SceneArg = m_Parser.getOptionArgAs<std::string>(m_hScene, 0);
            auto IdArg    = m_Parser.getOptionArgAs<std::string>(m_hId, 0);
            const std::uint64_t Scene = std::holds_alternative<xerr>(SceneArg) ? 0 : std::strtoull(std::get<std::string>(SceneArg).c_str(), nullptr, 16);
            const std::uint32_t Id    = std::holds_alternative<xerr>(IdArg) ? 0 : ParseEntityId(std::get<std::string>(IdArg));
            File.Write(Scene);
            File.Write(Id);

            const auto SceneGuid = xecs::scene::guid{ .m_Instance = { Scene } };
            auto* pScene = World().m_SceneMgr.Find(SceneGuid);
            if (!pScene || !pScene->m_LocalToRuntime.contains(static_cast<xecs::scene::permanent_id>(Id)))
            {
                File.Write(std::uint32_t{ 0 });
                File.Write(std::uint32_t{ 0 });
                return;
            }
            const auto RootEntity = pScene->m_LocalToRuntime.at(static_cast<xecs::scene::permanent_id>(Id));
            auto* pPI = xscene::FindPrefabInstance(World(), RootEntity);
            if (!pPI)
            {
                File.Write(std::uint32_t{ 0 });
                File.Write(std::uint32_t{ 0 });
                return;
            }

            // Bookkeeping + snapshot each Added child subtree (deleted in Redo).
            SnapshotAllOverrideBookkeeping(File, *pPI);
            std::vector<xecs::scene::permanent_id> AddedIds;
            for (auto& D : pPI->m_HierarchyDiffs)
            {
                if (!D.m_bAdded || D.m_MemberPath.empty()) continue;
                const auto Target = xlioncore::Ecs(World()).ResolveMemberPath(RootEntity, D.m_MemberPath);
                if (!Target.isValid()) continue;
                if (auto It = pScene->m_RuntimeToLocal.find(Target.m_Value); It != pScene->m_RuntimeToLocal.end())
                    AddedIds.push_back(It->second);
            }
            File.Write(static_cast<std::uint32_t>(AddedIds.size()));
            for (auto Aid : AddedIds)
            {
                File.Write(static_cast<std::uint32_t>(Aid));
                SnapshotSubtreeForRestore(SceneContext(), File, SceneGuid, Aid);
            }
        }

        void Undo(xundo::undo_file& File) noexcept override
        {
            std::uint64_t Scene = 0; File.Read(Scene);
            std::uint32_t Id = 0;    File.Read(Id);
            const auto SceneGuid = xecs::scene::guid{ .m_Instance = { Scene } };
            auto* pScene = World().m_SceneMgr.Find(SceneGuid);
            if (!pScene || !pScene->m_LocalToRuntime.contains(static_cast<xecs::scene::permanent_id>(Id))) return;
            const auto RootEntity = pScene->m_LocalToRuntime.at(static_cast<xecs::scene::permanent_id>(Id));
            auto* pPI = xscene::FindPrefabInstance(World(), RootEntity);
            if (!pPI) return;

            xecs::editor::prefab_instance TempPI;
            TempPI.m_PrefabInstance = pPI->m_PrefabInstance;
            RestoreAllOverrideBookkeeping(File, TempPI);

            std::uint32_t AddedCount = 0; File.Read(AddedCount);
            for (std::uint32_t i = 0; i < AddedCount; ++i)
            {
                std::uint32_t Aid = 0; File.Read(Aid);
                RestoreSubtreeFromSnapshot(SceneContext(), File, SceneGuid, static_cast<xecs::scene::permanent_id>(Aid));
            }

            // Drop children that Undo-of-Removed restored: any live child path that was a Removed
            // entry in TempPI and is present now but wasn't an Added restore. Simplest: delete
            // resolved Removed paths on the instance (they were re-cloned in Redo), deepest-first
            // and descending-index order so sibling shifts don't invalidate subsequent paths.
            std::vector<std::vector<std::uint32_t>> ToDelete;
            for (auto& D : TempPI.m_HierarchyDiffs)
                if (!D.m_bAdded && !D.m_MemberPath.empty()) ToDelete.push_back(D.m_MemberPath);
            std::sort(ToDelete.begin(), ToDelete.end(), [](const auto& A, const auto& B) noexcept {
                if (A.size() != B.size()) return A.size() > B.size();
                return std::lexicographical_compare(B.begin(), B.end(), A.begin(), A.end());
            });
            for (auto& Path : ToDelete)
            {
                const auto Target = xlioncore::Ecs(World()).ResolveMemberPath(RootEntity, Path);
                if (!Target.isValid()) continue;
                xscene::DeleteEntitySubtree(World(), *pScene, SceneGuid, Target, /*bRecordPrefabOverride*/ false);
            }

            pPI->m_lComponents    = std::move(TempPI.m_lComponents);
            pPI->m_HierarchyDiffs = std::move(TempPI.m_HierarchyDiffs);
            World().m_SceneMgr.MarkEntityDirty(SceneGuid, static_cast<xecs::scene::permanent_id>(Id));
            State().m_bEntityInspectorDirty = true;
        }

        xcmdline::parser::handle m_hScene, m_hId;
    };


    //================================================================================================
    // RevertAllOverrides - Unity-style "Revert All": wipe property + component + hierarchy overrides
    // by re-syncing from the Prefab while keeping scene placement (permanent_id, parent, folder,
    // root Transform). Does NOT modify the Prefab asset. Undo restores the pre-revert subtree via
    // SnapshotSubtreeForRestore / RestoreSubtreeFromSnapshot (same as DeleteEntity / MakePrefab).
    //================================================================================================
    struct revert_all_overrides_cmd : scene_command
    {
        // Layout must match xscene::transform (Position/Rotation/Scale as xmath::fvec3). Looked up by
        // component name so this header does not depend on the cpp-local transform type.
        struct root_transform_snapshot
        {
            xmath::fvec3 m_Position = xmath::fvec3::fromZero();
            xmath::fvec3 m_Rotation = xmath::fvec3::fromZero();
            xmath::fvec3 m_Scale    = xmath::fvec3::fromOne();
        };

        static const xecs::component::type::info* FindTransformInfo(
            xecs::game_mgr::instance& GameMgr, xecs::component::entity Entity) noexcept
        {
            std::vector<xlioncore::xECSEditor::component_view> Components;
            xlioncore::Ecs(GameMgr).DataComponentsOf(Entity, Components);
            for (const auto& [pInfo, pData] : Components)
            {
                if (pInfo && pInfo->m_pName && std::strcmp(pInfo->m_pName, "Transform") == 0)
                    return pInfo;
            }
            return nullptr;
        }

        static bool ReadTransform(xecs::game_mgr::instance& GameMgr, xecs::component::entity Entity,
            const xecs::component::type::info* pInfo, root_transform_snapshot& Out) noexcept
        {
            if (!pInfo || pInfo->m_Size != sizeof(root_transform_snapshot)) return false;
            const xecs::component::type::info* pFound = nullptr;
            auto* pData = xlioncore::Ecs(GameMgr).ResolveComponent(Entity, pInfo->m_Guid, pFound);
            if (!pData) return false;
            std::memcpy(&Out, pData, sizeof(Out));
            return true;
        }

        static void WriteTransform(xecs::game_mgr::instance& GameMgr, xecs::component::entity& Entity,
            xecs::scene::instance& Scene, xecs::scene::permanent_id Id,
            const xecs::component::type::info* pInfo, const root_transform_snapshot& In) noexcept
        {
            if (!pInfo || pInfo->m_Size != sizeof(root_transform_snapshot)) return;
            auto& Ecs = xlioncore::Ecs(GameMgr);
            const xecs::component::type::info* pFound = nullptr;
            auto* pData = Ecs.ResolveComponent(Entity, pInfo->m_Guid, pFound);
            if (!pData)
            {
                const std::array Add{ pInfo->m_Guid };
                Entity = Ecs.AddComponents(Entity, Add);
                Scene.m_LocalToRuntime[Id] = Entity;
                Scene.m_RuntimeToLocal[Entity.m_Value] = Id;
                pData = Ecs.ResolveComponent(Entity, pInfo->m_Guid, pFound);
                if (!pData) return;
            }
            std::memcpy(pData, &In, sizeof(In));
        }

        revert_all_overrides_cmd(xundo::system& System, void* pDataBase) noexcept
            : scene_command(System, "RevertAllOverrides", pDataBase) { RegisterArguments(); }

        const char* getCommandHelp() const noexcept override
        {
            return "Reverts ALL overrides on a prefab instance (properties, added/removed components, "
                   "hierarchy) by re-syncing from the Prefab; keeps the instance root Transform / "
                   "scene placement. Does not modify the Prefab asset. "
                   "Usage: RevertAllOverrides -Scene hexguid -Id hexid";
        }

        void RegisterArguments() noexcept override
        {
            m_hScene = m_Parser.addOption("Scene", "Scene guid, 16 hex digits", true, 1);
            m_hId    = m_Parser.addOption("Id",    "Prefab-instance root permanent_id, 8 hex digits", true, 1);
        }

        std::string Redo() noexcept override
        {
            auto SceneArg = m_Parser.getOptionArgAs<std::string>(m_hScene, 0);
            auto IdArg    = m_Parser.getOptionArgAs<std::string>(m_hId, 0);
            if (std::holds_alternative<xerr>(SceneArg) || std::holds_alternative<xerr>(IdArg))
                return "RevertAllOverrides: bad arguments";

            const auto SceneGuid = ParseSceneGuid(std::get<std::string>(SceneArg));
            const auto Id        = ParseEntityId(std::get<std::string>(IdArg));

            auto* pScene = World().m_SceneMgr.Find(SceneGuid);
            if (!pScene || !pScene->m_LocalToRuntime.contains(Id))
                return "RevertAllOverrides: target not found";

            auto Root = pScene->m_LocalToRuntime.at(Id);
            auto* pPI = xscene::FindPrefabInstance(World(), Root);
            if (!pPI) return "RevertAllOverrides: entity is not a prefab instance root";

            const auto PrefabGuid = pPI->m_PrefabInstance;
            if (auto Err = xlioncore::Ecs(World()).EnsureLoadedPrefab(PrefabGuid); Err)
                return std::format("RevertAllOverrides: {}", Err.getMessage());

            auto PrefabIt = World().m_PrefabMgr.m_PrefabList.find(PrefabGuid.m_Instance.m_Value);
            if (PrefabIt == World().m_PrefabMgr.m_PrefabList.end())
                return "RevertAllOverrides: prefab root not resident";

            xecs::component::entity OriginalParent{};
            if (auto* pRootParent = xlioncore::Ecs(World()).ParentOf(Root))
                OriginalParent = pRootParent->m_Value;
            const auto* pXformInfo = FindTransformInfo(World(), Root);
            root_transform_snapshot SavedXform{};
            const bool bHasTransform = ReadTransform(World(), Root, pXformInfo, SavedXform);
            const auto OriginalFolderId = xscene::FindFolderContaining(*pScene, Id);
            const auto StaleRootValue   = Root.m_Value;
            const bool bRootWasSelected = &State()
                && State().m_SelectedEntityId == Id
                && State().m_SelectedEntityScene == SceneGuid;

            // Rebuild: wipe live instance (no HierarchyDiff bookkeeping — we own the PI root).
            xscene::DeleteEntitySubtree(World(), *pScene, SceneGuid, Root, /*bRecordPrefabOverride*/ false);

            auto NewRoot = xlioncore::Ecs(World()).CreatePrefabInstance(PrefabIt->second, /*bRemoveRoot=*/false);

            if (OriginalParent.isValid())
            {
                auto& Ecs = xlioncore::Ecs(World());
                NewRoot = xlioncore::AddComponentsOf<xecs::component::parent>(Ecs, NewRoot);
                Ecs.ParentOf(NewRoot)->m_Value = OriginalParent;

                if (auto* pOriginalChildren = Ecs.ChildrenOf(OriginalParent))
                {
                    auto& OPChildren = pOriginalChildren->m_List;
                    for (auto& C : OPChildren)
                        if (C.m_Value == StaleRootValue) { C = NewRoot; break; }
                }
            }

            {
                if (auto* pNewChildren = xlioncore::Ecs(World()).ChildrenOf(NewRoot))
                {
                    auto ChildEntities = pNewChildren->m_List;
                    for (auto Child : ChildEntities)
                        xscene::RegisterInstantiatedSubtree(World(), *pScene, SceneGuid, Child);
                }
            }

            pScene->m_LocalToRuntime[Id]              = NewRoot;
            pScene->m_RuntimeToLocal[NewRoot.m_Value] = Id;

            if (false == OriginalParent.isValid())
                xscene::ReparentEntityIntoFolder(*pScene, Id, OriginalFolderId);

            xscene::AttachPrefabInstanceComponent(World(), *pScene, Id, NewRoot, PrefabGuid, &State());
            // Attach clears m_lComponents / m_ComponentDiffs; HierarchyDiffs may remain on some revisions.
            if (auto* pNewPI = xscene::FindPrefabInstance(World(), NewRoot))
            {
                pNewPI->m_lComponents.clear();
                pNewPI->m_ComponentDiffs.clear();
                pNewPI->m_HierarchyDiffs.clear();
            }

            if (bHasTransform)
                WriteTransform(World(), NewRoot, *pScene, Id, pXformInfo, SavedXform);

            // Re-resolve after possible AddOrRemoveComponents inside WriteTransform.
            if (auto It = pScene->m_LocalToRuntime.find(Id); It != pScene->m_LocalToRuntime.end())
                NewRoot = It->second;
            if (auto* pNewPI2 = xscene::FindPrefabInstance(World(), NewRoot))
            {
                pNewPI2->m_lComponents.clear();
                pNewPI2->m_ComponentDiffs.clear();
                pNewPI2->m_HierarchyDiffs.clear();
            }

            World().m_SceneMgr.MarkEntityDirty(SceneGuid, Id);
            if (bRootWasSelected)
            {
                State().m_SelectedEntity      = NewRoot;
                State().m_SelectedEntityId    = Id;
                State().m_SelectedEntityScene = SceneGuid;
            }
            State().m_bEntityInspectorDirty = true;
            return {};
        }

        void BackupCurrenState(xundo::undo_file& File) noexcept override
        {
            auto SceneArg = m_Parser.getOptionArgAs<std::string>(m_hScene, 0);
            auto IdArg    = m_Parser.getOptionArgAs<std::string>(m_hId, 0);
            const std::uint64_t Scene = std::holds_alternative<xerr>(SceneArg) ? 0
                : std::strtoull(std::get<std::string>(SceneArg).c_str(), nullptr, 16);
            const std::uint32_t Id = std::holds_alternative<xerr>(IdArg) ? 0
                : ParseEntityId(std::get<std::string>(IdArg));
            File.Write(Scene);
            File.Write(Id);
            SnapshotSubtreeForRestore(SceneContext(), File,
                xecs::scene::guid{ .m_Instance = { Scene } },
                static_cast<xecs::scene::permanent_id>(Id));
        }

        void Undo(xundo::undo_file& File) noexcept override
        {
            std::uint64_t Scene = 0; File.Read(Scene);
            std::uint32_t Id = 0;    File.Read(Id);

            const auto SceneGuid = xecs::scene::guid{ .m_Instance = { Scene } };
            auto* pScene = World().m_SceneMgr.Find(SceneGuid);
            if (pScene && pScene->m_LocalToRuntime.contains(static_cast<xecs::scene::permanent_id>(Id)))
            {
                xscene::DeleteEntitySubtree(World(), *pScene, SceneGuid,
                    pScene->m_LocalToRuntime.at(static_cast<xecs::scene::permanent_id>(Id)),
                    /*bRecordPrefabOverride*/ false);
            }

            RestoreSubtreeFromSnapshot(SceneContext(), File, SceneGuid, static_cast<xecs::scene::permanent_id>(Id));

            State().m_bEntityInspectorDirty = true;
        }

        xcmdline::parser::handle m_hScene, m_hId;
    };


}

#endif // XSCENE_COMMANDS_APPLY_OVERRIDES_H
