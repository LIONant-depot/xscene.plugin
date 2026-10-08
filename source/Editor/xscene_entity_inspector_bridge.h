#pragma once
#include "plugins/xscene.plugin/source/Editor/xscene_system_usage.h"
#include "dependencies/xeditor/include/xeditor/hint.h"

// entity_inspector_bridge: inspector callbacks -> prefab-override and entity-reference commands.
// Split out of xscene_entity_inspector_bridge.h; included from there at the position this code used to occupy.
namespace xscene
{
    //---------------------------------------------------------------------------
    // Entity Properties inspector wiring - bundles the prefab-override tracking/revert bookkeeping
    // and the entity_reference drag-drop-assign rendering that any xECS editor built on this kit
    // needs the moment it lets a component hold a raw xecs::component::entity field. Constructed once
    // alongside the owning example's own xproperty::inspector; RegisterCallbacks(...) wires all five
    // delegates in one call.
    //
    // Callbacks are stored as std::function MEMBERS (not locals inside RegisterCallbacks) specifically
    // so they outlive that one setup call: xdelegate::Register(T_CLASS&) binds to the callable object
    // BY REFERENCE, so whatever it's given must live as long as the inspector keeps calling it - a
    // local lambda inside RegisterCallbacks would be destroyed the moment that function returns (the
    // same "must be a named local that outlives the registration, not a temporary passed straight into
    // Register(...)" pitfall this codebase's own established convention already warns about elsewhere).
    // A std::function member is a fixed, nameable type a struct CAN hold (unlike the anonymous type of
    // a raw lambda), and since Register binds to ITS address, that address stays valid for exactly as
    // long as this bridge object does.
    //---------------------------------------------------------------------------
    struct entity_inspector_bridge
    {
        // Inspector-to-override pipeline: the inspector's callbacks only give us (type::object&, void*
        // pInstance) - m_ComponentMap closes the gap back to "which xECS component type is this",
        // rebuilt every time the inspector's content is (see RenderEntityPropertiesPanel's own
        // m_bEntityInspectorDirty block). m_bSuppressOverrideTracking guards the one re-entrancy risk:
        // the revert callback's own BeginEdit/CommitEdit bracket (used to write the prefab's base value
        // back into the instance) fires m_OnChangeEvent itself once committed - without the guard, a
        // revert would immediately re-record the very override it just removed.
        std::unordered_map<void*, const xecs::component::type::info*> m_ComponentMap;

        // What the Inspector is given for a SHARE component instead of the shared instance itself (see m_OnGetComponentPointer).
        std::unordered_map<const xecs::component::type::info*, std::vector<std::byte>> m_ShareScratch;
        bool                                                           m_bSuppressOverrideTracking = false;

        // Set by the component-header callback when its "[X]" is clicked - processed once, right
        // after the owning inspector's Show(...) returns for the frame, rather than mutating the
        // entity's archetype WHILE the inspector is still mid-iteration over this same entity's
        // component list.
        const xecs::component::type::info* m_pPendingRemoveComponent = nullptr;

        // Zero-property components (real xecs tags, and any DATA component with an empty
        // XPROPERTY_DEF like physics_body) rendered as a compact chip row instead of going through
        // EntityInspector's own per-component foldout - rebuilt in the same m_bEntityInspectorDirty
        // block as SortedComponents (RenderEntityPropertiesPanel), read every frame to draw the row.
        std::vector<const xecs::component::type::info*> m_TagComponents;

        std::function<void(xproperty::inspector&, const xproperty::ui::undo::cmd&)>                                                      m_OnPropertyChanged;
        std::function<void(xproperty::inspector&, const xproperty::type::object&, void*, std::string_view, const xproperty::any&, bool&)> m_OnOverrideCheck;
        std::function<void(xproperty::inspector&, const xproperty::type::object&, void*, std::string_view)>                              m_OnOverrideReset;
        std::function<void(xproperty::inspector&, const xproperty::type::object&, void*)>                                                m_OnComponentHeaderRender;
        std::function<void(xproperty::inspector&, const xproperty::type::object&, void*, ImVec4&, bool&)>                                m_OnComponentHeaderColor;

