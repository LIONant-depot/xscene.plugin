#ifndef XSCENE_PREFAB_OVERRIDE_REPORT_H
#define XSCENE_PREFAB_OVERRIDE_REPORT_H
#pragma once

// What a prefab instance does differently from its prefab, as a person reads it: the "Prefab Overrides" popup of the Entity Properties draws this report, and the
// DescribePrefabOverrides command prints it, one line per item (member + component + Added / Removed / Modified, a property as "old -> new", hierarchy rows, orphans and the
// counts), so that the AI and the tests see exactly what the popup shows. Built from the live instance and its prefab (the baked member, nested recipes applied), never from
// bookkeeping alone: a component the root gained is a difference whether or not its recipe was refreshed yet. Included after xscene_commands_apply_overrides.h and
// xscene_shared_component_template.h (ResolveComponentPointer).

namespace xscene
{
    struct prefab_override_property
    {
        std::string   m_Path;                       // the property's path in its component
        std::string   m_Old;                        // the prefab's value
        std::string   m_New;                        // the instance's value
        std::uint32_t m_TypeGuid = 0;               // the value's type, as RevertOverride takes it
    };

    struct prefab_override_component
    {
        enum class kind { Added, Removed, Modified };
        kind                                    m_Kind = kind::Modified;
        std::uint64_t                           m_Guid = 0;
        std::string                             m_Name;
        std::vector<prefab_override_property>   m_Properties;       // Modified: the properties that differ
    };

    // One member of the instance (the instance's root included, empty address) that differs from its prefab in some component.
    struct prefab_override_member
    {
        xecs::scene::permanent_id               m_Id = xecs::scene::invalid_permanent_id_v;
        xecs::editor::member_address            m_Address;
        std::string                             m_Name;
        bool                                    m_bRoot = false;
        std::vector<prefab_override_component>  m_Components;
    };

    // A child the instance lost (a member of its prefab it removed) or gained (an entity of the scene under one of its members).
    struct prefab_override_hierarchy
    {
        bool                                    m_bAdded = false;
        xecs::scene::permanent_id               m_Id = xecs::scene::invalid_permanent_id_v;     // the added entity's id, or the removed member's derived id
        std::string                             m_Name;
        xecs::editor::member_address            m_Address;      // removed: the member's address; added: the address of the member it hangs from
        xecs::scene::permanent_id               m_Parent = xecs::scene::invalid_permanent_id_v;
    };

    // An override or a component diff that names a member the prefab no longer has.
    struct prefab_override_orphan
    {
        xecs::editor::member_address            m_Address;
        std::string                             m_Text;
    };

    struct prefab_override_report
    {
        bool                                    m_bValid = false;
        std::string                             m_Error;
        xecs::scene::guid                       m_Scene;
        xecs::scene::permanent_id               m_RootId = xecs::scene::invalid_permanent_id_v;     // the instance placed in the scene (the outermost one for a member of a nested instance)
        xecs::scene::permanent_id               m_AskedId = xecs::scene::invalid_permanent_id_v;    // the entity the report was asked about (a member, or the root)
        std::string                             m_RootName;
        xresource::full_guid                    m_Prefab;
        std::string                             m_PrefabName;
        std::vector<prefab_override_member>     m_Members;      // the asked entity's group first, then the root, then by address
        std::vector<prefab_override_hierarchy>  m_Hierarchy;
        std::vector<prefab_override_orphan>     m_Orphans;
        int                                     m_nAdded = 0, m_nRemoved = 0, m_nModified = 0;      // components added, components removed, properties modified

        int  HierarchyCount() const noexcept { return static_cast<int>(m_Hierarchy.size()); }
        int  Total()          const noexcept { return m_nAdded + m_nRemoved + m_nModified + HierarchyCount() + static_cast<int>(m_Orphans.size()); }
    };

