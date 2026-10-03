#ifndef XSCENE_PANEL_ENTITY_PROPERTIES_H
#define XSCENE_PANEL_ENTITY_PROPERTIES_H
#pragma once

// Extracted from xscene_entity_inspector_bridge.h (mechanical move, phase 1 of the kit split - see that
// file's own top comment). Meant to be included via the umbrella (xscene_entity_inspector_bridge.h) only,
// after entity_inspector_bridge and everything it depends on are already defined - not designed to
// be included standalone.
//
// Add/Remove Component commands (xscene_commands_component_edit.h, which pulls in
// xscene_commands_property_edit.h/xscene_command_context.h/xundo_system.h itself) included directly here -
// same self-sufficiency reasoning as the Level tree panel's own top comment for why.
#include "plugins/xscene.plugin/source/Editor/xscene_commands_component_edit.h"
#include "dependencies/xeditor/include/xeditor/hint.h"
#include "plugins/xscene.plugin/source/Editor/xscene_commands_apply_overrides.h"
#include "plugins/xscene.plugin/source/Editor/xscene_component_display.h"
#include "plugins/xscene.plugin/source/Editor/xscene_system_usage.h"
#include "dependencies/xeditor/include/xeditor/diagnostics.h"

namespace xscene
{
    //---------------------------------------------------------------------------
    // "Systems (N)" popup - what runs on this entity and what it touches, what doesn't run and why,
    // and what removing each component would change. Same data as DescribeEntity's pipe output.
    //---------------------------------------------------------------------------
    inline void RenderEntitySystemsPopupContents(xecs::game_mgr::instance& GameMgr, xecs::component::entity Entity) noexcept
    {
        namespace su = xscene::system_usage;
        const auto  Systems    = su::AllSystems(GameMgr);
        const auto  Bits       = su::SetOf(GameMgr, Entity);
        const auto  Components = UserComponents(xlioncore::Ecs(GameMgr), Entity);

        const ImVec4 WriteColor   = ImVec4(1.00f, 0.70f, 0.35f, 1.0f);
        const ImVec4 ReadColor    = ImVec4(0.55f, 0.75f, 1.00f, 1.0f);
        const ImVec4 WarnColor    = ImVec4(1.00f, 0.80f, 0.30f, 1.0f);

        auto AccessLine = [&](const su::system_ref& S, su::access A, const ImVec4& Color)
        {
            std::string Names;
            for (auto& E : S.m_pInfo->m_Access)
                if (E.m_Access == A && su::HasComponent(Bits, E.m_ComponentGuid.m_Value))
                    Names += std::format("{}{}", Names.empty() ? "" : ", ", E.m_pComponentName);
            if (Names.empty()) return;
            ImGui::TextColored(Color, "%-6s", su::AccessLabel(A));
            ImGui::SameLine();
            ImGui::TextWrapped("%s", Names.c_str());
        };

        ImGui::SeparatorText("Running on this entity");
        bool bAny = false;
        for (auto& S : Systems)
        {
            if (!su::Matches(S, Bits)) continue;
            bAny = true;
            ImGui::TextUnformatted(su::SystemName(S));
            if (ImGui::IsItemHovered())
            {
                const std::string Body = S.m_bUpdate ? std::format("Update system, runs #{} in the frame", S.m_Order) : std::string("Notifier system (runs on entity create/destroy/move events)");
                const std::string From = xscene::DescribeSource(xscene::SourceOfType(true, S.m_pInfo->m_Guid.m_Value));
                xeditor::hint::Draw({ .m_Topic = su::SystemName(S), .m_Body = Body, .m_Detail = From.empty() ? std::string_view() : std::string_view(From) });
            }
            if (!S.m_bEnabled) { ImGui::SameLine(); ImGui::TextColored(WarnColor, "(disabled in System Registry)"); }
            ImGui::Indent();
            AccessLine(S, su::access::WRITE, WriteColor);
            AccessLine(S, su::access::READ,  ReadColor);
            ImGui::Unindent();
        }
        if (!bAny) ImGui::TextDisabled("No system runs on this entity.");

        ImGui::SeparatorText("Not running");
        bAny = false;
        for (auto& S : Systems)
        {
            auto Why = su::WhyNotRunning(S, Bits);
            if (Why.empty()) continue;
            bAny = true;
            ImGui::TextDisabled("%s:", su::SystemName(S));
            ImGui::SameLine();
            ImGui::TextWrapped("%s", Why.c_str());
        }
        if (!bAny) ImGui::TextDisabled("Every system runs on this entity.");

        ImGui::SeparatorText("Removing a component would");
        bAny = false;
        for (auto* pInfo : Components)
        {
            const auto Change = su::WhatIf(Systems, Bits, *pInfo, false);
            if (Change.empty()) continue;
            bAny = true;
            ImGui::TextUnformatted(pInfo->m_pName ? pInfo->m_pName : "?");
            ImGui::SameLine();
            if (!Change.m_Stops.empty())  { ImGui::TextColored(WarnColor, "stop %s", su::JoinNames(Change.m_Stops).c_str()); if (!Change.m_Starts.empty()) ImGui::SameLine(); }
            if (!Change.m_Starts.empty())   ImGui::TextColored(ReadColor, "start %s", su::JoinNames(Change.m_Starts).c_str());
        }
        if (!bAny) ImGui::TextDisabled("No component removal changes which systems run.");

        ImGui::Separator();
        if (ImGui::Button("Copy as text"))
            ImGui::SetClipboardText(su::DescribeEntitySystems(GameMgr, Bits, Components).c_str());
        if (ImGui::IsItemHovered())
            xeditor::hint::Text("Same report as the DescribeEntity command - paste it into a bug report or an AI chat.");
    }

