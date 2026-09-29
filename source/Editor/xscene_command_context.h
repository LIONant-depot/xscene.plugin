#pragma once

// The database the scene commands work on is the scene context; scene_command gives them World(), State() and SceneContext().
// Also the guid and value formats the scene commands take on the command line.
#include "dependencies/xundo/source/xundo_system.h"
#include "dependencies/xeditor/include/xeditor/commands.h"
#include "dependencies/xeditor/include/xeditor/serialize.h"
#include "dependencies/xLIONCore/src/tags/xlioncore_tags.h"
#include "dependencies/xLIONCore/src/physics/xlioncore_physics_api.h"

namespace xscene::commands
{
    // Every scene command reaches its editor's world and state through its scene context: World(), State(), SceneContext().
    template<typename T_BASE>
    struct scene_command_mixin : T_BASE
    {
        using T_BASE::T_BASE;
        xecs::game_mgr::instance& World() noexcept { return this->template get<scene_context>().World(); }
        scene_state&              State() noexcept { return this->template get<scene_context>().m_State; }
        scene_context&       SceneContext() noexcept { return this->template get<scene_context>(); }
    };
    using scene_command       = scene_command_mixin<xundo::command_base>;
    using scene_query_command = scene_command_mixin<xundo::query_command_base>;

    // Shared by select_cmd/toggle_multi_select_cmd/clear_selection_cmd - all three snapshot/restore
    // the exact same fields. m_SelectedEntity (the live runtime handle) is deliberately NOT part of
    // the snapshot - it's a cache, always re-resolved fresh from {m_SelectedEntityScene,
    // m_SelectedEntityId} via the scene's own m_LocalToRuntime map on restore (through the context's
    // World()), matching this project's own
    // standing rule of never carrying a raw runtime handle across a boundary where the world could
    // have changed underneath it - an Undo/Redo step is exactly such a boundary, potentially long
    // after the entity in question was last touched.
    inline void BackupSelection(scene_context& Ctx, xundo::undo_file& File) noexcept
    {
        auto& S = Ctx.m_State;
        File.Write(S.m_SelectedEntityId);
        File.Write(S.m_SelectedEntityScene);
        File.Write(static_cast<std::uint32_t>(S.m_MultiSelectedEntityIds.size()));
        for (auto Id : S.m_MultiSelectedEntityIds) File.Write(Id);
        File.Write(static_cast<std::uint32_t>(S.m_MultiSelectOrder.size()));
        for (auto Id : S.m_MultiSelectOrder) File.Write(Id);
        File.Write(S.m_MultiSelectScene);
    }

    inline void RestoreSelection(scene_context& Ctx, xundo::undo_file& File) noexcept
    {
        auto& S = Ctx.m_State;

        File.Read(S.m_SelectedEntityId);
        File.Read(S.m_SelectedEntityScene);

        S.m_SelectedEntity = {};
        if (S.m_SelectedEntityId != xecs::scene::invalid_permanent_id_v)
        {
            if (auto* pScene = Ctx.World().m_SceneMgr.Find(S.m_SelectedEntityScene))
            {
                if (auto It = pScene->m_LocalToRuntime.find(S.m_SelectedEntityId); It != pScene->m_LocalToRuntime.end())
                    S.m_SelectedEntity = It->second;
            }
        }

        std::uint32_t Count = 0;
        File.Read(Count);
        S.m_MultiSelectedEntityIds.clear();
        for (std::uint32_t i = 0; i < Count; ++i)
        {
            xecs::scene::permanent_id Id{};
            File.Read(Id);
            S.m_MultiSelectedEntityIds.insert(Id);
        }

        File.Read(Count);
        S.m_MultiSelectOrder.clear();
        S.m_MultiSelectOrder.reserve(Count);
        for (std::uint32_t i = 0; i < Count; ++i)
        {
            xecs::scene::permanent_id Id{};
            File.Read(Id);
            S.m_MultiSelectOrder.push_back(Id);
        }

        File.Read(S.m_MultiSelectScene);
        S.m_bEntityInspectorDirty = true;
    }

    // Parses a scene guid formatted as 16 hex digits (see FormatSceneGuid, below) back into a
    // xecs::scene::guid - the two are always used as a pair, never guid <-> guid elsewhere in the
    // codebase, since this hex text form only exists for command-line argument round-tripping. Shared
    // by every command that needs to name a scene (selection, property edits, and whatever future
    // phases need it too) - not scoped to one command file.
    inline xecs::scene::guid ParseSceneGuid(std::string_view Text) noexcept
    {
        return xecs::scene::guid{ .m_Instance = { std::strtoull(std::string(Text).c_str(), nullptr, 16) } };
    }

    inline std::string FormatSceneGuid(xecs::scene::guid Guid) noexcept
    {
        return std::format("{:016X}", Guid.m_Instance.m_Value);
    }