    namespace details
    {
        // The property's value as text, for the component of an entity. False when the entity has not got the component or the property.
        inline bool ReadPropertyText(xecs::game_mgr::instance& GameMgr, xecs::component::entity E, const xecs::component::type::info& Info, std::string_view Path, std::string& Out, std::uint32_t* pType = nullptr) noexcept
        {
            if (!E.isValid() || Info.m_pPropertyTable == nullptr) return false;
            void* pData = xscene::ResolveComponentPointer(GameMgr, E, Info);
            if (pData == nullptr) return false;
            bool bFound = false;
            xproperty::settings::context Context;
            xproperty::sprop::collector(pData, *Info.m_pPropertyTable, Context, [&](const char* pName, xproperty::any&& Data, const xproperty::type::members&, bool, const void*) noexcept
            {
                if (Path != pName) return;
                std::array<char, 256> Buffer{};
                const auto Len = xscene::commands::FormatPropertyValue(Buffer, Data);
                Out.assign(Buffer.data(), Len > 0 ? static_cast<std::size_t>(Len) : 0);
                if (pType) *pType = Data.m_pType ? Data.m_pType->m_GUID : 0;
                bFound = true;
            });
            return bFound;
        }

        // What the instance's component set of a member is worth comparing: not the hierarchy bookkeeping, not what the editor adds for its own state (disabled, hidden).
        inline bool ComparableComponent(xecs::game_mgr::instance& GameMgr, const xecs::component::type::info* pInfo) noexcept
        {
            if (pInfo == nullptr || xscene::IsInternalComponent(pInfo) || xscene::IsEditorStateComponent(pInfo)) return false;
            return !(GameMgr.areBuildersEnabled() && pInfo->m_bBuilder);        // a running world has consumed its builders
        }

        inline std::vector<const xecs::component::type::info*> ComponentSet(xecs::game_mgr::instance& GameMgr, xecs::component::entity E) noexcept
        {
            std::vector<const xecs::component::type::info*> Data, Share, Tags;
            if (!E.isValid()) return Data;
            xlioncore::Ecs(GameMgr).ComponentTypesOf(E, Data, Share, Tags);
            Data.insert(Data.end(), Share.begin(), Share.end());
            Data.insert(Data.end(), Tags.begin(), Tags.end());
            std::erase_if(Data, [&](const xecs::component::type::info* p) noexcept { return !ComparableComponent(GameMgr, p); });
            return Data;
        }
    }

