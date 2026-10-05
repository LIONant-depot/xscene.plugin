#ifndef XSCENE_PANEL_COMPONENT_SELECTOR_H
#define XSCENE_PANEL_COMPONENT_SELECTOR_H
#pragma once

// Component "Add" popup contents for Entity Properties.
// Grouped by g_ComponentDisplayInfo category (E29_REGISTER_COMPONENT), collapsible
// (initially closed), with a top search bar matching Level Tree / Asset views. The popup itself is
// xeditor::RenderGroupedList (xeditor/grouped_list.h): this file only says what the items are.
//
// Meant to be included via the umbrella (xscene_entity_inspector_bridge.h) only, after
// scene_state, g_ComponentDisplayInfo, and command infrastructure are defined.
// Not designed to be included standalone. Not a docked editor window â€” Entity
// Properties opens this as an ImGui popup that replaces the old BeginCombo list.
#include "plugins/xscene.plugin/source/Editor/xscene_commands_component_edit.h"
#include "dependencies/xeditor/include/xeditor/hint.h"
#include "plugins/xscene.plugin/source/Editor/xscene_component_display.h"
#include "plugins/xscene.plugin/source/Editor/xscene_system_usage.h"
#include "dependencies/xeditor/include/xeditor/widgets.h"
#include "dependencies/xeditor/include/xeditor/grouped_list.h"
#include <cstring>

namespace xscene
{
    //---------------------------------------------------------------------------
    // Render inside an already-open ImGui popup (caller OpenPopup / BeginPopup).
    // Returns true if a component was added this frame.
    //---------------------------------------------------------------------------
    inline bool RenderComponentSelectorPopupContents(
        scene_context&         Ed,
        xecs::component::entity Entity) noexcept
    {
        auto& State = Ed.m_State;
        auto& Ecs   = xlioncore::Ecs(Ed.World());
        if (!Ecs.IsAlive(Entity))
        {
            ImGui::TextDisabled("Select an entity.");
            return false;
        }

        // Registry (not the entity DataSpan) - same source as the old BeginCombo list.
        const auto Bits    = xscene::system_usage::SetOf(Ed.World(), Entity);            // what the entity has already (every kind: data, share, tags)
        const auto Systems = xscene::system_usage::AllSystems(Ed.World());
        std::vector<const xecs::component::type::info*> Registered;
        Ecs.ListComponentTypes(Registered);

        std::vector<const xecs::component::type::info*> Infos;                       // Infos[i] is what Items[i] is
        std::vector<xeditor::grouped_list_item>         Items;
        Infos.reserve(Registered.size());
        Items.reserve(Registered.size());

        for (auto* pInfo : Registered)
        {
            // DATA + SHARE (V1 SharedComponentTemplate pipeline: share components are addable like
            // data) + TAG (real zero-storage markers like static_tag - excluding them here predates
            // any tag component existing at all; direct user report: "I do not see any component tag
            // in the list of components").
            if (pInfo->m_TypeID != xecs::component::type::id::DATA
                && pInfo->m_TypeID != xecs::component::type::id::SHARE
                && pInfo->m_TypeID != xecs::component::type::id::TAG) continue;
            if (xscene::IsInternalComponent(pInfo)) continue;
            if (Bits.Has(pInfo->m_Guid.m_Value)) continue;

            xeditor::grouped_list_item Item;
            Item.m_Name        = pInfo->m_pName ? pInfo->m_pName : "";
            Item.m_SearchExtra = Ed.Display().SourceOf(false, pInfo->m_Guid.m_Value).m_ModuleName;          // typing "Soccer" finds the components of the SoccerGame module
            if (auto It = Ed.Display().m_Categories.find(pInfo->m_pName); It != Ed.Display().m_Categories.end())
            {
                Item.m_Group    = It->second.m_Category;
                Item.m_Priority = It->second.m_Priority;
            }

            // The hint of the item: what adding it changes (the systems that start and stop), and where it comes from.
            Item.m_OnHover = [&Ed, &Systems, &Bits, pInfo]() noexcept
            {
                const std::string Change = xscene::system_usage::DescribeChange(Systems, Bits, *pInfo, true);
                const std::string From   = xscene::DescribeSource(Ed.Display().SourceOf(false, pInfo->m_Guid.m_Value));
                xeditor::hint::Draw({ .m_Topic = pInfo->m_pName ? pInfo->m_pName : "?", .m_Body = Change, .m_Detail = From.empty() ? std::string_view() : std::string_view(From) });
            };

            // At-a-glance hint, right-aligned: which systems this entity would gain/lose.
            if (const auto Change = xscene::system_usage::WhatIf(Systems, Bits, *pInfo, true); !Change.empty())
            {
                if (!Change.m_Starts.empty()) Item.m_RightText += "+" + xscene::system_usage::JoinNames(Change.m_Starts);
                if (!Change.m_Stops.empty())  Item.m_RightText += std::string(Item.m_RightText.empty() ? "" : "  ") + "-" + xscene::system_usage::JoinNames(Change.m_Stops);
            }

            Infos.push_back(pInfo);
            Items.push_back(std::move(Item));
        }

        // The search, the groups and the items are the one popup of the editors (xeditor/grouped_list.h), the same as the "+" of the resource view.
        const int Chosen = xeditor::RenderGroupedList(State.m_ComponentSelectorSearchString, State.m_ComponentSelectorCategoryOpen, Items
            , { .m_NoMatch = "No matching components.", .m_NoItems = "No components to add." });
        if (Chosen < 0) return false;

        xeditor::Run(Ed.m_Undo, std::format("AddComponent -Scene {} -Id {} -Component {:016X}"
            , xscene::commands::FormatSceneGuid(State.m_SelectedEntityScene)
            , xscene::commands::FormatEntityId(State.m_SelectedEntityId)
            , Infos[Chosen]->m_Guid.m_Value
        ));
        return true;
    }
} // namespace xscene

#endif // XSCENE_PANEL_COMPONENT_SELECTOR_H
