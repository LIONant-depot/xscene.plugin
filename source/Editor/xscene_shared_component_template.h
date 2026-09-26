#ifndef XSCENE_SHARED_COMPONENT_TEMPLATE_H
#define XSCENE_SHARED_COMPONENT_TEMPLATE_H
#pragma once
#include <cstring>
#include <vector>

// V1 SharedComponentTemplate pipeline:
//   - Drag SHARE header [S] onto a Resource View folder to create an export-only snapshot asset
//   - Drop a SharedComponentTemplate onto Add Component to add+intern from serialized values
// Never creates a persistent link from entity to template. Prefer existing CreateAsset /
// descriptor::Serialize / DESCRIPTOR_GUID DnD / property snapshot shapes.
#include "dependencies/xresource_pipeline_v2/source/editor/E10_Commands_Assets.h"
#include "plugins/xscene.plugin/source/Editor/xscene_commands_component_edit.h"

namespace xscene
{
    //---------------------------------------------------------------------------
    // Resolve a live component pointer for DATA (entity pool) or SHARE (share-entity pool).
    //---------------------------------------------------------------------------
    inline void* ResolveComponentPointer(xecs::game_mgr::instance& GameMgr, xecs::component::entity Entity, const xecs::component::type::info& Info) noexcept
    {
        if (Entity.isValid() == false) return nullptr;
        auto& Details = GameMgr.m_ComponentMgr.getEntityDetails(Entity);
        if (Details.m_pPool == nullptr) return nullptr;

        const auto iType = Details.m_pPool->findIndexComponentFromInfo(Info);
        if (iType >= 0)
            return &Details.m_pPool->m_pComponent[iType][Details.m_PoolIndex.m_Value * Info.m_Size];

        if (Info.m_TypeID != xecs::component::type::id::SHARE) return nullptr;
        auto* pFamily = Details.m_pPool->m_pMyFamily;
        if (pFamily == nullptr) return nullptr;

        for (int i = 0, end = static_cast<int>(pFamily->m_ShareInfos.size()); i < end; ++i)
        {
            if (pFamily->m_ShareInfos[i]->m_Guid.m_Value != Info.m_Guid.m_Value) continue;
            auto& ShareDetails = GameMgr.m_ComponentMgr.getEntityDetails(pFamily->m_ShareDetails[i].m_Entity);
            if (ShareDetails.m_pPool == nullptr) return nullptr;
            const auto iShare = ShareDetails.m_pPool->findIndexComponentFromInfo(Info);
            if (iShare < 0) return nullptr;
            return &ShareDetails.m_pPool->m_pComponent[iShare][ShareDetails.m_PoolIndex.m_Value * Info.m_Size];
        }
        return nullptr;
    }

    //---------------------------------------------------------------------------
    // Collect property rows from a live component instance (DATA or SHARE).
    //---------------------------------------------------------------------------
    inline void CollectComponentPropertyValues(void* pInstance, const xecs::component::type::info& Info, std::vector<xecs::shared_component_template::property_value>& Out) noexcept
    {
        Out.clear();
        if (pInstance == nullptr || Info.m_pPropertyTable == nullptr) return;

        xproperty::settings::context Context;
        xproperty::sprop::collector(pInstance, *Info.m_pPropertyTable, Context, [&](const char* pPropertyName, xproperty::any&& Data, const xproperty::type::members&, bool, const void*) noexcept
        {
            std::array<char, 256> Buffer{};
            const auto Len = xscene::commands::FormatPropertyValue(Buffer, Data);
            Out.push_back({
                pPropertyName ? pPropertyName : "",
                Data.m_pType ? Data.m_pType->m_GUID : 0u,
                std::string(Buffer.data(), Len > 0 ? static_cast<std::size_t>(Len) : 0)
            });
        });
    }

    //---------------------------------------------------------------------------
    // Apply property rows onto a byte buffer via setProperty (temp share value).
    //---------------------------------------------------------------------------
    inline void ApplyPropertyValuesToInstance(void* pInstance, const xecs::component::type::info& Info, std::span<const xecs::shared_component_template::property_value> Properties) noexcept
    {
        if (pInstance == nullptr || Info.m_pPropertyTable == nullptr) return;
        for (auto& Prop : Properties)
        {
            xproperty::any Value;
            std::string    ValueStrMutable = Prop.m_Value;
            xproperty::settings::StringToAny(Value, Prop.m_TypeGuid, std::span<char>(ValueStrMutable.data(), ValueStrMutable.size()));
            std::string SetError;
            xproperty::settings::context Context;
            xproperty::sprop::setProperty(SetError, pInstance, *Info.m_pPropertyTable, xproperty::sprop::container::prop{ Prop.m_Path, Value }, Context);
        }
    }