        // The components of the selected entity that differ from its prefab (set by the Entity Properties from its Prefab Overrides report, every frame): the header of an ADDED component
        // is tinted grey-blue, a MODIFIED one gets a thin blue mark at its left. A removed component has no header (it is only in the popup).
        // The Prefab Overrides report of the instance the selection belongs to, kept between frames (built again when the selection changes, after a command of the popup, and twice a second: it walks the instance)
        prefab_override_report            m_Report;
        double                            m_ReportTime     = -1.0;
        xecs::scene::guid                 m_ReportScene;
        xecs::scene::permanent_id         m_ReportAsked    = xecs::scene::invalid_permanent_id_v;
        bool                              m_bAskRevertAll  = false;          // "Revert All" was pressed in the popup: the confirmation opens (outside the popup, which closes)
        std::string                       m_RevertAllCommand;                // what the confirmation runs, and what it says
        std::string                       m_RevertAllText;
        std::unordered_set<std::uint64_t> m_AddedComponents;
        std::unordered_set<std::uint64_t> m_ModifiedComponents;
        static inline const ImVec4 kAddedHeaderColor = ImVec4(0.30f, 0.40f, 0.52f, 1.0f);     // grey-blue, one colour for every theme: the instance's own components stand out from the prefab's
        static constexpr ImU32 kModifiedMarkColor = IM_COL32(70, 150, 255, 255);
        std::function<void(xproperty::inspector&, const xproperty::type::object&, void*, std::string_view, const xproperty::any&, bool&)> m_OnEntityReferenceRender;

        // The "fake pointer, resolved by callback" indirection xproperty's own inspector expects
        // for any component instance whose address isn't a stable, owning member (see
        // E10_TextureResourcePipeline.cpp's identical use for the same reason - a selected asset's
        // descriptor can be reloaded/relocated out from under the inspector). xECS component-pool
        // addresses are exactly this case: an archetype migration, or a full world destroy/recreate
        // (Phase 8's hot reload) invalidates them independent of anything on the inspector's own
        // side. Registered in RegisterCallbacks; re-derives the CURRENT real pointer fresh every
        // time xproperty::inspector::Show() calls it (twice a frame - see xPropertyImGuiInspector's
        // own m_OnGetComponentPointer comment) rather than trusting anything cached from a prior
        // frame. Also the only place m_ComponentMap gets written now (previously written once, at
        // rebuild time, keyed by the same pointer that's now fake) - keyed by the freshly-resolved
        // real pointer, matching what m_OnOverrideCheck/m_OnPropertyChanged/etc. actually receive
        // from xproperty this same frame.
        std::function<void(xproperty::inspector&, const int, void*&, void*)> m_OnGetComponentPointer;