    // Parses/formats an entity permanent_id as 8 hex digits - standardizes it to match every other
    // id/guid a command ever takes (Scene/Component/TypeGuid/Level/Folder are all hex already).
    // permanent_id used to be the one remaining decimal field (parsed via plain std::stoul) - direct
    // user report: "I think we need to standardize the way we do GUIDs.... I think they should always
    // be in hex." xecs::scene::permanent_id is a plain std::uint32_t (xecs_scene.h), so 8 hex digits
    // matches Folder/TypeGuid's own existing width exactly, not an arbitrary new choice.
    inline xecs::scene::permanent_id ParseEntityId(std::string_view Text) noexcept
    {
        return static_cast<xecs::scene::permanent_id>(std::strtoul(std::string(Text).c_str(), nullptr, 16));
    }

    inline std::string FormatEntityId(xecs::scene::permanent_id Id) noexcept
    {
        return std::format("{:08X}", Id);
    }

    // xproperty::settings::AnyToString (my_properties.h, shared xproperty lib) only knows the
    // project-wide atomic_types_tuple - it has no case for xecs::component::entity, which is
    // registered as ITS OWN var_type<> specialization inside xECS instead
    // (xecs_entity_xproperty_bridge.h), specifically because my_properties.h can't see xecs types
    // without a circular include. That's fine for every existing caller (SetLivePropertyValue etc.
    // never had to print one back out as text) - but any generic property-value-to-string walk
    // (DescribeEntity, SnapshotComponentProperties for Add/Remove Component undo) hits a live
    // component with an entity_reference (or any other entity-handle-valued property) and asserts
    // in AnyToString's `default: assert(false)` - confirmed live via a real crash: DescribeEntity on
    // an entity carrying an EntityReference component. Fixed by special-casing entity's own guid
    // here (the xECS consumer layer), matching the same layering the var_type<> bridge
    // itself already established, rather than teaching the shared xproperty lib about a type it's
    // architecturally not allowed to know about.
    inline bool bIsAnyToStringSafeType(std::uint32_t GUID) noexcept
    {
        return GUID == xproperty::settings::var_type<std::int32_t>::guid_v
            || GUID == xproperty::settings::var_type<std::uint32_t>::guid_v
            || GUID == xproperty::settings::var_type<std::int16_t>::guid_v
            || GUID == xproperty::settings::var_type<std::uint16_t>::guid_v
            || GUID == xproperty::settings::var_type<std::int8_t>::guid_v
            || GUID == xproperty::settings::var_type<std::uint8_t>::guid_v
            || GUID == xproperty::settings::var_type<float>::guid_v
            || GUID == xproperty::settings::var_type<double>::guid_v
            || GUID == xproperty::settings::var_type<std::string>::guid_v
            || GUID == xproperty::settings::var_type<std::wstring>::guid_v
            || GUID == xproperty::settings::var_type<std::uint64_t>::guid_v
            || GUID == xproperty::settings::var_type<std::int64_t>::guid_v
            || GUID == xproperty::settings::var_type<bool>::guid_v
            || GUID == xproperty::settings::var_type<xresource::full_guid>::guid_v
            ;
    }

    // AnyToString (my_properties.h, shared xproperty lib) asserts(false) on any type outside its own
    // fixed atomic list - by design, meant to catch a genuinely new atomic type nobody taught it to
    // print yet (xPropertyImGuiInspector.cpp's own bIsSnapshotableType guards its one internal caller
    // the same way, with the identical comment). FormatPropertyValue is xscene's only other caller and
    // had no such guard - confirmed live as a real Debug-build crash the moment a property whose value
    // isn't one of those atomic types (a compound/vector-valued leaf, an enum-backed virtual property,
    // etc.) reached here. Mirrors the existing xecs::component::entity special-case below: unknown
    // types now degrade to a placeholder instead of crashing the whole editor.
    inline int FormatPropertyValue(std::span<char> Buffer, const xproperty::any& Data) noexcept
    {
        if (Data.getTypeGuid() == xproperty::settings::var_type<xecs::component::entity>::guid_v)
        {
            const auto E = Data.get<xecs::component::entity>();
            if (!E.isValid()) return sprintf_s(Buffer.data(), Buffer.size(), "invalid");
            return sprintf_s(Buffer.data(), Buffer.size(), "runtime-entity %016llX", (unsigned long long)E.m_Value);
        }
        if (Data.isEnum())
        {
            const char* pName = Data.getEnumString();
            return pName ? sprintf_s(Buffer.data(), Buffer.size(), "%s", pName) : sprintf_s(Buffer.data(), Buffer.size(), "%u", Data.getEnumValue());
        }
        if (!bIsAnyToStringSafeType(Data.getTypeGuid()))
            return sprintf_s(Buffer.data(), Buffer.size(), "<unsupported>");
        return xproperty::settings::AnyToString(Buffer, Data);
    }