    //---------------------------------------------------------------------------
    // Copy-on-write re-intern for a SHARE component: mutate a temp copy, then move
    // the entity into the matching family (may reuse an identical existing value).
    // Entity handle stays valid (same-archetype family MoveIn).
    //---------------------------------------------------------------------------
    inline bool ReinternShareComponentValue(xecs::game_mgr::instance& GameMgr, xecs::component::entity Entity, const xecs::component::type::info& Info, std::span<const xecs::shared_component_template::property_value> Properties) noexcept
    {
        if (Info.m_TypeID != xecs::component::type::id::SHARE) return false;
        if (Entity.isValid() == false) return false;

        auto& Details = GameMgr.m_ComponentMgr.getEntityDetails(Entity);
        if (Details.m_pPool == nullptr || Details.m_pPool->m_pMyFamily == nullptr || Details.m_pPool->m_pArchetype == nullptr)
            return false;

        void* pCurrent = ResolveComponentPointer(GameMgr, Entity, Info);
        if (pCurrent == nullptr) return false;

        std::vector<std::byte> Temp(Info.m_Size);
        std::memcpy(Temp.data(), pCurrent, Info.m_Size);
        ApplyPropertyValuesToInstance(Temp.data(), Info, Properties);

        return GameMgr.ReinternShareComponent(Entity, Info, Temp.data());
    }

    //---------------------------------------------------------------------------
    // Apply a single property to a SHARE component via re-intern (editing path).
    //---------------------------------------------------------------------------
    inline bool SetShareLivePropertyValue(xecs::game_mgr::instance& GameMgr, const xscene::commands::resolved_property_target& Target, const std::string& Path, std::uint32_t TypeGuid, const std::string& ValueStr) noexcept
    {
        if (!Target.m_pInfo || Target.m_pInfo->m_TypeID != xecs::component::type::id::SHARE) return false;
        xecs::shared_component_template::property_value Prop{ Path, TypeGuid, ValueStr };
        return ReinternShareComponentValue(GameMgr, Target.m_Entity, *Target.m_pInfo, { &Prop, 1 });
    }

    //---------------------------------------------------------------------------
    // Load descriptor from an existing SharedComponentTemplate asset.
    //---------------------------------------------------------------------------
    inline bool LoadSharedComponentTemplateDescriptor(e10::library_mgr& LibMgr, e10::library::guid LibraryGuid, xresource::full_guid AssetGuid, xecs::shared_component_template::descriptor& Out) noexcept
    {
        bool bOk = false;
        LibMgr.getNodeInfo(LibraryGuid, AssetGuid, [&](e10::library_db::info_node& Node)
        {
            std::wstring DescPath = Node.m_Path;
            if (const auto Slash = DescPath.find_last_of(L'\\'); Slash != std::wstring::npos)
                DescPath = DescPath.substr(0, Slash + 1) + L"Descriptor.txt";
            xproperty::settings::context Context;
            if (auto Err = Out.Serialize(true, DescPath, Context); Err)
            {
                xeditor::NotifyError(std::format("SharedComponentTemplate load failed: {}", Err.getMessage()));
                return;
            }
            bOk = true;
        });
        return bOk;
    }

    //---------------------------------------------------------------------------
    // Write descriptor next to info.txt for a just-created asset.
    //---------------------------------------------------------------------------
    inline bool SaveSharedComponentTemplateDescriptor(e10::library_mgr& LibMgr, e10::library::guid LibraryGuid, xresource::full_guid AssetGuid, const xecs::shared_component_template::descriptor& Desc) noexcept
    {
        bool bOk = false;
        LibMgr.getNodeInfo(LibraryGuid, AssetGuid, [&](e10::library_db::info_node& Node)
        {
            std::wstring DescPath = Node.m_Path;
            if (const auto Slash = DescPath.find_last_of(L'\\'); Slash != std::wstring::npos)
                DescPath = DescPath.substr(0, Slash + 1) + L"Descriptor.txt";
            xproperty::settings::context Context;
            // Serialize takes non-const this for read path; write is const-safe in practice.
            auto& Mutable = const_cast<xecs::shared_component_template::descriptor&>(Desc);
            if (auto Err = Mutable.Serialize(false, DescPath, Context); Err)
            {
                xeditor::NotifyError(std::format("SharedComponentTemplate save failed: {}", Err.getMessage()));
                return;
            }
            Node.m_bHasDescriptor = true;
            bOk = true;
        });
        return bOk;
    }