    // The report of the instance that Id (the instance's root or any of its members) belongs to. Refreshes the instance's recipe first (as ListPrefabOverrides does): what its members are now.
    inline prefab_override_report BuildPrefabOverrideReport(xecs::game_mgr::instance& GameMgr, xecs::scene::guid SceneGuid, xecs::scene::permanent_id Id) noexcept
    {
        prefab_override_report R;
        R.m_Scene   = SceneGuid;
        R.m_AskedId = Id;
        auto* pScene = GameMgr.m_SceneMgr.Find(SceneGuid);
        if (!pScene || !pScene->m_LocalToRuntime.contains(Id)) { R.m_Error = "target not found"; return R; }
        auto& Ecs = xlioncore::Ecs(GameMgr);

        R.m_RootId = Id;
        if (auto M = pScene->m_InstanceMembers.find(Id); M != pScene->m_InstanceMembers.end()) R.m_RootId = M->second.m_Root;
        if (!pScene->m_LocalToRuntime.contains(R.m_RootId)) { R.m_Error = "not a prefab instance"; return R; }
        auto* pPI = xscene::FindPrefabInstance(GameMgr, pScene->m_LocalToRuntime.at(R.m_RootId));
        if (!pPI) { R.m_Error = "not a prefab instance"; return R; }
        R.m_Prefab = pPI->m_PrefabInstance;
        if (auto Err = Ecs.EnsureLoadedPrefab(pPI->m_PrefabInstance); Err) { R.m_Error = std::format("the prefab could not be loaded: {}", Err.getMessage()); return R; }
        Ecs.RefreshPrefabRecipe(*pScene, R.m_RootId);
        pPI = xscene::FindPrefabInstance(GameMgr, pScene->m_LocalToRuntime.at(R.m_RootId));         // loading a prefab makes entities: fetched again
        if (!pPI) { R.m_Error = "not a prefab instance"; return R; }

        xresource_editor::RemapGUIDToString(R.m_PrefabName, R.m_Prefab);
        R.m_RootName = xscene::EntityDisplayName(*pScene, R.m_RootId);

        const auto  Prefab = pPI->m_PrefabInstance;
        const auto  Known  = xscene::commands::PrefabAddressesOf(GameMgr, *pPI);
        const auto  NameOfComponent = [&](std::uint64_t Guid) -> std::string
        {
            auto* pInfo = Ecs.FindComponentType(xecs::component::type::guid{ Guid });
            return pInfo && pInfo->m_pName ? pInfo->m_pName : std::format("{:016X}", Guid);
        };

        // The members, the root first
        struct live_member { xecs::editor::member_address m_Address; xecs::scene::permanent_id m_Id; std::string m_Name; };
        std::vector<live_member> Live;
        Live.push_back({ {}, R.m_RootId, R.m_RootName });
        for (auto& [MId, M] : pScene->m_InstanceMembers)
            if (M.m_Root == R.m_RootId)
            {
                std::string Name = pScene->m_EntityNames.contains(MId) ? pScene->m_EntityNames.at(MId) : M.m_Name;
                if (Name.empty()) Name = xscene::EntityDisplayName(*pScene, MId);
                Live.push_back({ M.m_Address, MId, std::move(Name) });
            }
        std::sort(Live.begin() + 1, Live.end(), [](const live_member& A, const live_member& B) noexcept { return A.m_Address < B.m_Address; });

        for (auto& L : Live)
        {
            const auto Entity = pScene->m_LocalToRuntime.contains(L.m_Id) ? pScene->m_LocalToRuntime.at(L.m_Id) : xecs::component::entity{};
            if (!Entity.isValid() || !Ecs.IsAlive(Entity)) continue;
            const auto Baked    = Ecs.ResolveBakedPrefabMember(Prefab, L.m_Address);
            const auto Template = Ecs.ResolvePrefabMember(Prefab, L.m_Address);
            if (!Baked.isValid() && !Template.isValid()) continue;

            prefab_override_member Member{ .m_Id = L.m_Id, .m_Address = L.m_Address, .m_Name = L.m_Name, .m_bRoot = L.m_Address.empty() };
            const auto LiveSet  = details::ComponentSet(GameMgr, Entity);
            const auto BaseSet  = details::ComponentSet(GameMgr, Baked.isValid() ? Baked : Template);
            const auto Has      = [](const std::vector<const xecs::component::type::info*>& Set, const xecs::component::type::info* p) noexcept { return std::ranges::find(Set, p) != Set.end(); };
            const auto Add      = [&](prefab_override_component::kind K, const xecs::component::type::info* p, std::vector<prefab_override_property> Props = {})
            {
                Member.m_Components.push_back({ .m_Kind = K, .m_Guid = p->m_Guid.m_Value, .m_Name = p->m_pName ? p->m_pName : "?", .m_Properties = std::move(Props) });
                (K == prefab_override_component::kind::Added ? R.m_nAdded : K == prefab_override_component::kind::Removed ? R.m_nRemoved : R.m_nModified) += K == prefab_override_component::kind::Modified ? static_cast<int>(Member.m_Components.back().m_Properties.size()) : 1;
            };

            for (auto* p : LiveSet) if (!Has(BaseSet, p)) Add(prefab_override_component::kind::Added, p);
            for (auto* p : BaseSet) if (!Has(LiveSet, p)) Add(prefab_override_component::kind::Removed, p);

            // Modified: the properties the recipe overrides, whose value differs from the prefab's (an override equal to its prefab's value is no difference)
            for (auto& C : pPI->m_lComponents)
            {
                if (!xscene::SameMember(C.m_Member, L.m_Address)) continue;
                auto* pInfo = Ecs.FindComponentType(xecs::component::type::guid{ C.m_ComponentTypeGuid });
                if (!pInfo || !Has(LiveSet, pInfo) || !Has(BaseSet, pInfo)) continue;
                std::vector<prefab_override_property> Props;
                for (auto& O : C.m_PropertyOverrides)
                {
                    prefab_override_property P{ .m_Path = O.m_PropertyName };
                    if (!details::ReadPropertyText(GameMgr, Entity, *pInfo, O.m_PropertyName, P.m_New, &P.m_TypeGuid)) continue;
                    // The baked member has the nested recipes applied but holds no references (a plan has none): the template member answers for those and for what the baked one lacks
                    if (!(Baked.isValid() && details::ReadPropertyText(GameMgr, Baked, *pInfo, O.m_PropertyName, P.m_Old)) && !details::ReadPropertyText(GameMgr, Template, *pInfo, O.m_PropertyName, P.m_Old)) continue;
                    if (P.m_Old != P.m_New) Props.push_back(std::move(P));
                }
                if (!Props.empty()) Add(prefab_override_component::kind::Modified, pInfo, std::move(Props));
            }
            if (!Member.m_Components.empty()) R.m_Members.push_back(std::move(Member));
        }
        std::stable_sort(R.m_Members.begin(), R.m_Members.end(), [&](const prefab_override_member& A, const prefab_override_member& B) noexcept
        {
            const bool bA = A.m_Id == Id, bB = B.m_Id == Id;
            if (bA != bB) return bA;
            return false;
        });

        // The hierarchy: members of the prefab this instance removed (the refreshed recipe says which), and the entities of the scene hanging from its root or its members
        for (auto& H : pPI->m_HierarchyDiffs)
        {
            if (H.m_bAdded || xscene::commands::IsOrphan(Known, H.m_Member)) continue;
            prefab_override_hierarchy Row{ .m_bAdded = false, .m_Id = xecs::scene::DeriveMemberId(R.m_RootId, H.m_Member), .m_Address = H.m_Member };
            if (H.m_Member.size() == 1)
                if (auto G = GameMgr.m_PrefabMgr.m_PrefabGroups.find(Prefab.m_Instance.m_Value); G != GameMgr.m_PrefabMgr.m_PrefabGroups.end())
                    if (auto N = G->second.m_EntityNames.find(H.m_Member[0]); N != G->second.m_EntityNames.end()) Row.m_Name = N->second;
            if (Row.m_Name.empty()) Row.m_Name = "Member " + xscene::commands::FormatMemberAddress(H.m_Member);
            R.m_Hierarchy.push_back(std::move(Row));
        }
        for (auto& L : Live)
        {
            auto It = pScene->m_LocalToRuntime.find(L.m_Id);
            if (It == pScene->m_LocalToRuntime.end()) continue;
            auto* pKids = Ecs.ChildrenOf(It->second);
            if (pKids == nullptr) continue;
            for (auto K : pKids->m_List)
                if (auto KIt = pScene->m_RuntimeToLocal.find(K.m_Value); KIt != pScene->m_RuntimeToLocal.end() && !pScene->m_InstanceMembers.contains(KIt->second))
                    R.m_Hierarchy.push_back({ .m_bAdded = true, .m_Id = KIt->second, .m_Name = xscene::EntityDisplayName(*pScene, KIt->second), .m_Address = L.m_Address, .m_Parent = L.m_Id });
        }

        // Orphans: what addresses a member the prefab no longer has
        for (auto& C : pPI->m_lComponents)
            if (xscene::commands::IsOrphan(Known, C.m_Member))
                for (auto& O : C.m_PropertyOverrides)
                    R.m_Orphans.push_back({ C.m_Member, std::format("{}.{} = {}", NameOfComponent(C.m_ComponentTypeGuid), O.m_PropertyName, O.m_PropertyValueAsString) });
        for (auto& D : pPI->m_ComponentDiffs)
            if (xscene::commands::IsOrphan(Known, D.m_Member))
                R.m_Orphans.push_back({ D.m_Member, std::format("{} {}", D.m_bAdded ? "added" : "removed", NameOfComponent(D.m_ComponentTypeGuid)) });

        R.m_bValid = true;
        return R;
    }