    //---------------------------------------------------------------------------
    // Entity Properties panel - the selected entity's components, plus Add/Remove Component and
    // (when applicable) prefab-override actions. Owns its own ImGui::Begin/End. Bridge carries the
    // inspector-to-override-tracking state (see entity_inspector_bridge's own comment) - construct
    // one alongside EntityInspector and call Bridge.RegisterCallbacks(...) once at setup before
    // calling this every frame.
    //---------------------------------------------------------------------------
    // bReadOnly: shown but not editable (no property edits, no Add/Remove Component); pReadOnlyReason, when given, says why.
    void RenderEntityPropertiesPanel(scene_context& Ed, const char* pWindowName, xproperty::inspector& EntityInspector, entity_inspector_bridge& Bridge, bool bReadOnly = false, const char* pReadOnlyReason = nullptr) noexcept
    {
        auto& GameMgr = Ed.World();
        auto& State   = Ed.m_State;
        ImGui::SetNextWindowPos(ImVec2(18, 18), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(480, 500), ImGuiCond_FirstUseEver);
        const bool bWindowVisible = ImGui::Begin(pWindowName);
        if (bWindowVisible && bReadOnly && pReadOnlyReason && *pReadOnlyReason)
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "%s", pReadOnlyReason);
        if (bWindowVisible && bReadOnly) ImGui::BeginDisabled();
        xeditor::diagnostics::Log("window begin: %s visible=%d", pWindowName, bWindowVisible ? 1 : 0);
        if (bWindowVisible)
        {
            if (State.m_SelectedEntity.isValid() == false || State.m_SelectedEntityScene.empty())
            {
                ImGui::TextDisabled("Select an entity in the Level Editor panel.");
            }
            else if (auto* pScene = GameMgr.m_SceneMgr.Find(State.m_SelectedEntityScene))
            {
                // Pointers (not references) so RefreshEntityView() below can rebind them after
                // AddOrRemoveComponents migrates State.m_SelectedEntity to a new handle - without this,
                // adding/removing a component and rebuilding the inspector in the SAME frame would
                // still walk the OLD archetype's DataSpan (captured before the migration), so the
                // just-added component silently wouldn't appear until some later, unrelated dirty flag
                // flip (e.g. reselecting the entity) rebuilt it with fresh data.
                // Asked of the xECSEditor of the world's copy of the core (not read from the pools): the component types of the selected entity, by kind. RefreshEntityView() asks again after
                // AddOrRemoveComponents migrates State.m_SelectedEntity to a new handle - without it, adding/removing a component and rebuilding the inspector in the SAME frame would still walk the
                // OLD archetype's components, so the just-added component silently wouldn't appear until some later, unrelated dirty flag flip rebuilt it with fresh data.
                auto& Ecs = xlioncore::Ecs(GameMgr);
                if (!Ecs.IsAlive(State.m_SelectedEntity))
                {
                    // Defensive hardening: an entity handle that is valid but has no pool anymore (a stale selection). IsAlive does not guard against the generation assert of getEntityDetails; the real fix
                    // is never reaching this call with a stale handle (DeleteSubtreeByPermanentId, xscene_commands_entity_lifecycle.h).
                    ImGui::TextDisabled("Select an entity in the Level Editor panel.");
                    ImGui::End();
                    return;
                }
                std::vector<const xecs::component::type::info*> DataSpan, ShareInfos, TagInfos;
                Ecs.ComponentTypesOf(State.m_SelectedEntity, DataSpan, ShareInfos, TagInfos);

                auto RefreshEntityView = [&]() noexcept
                {
                    Ecs.ComponentTypesOf(State.m_SelectedEntity, DataSpan, ShareInfos, TagInfos);
                };

                // Popup selector (grouped + searchable) â€” replaces the flat BeginCombo list.
                // OpenPopup/BeginPopup share this panel's ID stack (see Error popup comments in kit).
                {
                    constexpr const char* kAddComponentPopupId = "AddComponentPopup";
                    if (ImGui::Button("Add Component"))
                        ImGui::OpenPopup(kAddComponentPopupId);
                    // Drop a SharedComponentTemplate resource here to add+intern from serialized values.
                    if (xscene::TryAcceptSharedComponentTemplateDrop(Ed))
                        RefreshEntityView();

                    ImGui::SetNextWindowSize(ImVec2(320.0f, 360.0f), ImGuiCond_Appearing);
                    if (ImGui::BeginPopup(kAddComponentPopupId))
                    {
                        if (xscene::RenderComponentSelectorPopupContents(Ed, State.m_SelectedEntity))
                            RefreshEntityView();
                        ImGui::EndPopup();
                    }

                    // Which systems run on / modify this entity - the first stop when debugging it.
                    constexpr const char* kSystemsPopupId = "EntitySystemsPopup";
                    const auto  Systems = xscene::system_usage::AllSystems(GameMgr);
                    const auto  Bits    = xscene::system_usage::SetOf(GameMgr, State.m_SelectedEntity);
                    std::vector<xscene::system_usage::system_ref> Running;
                    for (auto& S : Systems) if (xscene::system_usage::Matches(S, Bits)) Running.push_back(S);

                    ImGui::SameLine();
                    if (ImGui::Button(std::format("Systems ({})###EntitySystems", Running.size()).c_str()))
                        ImGui::OpenPopup(kSystemsPopupId);
                    if (ImGui::IsItemHovered())
                        xeditor::hint::Text("Running on this entity: %s\nClick for what each one reads/writes, what isn't running and why,\nand what adding/removing a component would change.",
                                          Running.empty() ? "none" : xscene::system_usage::JoinNames(Running).c_str());

                    ImGui::SetNextWindowSize(ImVec2(460.0f, 0.0f), ImGuiCond_Appearing);
                    if (ImGui::BeginPopup(kSystemsPopupId))
                    {
                        RenderEntitySystemsPopupContents(GameMgr, State.m_SelectedEntity);
                        ImGui::EndPopup();
                    }
                }

                // Prefabs are created by dragging an entity from the Level Editor tree onto a folder
                // in the asset browser (see xscene::entity_to_prefab_drop) - Unity-style, no button.

// Unity-style Apply / Revert for instance overrides. Shown when the selection is
                // under a prefab instance that has property overrides and/or HierarchyDiffs.
                if (auto Ctx = xscene::FindContainingPrefabInstance(GameMgr, State.m_SelectedEntity); Ctx.m_pPI
                    && (!Ctx.m_pPI->m_lComponents.empty() || !Ctx.m_pPI->m_HierarchyDiffs.empty()))
                {
                    if (auto RootIt = pScene->m_RuntimeToLocal.find(Ctx.m_RootEntity.m_Value); RootIt != pScene->m_RuntimeToLocal.end())
                    {
                        const auto SceneHex = xscene::commands::FormatSceneGuid(State.m_SelectedEntityScene);
                        const auto RootHex  = xscene::commands::FormatEntityId(RootIt->second);

                        if (!Ctx.m_pPI->m_lComponents.empty() || !Ctx.m_pPI->m_HierarchyDiffs.empty())
                        {
                            if (ImGui::Button("Apply"))
                            {
                                xeditor::Run(Ed.m_Undo, std::format("ApplyOverrides -Scene {} -Id {}", SceneHex, RootHex));
                            }
                            // Tooltip (only show when hovering) â€” same format as Play transport buttons
                            if (ImGui::IsItemHovered())
                            {
                                xeditor::hint::PlaceAwayFromEdges(16.0f, ImVec2(380.0f, 220.0f)); ImGui::BeginTooltip();
                                ImGui::Text("Apply");
                                ImGui::TextDisabled("Push all overrides on this prefab instance into the Prefab asset");
                                ImGui::EndTooltip();
                            }
                        }
                        if (!Ctx.m_pPI->m_HierarchyDiffs.empty())
                        {
                            ImGui::SameLine();
                            if (ImGui::Button("Revert Hierarchy"))
                            {
                                xeditor::Run(Ed.m_Undo, std::format("RevertHierarchyOverrides -Scene {} -Id {}", SceneHex, RootHex));
                            }
                            if (ImGui::IsItemHovered())
                            {
                                xeditor::hint::PlaceAwayFromEdges(16.0f, ImVec2(380.0f, 220.0f)); ImGui::BeginTooltip();
                                ImGui::Text("Revert Hierarchy");
                                ImGui::TextDisabled("Restore removed children / drop added children; leave property overrides");
                                ImGui::EndTooltip();
                            }
                        }
                        if (!Ctx.m_pPI->m_lComponents.empty()
                            || !Ctx.m_pPI->m_ComponentDiffs.empty()
                            || !Ctx.m_pPI->m_HierarchyDiffs.empty())
                        {
                            ImGui::SameLine();
                            if (ImGui::Button("Revert All"))
                            {
                                xeditor::Run(Ed.m_Undo, std::format("RevertAllOverrides -Scene {} -Id {}", SceneHex, RootHex));
                            }
                            if (ImGui::IsItemHovered())
                            {
                                xeditor::hint::PlaceAwayFromEdges(16.0f, ImVec2(380.0f, 220.0f)); ImGui::BeginTooltip();
                                ImGui::Text("Revert All");
                                ImGui::TextDisabled("Re-sync from Prefab (keeps root Transform); clears all overrides");
                                ImGui::EndTooltip();
                            }
                        }
                    }
                }

                ImGui::Separator();

                // Category filter bar - direct user design: lives OUTSIDE the inspector (not grouped
                // headers inside the component list itself), defaults to "All" (no filter), and only
                // shows categories actually present on THIS entity's own attached components - "if
                // the entity does not have the category then we do not need to add that particular
                // category at the top". Categories come from xscene::g_ComponentDisplayInfo
                // (E29_GamePluginLoad.h), populated from whatever Script-Module components the
                // currently-loaded Game.dll generation self-registered with a category (built-in
                // engine components like Transform/Name never appear here, since they never go
                // through E29_REGISTER_COMPONENT - see that macro's own comment).
                {
                    std::vector<std::string> PresentCategories;
                    for (auto pInfo : DataSpan)
                    {
                        if (auto It = xscene::g_ComponentDisplayInfo.find(pInfo->m_pName); It != xscene::g_ComponentDisplayInfo.end() && !It->second.m_Category.empty())
                            if (std::find(PresentCategories.begin(), PresentCategories.end(), It->second.m_Category) == PresentCategories.end())
                                PresentCategories.push_back(It->second.m_Category);
                    }

                    if (!PresentCategories.empty())
                    {
                        std::sort(PresentCategories.begin(), PresentCategories.end());

                        auto FilterButton = [&](const std::string& Label, const std::string& Value) noexcept
                        {
                            const bool bSelected = (State.m_ComponentCategoryFilter == Value);
                            if (bSelected) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                            if (ImGui::SmallButton(Label.c_str()) && !bSelected)
                            {
                                State.m_ComponentCategoryFilter = Value;
                                State.m_bEntityInspectorDirty   = true;
                            }
                            if (bSelected) ImGui::PopStyleColor();
                            ImGui::SameLine();
                        };

                        FilterButton("All", "");
                        for (auto& Category : PresentCategories)
                            FilterButton(Category, Category);
                        ImGui::NewLine();
                        ImGui::Separator();
                    }
                }

                if (State.m_bEntityInspectorDirty)
                {
                    std::printf("[EntityDrag] Entity Properties inspector REBUILDING (m_bEntityInspectorDirty) for SelectedEntityId=%u\n", State.m_SelectedEntityId);
                    std::fflush(stdout);
                    EntityInspector.clear();
                    Bridge.m_ComponentMap.clear();
                    EntityInspector.AppendEntity();

                    // Filtered (per the category bar above, "" = All) + sorted by priority -
                    // uncategorized components (every built-in engine component) sort first, by
                    // construction, so Transform/Name stay pinned at the top exactly as they are
                    // today without needing to touch their own definitions.
                    std::vector<const xecs::component::type::info*> SortedComponents(DataSpan.begin(), DataSpan.end());
                    // SHARE components (pool family) - shown like data components; Save-as-template button is SHARE-only.
                    for (auto* pShareInfo : ShareInfos)
                        if (std::find(SortedComponents.begin(), SortedComponents.end(), pShareInfo) == SortedComponents.end())
                            SortedComponents.push_back(pShareInfo);
                    // TAG components (e.g. static_tag) are not data: the core lists them apart.
                    SortedComponents.insert(SortedComponents.end(), TagInfos.begin(), TagInfos.end());
                    std::erase_if(SortedComponents, [&](const xecs::component::type::info* pInfo) noexcept
                    {
                        if (State.m_ComponentCategoryFilter.empty()) return false;
                        auto It = xscene::g_ComponentDisplayInfo.find(pInfo->m_pName);
                        return It == xscene::g_ComponentDisplayInfo.end() || It->second.m_Category != State.m_ComponentCategoryFilter;
                    });
                    std::stable_sort(SortedComponents.begin(), SortedComponents.end(), [](const xecs::component::type::info* A, const xecs::component::type::info* B) noexcept
                    {
                        auto ItA = xscene::g_ComponentDisplayInfo.find(A->m_pName);
                        auto ItB = xscene::g_ComponentDisplayInfo.find(B->m_pName);
                        const bool bHasA = ItA != xscene::g_ComponentDisplayInfo.end();
                        const bool bHasB = ItB != xscene::g_ComponentDisplayInfo.end();
                        if (!bHasA && !bHasB) return false;
                        if (!bHasA) return true;
                        if (!bHasB) return false;
                        return ItA->second.m_Priority < ItB->second.m_Priority;
                    });

                    Bridge.m_TagComponents.clear();
                    for (auto pInfo : SortedComponents)
                    {
                        if (xscene::IsInternalComponent(pInfo)) continue;
                        if (pInfo->m_pPropertyTable == nullptr) continue;

                        // Genuine TAG kind only (not "zero reflected properties" - physics_body is DATA)
                        // gets the "[name][x]" chip. Checked before ResolveComponentPointer, which is
                        // always nullptr for a tag (no pool storage; presence came from archetype bits).
                        if (pInfo->m_TypeID == xecs::component::type::id::TAG)
                        {
                            Bridge.m_TagComponents.push_back(pInfo);
                            continue;
                        }

                        if (xscene::ResolveComponentPointer(GameMgr, State.m_SelectedEntity, *pInfo) == nullptr) continue; // not actually present (DATA or SHARE)

                        // pBase is a FAKE pointer (nullptr, never dereferenced) - pInfo is the stable
                        // identity carried as pUserData instead. The REAL pointer into pool memory is
                        // resolved fresh every frame by Bridge's m_OnGetComponentPointer (registered
                        // below in RegisterCallbacks), never cached here across frames - see that
                        // callback's own comment for why a raw pointer captured only at rebuild time
                        // (the previous design) goes stale the moment anything invalidates it without
                        // routing back through this dirty-flag rebuild first (an archetype
                        // migration elsewhere, or - the case that actually surfaced this - Phase 8's
                        // hot reload destroying and recreating the whole pool).
                        EntityInspector.AppendEntityComponent(*pInfo->m_pPropertyTable, nullptr, const_cast<xecs::component::type::info*>(pInfo));
                    }
                    State.m_bEntityInspectorDirty = false;
                }

                // Tag chip row - "[name][x]" pairs packed edge-to-edge on one line (SortedComponents'
                // own priority order, since m_TagComponents was built by filtering that same walk),
                // for every zero-property component collected above. Rendered once, outside the
                // dirty-rebuild block, so it still shows every frame between rebuilds.
                if (!Bridge.m_TagComponents.empty())
                {
                    bool bFirstChip = true;
                    for (auto* pInfo : Bridge.m_TagComponents)
                    {
                        if (!bFirstChip) ImGui::SameLine(0.0f, 4.0f);
                        bFirstChip = false;

                        ImGui::PushID(pInfo);
                        const char* pLabel = pInfo->m_pName ? pInfo->m_pName : "?";

                        // One shared drawn frame (background + border) spans both the name and the
                        // [x] - direct user follow-up on the first pass (two adjacent buttons):
                        // "it would be nice that the X is inside the header so it looks like a single
                        // connected piece... it needs to stand out... may be this can have a parent
                        // frame". Name and x are transparent hit-regions INSIDE that one frame (no
                        // separator, no per-half background at rest), not two separate boxed widgets.
                        const float  Height     = ImGui::GetFrameHeight();
                        const ImVec2 FramePad   = ImGui::GetStyle().FramePadding;
                        const float  NameWidth  = ImGui::CalcTextSize(pLabel).x + FramePad.x * 2.0f;
                        const float  XWidth     = ImGui::CalcTextSize("x").x    + FramePad.x * 2.0f;
                        const ImVec2 Min        = ImGui::GetCursorScreenPos();
                        const ImVec2 Max        = ImVec2(Min.x + NameWidth + XWidth, Min.y + Height);
                        const float  Rounding   = Height * 0.5f;
                        ImDrawList*  pDrawList  = ImGui::GetWindowDrawList();

                        pDrawList->AddRectFilled(Min, Max, ImGui::GetColorU32(ImVec4(0x3A / 255.0f, 0x3A / 255.0f, 0x50 / 255.0f, 1.0f)), Rounding);
                        pDrawList->AddRect(Min, Max, ImGui::GetColorU32(ImVec4(0x8F / 255.0f, 0x8F / 255.0f, 0xC8 / 255.0f, 1.0f)), Rounding, 0, 1.5f);

                        // Name region - tooltip only, no action.
                        ImGui::SetCursorScreenPos(Min);
                        ImGui::InvisibleButton("##name", ImVec2(NameWidth, Height));
                        if (ImGui::IsItemHovered())
                        {
                            const auto Used = xscene::system_usage::UsedBy(xscene::system_usage::AllSystems(GameMgr), pInfo->m_Guid.m_Value);
                            const std::string Body = Used.empty() ? std::string("Tag component - no properties.\nNot used by any system.") : "Tag component - no properties.\nUsed by: " + Used;
                            const std::string From = xscene::DescribeSource(xscene::SourceOfType(false, pInfo->m_Guid.m_Value));
                            xeditor::hint::Draw({ .m_Topic = pLabel, .m_Body = Body, .m_Detail = From.empty() ? std::string_view() : std::string_view(From) });
                        }
                        pDrawList->AddText(ImVec2(Min.x + FramePad.x, Min.y + FramePad.y), ImGui::GetColorU32(ImGuiCol_Text), pLabel);

                        // X region - same shared frame, its own hover highlight (right corners only)
                        // so it still reads as clickable (direct user design: "the X been more of a
                        // regular button"). Same removal path as a real component header's own [X]
                        // (entity_inspector_bridge::m_OnComponentHeaderRender), fired directly since
                        // this chip never goes through that generic per-component header at all.
                        const ImVec2 XMin = ImVec2(Min.x + NameWidth, Min.y);
                        ImGui::SetCursorScreenPos(XMin);
                        const bool bXClicked = ImGui::InvisibleButton("##x", ImVec2(XWidth, Height));
                        if (ImGui::IsItemHovered())
                        {
                            pDrawList->AddRectFilled(XMin, Max, ImGui::GetColorU32(ImVec4(0x55 / 255.0f, 0x55 / 255.0f, 0x75 / 255.0f, 1.0f)), Rounding, ImDrawFlags_RoundCornersRight);
                            xeditor::hint::Text("%s", xscene::system_usage::DescribeChange(xscene::system_usage::AllSystems(GameMgr), xscene::system_usage::SetOf(GameMgr, State.m_SelectedEntity), *pInfo, false).c_str());
                        }
                        const ImVec2 XTextSize = ImGui::CalcTextSize("x");
                        pDrawList->AddText(ImVec2(XMin.x + (XWidth - XTextSize.x) * 0.5f, Min.y + FramePad.y), ImGui::GetColorU32(ImGuiCol_Text), "x");
                        if (bXClicked && !xscene::IsInternalComponent(pInfo))
                            Bridge.m_pPendingRemoveComponent = pInfo;

                        // Re-register the WHOLE pill (both InvisibleButtons above only left the X
                        // half as ImGui's "last item") so the next chip's SameLine() packs against the
                        // full width, not just the X.
                        ImGui::SetCursorScreenPos(Min);
                        ImGui::Dummy(ImVec2(NameWidth + XWidth, Height));

                        ImGui::PopID();
                    }
                    ImGui::Spacing();
                }

                // Component headers use ImGuiCol_Header, which is ALSO the tree/list selection color
                // (E29_Theme.h's Unity-inspired blue) - fine for a hierarchy row, but real Unity's own
                // Inspector component headers are a neutral gray ("Inspector Titlebar", #3E3E3E), not
                // blue; blue is reserved for actual selection. Overridden locally, only around this
                // Show() call, so tree/list selection elsewhere stays the real selection blue.
                ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0x3E / 255.0f, 0x3E / 255.0f, 0x3E / 255.0f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0x4A / 255.0f, 0x4A / 255.0f, 0x4A / 255.0f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(0x4A / 255.0f, 0x4A / 255.0f, 0x4A / 255.0f, 1.0f));
                // Show() internally uses the legacy ImGui::Columns(2) (xPropertyImGuiInspector.cpp),
                // which draws a live, draggable ImGuiCol_Separator line down the whole label/value
                // split by default - a real border line, not a color mismatch, so the earlier Header
                // color fix alone couldn't remove it (direct user follow-up: "the gap still there").
                // Matched to WindowBg instead of touching the shared Columns() call itself (which
                // would also change every other example's own column-resize border) - resting state
                // blends into the flat row, but SeparatorHovered/Active are left as the theme's own
                // values so dragging the label/value split is still discoverable on hover.
                ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(0x38 / 255.0f, 0x38 / 255.0f, 0x38 / 255.0f, 1.0f));
                // REVERTED: pre-opening/closing our own Columns(2) here to seed a width for Show()'s
                // own internal Columns(2) call triggered a real ImGui "2 visible items with conflicting
                // ID" debug error (confirmed live) - not a safe pattern, back this out rather than
                // ship a broken column-ID state. The actual reported issue (a dark divider line
                // splitting the header's own background) is separate from column width entirely -
                // being investigated on its own, not re-attempting this approach.
                // ShowEmbedded (not Show(Context, Callback)) - that overload always opens its OWN
                // independent ImGui::Begin/End window using EntityInspector's own name ("Inspector"),
                // which, called from INSIDE this panel's already-open kInspectorWindow, created a
                // second, genuinely separate floating "Inspector" window instead of rendering the
                // properties into this one - the real bug behind "two windows both called Inspector".
                // ShowEmbedded draws directly into the current window, no Begin/End of its own.
                xproperty::settings::context Context;
                EntityInspector.ShowEmbedded(Context);
                ImGui::PopStyleColor(4);