        void RegisterCallbacks(xproperty::inspector& Inspector, scene_context& Ed) noexcept
        {
            // xdelegate::Register(...) unconditionally push_back's - it has no dedup and no
            // Unregister at all (confirmed reading dependencies/xdelegate/source/xdelegate.h
            // directly). This method is called MORE than once on the SAME Inspector across this
            // session's lifetime (once at startup, again after Phase 8's ReloadGame recreates
            // GameMgr) - without clearing first, every callback below silently accumulates a
            // second, third, ... registration and fires that many times per event, which is
            // exactly the "properties/rows rendering doubled" bug a reload produced (confirmed live
            // - stacked "X"/duplicate rows in the Entity Properties panel after one reload).
            // E10_TextureResourcePipeline.cpp already established this exact idiom
            // (`Inspectors[0].m_OnGetComponentPointer.m_Delegates.clear();` before its own
            // re-Register) for the same reason - mirrored here for all six delegates this bridge
            // owns, not just the one xresource_editor happened to need it for.
            Inspector.m_OnChangeEvent.m_Delegates.clear();
            Inspector.m_OnOverrideCheck.m_Delegates.clear();
            Inspector.m_OnOverrideReset.m_Delegates.clear();
            Inspector.m_OnComponentHeaderRender.m_Delegates.clear();
            Inspector.m_OnComponentHeaderColor.m_Delegates.clear();
            Inspector.m_OnCustomRenderReplaceValue.m_Delegates.clear();
            Inspector.m_OnGetComponentPointer.m_Delegates.clear();

            // xdelegate::Register(T_CLASS&) binds to the lambda OBJECT itself (by reference) rather
            // than copying/erasing it into a std::function - so each callback must be a named local
            // that outlives the registration; here that "local" is the std::function MEMBER itself
            // (see this struct's own comment), assigned below and then registered.
            // Routed through the command/undo system (the command/undo plan, phase
            // 2 - xscene_commands_property_edit.h) instead of applying the value/recording the
            // override directly here - this is the ORDINARY per-row commit path (Cmd.m_Name is a real
            // property path, Cmd.m_NewValue/m_Original real scalar values), never the whole-component
            // BeginEdit/CommitEdit snapshot bracket the Revert Override action below uses (that one's
            // own m_OnChangeEvent notification carries a bracket label and a multi-line blob instead -
            // m_bSuppressOverrideTracking is what keeps THIS callback from misinterpreting that case).
            // set_property_cmd's own Redo()/Undo() do what this callback used to do inline (mark
            // dirty, FindOrCreateOverrideEntry) - the difference, and the whole point of this move, is
            // that Undo() now runs that SAME logic with the BEFORE value, so undoing an edit correctly
            // reverts the override bookkeeping too, not just the live property (direct user caution:
            // "careful with resetting the overrides").
            m_OnPropertyChanged = [this, &Ed](xproperty::inspector&, const xproperty::ui::undo::cmd& Cmd)
            {
                if (m_bSuppressOverrideTracking) return;

                auto It = m_ComponentMap.find(Cmd.m_pClassObject);
                if (It == m_ComponentMap.end()) return;
                auto& State = Ed.m_State;

                // An edit bracket (array insert / delete / reorder): m_Name is a label and both values
                // are whole-component snapshots, not one property's scalar - replay them as a snapshot.
                // (Formatting a snapshot as a scalar below overflowed the 256-byte buffers and aborted.)
                if (Cmd.m_NewValue.is<std::string>() && Cmd.m_Original.is<std::string>() && !Cmd.m_Name.empty() && Cmd.m_Name.find('/') == std::string::npos)
                {
                    xeditor::Run(Ed.m_Undo, std::format("SnapshotEdit -Scene {} -Id {} -Component {:016X} -Label {} -Before {} -After {}"
                        , xscene::commands::FormatSceneGuid(State.m_SelectedEntityScene)
                        , xscene::commands::FormatEntityId(State.m_SelectedEntityId)
                        , It->second->m_Guid.m_Value
                        , xeditor::Quote(Cmd.m_Name)
                        , xeditor::Quote(Cmd.m_Original.get<std::string>())
                        , xeditor::Quote(Cmd.m_NewValue.get<std::string>())));
                    return;
                }

                std::array<char, 256> BeforeBuffer{}, AfterBuffer{};
                const auto BeforeLen = xscene::commands::FormatPropertyValue(BeforeBuffer, Cmd.m_Original);
                const auto AfterLen  = xscene::commands::FormatPropertyValue(AfterBuffer, Cmd.m_NewValue);
                const std::string Before(BeforeBuffer.data(), BeforeLen > 0 ? static_cast<std::size_t>(BeforeLen) : 0);
                const std::string After(AfterBuffer.data(), AfterLen > 0 ? static_cast<std::size_t>(AfterLen) : 0);
                const std::uint32_t TypeGuid = Cmd.m_NewValue.m_pType ? Cmd.m_NewValue.m_pType->m_GUID : 0;

                xeditor::Run(Ed.m_Undo, std::format("SetProperty -Scene {} -Id {} -Component {:016X} -Path {} -TypeGuid {:08X} -Before {} -After {}"
                    , xscene::commands::FormatSceneGuid(State.m_SelectedEntityScene)
                    , xscene::commands::FormatEntityId(State.m_SelectedEntityId)
                    , It->second->m_Guid.m_Value
                    , xeditor::Quote(Cmd.m_Name)
                    , TypeGuid
                    , xeditor::Quote(Before)
                    , xeditor::Quote(After)
                    ));
            };
            Inspector.m_OnChangeEvent.Register(m_OnPropertyChanged);

            m_OnOverrideCheck = [this, &Ed](xproperty::inspector&, const xproperty::type::object&, void* pInstance, std::string_view Path, const xproperty::any&, bool& bOut)
            {
                auto& GameMgr = Ed.World();
                auto& State   = Ed.m_State;
                bOut = false;

                auto It = m_ComponentMap.find(pInstance);
                if (It == m_ComponentMap.end()) return;

                auto Ctx = xscene::FindContainingPrefabInstance(GameMgr, State.m_SelectedEntity);
                if (Ctx.m_pPI == nullptr) return;
                // A component the instance added has no value in the prefab to go back to: its data is the instance's own (the recipe keeps it as overrides of every property so that it is saved), not a difference
                // from the prefab, and a marker there would be one that cannot be reverted (the header's colour and the popup's "Added" row say it; removing the component is its revert).
                if (m_AddedComponents.contains(It->second->m_Guid.m_Value)) return;

                for (auto& C : Ctx.m_pPI->m_lComponents)
                {
                    if (C.m_ComponentTypeGuid != It->second->m_Guid.m_Value) continue;
                    if (std::ranges::equal(C.m_Member, Ctx.m_Member) == false) continue;
                    for (auto& O : C.m_PropertyOverrides)
                        if (O.m_PropertyName == Path) { bOut = true; return; }
                }
            };
            Inspector.m_OnOverrideCheck.Register(m_OnOverrideCheck);

            // Routed through RevertOverride (xscene_commands_property_edit.h) instead of the
            // old inline BeginEdit/setProperty/erase_if path - same live+bookkeeping result, but
            // Ctrl+Z restores the overridden value and re-records the override entry.
            m_OnOverrideReset = [this, &Ed](xproperty::inspector& /*Inspector*/, const xproperty::type::object& Obj, void* pInstance, std::string_view Path)
            {
                auto& GameMgr = Ed.World();
                auto& State   = Ed.m_State;
                auto It = m_ComponentMap.find(pInstance);
                if (It == m_ComponentMap.end()) return;

                auto Ctx = xscene::FindContainingPrefabInstance(GameMgr, State.m_SelectedEntity);
                if (Ctx.m_pPI == nullptr) return;

                if (auto Err = xlioncore::Ecs(GameMgr).EnsureLoadedPrefab(Ctx.m_pPI->m_PrefabInstance); Err)
                {
                    xeditor::NotifyToast(std::format("Failed to load source prefab for revert: {}", Err.getMessage()));
                    return;
                }

                // The prefab's member at the same address (its id in the prefab: see xecs::editor::member_address) as an instance of the prefab starts from it: with the recipes of the prefab's nested
                // instances applied (the inner prefab's own template member would give the value without them).
                const auto BakedEntity    = xlioncore::Ecs(GameMgr).ResolveBakedPrefabMember(Ctx.m_pPI->m_PrefabInstance, Ctx.m_Member);
                const auto TemplateEntity = xlioncore::Ecs(GameMgr).ResolvePrefabMember(Ctx.m_pPI->m_PrefabInstance, Ctx.m_Member);
                if (BakedEntity.isValid() == false && TemplateEntity.isValid() == false) return;

                xproperty::settings::context Context;
                xproperty::any               BaseValue;
                xproperty::any               CurrentValue;
                bool                         bFoundBase = false;
                bool                         bFoundCurrent = false;

                // DATA components live in the entity's own pool row, SHARE components on the family's share
                // entity - findIndexComponentFromInfo is -1 for a SHARE one, which used to make this return
                // silently and left every overridden property of a shared component impossible to revert.
                const auto ReadBase = [&](xecs::component::entity Entity) noexcept
                {
                    bFoundBase = false;
                    if (Entity.isValid() == false) return;
                    void* pRootData = xscene::ResolveComponentPointer(GameMgr, Entity, *It->second);
                    if (pRootData == nullptr) return;
                    xproperty::sprop::collector(pRootData, Obj, Context, [&](const char* pPropertyName, xproperty::any&& Data, const xproperty::type::members&, bool, const void*) noexcept
                    {
                        if (Path == pPropertyName) { BaseValue = std::move(Data); bFoundBase = true; }
                    });
                };
                // The baked member has the recipes of the nested instances applied, but its references are null (a plan holds none): a reference property, and a component the baked member
                // does not have, are read from the template member instead.
                ReadBase(BakedEntity);
                if (bFoundBase == false || BaseValue.getTypeGuid() == xproperty::settings::var_type<xecs::component::entity>::guid_v) ReadBase(TemplateEntity);
                xproperty::sprop::collector(pInstance, Obj, Context, [&](const char* pPropertyName, xproperty::any&& Data, const xproperty::type::members&, bool, const void*) noexcept
                {
                    if (Path == pPropertyName) { CurrentValue = std::move(Data); bFoundCurrent = true; }
                });
                if (bFoundBase == false || bFoundCurrent == false)
                {
                    xeditor::NotifyToast(std::format("{} has no value in the prefab to go back to (the component is not the prefab's, or the property is not saved with it).", std::string(Path)));
                    return;
                }

                std::array<char, 256> BeforeBuffer{}, AfterBuffer{};
                const auto BeforeLen = xscene::commands::FormatPropertyValue(BeforeBuffer, CurrentValue);
                const auto AfterLen  = xscene::commands::FormatPropertyValue(AfterBuffer, BaseValue);
                const std::string Before(BeforeBuffer.data(), BeforeLen > 0 ? static_cast<std::size_t>(BeforeLen) : 0);
                const std::string After(AfterBuffer.data(), AfterLen > 0 ? static_cast<std::size_t>(AfterLen) : 0);
                const std::uint32_t TypeGuid = BaseValue.m_pType ? BaseValue.m_pType->m_GUID
                    : (CurrentValue.m_pType ? CurrentValue.m_pType->m_GUID : 0);

                xeditor::Run(Ed.m_Undo, std::format("RevertOverride -Scene {} -Id {} -Component {:016X} -Path {} -TypeGuid {:08X} -Before {} -After {}"
                    , xscene::commands::FormatSceneGuid(State.m_SelectedEntityScene)
                    , xscene::commands::FormatEntityId(State.m_SelectedEntityId)
                    , It->second->m_Guid.m_Value
                    , xeditor::Quote(std::string(Path))
                    , TypeGuid
                    , xeditor::Quote(Before)
                    , xeditor::Quote(After)
                    ));
            };
            Inspector.m_OnOverrideReset.Register(m_OnOverrideReset);

            // "[X]" on a component's own header row - resolves back to which xECS component this is via
            // the SAME m_ComponentMap the property callbacks above already use, so it stays in sync
            // with whatever's currently appended. Excludes the same components the "Remove Component"
            // combo already excludes (internal bookkeeping components, and Name - every entity stays
            // nameable) - one shared exclusion list, not two independently maintained ones. Only
            // records the request (m_pPendingRemoveComponent); the actual AddOrRemoveComponents call
            // happens after the inspector's Show(...) returns for this frame.
            m_OnComponentHeaderRender = [this, &Ed](xproperty::inspector&, const xproperty::type::object&, void* pInstance)
            {
                auto It = m_ComponentMap.find(pInstance);
                if (It == m_ComponentMap.end()) return;
                auto* pInfo = It->second;
                // A component whose properties differ from the prefab's: a thin blue mark at the left edge of its header (the added ones have their own colour)
                if (!m_ModifiedComponents.empty() && m_ModifiedComponents.contains(pInfo->m_Guid.m_Value))
                {
                    const ImVec2 Pos = ImGui::GetCursorScreenPos();
                    const float  X   = ImGui::GetWindowPos().x + 1.0f;
                    auto* pDraw = ImGui::GetWindowDrawList();                      // the header is in the second column, whose clip rectangle would cut the mark at the window's edge
                    pDraw->PushClipRectFullScreen();
                    pDraw->AddRectFilled(ImVec2(X, Pos.y), ImVec2(X + 3.0f, Pos.y + ImGui::GetFrameHeight()), kModifiedMarkColor);
                    pDraw->PopClipRect();
                }
                if (xscene::IsInternalComponent(pInfo)) return;
                // Name is a regular, removable component like any other now - an entity with none of
                // its own components at all (not even Name) is a legitimate state.

                // NOT ImGui::SameLine() here - SameLine(x) positions using CursorPosPrevLine.y (the Y
                // of whichever line a real widget last finished on), not the actual current cursor.
                // Nothing real draws between NextColumn() and this callback firing, so for every
                // component AFTER the first, CursorPosPrevLine.y is still stale from the PREVIOUS
                // component's last property row - visible live as this button rendering on top of
                // whatever row happened to be last, not its own header. GetCursorScreenPos() (the
                // real, current cursor - already correctly placed by the caller right before this
                // fires) has no such staleness, so compute the absolute position from that instead.
                const ImVec2 RowPos = ImGui::GetCursorScreenPos();
                const float  AvailW = ImGui::GetContentRegionAvail().x;
                // SHARE: the right-side label identifies the component while [S] remains a
                // drag source into Resource View (creates SharedComponentTemplate).
                // Delete [X] stays a click button immediately to its right.
                const bool bIsShare = (pInfo->m_TypeID == xecs::component::type::id::SHARE);
                // Keep the SHARE marker flush with the value-column divider, while the
                // action buttons remain right-aligned in that same column.
                const float RightEdgePad = 5.0f;
                const float ButtonGap = 4.0f;
                const float SmallButtonWidth = ImGui::CalcTextSize("S").x
                                             + ImGui::GetStyle().FramePadding.x * 2.0f;
                const float ShareButtonsWidth = SmallButtonWidth * 2.0f + ButtonGap;

                // Where the component comes from, as a quiet tag left of the buttons: the script module that defines it. Hover says the file; a click opens it (in the module's editor).
                // Nothing for the engine's own components, and nothing when the row has no room next to the name.
                const auto Source = Ed.Display().SourceOf(false, pInfo->m_Guid.m_Value);
                auto DrawSourceTag = [&](float RightLimitX) noexcept
                {
                    if (!Source.m_bKnown || Source.m_Module == 0 || Source.m_ModuleName.empty()) return;
                    const float W = ImGui::CalcTextSize(Source.m_ModuleName.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f;
                    const float X = RightLimitX - W - 4.0f;
                    if (AvailW < 240.0f || X < RowPos.x + 90.0f) return;
                    ImGui::SetCursorScreenPos(ImVec2(X, RowPos.y));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                    const bool bClicked = ImGui::SmallButton((Source.m_ModuleName + "##src").c_str());
                    ImGui::PopStyleColor();
                    if (bClicked && xscene::g_OpenTypeSource) xscene::g_OpenTypeSource(Source);
                    if (ImGui::IsItemHovered())
                    {
                        const std::string Body = "Defined in the script module " + Source.m_ModuleName + ", in " + Source.m_File + ".\nClick to open the file.";
                        xeditor::hint::Draw({ .m_Topic = pInfo->m_pName ? pInfo->m_pName : "?", .m_Body = Body, .m_Detail = Source.m_Path });
                    }
                };
                if (!bIsShare)
                    ImGui::SetCursorScreenPos(ImVec2(RowPos.x + AvailW - 20.0f, RowPos.y));
                // Borderless/transparent-at-rest, only picking up a background on hover - matches
                // Unity's own small inline toolbar icon buttons (direct user comparison screenshot:
                // a bordered gray box vs Unity's flat "?"/drag-handle/"..." icons that only highlight
                // on hover). ButtonHovered/ButtonActive are left as the theme's own values so the
                // hover feedback itself still reads as a real button, just not a boxed one at rest.
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
                // Kind labels in the theme's disabled grey, before the buttons: "Builder" (consumed when the entity is created in
                // the game, see doc/xecs_builder_components.md) first, then "Shared". Both: "Builder Shared".
                const bool bIsBuilder = pInfo->m_bBuilder;
                const char* pKind = (bIsBuilder && bIsShare) ? "Builder Shared" : bIsBuilder ? "Builder" : bIsShare ? "Shared" : nullptr;
                const char* pKindTip = bIsBuilder
                    ? "Builder component: it only configures the entity while it is created in the game (a builder system consumes it), it is not part of the running entity."
                    : nullptr;
                if (bIsBuilder && !bIsShare)
                {
                    // Left-aligned at the start of the value column, exactly where "Builder Shared" / "Shared" start.
                    ImGui::SetCursorScreenPos(RowPos);
                    ImGui::TextDisabled("%s", pKind);
                    if (pKindTip && ImGui::IsItemHovered()) xeditor::hint::Text("%s", pKindTip);
                    ImGui::SetCursorScreenPos(ImVec2(RowPos.x + AvailW - 20.0f, RowPos.y));
                }
                if (bIsShare)
                {
                    // TextDisabled is the Level Editor theme's darker grey (#7A7A7A),
                    // keeping the SHARE marker distinct without changing the DnD button.
                    ImGui::SetCursorScreenPos(RowPos);
                    ImGui::TextDisabled("%s", pKind);
                    if (pKindTip && ImGui::IsItemHovered()) xeditor::hint::Text("%s", pKindTip);
                    DrawSourceTag(RowPos.x + AvailW - RightEdgePad - ShareButtonsWidth);
                    ImGui::SetCursorScreenPos(ImVec2(RowPos.x + AvailW - RightEdgePad - ShareButtonsWidth, RowPos.y));
                    ImGui::SmallButton("S");
                    if (ImGui::IsItemHovered())
                        xeditor::hint::Text("Drag onto a Resource folder to save as Shared-Component Template");
                    // Gated BeginDragDropSource (same 12px pattern as asset browser / source control) so a
                    // plain click does not swallow the item; only a real drag starts the export payload.
                    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 12.0f)
                        && ImGui::BeginDragDropSource())
                    {
                        xscene::share_template_drag_payload_t Payload{
                            Ed.m_State.m_SelectedEntityScene,
                            Ed.m_State.m_SelectedEntityId,
                            pInfo->m_Guid.m_Value
                        };
                        ImGui::SetDragDropPayload("XSCENE_SHARE_TEMPLATE_DRAG", &Payload, sizeof(Payload));
                        ImGui::Text("Save %s as Shared-Component Template", pInfo->m_pName ? pInfo->m_pName : "Share");
                        ImGui::EndDragDropSource();
                    }
                    ImGui::SameLine(0.0f, 4.0f);
                }
                if (!bIsShare)
                {
                    DrawSourceTag(RowPos.x + AvailW - 20.0f);
                    ImGui::SetCursorScreenPos(ImVec2(RowPos.x + AvailW - 20.0f, RowPos.y));
                }
                if (ImGui::SmallButton("X")) m_pPendingRemoveComponent = pInfo;
                ImGui::PopStyleVar();
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                {
                    // Which systems stop/start running on this entity if it's removed.
                    if (xlioncore::Ecs(Ed.World()).IsAlive(Ed.m_State.m_SelectedEntity))
                        xeditor::hint::Text("%s", xscene::system_usage::DescribeChange(xscene::system_usage::AllSystems(Ed.World()), xscene::system_usage::SetOf(Ed.World(), Ed.m_State.m_SelectedEntity), *pInfo, false).c_str());
                }
            };
            Inspector.m_OnComponentHeaderRender.Register(m_OnComponentHeaderRender);