    //---------------------------------------------------------------------------
    // Create asset + write descriptor from the selected entity's current share value.
    //---------------------------------------------------------------------------
    inline xresource::full_guid CreateSharedComponentTemplateAsset(
        scene_context& Ed,
        xecs::scene::guid SceneGuid,
        xecs::scene::permanent_id Id,
        std::uint64_t ComponentTypeGuidValue,
        e10::library::guid LibraryGuid,
        xresource::full_guid ParentFolder,
        std::string_view Name) noexcept
    {
        auto* pInfo = Ed.World().m_ComponentMgr.findComponentTypeInfo(xecs::component::type::guid{ ComponentTypeGuidValue });
        if (!pInfo || pInfo->m_TypeID != xecs::component::type::id::SHARE)
        {
            xeditor::NotifyError("Save as Shared-Component Template: component is not a share type");
            return {};
        }

        auto Entity = xscene::commands::ResolveEntityHandle(Ed, SceneGuid, Id);
        if (Entity.isValid() == false)
        {
            xeditor::NotifyError("Save as Shared-Component Template: entity not found");
            return {};
        }

        void* pInstance = ResolveComponentPointer(Ed.World(), Entity, *pInfo);
        if (pInstance == nullptr)
        {
            xeditor::NotifyError("Save as Shared-Component Template: share value not found on entity");
            return {};
        }

        xecs::shared_component_template::descriptor Desc;
        Desc.m_ComponentTypeGuid = ComponentTypeGuidValue;
        CollectComponentPropertyValues(pInstance, *pInfo, Desc.m_Properties);

        xresource::instance_guid NewInstance{};
        NewInstance.GenerateGUID();
        const xresource::full_guid AssetGuid{ NewInstance, xecs::shared_component_template::type_guid_v };
        std::string AssetName;
        if (!Name.empty())
            AssetName = std::string(Name);
        else if (pInfo->m_pName && pInfo->m_pName[0])
            AssetName = std::format("{}_Template", pInfo->m_pName);
        else
            AssetName = "SharedComponentTemplate";

        e10::commands::CreateOrRestoreAsset(LibraryGuid, AssetGuid, ParentFolder, AssetName);
        if (!SaveSharedComponentTemplateDescriptor(e10::g_LibMgr, LibraryGuid, AssetGuid, Desc))
            return {};

        return AssetGuid;
    }

    //---------------------------------------------------------------------------
    // Instantiate: add the share component (if missing) then re-intern from template values.
    //---------------------------------------------------------------------------
    inline bool InstantiateSharedComponentTemplate(
        scene_context& Ed,
        xecs::scene::guid SceneGuid,
        xecs::scene::permanent_id Id,
        const xecs::shared_component_template::descriptor& Desc) noexcept
    {
        auto* pInfo = Ed.World().m_ComponentMgr.findComponentTypeInfo(xecs::component::type::guid{ Desc.m_ComponentTypeGuid });
        if (!pInfo || pInfo->m_TypeID != xecs::component::type::id::SHARE)
        {
            xeditor::NotifyError("SharedComponentTemplate: unknown or non-share component type");
            return false;
        }

        auto Entity = xscene::commands::ResolveEntityHandle(Ed, SceneGuid, Id);
        if (Entity.isValid() == false) return false;

        auto& Details = Ed.World().m_ComponentMgr.getEntityDetails(Entity);
        if (Details.m_pPool == nullptr) return false;

        const bool bAlreadyPresent = ResolveComponentPointer(Ed.World(), Entity, *pInfo) != nullptr;
        if (!bAlreadyPresent)
        {
            std::array Add{ pInfo };
            Entity = xscene::commands::MigrateEntityComponents(Ed, SceneGuid, Id, Add, {});
            if (Entity.isValid() == false) return false;
        }

        return ReinternShareComponentValue(Ed.World(), Entity, *pInfo, Desc.m_Properties);
    }

    //---------------------------------------------------------------------------
    // Payload for dragging a SHARE component header [S] onto a Resource View folder
    // to create a SharedComponentTemplate (export-only snapshot). Same external-drop
    // registration pattern as entity_to_prefab_drop / E29_ENTITY_DRAG.
    //---------------------------------------------------------------------------
    struct share_template_drag_payload_t
    {
        xecs::scene::guid           m_SceneGuid;
        xecs::scene::permanent_id   m_EntityId = xecs::scene::invalid_permanent_id_v;
        std::uint64_t               m_ComponentTypeGuid = 0;
    };

