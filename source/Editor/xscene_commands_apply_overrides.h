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
    // A member's address (xecs::editor::member_address) in an undo record.
    inline void WriteMemberPath(xundo::undo_file& File, const xecs::editor::member_address& Address) noexcept
    {
        File.Write(static_cast<std::uint32_t>(Address.size()));
        for (auto P : Address) File.Write(P);
    }

    inline xecs::editor::member_address ReadMemberPath(xundo::undo_file& File) noexcept
    {
        std::uint32_t Count = 0; File.Read(Count);
        xecs::editor::member_address Address(Count);
        for (auto& P : Address) File.Read(P);
        return Address;
    }

    // Full PI.m_lComponents dump - same length-prefixed shape SnapshotComponentOverrideEntry uses,
    // but for EVERY component-override entry on the instance (Apply clears the whole list).
    inline void SnapshotAllOverrideBookkeeping(xundo::undo_file& File, const xecs::editor::prefab_instance& PI) noexcept
    {
        File.Write(static_cast<std::uint32_t>(PI.m_lComponents.size()));
        for (auto& C : PI.m_lComponents)
        {
            File.Write(C.m_ComponentTypeGuid);
            WriteMemberPath(File, C.m_Member);
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
            WriteMemberPath(File, H.m_Member);
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
            Comp.m_Member = ReadMemberPath(File);
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
            H.m_Member = ReadMemberPath(File);
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
            std::uint64_t                m_ComponentTypeGuid = 0;
            xecs::editor::member_address m_MemberPath;
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

                    const auto PrefabEntity = xlioncore::Ecs(GameMgr).ResolvePrefabMember(PI.m_PrefabInstance, CompOverride.m_Member);
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
                        R.m_MemberPath        = CompOverride.m_Member;
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

            const auto PrefabEntity = xlioncore::Ecs(GameMgr).ResolvePrefabMember(PI.m_PrefabInstance, MemberPath);
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
            m_hId    = m_Parser.addOption("Id",    "Prefab-instance root permanent_id, 8 or 16 hex digits", true, 1);
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

            if (auto Err = xlioncore::Ecs(World()).ApplyInstanceOverridesToPrefab(*pScene, Id); Err)
                return std::format("ApplyOverrides: {}", Err.getMessage());

            World().m_SceneMgr.MarkEntityDirty(SceneGuid, Id);
            ResolveSelection(World(), State());         // the other instances of the prefab in this world were spawned again with the change (live update, prefabs_plan.md phase 6)
            return {};
        }

        void BackupCurrenState(xundo::undo_file& File) noexcept override
        {
            auto SceneArg = m_Parser.getOptionArgAs<std::string>(m_hScene, 0);
            auto IdArg    = m_Parser.getOptionArgAs<std::string>(m_hId, 0);
            const std::uint64_t Scene = std::holds_alternative<xerr>(SceneArg) ? 0 : std::strtoull(std::get<std::string>(SceneArg).c_str(), nullptr, 16);
            const xecs::scene::permanent_id Id    = std::holds_alternative<xerr>(IdArg) ? 0 : ParseEntityId(std::get<std::string>(IdArg));
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
            xecs::scene::permanent_id Id = 0;    File.Read(Id);

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
            // The live members say what the overrides say again: since the Apply the instance may have been spawned again without them (a Prefab Editor that turned the change down
            // brings the Level back to the file), and the live update below reads its recipe from what the members hold.
            xlioncore::Ecs(World()).ApplyPrefabRecipeToMembers(*pScene, static_cast<xecs::scene::permanent_id>(Id));
            const auto Prefab = PI.m_PrefabInstance;
            if (auto Err = xlioncore::Ecs(World()).SavePrefab(Prefab); Err)
                xeditor::NotifyToast(std::format("ApplyOverrides Undo: Prefab Save failed: {}", Err.getMessage()));

            World().m_SceneMgr.MarkEntityDirty(SceneGuid, static_cast<xecs::scene::permanent_id>(Id));
            // The instances of the prefab in this world (this one too: PI is not used after this) are spawned again from the template as it is back to (live update,
            // prefabs_plan.md phase 6; the template's values only changed, so their recipes read the same against it). The other editors heard of the save.
            xlioncore::Ecs(World()).LiveUpdatePrefab(Prefab, /*bFromFile*/ false);
            ResolveSelection(World(), State());
        }

        xcmdline::parser::handle m_hScene, m_hId;
    };

    //------------------------------------------------------------------------------------------------
    // RevertHierarchyOverrides - Unity-style discard of hierarchy overrides on one instance:
    //   the entities of the scene under the instance (under its root or a member) -> deleted
    //   the members the instance removed -> spawned again from the prefab (same derived ids, the recipe's overrides on them)
    // Undo restores the deleted entities (the subtree snapshot helpers) and deletes the members Redo spawned.
    //------------------------------------------------------------------------------------------------
    // The entities of the scene directly under an instance (its root or its members) that are no members of it.
    inline std::vector<xecs::scene::permanent_id> EntitiesAddedUnderInstance(xecs::game_mgr::instance& GameMgr, xecs::scene::instance& Scene, xecs::scene::permanent_id RootId) noexcept
    {
        std::vector<xecs::scene::permanent_id> Out;
        auto& Ecs = xlioncore::Ecs(GameMgr);
        const auto Collect = [&](xecs::scene::permanent_id Id) noexcept
        {
            auto It = Scene.m_LocalToRuntime.find(Id);
            if (It == Scene.m_LocalToRuntime.end()) return;
            auto* pKids = Ecs.ChildrenOf(It->second);
            if (pKids == nullptr) return;
            for (auto K : pKids->m_List)
                if (auto KIt = Scene.m_RuntimeToLocal.find(K.m_Value); KIt != Scene.m_RuntimeToLocal.end() && !Scene.m_InstanceMembers.contains(KIt->second))
                    Out.push_back(KIt->second);
        };
        Collect(RootId);
        for (auto& [Id, M] : Scene.m_InstanceMembers) if (M.m_Root == RootId) Collect(Id);
        return Out;
    }

    struct revert_hierarchy_overrides_cmd : scene_command
    {
        revert_hierarchy_overrides_cmd(xundo::system& System, void* pDataBase) noexcept
            : scene_command(System, "RevertHierarchyOverrides", pDataBase) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override
        {
            return "Reverts hierarchy overrides on a prefab instance (deletes the entities added under it; brings back from the Prefab the members it removed). Usage: RevertHierarchyOverrides -Scene hexguid -Id hexid";
        }
        void RegisterArguments() noexcept override
        {
            m_hScene = m_Parser.addOption("Scene", "Scene guid, 16 hex digits", true, 1);
            m_hId    = m_Parser.addOption("Id",    "Prefab-instance root permanent_id, 8 or 16 hex digits", true, 1);
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
            if (!xscene::FindPrefabInstance(World(), pScene->m_LocalToRuntime.at(Id)) || pScene->m_InstanceMembers.contains(Id)) return "RevertHierarchyOverrides: not a prefab instance";

            for (auto Added : EntitiesAddedUnderInstance(World(), *pScene, Id))
                if (auto It = pScene->m_LocalToRuntime.find(Added); It != pScene->m_LocalToRuntime.end())
                    xscene::DeleteEntitySubtree(World(), *pScene, SceneGuid, It->second, /*bRecordPrefabOverride*/ false);

            if (auto* pPI = xscene::FindPrefabInstance(World(), pScene->m_LocalToRuntime.at(Id)))
                pPI->m_HierarchyDiffs.clear();
            xlioncore::Ecs(World()).SpawnMissingPrefabMembers(*pScene, Id);

            World().m_SceneMgr.MarkEntityDirty(SceneGuid, Id);
            State().m_bEntityInspectorDirty = true;
            return {};
        }

        void BackupCurrenState(xundo::undo_file& File) noexcept override
        {
            auto SceneArg = m_Parser.getOptionArgAs<std::string>(m_hScene, 0);
            auto IdArg    = m_Parser.getOptionArgAs<std::string>(m_hId, 0);
            const std::uint64_t Scene = std::holds_alternative<xerr>(SceneArg) ? 0 : std::strtoull(std::get<std::string>(SceneArg).c_str(), nullptr, 16);
            const xecs::scene::permanent_id Id    = std::holds_alternative<xerr>(IdArg) ? 0 : ParseEntityId(std::get<std::string>(IdArg));
            File.Write(Scene);
            File.Write(Id);

            const auto SceneGuid = xecs::scene::guid{ .m_Instance = { Scene } };
            auto* pScene = World().m_SceneMgr.Find(SceneGuid);
            xecs::editor::prefab_instance* pPI = nullptr;
            if (pScene && pScene->m_LocalToRuntime.contains(Id))
            {
                xlioncore::Ecs(World()).RefreshPrefabRecipe(*pScene, Id);           // the members it removed are in its recipe now
                pPI = xscene::FindPrefabInstance(World(), pScene->m_LocalToRuntime.at(Id));
            }
            if (!pPI)
            {
                File.Write(std::uint32_t{ 0 });
                File.Write(std::uint32_t{ 0 });
                File.Write(std::uint32_t{ 0 });
                return;
            }

            // Bookkeeping + snapshot each added entity's subtree (deleted in Redo).
            SnapshotAllOverrideBookkeeping(File, *pPI);
            const auto Added = EntitiesAddedUnderInstance(World(), *pScene, Id);
            File.Write(static_cast<std::uint32_t>(Added.size()));
            for (auto Aid : Added)
            {
                File.Write(Aid);
                SnapshotSubtreeForRestore(SceneContext(), File, SceneGuid, Aid);
            }
        }

        void Undo(xundo::undo_file& File) noexcept override
        {
            std::uint64_t Scene = 0; File.Read(Scene);
            xecs::scene::permanent_id Id = 0;    File.Read(Id);
            const auto SceneGuid = xecs::scene::guid{ .m_Instance = { Scene } };
            auto* pScene = World().m_SceneMgr.Find(SceneGuid);

            xecs::editor::prefab_instance TempPI;
            RestoreAllOverrideBookkeeping(File, TempPI);

            std::uint32_t AddedCount = 0; File.Read(AddedCount);
            for (std::uint32_t i = 0; i < AddedCount; ++i)
            {
                xecs::scene::permanent_id Aid = 0; File.Read(Aid);
                RestoreSubtreeFromSnapshot(SceneContext(), File, SceneGuid, Aid);
            }
            if (!pScene || !pScene->m_LocalToRuntime.contains(Id)) return;

            // the members Redo brought back: removed again
            for (auto& D : TempPI.m_HierarchyDiffs)
            {
                if (D.m_bAdded || D.m_Member.empty()) continue;
                if (auto It = pScene->m_LocalToRuntime.find(xecs::scene::DeriveMemberId(Id, D.m_Member)); It != pScene->m_LocalToRuntime.end())
                    xscene::DeleteEntitySubtree(World(), *pScene, SceneGuid, It->second, /*bRecordPrefabOverride*/ false);
            }

            if (auto* pPI = xscene::FindPrefabInstance(World(), pScene->m_LocalToRuntime.at(Id)))
            {
                pPI->m_lComponents    = std::move(TempPI.m_lComponents);
                pPI->m_HierarchyDiffs = std::move(TempPI.m_HierarchyDiffs);
            }
            World().m_SceneMgr.MarkEntityDirty(SceneGuid, Id);
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
            m_hId    = m_Parser.addOption("Id",    "Prefab-instance root permanent_id, 8 or 16 hex digits", true, 1);
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

            if (pScene->m_InstanceMembers.contains(Id)) return "RevertAllOverrides: the entity is a member of another instance (revert that instance)";

            xecs::component::entity OriginalParent{};
            std::size_t             OriginalIndex = 0;
            if (auto* pRootParent = xlioncore::Ecs(World()).ParentOf(Root))
            {
                OriginalParent = pRootParent->m_Value;
                if (auto* pSiblings = OriginalParent.isValid() ? xlioncore::Ecs(World()).ChildrenOf(OriginalParent) : nullptr)
                    if (auto SIt = std::ranges::find(pSiblings->m_List, Root.m_Value, &xecs::component::entity::m_Value); SIt != pSiblings->m_List.end())
                        OriginalIndex = static_cast<std::size_t>(SIt - pSiblings->m_List.begin());
            }
            const auto* pXformInfo = FindTransformInfo(World(), Root);
            root_transform_snapshot SavedXform{};
            const bool bHasTransform = ReadTransform(World(), Root, pXformInfo, SavedXform);
            const auto OriginalFolderId = xscene::FindFolderContaining(*pScene, Id);
            const bool bRootWasSelected = State().m_SelectedEntityId == Id
                && State().m_SelectedEntityScene == SceneGuid;

            // Rebuild: wipe live instance (no HierarchyDiff bookkeeping — we own the PI root).
            xscene::DeleteEntitySubtree(World(), *pScene, SceneGuid, Root, /*bRecordPrefabOverride*/ false);

            auto NewRoot = xlioncore::Ecs(World()).InstantiatePrefabInScene(*pScene, PrefabGuid, Id, OriginalParent);
            if (!NewRoot.isValid()) return "RevertAllOverrides: the prefab could not be instantiated";
            if (OriginalParent.isValid())
            {
                if (auto* pSiblings = xlioncore::Ecs(World()).ChildrenOf(xlioncore::Ecs(World()).ParentOf(NewRoot)->m_Value))
                {
                    auto& L = pSiblings->m_List;
                    std::erase_if(L, [&](auto& E) noexcept { return E.m_Value == NewRoot.m_Value; });
                    L.insert(L.begin() + static_cast<std::ptrdiff_t>(std::min(OriginalIndex, L.size())), NewRoot);
                }
            }
            else xscene::ReparentEntityIntoFolder(*pScene, Id, OriginalFolderId);

            // The id lives on: not a delete (SaveScene would remove its file) and not a new entity (its file exists) - written again.
            pScene->m_PendingChanges[Id].m_Deleted -= 1;
            pScene->m_PendingChanges[Id].m_New     -= 1;

            if (bHasTransform)
                WriteTransform(World(), NewRoot, *pScene, Id, pXformInfo, SavedXform);

            // Re-resolve after possible AddOrRemoveComponents inside WriteTransform.
            if (auto It = pScene->m_LocalToRuntime.find(Id); It != pScene->m_LocalToRuntime.end())
                NewRoot = It->second;

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
            const xecs::scene::permanent_id Id = std::holds_alternative<xerr>(IdArg) ? 0
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
            xecs::scene::permanent_id Id = 0;    File.Read(Id);

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



    //================================================================================================
    // ListPrefabOverrides / RemoveOrphanOverrides - what a prefab instance's recipe holds (documentation/Editors/
    // prefabs_plan.md, phase 3): its property overrides, component diffs and removed members, each with the
    // address of its member (the member's id in the prefab, one per nested instance crossed). One whose member
    // the prefab no longer has is an ORPHAN: it is kept (a prefab change never silently moves an override to
    // another member) and listed, and RemoveOrphanOverrides takes them away (undoable).
    //================================================================================================
    inline std::string FormatMemberAddress(const xecs::editor::member_address& A) noexcept
    {
        if (A.empty()) return "(root)";
        std::string Out;
        for (auto Id : A) Out += (Out.empty() ? "" : "/") + xecs::scene::FormatPermanentId(Id);
        return Out;
    }

    // The addresses the instance's prefab has now (its own members and its nested instances').
    inline std::vector<xecs::editor::member_address> PrefabAddressesOf(xecs::game_mgr::instance& GameMgr, const xecs::editor::prefab_instance& PI) noexcept
    {
        std::vector<xecs::editor::member_address> Out;
        xlioncore::Ecs(GameMgr).PrefabMemberAddresses(PI.m_PrefabInstance, Out);
        return Out;
    }

    inline bool IsOrphan(const std::vector<xecs::editor::member_address>& Known, const xecs::editor::member_address& A) noexcept
    {
        return std::ranges::find(Known, A) == Known.end();
    }

    struct list_prefab_overrides_query_cmd : scene_query_command
    {
        list_prefab_overrides_query_cmd(xundo::system& System, void* pDataBase) noexcept : scene_query_command(System, "ListPrefabOverrides", pDataBase) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override
        {
            return "Lists what a prefab instance does differently from its prefab (its recipe): property overrides, component diffs, removed members, each with its member's address (ids in the prefab, / between nested instances; (root) for the instance itself) and ORPHAN when the prefab no longer has that member; then its members and their ids. Usage: ListPrefabOverrides -Scene hexguid -Id hexid";
        }
        void RegisterArguments() noexcept override
        {
            m_hScene = m_Parser.addOption("Scene", "Scene guid, 16 hex digits", true, 1);
            m_hId    = m_Parser.addOption("Id",    "Prefab-instance root permanent_id, 8 or 16 hex digits", true, 1);
        }

        std::string Query() noexcept override
        {
            auto SceneArg = m_Parser.getOptionArgAs<std::string>(m_hScene, 0);
            auto IdArg    = m_Parser.getOptionArgAs<std::string>(m_hId, 0);
            if (std::holds_alternative<xerr>(SceneArg) || std::holds_alternative<xerr>(IdArg)) return "ListPrefabOverrides: bad arguments";
            const auto SceneGuid = ParseSceneGuid(std::get<std::string>(SceneArg));
            const auto Id        = ParseEntityId(std::get<std::string>(IdArg));
            auto* pScene = World().m_SceneMgr.Find(SceneGuid);
            if (!pScene || !pScene->m_LocalToRuntime.contains(Id)) return "ListPrefabOverrides: target not found";
            if (pScene->m_InstanceMembers.contains(Id)) return "ListPrefabOverrides: the entity is a member of a prefab instance (list that instance)";
            if (!xscene::FindPrefabInstance(World(), pScene->m_LocalToRuntime.at(Id))) return "ListPrefabOverrides: not a prefab instance";

            xlioncore::Ecs(World()).RefreshPrefabRecipe(*pScene, Id);      // what its members are now
            const auto& PI    = *xscene::FindPrefabInstance(World(), pScene->m_LocalToRuntime.at(Id));
            const auto  Known = PrefabAddressesOf(World(), PI);
            const auto  Tag   = [&](const xecs::editor::member_address& A) { return IsOrphan(Known, A) ? "  ORPHAN" : ""; };
            const auto  Name  = [&](std::uint64_t Guid) -> std::string
            {
                auto* pInfo = xlioncore::Ecs(World()).FindComponentType(xecs::component::type::guid{ Guid });
                return pInfo && pInfo->m_pName ? pInfo->m_pName : std::format("{:016X}", Guid);
            };

            std::string Out = std::format("Prefab {:016X}  Instance {}\n", PI.m_PrefabInstance.m_Instance.m_Value, FormatEntityId(Id));
            int nOrphans = 0;
            Out += "Overrides:\n";
            for (auto& C : PI.m_lComponents)
            {
                if (IsOrphan(Known, C.m_Member)) ++nOrphans;
                for (auto& O : C.m_PropertyOverrides)
                    Out += std::format("  {}  {}  {} = {}{}\n", FormatMemberAddress(C.m_Member), Name(C.m_ComponentTypeGuid), O.m_PropertyName, O.m_PropertyValueAsString, Tag(C.m_Member));
            }
            Out += "Component diffs:\n";
            for (auto& D : PI.m_ComponentDiffs)
            {
                if (IsOrphan(Known, D.m_Member)) ++nOrphans;
                Out += std::format("  {}  {} {}{}\n", FormatMemberAddress(D.m_Member), D.m_bAdded ? "added" : "removed", Name(D.m_ComponentTypeGuid), Tag(D.m_Member));
            }
            Out += "Hierarchy:\n";
            for (auto& H : PI.m_HierarchyDiffs)
            {
                if (IsOrphan(Known, H.m_Member)) ++nOrphans;
                Out += std::format("  {}  {}{}\n", FormatMemberAddress(H.m_Member), H.m_bAdded ? "has entities of the scene under it" : "removed", Tag(H.m_Member));
            }
            Out += "Members:\n";
            std::vector<std::pair<xecs::editor::member_address, xecs::scene::permanent_id>> Members;
            for (auto& [MId, M] : pScene->m_InstanceMembers) if (M.m_Root == Id) Members.push_back({ M.m_Address, MId });
            std::sort(Members.begin(), Members.end());
            for (auto& [A, MId] : Members) Out += std::format("  {}  {}\n", FormatEntityId(MId), FormatMemberAddress(A));
            Out += std::format("Orphans: {}\n", nOrphans);
            return Out;
        }

        xcmdline::parser::handle m_hScene, m_hId;
    };

    struct remove_orphan_overrides_cmd : scene_command
    {
        remove_orphan_overrides_cmd(xundo::system& System, void* pDataBase) noexcept : scene_command(System, "RemoveOrphanOverrides", pDataBase) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override
        {
            return "Removes from a prefab instance's recipe what addresses a member its prefab no longer has (orphan overrides, component diffs and removals - see ListPrefabOverrides). Undoable. Usage: RemoveOrphanOverrides -Scene hexguid -Id hexid";
        }
        void RegisterArguments() noexcept override
        {
            m_hScene = m_Parser.addOption("Scene", "Scene guid, 16 hex digits", true, 1);
            m_hId    = m_Parser.addOption("Id",    "Prefab-instance root permanent_id, 8 or 16 hex digits", true, 1);
        }

        xecs::editor::prefab_instance* Target(xecs::scene::guid& SceneGuid, xecs::scene::permanent_id& Id) noexcept
        {
            auto SceneArg = m_Parser.getOptionArgAs<std::string>(m_hScene, 0);
            auto IdArg    = m_Parser.getOptionArgAs<std::string>(m_hId, 0);
            if (std::holds_alternative<xerr>(SceneArg) || std::holds_alternative<xerr>(IdArg)) return nullptr;
            SceneGuid = ParseSceneGuid(std::get<std::string>(SceneArg));
            Id        = ParseEntityId(std::get<std::string>(IdArg));
            auto* pScene = World().m_SceneMgr.Find(SceneGuid);
            if (!pScene || !pScene->m_LocalToRuntime.contains(Id) || pScene->m_InstanceMembers.contains(Id)) return nullptr;
            return xscene::FindPrefabInstance(World(), pScene->m_LocalToRuntime.at(Id));
        }

        std::string Redo() noexcept override
        {
            xecs::scene::guid SceneGuid; xecs::scene::permanent_id Id = 0;
            auto* pPI = Target(SceneGuid, Id);
            if (!pPI) return "RemoveOrphanOverrides: not a prefab instance";
            const auto Known = PrefabAddressesOf(World(), *pPI);
            pPI = Target(SceneGuid, Id);            // loading the prefab makes entities: fetched again
            std::erase_if(pPI->m_lComponents,    [&](auto& C) noexcept { return IsOrphan(Known, C.m_Member); });
            std::erase_if(pPI->m_ComponentDiffs, [&](auto& D) noexcept { return IsOrphan(Known, D.m_Member); });
            std::erase_if(pPI->m_HierarchyDiffs, [&](auto& H) noexcept { return IsOrphan(Known, H.m_Member); });
            World().m_SceneMgr.MarkEntityDirty(SceneGuid, Id);
            State().m_bEntityInspectorDirty = true;
            return {};
        }

        void BackupCurrenState(xundo::undo_file& File) noexcept override
        {
            xecs::scene::guid SceneGuid; xecs::scene::permanent_id Id = 0;
            auto* pPI = Target(SceneGuid, Id);
            File.Write(SceneGuid.m_Instance.m_Value);
            File.Write(Id);
            File.Write(pPI != nullptr);
            if (!pPI) return;
            SnapshotAllOverrideBookkeeping(File, *pPI);
            File.Write(static_cast<std::uint32_t>(pPI->m_ComponentDiffs.size()));
            for (auto& D : pPI->m_ComponentDiffs)
            {
                File.Write(D.m_ComponentTypeGuid);
                File.Write(D.m_bAdded);
                WriteMemberPath(File, D.m_Member);
            }
        }

        void Undo(xundo::undo_file& File) noexcept override
        {
            std::uint64_t Scene = 0;           File.Read(Scene);
            xecs::scene::permanent_id Id = 0;  File.Read(Id);
            bool bHad = false;                 File.Read(bHad);
            if (!bHad) return;
            xecs::editor::prefab_instance Temp;
            RestoreAllOverrideBookkeeping(File, Temp);
            std::uint32_t n = 0; File.Read(n);
            Temp.m_ComponentDiffs.resize(n);
            for (auto& D : Temp.m_ComponentDiffs)
            {
                File.Read(D.m_ComponentTypeGuid);
                File.Read(D.m_bAdded);
                D.m_Member = ReadMemberPath(File);
            }

            const auto SceneGuid = xecs::scene::guid{ .m_Instance = { Scene } };
            auto* pScene = World().m_SceneMgr.Find(SceneGuid);
            if (!pScene || !pScene->m_LocalToRuntime.contains(Id)) return;
            if (auto* pPI = xscene::FindPrefabInstance(World(), pScene->m_LocalToRuntime.at(Id)))
            {
                pPI->m_lComponents    = std::move(Temp.m_lComponents);
                pPI->m_HierarchyDiffs = std::move(Temp.m_HierarchyDiffs);
                pPI->m_ComponentDiffs = std::move(Temp.m_ComponentDiffs);
            }
            World().m_SceneMgr.MarkEntityDirty(SceneGuid, Id);
            State().m_bEntityInspectorDirty = true;
        }

        xcmdline::parser::handle m_hScene, m_hId;
    };

}

#endif // XSCENE_COMMANDS_APPLY_OVERRIDES_H
