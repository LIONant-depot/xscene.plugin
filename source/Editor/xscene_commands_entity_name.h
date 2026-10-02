#ifndef XSCENE_COMMANDS_ENTITY_NAME_H
#define XSCENE_COMMANDS_ENTITY_NAME_H
#pragma once

// RenameEntity - gives an entity a display name (or clears it back to "Entity #id"). The name lives in the
// scene (xecs::scene::instance::m_EntityNames), is saved with it, and is what the Level Tree inline rename runs.

namespace xscene::commands
{
    struct rename_entity_cmd : scene_command
    {
        rename_entity_cmd(xundo::system& System, void* pDataBase) noexcept : scene_command(System, "RenameEntity", pDataBase) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override
        {
            return "Gives an entity a display name (undoable). Usage: RenameEntity -Scene hexguid -Id hexid -Name text | -Clear 1 (back to Entity #id)";
        }
        void RegisterArguments() noexcept override
        {
            m_hScene = m_Parser.addOption("Scene", "Scene guid, 16 hex digits",        true,  1);
            m_hId    = m_Parser.addOption("Id",    "Entity permanent_id, 8 hex digits", true,  1);
            m_hName  = m_Parser.addOption("Name",  "New name",          false, 1);
            m_hClear = m_Parser.addOption("Clear", "1 = remove the name",               false, 1);
        }

        std::string Redo() noexcept override
        {
            auto SceneArg = m_Parser.getOptionArgAs<std::string>(m_hScene, 0);
            auto IdArg    = m_Parser.getOptionArgAs<std::string>(m_hId, 0);
            if (std::holds_alternative<xerr>(SceneArg) || std::holds_alternative<xerr>(IdArg)) return "RenameEntity: bad arguments";

            auto* pScene = World().m_SceneMgr.Find(ParseSceneGuid(std::get<std::string>(SceneArg)));
            if (!pScene) return "RenameEntity: scene not open";
            const auto Id = ParseEntityId(std::get<std::string>(IdArg));
            if (!pScene->m_LocalToRuntime.contains(Id)) return "RenameEntity: entity not found in scene";

            auto NameArg  = m_Parser.getOptionArgAs<std::string>(m_hName, 0);
            auto ClearArg = m_Parser.getOptionArgAs<std::string>(m_hClear, 0);
            const bool bClear = !std::holds_alternative<xerr>(ClearArg) && std::get<std::string>(ClearArg) == "1";
            if (bClear) pScene->m_EntityNames.erase(Id);
            else
            {
                if (std::holds_alternative<xerr>(NameArg)) return "RenameEntity: needs -Name or -Clear 1";
                const std::string Name = std::get<std::string>(NameArg);
                if (Name.empty()) pScene->m_EntityNames.erase(Id);
                else              pScene->m_EntityNames[Id] = Name;
            }
            return {};
        }

        void BackupCurrenState(xundo::undo_file& File) noexcept override
        {
            auto SceneArg = m_Parser.getOptionArgAs<std::string>(m_hScene, 0);
            auto IdArg    = m_Parser.getOptionArgAs<std::string>(m_hId, 0);
            const auto SceneGuid = std::holds_alternative<xerr>(SceneArg) ? xecs::scene::guid{} : ParseSceneGuid(std::get<std::string>(SceneArg));
            const auto Id        = std::holds_alternative<xerr>(IdArg) ? xecs::scene::invalid_permanent_id_v : ParseEntityId(std::get<std::string>(IdArg));

            const std::string* pOld = nullptr;
            if (auto* pScene = World().m_SceneMgr.Find(SceneGuid); pScene) pOld = FindEntityName(*pScene, Id);

            File.Write(SceneGuid.m_Instance.m_Value);
            File.Write(static_cast<std::uint32_t>(Id));
            File.Write(static_cast<std::uint8_t>(pOld ? 1 : 0));
            xeditor::WriteString(File, pOld ? *pOld : std::string{});
        }

        void Undo(xundo::undo_file& File) noexcept override
        {
            std::uint64_t Scene = 0; File.Read(Scene);
            std::uint32_t Id    = 0; File.Read(Id);
            std::uint8_t  bHad  = 0; File.Read(bHad);
            const std::string Old = xeditor::ReadString(File);

            auto* pScene = World().m_SceneMgr.Find(xecs::scene::guid{ .m_Instance = { Scene } });
            if (!pScene) return;
            if (bHad) pScene->m_EntityNames[Id] = Old;
            else      pScene->m_EntityNames.erase(Id);
        }

        xcmdline::parser::handle m_hScene, m_hId, m_hName, m_hClear;
    };
}

#endif // XSCENE_COMMANDS_ENTITY_NAME_H
