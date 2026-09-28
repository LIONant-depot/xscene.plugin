#ifndef XSCENE_SYSTEM_USAGE_H
#define XSCENE_SYSTEM_USAGE_H
#pragma once

// "Which systems touch this entity, and what changes if I add/remove a component?" - one shared
// answer for the Inspector (Systems popup, component [X] tooltips, Add Component hints) and for the
// command pipe (DescribeEntity / ListSystems), so a human and an AI always see the same thing.
//
// Matching uses each system's real m_Query (the same test the scheduler uses); read/write comes from
// its compile-time m_Access table (query tuple + operator() parameters - see
// xecs::system::type::component_access). A system that declares no components (e.g. a logger that
// never iterates entities) is not an entity system and is left out, otherwise its empty query would
// "match" every entity.
#include <format>
#include <span>
#include <string>
#include <vector>

namespace xscene
{
    bool IsInternalComponent(const xecs::component::type::info* pInfo) noexcept;   // xscene_prefab_overrides.h

    // Every user-visible component on an archetype: data, share and tags (internal bookkeeping excluded).
    inline std::vector<const xecs::component::type::info*> UserComponents(const xecs::archetype::instance& Archetype) noexcept
    {
        std::vector<const xecs::component::type::info*> Out;
        for (auto p : Archetype.getDataComponentInfos())  Out.push_back(p);
        for (auto p : Archetype.getShareComponentInfos()) Out.push_back(p);
        Archetype.AppendTagComponentInfos(Out);
        std::erase_if(Out, [](auto* p) noexcept { return xscene::IsInternalComponent(p); });
        return Out;
    }
}

namespace xscene::system_usage
{
    using access = xecs::system::type::access;
    using match  = xecs::system::type::match;

    struct system_ref
    {
        const xecs::system::type::info* m_pInfo     = nullptr;
        bool                            m_bUpdate   = true;     // false = notifier (runs on create/destroy/move events)
        bool                            m_bEnabled  = true;
        int                             m_Order     = -1;       // execution order among update systems
    };

    struct change
    {
        std::vector<system_ref> m_Starts;
        std::vector<system_ref> m_Stops;
        bool empty() const noexcept { return m_Starts.empty() && m_Stops.empty(); }
    };

    inline const char* AccessLabel(access A) noexcept
    {
        switch (A)
        {
        case access::READ:  return "reads";
        case access::WRITE: return "writes";
        default:            return "filter";
        }
    }

    inline const char* MatchLabel(match M) noexcept
    {
        switch (M)
        {
        case match::MUST:       return "must";
        case match::ONE_OF:     return "one-of";
        case match::NONE_OF:    return "none-of";
        default:                return "if-present";
        }
    }

    inline const char* SystemName(const system_ref& S) noexcept
    {
        return S.m_pInfo->m_pName ? S.m_pInfo->m_pName : "(unnamed system)";
    }

    inline std::vector<system_ref> AllSystems(xecs::game_mgr::instance& GameMgr, bool bEntitySystemsOnly = true) noexcept
    {
        auto& Mgr  = GameMgr.m_SystemMgr;
        auto  Rows = Mgr.GetUpdateSystemRows();   // index-aligned with m_UpdaterSystems
        std::vector<system_ref> Out;
        for (std::size_t i = 0; i < Mgr.m_UpdaterSystems.size(); ++i)
        {
            auto* pInfo = Mgr.m_UpdaterSystems[i].first;
            if (bEntitySystemsOnly && pInfo->m_Access.empty()) continue;
            Out.push_back({ pInfo, true, i < Rows.size() ? Rows[i].m_bEnabled : true, static_cast<int>(i) });
        }
        for (auto& E : Mgr.m_NotifierSystems)
        {
            if (bEntitySystemsOnly && E.first->m_Access.empty()) continue;
            Out.push_back({ E.first, false, true, -1 });
        }
        return Out;
    }

