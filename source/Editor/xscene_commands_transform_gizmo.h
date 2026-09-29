#ifndef XSCENE_COMMANDS_TRANSFORM_GIZMO_H
#define XSCENE_COMMANDS_TRANSFORM_GIZMO_H
#pragma once

// Gizmo-driven Transform edits (Translate/Rotate/Scale) - one committed command per gizmo drag
// (mouse-down to mouse-up, not per-frame). Dedicated commands rather than routing through
// set_property_cmd (xscene_commands_property_edit.h) because a gizmo result is a whole
// xmath::fvec3/fquat, and applying it correctly also means flipping xlioncore::transform's own
// m_DirtyToPhysics (MarkDirtyToPhysics) and, for rotation, keeping m_EditorRotation's cached Euler
// in sync with m_Rotation (the quaternion source of truth) - bookkeeping SetProperty's per-scalar
// path already handles for hand-typed Inspector edits, but that a whole-vector/whole-quaternion
// gizmo result doesn't decompose into cleanly.
//
// Before/After travel as a base64'd raw byte blob of the xmath type itself (PackBlob/UnpackBlob) -
// simpler than SetProperty's xproperty::any + AnyToString round-trip since the concrete type is
// already known here, and exact (no text/float round-trip loss).
#include "plugins/xscene.plugin/source/Editor/xscene_command_context.h"
#include "plugins/xscene.plugin/source/Editor/xscene_commands_property_edit.h"
#include "dependencies/xLIONCore/src/transform/xlioncore_transform.h"
#include <cstring>

namespace xscene::commands
{
    template<typename T>
    inline std::string PackBlob(const T& V) noexcept
    {
        return xeditor::Base64Encode(std::string(reinterpret_cast<const char*>(&V), sizeof(T)));
    }

    template<typename T>
    inline T UnpackBlob(const std::string& Text) noexcept
    {
        T Value{};
        const auto Raw = xeditor::Base64Decode(Text);
        if (Raw.size() == sizeof(T)) std::memcpy(&Value, Raw.data(), sizeof(T));
        return Value;
    }

    // Resolves the live xlioncore::transform* for {Scene,Id} - same pool-lookup ResolvePropertyTarget
    // already does for the generic property system, returning the typed pointer directly since these
    // commands write whole Position/Rotation/Scale values, not per-scalar xproperty::any.
    inline xlioncore::transform* ResolveTransform(scene_context& Ed, xecs::scene::guid SceneGuid, xecs::scene::permanent_id Id) noexcept
    {
        const auto Target = ResolvePropertyTarget(Ed, SceneGuid, Id, xlioncore::transform::typedef_v.m_Guid.m_Value);
        return Target.m_pInfo ? reinterpret_cast<xlioncore::transform*>(Target.m_pInstance) : nullptr;
    }

    // ponytail: prefab-property-override bookkeeping (RecordPropertyOverride/RemovePropertyOverride,
    // xscene_commands_property_edit.h) is not wired here - a gizmo edit on a prefab instance changes
    // the live value but won't show the Inspector's override tint or interact with Revert/Apply
    // Overrides yet. Add by recording Position.X/Y/Z (or RotationDegrees.X/Y/Z, Scale.X/Y/Z)
    // overrides the same way set_property_cmd does, if/when that parity matters.

    //================================================================================================
    // Translate - sets Transform.Position (undoable).
    //================================================================================================
    struct translate_cmd : scene_command
    {
        translate_cmd(xundo::system& System, void* pDataBase) noexcept : scene_command(System, "Translate", pDataBase) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override
        {
            return "Sets an entity's Transform.Position (undoable). Usage: Translate -Scene hexguid -Id hexid -Before base64 -After base64";
        }
        void RegisterArguments() noexcept override
        {
            m_hScene  = m_Parser.addOption("Scene",  "Scene guid, 16 hex digits",    true, 1);
            m_hId     = m_Parser.addOption("Id",     "Entity permanent_id, 8 hex digits", true, 1);
            m_hBefore = m_Parser.addOption("Before", "Previous position, base64",    true, 1);
            m_hAfter  = m_Parser.addOption("After",  "New position, base64",         true, 1);
        }