                // A component header's "[X]" (entity_inspector_bridge::m_OnComponentHeaderRender) only
                // ever records the request while Show() is mid-iteration over this same component list
                // - now that it's returned, it's safe to actually mutate the archetype, same call the
                // "Remove Component" combo below makes for the same action.
                if (Bridge.m_pPendingRemoveComponent)
                {
                    // Routed through the command/undo system (the command/undo plan
                    // memory, phase 3 - xscene_commands_component_edit.h) - remove_component_cmd
                    // snapshots the component's current property values before removing it, so Undo
                    // can restore it exactly, not just re-add it with default values.
                    xeditor::Run(Ed.m_Undo, std::format("RemoveComponent -Scene {} -Id {} -Component {:016X}"
                        , xscene::commands::FormatSceneGuid(State.m_SelectedEntityScene)
                        , xscene::commands::FormatEntityId(State.m_SelectedEntityId)
                        , Bridge.m_pPendingRemoveComponent->m_Guid.m_Value
                        ));
                    Bridge.m_pPendingRemoveComponent = nullptr;
                    RefreshEntityView();
                }

            }
        }
                if (bWindowVisible && bReadOnly) ImGui::EndDisabled();
        ImGui::End();
        xeditor::diagnostics::Log("window end: %s", pWindowName);
    }
} // namespace xscene

#endif // XSCENE_PANEL_ENTITY_PROPERTIES_H