    // Same Compare overloads the runtime uses: Search (update systems) also checks exclusive tags,
    // notifier registration (system::mgr::OnNewArchetype) does not.
    inline bool Matches(const system_ref& S, const xecs::tools::bits& Bits) noexcept
    {
        if (S.m_bUpdate)
        {
            xecs::tools::bits Exclusive;
            Exclusive.setupAnd(Bits, xecs::component::mgr::s_Registry.m_ExclusiveTagsBits);
            return S.m_pInfo->m_Query.Compare(Bits, Exclusive);
        }
        return S.m_pInfo->m_Query.Compare(Bits);
    }

    inline const xecs::system::type::component_access* FindAccess(const system_ref& S, std::uint64_t ComponentGuid) noexcept
    {
        for (auto& A : S.m_pInfo->m_Access)
            if (A.m_ComponentGuid.m_Value == ComponentGuid) return &A;
        return nullptr;
    }

    inline bool HasComponent(const xecs::tools::bits& Bits, std::uint64_t ComponentGuid) noexcept
    {
        auto* pInfo = xecs::component::mgr::findComponentTypeInfo(xecs::component::type::guid{ ComponentGuid });
        return pInfo && Bits.getBit(pInfo->m_BitID);
    }

    // A system's own declaration, one component per line ("must      writes  Transform").
    inline std::string DescribeDeclaration(const xecs::system::type::info& Info, const char* pIndent = "  ") noexcept
    {
        if (Info.m_Access.empty()) return std::format("{}(declares no components - does not iterate entities)\n", pIndent);
        std::string Out;
        for (auto& A : Info.m_Access)
            Out += std::format("{}{:<10} {:<7} {}\n", pIndent, MatchLabel(A.m_Match), AccessLabel(A.m_Access), A.m_pComponentName ? A.m_pComponentName : "?");
        return Out;
    }

    // What starts/stops running on an entity whose archetype is Before if Info is added (bAdd) or removed.
    inline change WhatIf(const std::vector<system_ref>& Systems, const xecs::tools::bits& Before, const xecs::component::type::info& Info, bool bAdd) noexcept
    {
        xecs::tools::bits After = Before;
        if (bAdd) After.setBit(Info.m_BitID);
        else      After.clearBit(Info.m_BitID);

        change Out;
        for (auto& S : Systems)
        {
            const bool bBefore = Matches(S, Before);
            const bool bAfter  = Matches(S, After);
            if (!bBefore &&  bAfter) Out.m_Starts.push_back(S);
            if ( bBefore && !bAfter) Out.m_Stops.push_back(S);
        }
        return Out;
    }

    // Why S doesn't match Bits, in the system's own declared terms. Empty if it does match.
    inline std::string WhyNotRunning(const system_ref& S, const xecs::tools::bits& Bits) noexcept
    {
        if (Matches(S, Bits)) return {};
        std::string Missing, Blocking, OneOf;
        bool        bAnyOneOfPresent = false;
        for (auto& A : S.m_pInfo->m_Access)
        {
            const bool bHas = HasComponent(Bits, A.m_ComponentGuid.m_Value);
            if (A.m_Match == match::MUST    && !bHas) Missing  += std::format("{}{}", Missing.empty()  ? "" : ", ", A.m_pComponentName);
            if (A.m_Match == match::NONE_OF &&  bHas) Blocking += std::format("{}{}", Blocking.empty() ? "" : ", ", A.m_pComponentName);
            if (A.m_Match == match::ONE_OF)
            {
                OneOf += std::format("{}{}", OneOf.empty() ? "" : " | ", A.m_pComponentName);
                bAnyOneOfPresent |= bHas;
            }
        }
        std::string Out;
        if (!Missing.empty())                   Out += "missing " + Missing;
        if (!Blocking.empty())                  Out += std::format("{}excluded by {}", Out.empty() ? "" : "; ", Blocking);
        if (!OneOf.empty() && !bAnyOneOfPresent) Out += std::format("{}needs one of {}", Out.empty() ? "" : "; ", OneOf);
        if (Out.empty())                        Out = "excluded by an exclusive tag";
        return Out;
    }

    inline std::string JoinNames(const std::vector<system_ref>& Systems) noexcept
    {
        std::string Out;
        for (auto& S : Systems) Out += std::format("{}{}", Out.empty() ? "" : ", ", SystemName(S));
        return Out;
    }