        std::string Redo() noexcept override
        {
            auto SceneArg = m_Parser.getOptionArgAs<std::string>(m_hScene, 0);
            auto IdArg    = m_Parser.getOptionArgAs<std::string>(m_hId, 0);
            auto AfterArg = m_Parser.getOptionArgAs<std::string>(m_hAfter, 0);
            if (std::holds_alternative<xerr>(SceneArg) || std::holds_alternative<xerr>(IdArg) || std::holds_alternative<xerr>(AfterArg))
                return "Translate: bad arguments";

            const auto SceneGuid = ParseSceneGuid(std::get<std::string>(SceneArg));
            const auto Id        = ParseEntityId(std::get<std::string>(IdArg));
            auto* pXform = ResolveTransform(SceneContext(), SceneGuid, Id);
            if (!pXform) return "Translate: target not found";

            pXform->m_Position = UnpackBlob<xmath::fvec3>(std::get<std::string>(AfterArg));
            pXform->MarkDirtyToPhysics();
            World().m_SceneMgr.MarkEntityDirty(SceneGuid, Id);
            State().m_bEntityInspectorDirty = true;
            DemoteStaticIfPlaying(SceneContext(), SceneGuid, Id);   // see its own comment, xscene_command_context.h
            TeleportDynamicIfPlaying(SceneContext(), SceneGuid, Id, pXform->m_Position, pXform->m_Rotation);
            return {};
        }

        void BackupCurrenState(xundo::undo_file& File) noexcept override
        {
            auto SceneArg  = m_Parser.getOptionArgAs<std::string>(m_hScene, 0);
            auto IdArg     = m_Parser.getOptionArgAs<std::string>(m_hId, 0);
            auto BeforeArg = m_Parser.getOptionArgAs<std::string>(m_hBefore, 0);

            File.Write(std::holds_alternative<xerr>(SceneArg) ? std::uint64_t{0} : std::strtoull(std::get<std::string>(SceneArg).c_str(), nullptr, 16));
            File.Write(std::holds_alternative<xerr>(IdArg)    ? std::uint32_t{0} : static_cast<std::uint32_t>(ParseEntityId(std::get<std::string>(IdArg))));
            File.Write(std::holds_alternative<xerr>(BeforeArg) ? xmath::fvec3::fromZero() : UnpackBlob<xmath::fvec3>(std::get<std::string>(BeforeArg)));
        }

        void Undo(xundo::undo_file& File) noexcept override
        {
            std::uint64_t Scene = 0; File.Read(Scene);
            std::uint32_t Id = 0;    File.Read(Id);
            xmath::fvec3  Before{};  File.Read(Before);

            const auto SceneGuid = xecs::scene::guid{ .m_Instance = { Scene } };
            auto* pXform = ResolveTransform(SceneContext(), SceneGuid, static_cast<xecs::scene::permanent_id>(Id));
            if (!pXform) return;

            pXform->m_Position = Before;
            pXform->MarkDirtyToPhysics();
            World().m_SceneMgr.MarkEntityDirty(SceneGuid, static_cast<xecs::scene::permanent_id>(Id));
            State().m_bEntityInspectorDirty = true;
        }

        xcmdline::parser::handle m_hScene, m_hId, m_hBefore, m_hAfter;
    };

    //================================================================================================
    // Rotate - sets Transform.Rotation (quaternion, undoable) and refreshes m_EditorRotation (the
    // cached Euler the Inspector's RotationDegrees fields read) via fquat::ToEuler() so the Inspector
    // doesn't show a stale angle after a gizmo-driven rotate.
    //================================================================================================
    struct rotate_cmd : scene_command
    {
        rotate_cmd(xundo::system& System, void* pDataBase) noexcept : scene_command(System, "Rotate", pDataBase) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override
        {
            return "Sets an entity's Transform.Rotation (undoable). Usage: Rotate -Scene hexguid -Id hexid -Before base64 -After base64";
        }
        void RegisterArguments() noexcept override
        {
            m_hScene  = m_Parser.addOption("Scene",  "Scene guid, 16 hex digits",    true, 1);
            m_hId     = m_Parser.addOption("Id",     "Entity permanent_id, 8 hex digits", true, 1);
            m_hBefore = m_Parser.addOption("Before", "Previous rotation, base64",    true, 1);
            m_hAfter  = m_Parser.addOption("After",  "New rotation, base64",         true, 1);
        }