    struct share_to_template_drop final : e10::external_drop_registration_base
    {
        share_to_template_drop() noexcept : e10::external_drop_registration_base{ "XSCENE_SHARE_TEMPLATE_DRAG" } {}

        xresource::full_guid OnDrop(e10::library_mgr& /*AssetMgr*/, e10::library::guid LibraryGUID, xresource::full_guid ParentGUID, const void* pData, std::size_t Size) const noexcept override
        {
            if (Size != sizeof(share_template_drag_payload_t)) return {};
            auto* pEd = FindSceneContext();
            if (pEd == nullptr) return {};
            auto& Payload = *reinterpret_cast<const share_template_drag_payload_t*>(pData);
            // Empty name -> CreateSharedComponentTemplateAsset picks "{TypeName}_Template".
            return CreateSharedComponentTemplateAsset(
                *pEd, Payload.m_SceneGuid, Payload.m_EntityId, Payload.m_ComponentTypeGuid,
                LibraryGUID, ParentGUID, {});
        }
    };
    inline static share_to_template_drop g_ShareToTemplateDrop{};

    //---------------------------------------------------------------------------
    // Accept DESCRIPTOR_GUID drop of SharedComponentTemplate onto Add Component.
    //---------------------------------------------------------------------------
    inline bool TryAcceptSharedComponentTemplateDrop(scene_context& Ed) noexcept
    {
        if (!ImGui::BeginDragDropTarget()) return false;
        bool bHandled = false;
        if (const ImGuiPayload* Payload = ImGui::AcceptDragDropPayload("DESCRIPTOR_GUID"))
        {
            if (Payload->DataSize >= sizeof(e10::drag_and_drop_folder_payload_t))
            {
                auto& Dropped = *reinterpret_cast<const e10::drag_and_drop_folder_payload_t*>(Payload->Data);
                if (Dropped.m_Source.m_Type == xecs::shared_component_template::type_guid_v)
                {
                    auto& State = Ed.m_State;
                    if (State.m_SelectedEntity.isValid() && !State.m_SelectedEntityScene.empty())
                    {
                        xecs::shared_component_template::descriptor Desc;
                        // Prefer project library; fall back to scanning open libraries.
                        e10::library::guid Lib = e10::g_LibMgr.m_ProjectGUID;
                        bool bLoaded = LoadSharedComponentTemplateDescriptor(e10::g_LibMgr, Lib, Dropped.m_Source, Desc);
                        if (!bLoaded)
                        {
                            for (auto& L : e10::g_LibMgr.m_mLibraryDB)
                            {
                                if (LoadSharedComponentTemplateDescriptor(e10::g_LibMgr, L.first, Dropped.m_Source, Desc))
                                {
                                    Lib = L.first;
                                    bLoaded = true;
                                    break;
                                }
                            }
                        }
                        if (bLoaded)
                        {
                            // Route add through AddComponent command when the type isn't present yet,
                            // then apply values. Values themselves are not undo-routed in V1 (noted).
                            auto* pInfo = Ed.World().m_ComponentMgr.findComponentTypeInfo(xecs::component::type::guid{ Desc.m_ComponentTypeGuid });
                            if (pInfo)
                            {
                                auto Entity = State.m_SelectedEntity;
                                const bool bPresent = ResolveComponentPointer(Ed.World(), Entity, *pInfo) != nullptr;
                                if (!bPresent)
                                {
                                    xeditor::Run(Ed.m_Undo, std::format("AddComponent -Scene {} -Id {} -Component {:016X}"
                                        , xscene::commands::FormatSceneGuid(State.m_SelectedEntityScene)
                                        , xscene::commands::FormatEntityId(State.m_SelectedEntityId)
                                        , pInfo->m_Guid.m_Value));
                                }
                                // Re-resolve after possible AddComponent migration.
                                Entity = xscene::commands::ResolveEntityHandle(Ed, State.m_SelectedEntityScene, State.m_SelectedEntityId);
                                if (Entity.isValid())
                                    ReinternShareComponentValue(Ed.World(), Entity, *pInfo, Desc.m_Properties);
                                State.m_bEntityInspectorDirty = true;
                                bHandled = true;
                            }
                        }
                    }
                }
            }
        }
        ImGui::EndDragDropTarget();
        return bHandled;
    }
} // namespace xscene

#endif
