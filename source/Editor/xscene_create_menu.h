#pragma once

// The 'create entity / folder' menu items shared by the Level tree's context menus.
// Split out of xscene_entity_inspector_bridge.h; included from there at the position this code used to occupy.
namespace xscene
{
    // Shared "New Entity"/"New Folder" menu content, landing directly under TargetFolder (invalid =
    // loose at scene root) - used by
    // BOTH the Scene row's and the Folder row's own right-click context menu. A separate toolbar "+"
    // with a persistent "which row is the target" selection was tried first and dropped per direct
    // user feedback once right-click-in-place existed - it made the "+" redundant.
    // Assumes it's called from inside an already-open popup (BeginPopupContextItem/BeginPopup).
    //
    // "New Entity" routed through the command/undo system (the command/undo plan,
    // phase 4 - xscene_commands_entity_lifecycle.h) - create_entity_cmd::Redo does the exact
    // migration this used to do inline, Undo deletes it again. "New Folder" now routed too (external
    // review flagged it as the one glaring inconsistency left next to CreateEntity/DeleteEntity sitting
    // right beside it in this same menu) - CreateFolder/DeleteFolder commands, commands/
    // xscene_commands_scene_organization.h. Moved here (was originally much earlier in this file) since
    // this routing needs the editor's context (E29_EditorState.h) and
    // xeditor::Run (just included above) - neither was available at the function's original
    // position.
    void ShowCreateMenuItems(scene_context& Ed, xecs::scene::guid SceneGuid, xecs::scene::instance& Scene, xecs::scene::folder_id TargetFolder) noexcept
    {
        if (ImGui::MenuItem("New Entity"))
        {
            const auto Id = NextFreeEntityId(Scene);
            xeditor::Run(Ed.m_Undo, std::format("CreateEntity -Scene {} -Id {} -Folder {:08X}"
                , xscene::commands::FormatSceneGuid(SceneGuid)
                , xscene::commands::FormatEntityId(Id)
                , static_cast<std::uint32_t>(TargetFolder)
                ));
        }
        if (ImGui::MenuItem("New Folder"))
        {
            const auto Id = NextFreeFolderId(Scene);
            xeditor::Run(Ed.m_Undo, std::format("CreateFolder -Scene {} -Id {:08X} -Parent {:08X} -Name {}"
                , xscene::commands::FormatSceneGuid(SceneGuid)
                , static_cast<std::uint32_t>(Id)
                , static_cast<std::uint32_t>(TargetFolder)
                , xeditor::Base64Encode("New Folder")
                ));
        }
    }
}