    // Direct user design (2026-09-29): moving a static entity's Transform while Playing must not
    // silently desync visuals (Render reads Transform directly, unaffected by static_tag) from physics
    // (statics are excluded from Physics's own per-frame scan - xlioncore_physics_system.h - so it
    // would never notice a live move). Demoting to Kinematic instead reuses the exact same
    // static/dynamics/kinematic resolution physics_body already has - no new mechanism, Physics's
    // regular scan just picks the entity back up next tick and pushes the moved pose into Box3D
    // correctly. Safe by construction: any edit made while Playing is discarded on Stop (StopPlay does
    // a full GameMgr rebuild + reload from disk - xlevel_session.h) unless explicitly kept via "Keep
    // Play Mode Changes" - this demotion reverts right along with everything else, matching Unity's
    // own "Play Mode changes don't persist" convention.
    //
    // Deliberately NOT xscene_commands_component_edit.h's MigrateEntityComponents (same shape, scene
    // map re-pointing included) - that file already includes THIS one, so calling it from here would
    // be circular. Small enough to inline directly rather than restructure the include graph for it.
    // Not undo-routed: an implicit side effect of an edit that's already its own undo step, not a
    // separate user action.
    inline void DemoteStaticIfPlaying(scene_context& Ed, xecs::scene::guid SceneGuid, xecs::scene::permanent_id Id) noexcept
    {
        if (!Ed.World().m_isRunning) return;

        auto* pScene = Ed.World().m_SceneMgr.Find(SceneGuid);
        if (!pScene) return;
        auto It = pScene->m_LocalToRuntime.find(Id);
        if (It == pScene->m_LocalToRuntime.end()) return;
        const auto OldEntity = It->second;

        // info_v<xlioncore::static_tag> is a per-BINARY compile-time singleton (xecs_component_type.h's
        // own comment) - this file compiles into xscene.plugin/xLION.exe, a DIFFERENT binary than
        // xLIONCore.dll, where static_tag is actually registered. Raw .m_BitID off this binary's own
        // copy is safe to read directly here (no per-call registry lookup needed) because
        // xlevel_session.h's RegisterHostSystems calls SyncLocalBitIDs<xlioncore::static_tag,
        // xlioncore::transform>() once, right after Lock, specifically so this binary's copies of
        // these two cross-referenced types stay correct - see that call site's own comment for the
        // full story (this used to resolve through findComponentTypeInfo every call instead, before
        // that root-cause fix landed).
        auto* pStaticTagInfo = &xecs::component::type::info_v<xlioncore::static_tag>;

        auto& Details = Ed.World().m_ComponentMgr.getEntityDetails(OldEntity);
        if (!Details.m_pPool) return;
        // Tags have no pool storage - findIndexComponentFromInfo is always -1 for one (see
        // xecs_tag_components_bits_only, the same lesson static_tag's own Inspector-chip and Add
        // Component checks already had to learn tonight). Presence is archetype bits only.
        if (!Details.m_pPool->m_pArchetype->getComponentBits().getBit(pStaticTagInfo->m_BitID)) return;

        std::array<const xecs::component::type::info*, 1> Sub{ pStaticTagInfo };
        const auto NewEntity = Ed.World().AddOrRemoveComponents(OldEntity, std::span<const xecs::component::type::info* const>{}, std::span<const xecs::component::type::info* const>{ Sub });

        pScene->m_RuntimeToLocal.erase(OldEntity.m_Value);
        pScene->m_LocalToRuntime[Id]                = NewEntity;
        pScene->m_RuntimeToLocal[NewEntity.m_Value] = Id;

        if (Ed.m_State.m_SelectedEntityId == Id && Ed.m_State.m_SelectedEntityScene == SceneGuid)
        {
            Ed.m_State.m_SelectedEntity        = NewEntity;
            Ed.m_State.m_bEntityInspectorDirty = true;
        }
    }

    // The dynamic-body counterpart to DemoteStaticIfPlaying above. Physics::OnUpdate deliberately
    // never looks at a dynamic body's Dirty flag (a live dynamic body is physics-authoritative - see
    // xlioncore_physics_system.h's own comment) - the editor is a privileged caller here, not
    // gameplay code: a gizmo drag or Inspector edit while Playing is a deliberate human action, so it
    // goes straight through xlioncore::physics::TeleportDynamicBody (ordinary cross-DLL call, not
    // GetProcAddress - see xlioncore_physics_api.h). No-ops via that function's own checks if the
    // entity has no live body or isn't currently Dynamic (static/kinematic already have their own
    // paths). Position/Rotation only, not Scale - resize is collider geometry, not pose, and isn't
    // handled for anyone yet (see xlion_construction_component_idea memory).
    inline void TeleportDynamicIfPlaying(scene_context& Ed, xecs::scene::guid SceneGuid, xecs::scene::permanent_id Id, const xmath::fvec3& Position, const xmath::fquat& Rotation) noexcept
    {
        if (!Ed.World().m_isRunning) return;

        auto* pScene = Ed.World().m_SceneMgr.Find(SceneGuid);
        if (!pScene) return;
        auto It = pScene->m_LocalToRuntime.find(Id);
        if (It == pScene->m_LocalToRuntime.end()) return;

        xlioncore::physics::TeleportDynamicBody(Ed.World(), It->second, Position, Rotation);
    }
}