    // "Physics (writes), Render (reads)" - the systems whose declaration mentions this component.
    inline std::string UsedBy(const std::vector<system_ref>& Systems, std::uint64_t ComponentGuid) noexcept
    {
        std::string Out;
        for (auto& S : Systems)
            if (auto* pA = FindAccess(S, ComponentGuid))
                Out += std::format("{}{} ({}{})", Out.empty() ? "" : ", ", SystemName(S), AccessLabel(pA->m_Access),
                                   pA->m_Match == match::NONE_OF ? ", excludes" : "");
        return Out;
    }

    // Tooltip/CLI text for adding (bAdd) or removing a component on an entity with archetype Bits.
    inline std::string DescribeChange(const std::vector<system_ref>& Systems, const xecs::tools::bits& Bits, const xecs::component::type::info& Info, bool bAdd) noexcept
    {
        const auto  Change = WhatIf(Systems, Bits, Info, bAdd);
        const auto  Used   = UsedBy(Systems, Info.m_Guid.m_Value);
        std::string Out    = std::format("{} {}:\n", bAdd ? "Adding" : "Removing", Info.m_pName ? Info.m_pName : "?");
        if (Change.empty())            Out += "  no system starts or stops on this entity\n";
        if (!Change.m_Starts.empty())  Out += "  starts: " + JoinNames(Change.m_Starts) + "\n";
        if (!Change.m_Stops.empty())   Out += "  stops:  " + JoinNames(Change.m_Stops)  + "\n";
        Out += Used.empty() ? "Not used by any system." : "Used by: " + Used;
        return Out;
    }

    // Everything about one entity (Bits = its archetype) - the CLI's DescribeEntity section and the
    // Inspector popup's copyable text.
    inline std::string DescribeEntitySystems(xecs::game_mgr::instance& GameMgr, const xecs::tools::bits& Bits, std::span<const xecs::component::type::info* const> Components) noexcept
    {
        const auto  Systems = AllSystems(GameMgr);
        std::string Out     = "Systems running on this entity:\n";
        bool        bAny    = false;
        for (auto& S : Systems)
        {
            if (!Matches(S, Bits)) continue;
            bAny = true;
            Out += std::format("  {}  [{}{}]\n", SystemName(S), S.m_bUpdate ? std::format("update #{}", S.m_Order) : std::string("notifier"), S.m_bEnabled ? "" : ", DISABLED");
            for (auto A : { access::WRITE, access::READ })
            {
                std::string Names;
                for (auto& E : S.m_pInfo->m_Access)
                    if (E.m_Access == A && HasComponent(Bits, E.m_ComponentGuid.m_Value))
                        Names += std::format("{}{}", Names.empty() ? "" : ", ", E.m_pComponentName);
                if (!Names.empty()) Out += std::format("    {:6}  {}\n", AccessLabel(A), Names);
            }
        }
        if (!bAny) Out += "  (none)\n";

        Out += "Systems NOT running on this entity:\n";
        bAny = false;
        for (auto& S : Systems)
        {
            auto Why = WhyNotRunning(S, Bits);
            if (Why.empty()) continue;
            bAny = true;
            Out += std::format("  {}: {}\n", SystemName(S), Why);
        }
        if (!bAny) Out += "  (none)\n";

        Out += "Removing a component would:\n";
        bAny = false;
        for (auto* pInfo : Components)
        {
            const auto Change = WhatIf(Systems, Bits, *pInfo, false);
            if (Change.empty()) continue;
            bAny = true;
            Out += std::format("  {}:", pInfo->m_pName ? pInfo->m_pName : "?");
            if (!Change.m_Stops.empty())  Out += " stop "  + JoinNames(Change.m_Stops);
            if (!Change.m_Starts.empty()) Out += std::format("{} start {}", Change.m_Stops.empty() ? "" : ";", JoinNames(Change.m_Starts));
            Out += "\n";
        }
        if (!bAny) Out += "  (no effect on any system)\n";
        return Out;
    }
}

#endif // XSCENE_SYSTEM_USAGE_H