    // The report as text, one line per item. The popup of the Entity Properties shows exactly this.
    inline std::string FormatPrefabOverrideReport(const prefab_override_report& R) noexcept
    {
        if (!R.m_bValid) return std::format("DescribePrefabOverrides: {}", R.m_Error);
        using kind = prefab_override_component::kind;
        std::string Out = std::format("Instance {} \"{}\"  Prefab {:016X} \"{}\"\n", xscene::commands::FormatEntityId(R.m_RootId), R.m_RootName, R.m_Prefab.m_Instance.m_Value, R.m_PrefabName);
        Out += std::format("Asked {}\n", xscene::commands::FormatEntityId(R.m_AskedId));
        Out += std::format("Changes: {}  ComponentsAdded={} ComponentsRemoved={} PropertiesModified={} Hierarchy={} Orphans={}\n", R.Total(), R.m_nAdded, R.m_nRemoved, R.m_nModified, R.HierarchyCount(), R.m_Orphans.size());
        for (auto& M : R.m_Members)
        {
            Out += std::format("Member {} \"{}\" {}\n", xscene::commands::FormatEntityId(M.m_Id), M.m_Name, xscene::commands::FormatMemberAddress(M.m_Address));
            for (auto& C : M.m_Components)
            {
                if (C.m_Kind == kind::Added)        Out += std::format("  Added {}\n", C.m_Name);
                else if (C.m_Kind == kind::Removed) Out += std::format("  Removed {}\n", C.m_Name);
                else
                {
                    Out += std::format("  Modified {} ({})\n", C.m_Name, C.m_Properties.size());
                    for (auto& P : C.m_Properties) Out += std::format("    {}: {} -> {}\n", P.m_Path, P.m_Old, P.m_New);
                }
            }
        }
        for (auto& H : R.m_Hierarchy)
            Out += H.m_bAdded ? std::format("Hierarchy Added {} \"{}\" under {} {}\n", xscene::commands::FormatEntityId(H.m_Id), H.m_Name, xscene::commands::FormatEntityId(H.m_Parent), xscene::commands::FormatMemberAddress(H.m_Address))
                              : std::format("Hierarchy Removed {} \"{}\" {}\n", xscene::commands::FormatEntityId(H.m_Id), H.m_Name, xscene::commands::FormatMemberAddress(H.m_Address));
        for (auto& O : R.m_Orphans)
            Out += std::format("Orphan {} {}\n", xscene::commands::FormatMemberAddress(O.m_Address), O.m_Text);
        return Out;
    }
}