            m_OnComponentHeaderColor = [this](xproperty::inspector&, const xproperty::type::object&, void* pInstance, ImVec4& Color, bool& bSet)
            {
                if (m_AddedComponents.empty()) return;
                auto It = m_ComponentMap.find(pInstance);
                if (It == m_ComponentMap.end() || !m_AddedComponents.contains(It->second->m_Guid.m_Value)) return;
                Color = kAddedHeaderColor;
                bSet  = true;
            };
            Inspector.m_OnComponentHeaderColor.Register(m_OnComponentHeaderColor);

            // Custom render for ANY xecs::component::entity-valued property (today, only
            // xecs::component::entity_reference::m_Target - but this is a value-type check, not a
            // per-property tag, so it applies automatically to any FUTURE component with an
            // entity-reference field too) - the shared inspector has no default draw style registered
            // for the raw 'entity' atomic type at all, so this is not optional polish, it's what makes
            // entity_reference safe to add to an entity in the first place. Drag a row from the Level
            // tree (LEVEL_ENTITY_DRAG, the same shared payload reparenting/prefab-creation already use)
            // onto this property to assign it; "X" clears it. Shows "<unresolved>" rather than
            // crashing when the target is valid but its owning scene isn't currently open
            // (ResolveEntityReference can't search a scene nobody loaded) - the underlying
            // value/reference is untouched either way, this is purely a display limitation.
            m_OnEntityReferenceRender = [this, &Ed](xproperty::inspector& Inspector, const xproperty::type::object& Obj, void* pInstance, std::string_view Path, const xproperty::any& Value, bool& bHandled)
            {
                auto& GameMgr = Ed.World();
                auto& State   = Ed.m_State;
                if (Value.m_pType == nullptr || Value.m_pType->m_GUID != xproperty::settings::var_type<xecs::component::entity>::guid_v) return;
                bHandled = true;

                const auto CurrentValue = Value.get<xecs::component::entity>();
                std::string     Label;
                xecs::scene::guid TargetScene;
                const bool bResolved = xscene::ResolveEntityReference(GameMgr, State, CurrentValue, Label, TargetScene);
                if (!bResolved) Label = CurrentValue.isValid() ? "<unresolved>" : "None";

                // The hierarchy (the parent, the children): shown, never assigned or cleared here
                if (auto ItC = m_ComponentMap.find(pInstance); ItC != m_ComponentMap.end() && xscene::IsStructuralComponent(ItC->second))
                {
                    ImGui::TextUnformatted(Label.c_str());
                    return;
                }

                // A plain Text/TextUnformatted's own "last item" rect is only as wide as its glyphs -
                // dropping anywhere else in this (usually much wider) property cell would silently miss
                // BeginDragDropTarget's hover check entirely. Selectable with an explicit size fills the
                // REST of the cell with a real, hoverable rect.
                const float AvailWidth  = ImGui::GetContentRegionAvail().x;
                const bool  bShowClear  = CurrentValue.isValid();
                ImGui::Selectable(Label.c_str(), false, ImGuiSelectableFlags_None, ImVec2(bShowClear ? AvailWidth - 24.0f : AvailWidth, 0.0f));

                // Attached to the Selectable specifically, immediately after it and BEFORE the "X"
                // button below (which would otherwise become the new "last item" and steal the drop
                // target down to its own tiny rect the moment a reference is already assigned).
                const bool bIsDropTarget = ImGui::BeginDragDropTarget();
                if (bIsDropTarget)
                {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("LEVEL_ENTITY_DRAG"))
                    {
                        IM_ASSERT(payload->DataSize == sizeof(xscene::entity_drag_payload_t));
                        auto& Dropped = *reinterpret_cast<const xscene::entity_drag_payload_t*>(payload->Data);
                        if (auto* pDropScene = GameMgr.m_SceneMgr.Find(Dropped.m_SceneGuid))
                        {
                            if (auto It = pDropScene->m_LocalToRuntime.find(Dropped.m_Id); It != pDropScene->m_LocalToRuntime.end())
                            {
                                // A reference pointing outside the owning entity's own scene needs an
                                // EXPLICIT Dependencies entry (pOwningScene->m_ParentScenes) to resolve
                                // on save/reload. That edge is authored only via the Dependencies folder
                                // (AddSceneDependency) - never auto-inferred from this drop. Missing or
                                // cyclic edges are refused before SetEntityReference runs.
                                bool bRefused = false;
                                if (Dropped.m_SceneGuid != State.m_SelectedEntityScene)
                                {
                                    if (auto* pOwningScene = GameMgr.m_SceneMgr.Find(State.m_SelectedEntityScene))
                                    {
                                        const bool bAlreadyDependency = std::find(pOwningScene->m_ParentScenes.begin(), pOwningScene->m_ParentScenes.end(), Dropped.m_SceneGuid) != pOwningScene->m_ParentScenes.end();
                                        if (!bAlreadyDependency && xscene::WouldCreateDependencyCycle(GameMgr, State.m_SelectedEntityScene, Dropped.m_SceneGuid))
                                        {
                                            xeditor::NotifyToast("Can't assign that reference: its scene already depends on this one (would create a circular scene dependency)");
                                            bRefused = true;
                                        }
                                        else if (!bAlreadyDependency)
                                        {
                                            // Explicit deps only: cross-scene refs require the user to
                                            // drag the target scene into this scene's Dependencies folder
                                            // first. Auto-adding ParentScenes from entity refs is what made
                                            // the graph unstable / hard to reason about.
                                            xeditor::NotifyToast("Can't assign that reference: add the target scene under Dependencies first");
                                            bRefused = true;
                                        }
                                    }
                                }

                                // Routed through the command system (gap #3, the command/undo known-gaps list)
                                // instead of BeginEdit/setProperty/CommitEdit directly - see this file's
                                // own top comment (xscene_commands_entity_reference.h) for why AfterScene/
                                // AfterId (not the raw runtime handle It->second) are what actually cross
                                // into the command string.
                                if (!bRefused)
                                {
                                    auto CompIt = m_ComponentMap.find(pInstance);
                                    if (CompIt != m_ComponentMap.end())
                                    {
                                        xeditor::Run(Ed.m_Undo, std::format("SetEntityReference -Scene {} -Id {} -Component {:016X} -Path {} -AfterScene {} -AfterId {}"
                                            , xscene::commands::FormatSceneGuid(State.m_SelectedEntityScene)
                                            , xscene::commands::FormatEntityId(State.m_SelectedEntityId)
                                            , CompIt->second->m_Guid.m_Value
                                            , xeditor::Quote(std::string(Path))
                                            , xscene::commands::FormatSceneGuid(Dropped.m_SceneGuid)
                                            , xscene::commands::FormatEntityId(Dropped.m_Id)));
                                    }
                                }
                            }
                        }
                    }
                    ImGui::EndDragDropTarget();
                }