        std::string Redo() noexcept override
        {
            auto SceneArg = m_Parser.getOptionArgAs<std::string>(m_hScene, 0);
            auto IdArg    = m_Parser.getOptionArgAs<std::string>(m_hId, 0);
            auto AfterArg = m_Parser.getOptionArgAs<std::string>(m_hAfter, 0);
            if (std::holds_alternative<xerr>(SceneArg) || std::holds_alternative<xerr>(IdArg) || std::holds_alternative<xerr>(AfterArg))
                return "Rotate: bad arguments";

            const auto SceneGuid = ParseSceneGuid(std::get<std::string>(SceneArg));
            const auto Id        = ParseEntityId(std::get<std::string>(IdArg));
            auto* pXform = ResolveTransform(SceneContext(), SceneGuid, Id);
            if (!pXform) return "Rotate: target not found";

            pXform->m_Rotation       = UnpackBlob<xmath::fquat>(std::get<std::string>(AfterArg));
            pXform->m_EditorRotation = pXform->m_Rotation.ToEuler();
            pXform->MarkDirtyToPhysics();
            World().m_SceneMgr.MarkEntityDirty(SceneGuid, Id);
            State().m_bEntityInspectorDirty = true;
            DemoteStaticIfPlaying(SceneContext(), SceneGuid, Id);   // see its own comment, xscene_command_context.h
            TeleportDynamicIfPlaying(SceneContext(), SceneGuid, Id, pXform->m_Position, pXform->m_Rotation);
            return {};
        }

        void BackupCurrenState(xundo::undo_file& File) noexcept override
        {
            auto SceneArg  = m_Parser.getOptionArgAs<std::string>(m_hScene, 0);
            auto IdArg     = m_Parser.getOptionArgAs<std::string>(m_hId, 0);
            auto BeforeArg = m_Parser.getOptionArgAs<std::string>(m_hBefore, 0);

            File.Write(std::holds_alternative<xerr>(SceneArg) ? std::uint64_t{0} : std::strtoull(std::get<std::string>(SceneArg).c_str(), nullptr, 16));
            File.Write(std::holds_alternative<xerr>(IdArg)    ? std::uint32_t{0} : static_cast<std::uint32_t>(ParseEntityId(std::get<std::string>(IdArg))));
            File.Write(std::holds_alternative<xerr>(BeforeArg) ? xmath::fquat::fromIdentity() : UnpackBlob<xmath::fquat>(std::get<std::string>(BeforeArg)));
        }

        void Undo(xundo::undo_file& File) noexcept override
        {
            std::uint64_t Scene = 0; File.Read(Scene);
            std::uint32_t Id = 0;    File.Read(Id);
            xmath::fquat  Before{};  File.Read(Before);

            const auto SceneGuid = xecs::scene::guid{ .m_Instance = { Scene } };
            auto* pXform = ResolveTransform(SceneContext(), SceneGuid, static_cast<xecs::scene::permanent_id>(Id));
            if (!pXform) return;

            pXform->m_Rotation       = Before;
            pXform->m_EditorRotation = Before.ToEuler();
            pXform->MarkDirtyToPhysics();
            World().m_SceneMgr.MarkEntityDirty(SceneGuid, static_cast<xecs::scene::permanent_id>(Id));
            State().m_bEntityInspectorDirty = true;
        }

        xcmdline::parser::handle m_hScene, m_hId, m_hBefore, m_hAfter;
    };