namespace xscene::commands
{
    //================================================================================================
    // DescribePrefabOverrides - what the "Prefab Overrides" popup of the Entity Properties shows, as plain text, one line per item: the instance (its root id, its prefab), the counts,
    // then for each member that differs its components Added / Removed / Modified (n) with each modified property as "Name: old -> new", the hierarchy rows (Removed members, Added
    // entities) and the orphan overrides. -Id may be the instance's root or any of its members (a member of a nested instance answers for the instance placed in the scene).
    //================================================================================================
    struct describe_prefab_overrides_query_cmd : scene_query_command
    {
        describe_prefab_overrides_query_cmd(xundo::system& System, void* pDataBase) noexcept : scene_query_command(System, "DescribePrefabOverrides", pDataBase) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override
        {
            return "Describes in plain text what a prefab instance does differently from its prefab, as the Prefab Overrides popup of the Inspector shows it: the instance's root id and prefab, the counts, per member the components Added / Removed / Modified (n) with each property as 'Name: old -> new', the Hierarchy rows and the Orphans. Usage: DescribePrefabOverrides -Scene hexguid -Id hexid (the instance's root or one of its members)";
        }
        void RegisterArguments() noexcept override
        {
            m_hScene = m_Parser.addOption("Scene", "Scene guid, 16 hex digits", true, 1);
            m_hId    = m_Parser.addOption("Id",    "Permanent id of the instance's root or of one of its members, 8 or 16 hex digits", true, 1);
        }
        std::string Query() noexcept override
        {
            auto SceneArg = m_Parser.getOptionArgAs<std::string>(m_hScene, 0);
            auto IdArg    = m_Parser.getOptionArgAs<std::string>(m_hId, 0);
            if (std::holds_alternative<xerr>(SceneArg) || std::holds_alternative<xerr>(IdArg)) return "DescribePrefabOverrides: bad arguments";
            return FormatPrefabOverrideReport(BuildPrefabOverrideReport(World(), ParseSceneGuid(std::get<std::string>(SceneArg)), ParseEntityId(std::get<std::string>(IdArg))));
        }
        xcmdline::parser::handle m_hScene, m_hId;
    };
}

#endif // XSCENE_PREFAB_OVERRIDE_REPORT_H