                if (bShowClear)
                {
                    ImGui::SameLine();
                    // Same borderless/hover-only treatment as the component-header "X" above - this is
                    // the entity-reference "Target" field's own clear button, visible in the same panel.
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
                    const bool bClearClicked = ImGui::SmallButton("X");
                    ImGui::PopStyleVar();
                    ImGui::PopStyleColor();
                    if (bClearClicked)
                    {
                        // AfterScene/AfterId 0/0 is SetEntityReference's own "clear" sentinel - same
                        // routing/reasoning as the assign path just above.
                        auto CompIt = m_ComponentMap.find(pInstance);
                        if (CompIt != m_ComponentMap.end())
                        {
                            xeditor::Run(Ed.m_Undo, std::format("SetEntityReference -Scene {} -Id {} -Component {:016X} -Path {} -AfterScene {} -AfterId {}"
                                , xscene::commands::FormatSceneGuid(State.m_SelectedEntityScene)
                                , xscene::commands::FormatEntityId(State.m_SelectedEntityId)
                                , CompIt->second->m_Guid.m_Value
                                , xeditor::Quote(std::string(Path))
                                , xscene::commands::FormatSceneGuid(xecs::scene::guid{})
                                , xscene::commands::FormatEntityId(xecs::scene::invalid_permanent_id_v)));
                        }
                    }
                }
            };
            Inspector.m_OnCustomRenderReplaceValue.Register(m_OnEntityReferenceRender);