    //================================================================================================
    // Scale - sets Transform.Scale (undoable).
    //================================================================================================
    struct scale_cmd : scene_command
    {
        scale_cmd(xundo::system& System, void* pDataBase) noexcept : scene_command(System, "Scale", pDataBase) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override
        {
            return "Sets an entity's Transform.Scale (undoable). Usage: Scale -Scene hexguid -Id hexid -Before base64 -After base64";
        }
        void RegisterArguments() noexcept override
        {
            m_hScene  = m_Parser.addOption("Scene",  "Scene guid, 16 hex digits",    true, 1);
            m_hId     = m_Parser.addOption("Id",     "Entity permanent_id, 8 hex digits", true, 1);
            m_hBefore = m_Parser.addOption("Before", "Previous scale, base64",       true, 1);
            m_hAfter  = m_Parser.addOption("After",  "New scale, base64",            true, 1);
        }

        std::string Redo() noexcept override
        {
            auto SceneArg = m_Parser.getOptionArgAs<std::string>(m_hScene, 0);
            auto IdArg    = m_Parser.getOptionArgAs<std::string>(m_hId, 0);
            auto AfterArg = m_Parser.getOptionArgAs<std::string>(m_hAfter, 0);
            if (std::holds_alternative<xerr>(SceneArg) || std::holds_alternative<xerr>(IdArg) || std::holds_alternative<xerr>(AfterArg))
                return "Scale: bad arguments";

            const auto SceneGuid = ParseSceneGuid(std::get<std::string>(SceneArg));
            const auto Id        = ParseEntityId(std::get<std::string>(IdArg));
            auto* pXform = ResolveTransform(SceneContext(), SceneGuid, Id);
            if (!pXform) return "Scale: target not found";

            pXform->m_Scale = UnpackBlob<xmath::fvec3>(std::get<std::string>(AfterArg));
            pXform->MarkDirtyToPhysics();
            World().m_SceneMgr.MarkEntityDirty(SceneGuid, Id);
            State().m_bEntityInspectorDirty = true;
            DemoteStaticIfPlaying(SceneContext(), SceneGuid, Id);   // see its own comment, xscene_command_context.h
            return {};
        }

        void BackupCurrenState(xundo::undo_file& File) noexcept override
        {
            auto SceneArg  = m_Parser.getOptionArgAs<std::string>(m_hScene, 0);
            auto IdArg     = m_Parser.getOptionArgAs<std::string>(m_hId, 0);
            auto BeforeArg = m_Parser.getOptionArgAs<std::string>(m_hBefore, 0);

            File.Write(std::holds_alternative<xerr>(SceneArg) ? std::uint64_t{0} : std::strtoull(std::get<std::string>(SceneArg).c_str(), nullptr, 16));
            File.Write(std::holds_alternative<xerr>(IdArg)    ? std::uint32_t{0} : static_cast<std::uint32_t>(ParseEntityId(std::get<std::string>(IdArg))));
            File.Write(std::holds_alternative<xerr>(BeforeArg) ? xmath::fvec3::fromOne() : UnpackBlob<xmath::fvec3>(std::get<std::string>(BeforeArg)));
        }

        void Undo(xundo::undo_file& File) noexcept override
        {
            std::uint64_t Scene = 0; File.Read(Scene);
            std::uint32_t Id = 0;    File.Read(Id);
            xmath::fvec3  Before{};  File.Read(Before);

            const auto SceneGuid = xecs::scene::guid{ .m_Instance = { Scene } };
            auto* pXform = ResolveTransform(SceneContext(), SceneGuid, static_cast<xecs::scene::permanent_id>(Id));
            if (!pXform) return;

            pXform->m_Scale = Before;
            pXform->MarkDirtyToPhysics();
            World().m_SceneMgr.MarkEntityDirty(SceneGuid, static_cast<xecs::scene::permanent_id>(Id));
            State().m_bEntityInspectorDirty = true;
        }

        xcmdline::parser::handle m_hScene, m_hId, m_hBefore, m_hAfter;
    };
}

#endif // XSCENE_COMMANDS_TRANSFORM_GIZMO_H