            // See this member's own declaration comment for why this exists at all. pUserData is
            // the pInfo passed to AppendEntityComponent's own pUserData argument (RenderEntity
            // PropertiesPanel's dirty-rebuild block) - re-derive the CURRENT pool address for that
            // exact component type on the CURRENTLY selected entity, the same lookup that block
            // itself uses, just re-run fresh instead of cached.
            m_OnGetComponentPointer = [this, &Ed](xproperty::inspector&, const int, void*& pObject, void* pUserData) noexcept
            {
                auto& GameMgr = Ed.World();
                auto& State   = Ed.m_State;
                pObject = nullptr;
                if (State.m_SelectedEntity.isValid() == false) return;

                auto* pInfo = static_cast<const xecs::component::type::info*>(pUserData);
                auto* pData = static_cast<std::byte*>(xscene::ResolveComponentPointer(GameMgr, State.m_SelectedEntity, *pInfo));
                if (pData == nullptr) return;
                pObject = pData;

                // A SHARE value is one instance used by every entity that has that value: the Inspector writes the value of an edited row into
                // the object it was given, before the SetProperty command runs, and that would change the instance of all of them (and the
                // command, which then moves the entity to the family of the new value, would find the old family holding the new value too).
                // So the Inspector gets a copy, refreshed every frame, and the command stays the only thing that changes share data.
                if (pInfo->m_TypeID == xecs::component::type::id::SHARE)
                {
                    auto& Scratch = m_ShareScratch[pInfo];
                    Scratch.resize(pInfo->m_Size);
                    std::memcpy(Scratch.data(), pData, pInfo->m_Size);
                    pData = Scratch.data();
                    pObject = pData;
                }

                // Keyed by the freshly-resolved real pointer, matching what m_OnOverrideCheck/
                // m_OnPropertyChanged/m_OnComponentHeaderRender actually receive from xproperty this
                // same frame (xproperty temporarily overwrites the fake pointer with exactly this
                // value for the duration of its own Render pass - see xPropertyImGuiInspector.cpp's
                // own Show()).
                m_ComponentMap[pData] = pInfo;
            };
            Inspector.m_OnGetComponentPointer.Register(m_OnGetComponentPointer);
        }
    };
}
