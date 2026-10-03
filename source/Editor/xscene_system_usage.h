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
#include "dependencies/xLIONCore/src/game/xlioncore_editor.h"
#include <algorithm>
#include <format>
#include <span>
#include <string>
#include <vector>

namespace xscene
{
    bool IsInternalComponent(const xecs::component::type::info* pInfo) noexcept;   // xscene_prefab_overrides.h

    // Every user-visible component of an entity: data, share and tags (internal bookkeeping excluded).
    inline std::vector<const xecs::component::type::info*> UserComponents(xlioncore::xECSEditor& Ecs, xecs::component::entity Entity) noexcept
    {
        std::vector<const xecs::component::type::info*> Out, Share, Tags;
        Ecs.ComponentTypesOf(Entity, Out, Share, Tags);
        Out.insert(Out.end(), Share.begin(), Share.end());
        Out.insert(Out.end(), Tags.begin(), Tags.end());
        std::erase_if(Out, [](auto* p) noexcept { return xscene::IsInternalComponent(p); });
        return Out;
    }
}

namespace xscene::system_usage
{
    using access = xecs::system::type::access;
    using match  = xecs::system::type::match;

    using system_ref = xlioncore::system_view;

    // The component types an entity has (every kind, internal ones too), and the copy of the core that says which systems run on them: what the systems are matched against, by guid, so that
    // nothing here reads a bit id.
    struct component_set
    {
        xlioncore::xECSEditor*                      m_pEcs = nullptr;
        std::vector<xecs::component::type::guid>    m_Guids;

        bool Has(std::uint64_t Guid) const noexcept { return std::any_of(m_Guids.begin(), m_Guids.end(), [&](auto G) noexcept { return G.m_Value == Guid; }); }
        void Add(xecs::component::type::guid G) noexcept { if (!Has(G.m_Value)) m_Guids.push_back(G); }
        void Remove(xecs::component::type::guid G) noexcept { std::erase_if(m_Guids, [&](auto X) noexcept { return X.m_Value == G.m_Value; }); }
    };

    inline component_set SetOf(xecs::game_mgr::instance& GameMgr, xecs::component::entity Entity) noexcept
    {
        component_set Out;
        Out.m_pEcs = &xlioncore::Ecs(GameMgr);
        std::vector<const xecs::component::type::info*> Data, Share, Tags;
        Out.m_pEcs->ComponentTypesOf(Entity, Data, Share, Tags);
        for (auto* p : Data)  Out.m_Guids.push_back(p->m_Guid);
        for (auto* p : Share) Out.m_Guids.push_back(p->m_Guid);
        for (auto* p : Tags)  Out.m_Guids.push_back(p->m_Guid);
        return Out;
    }

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
        std::vector<system_ref> Out;
        xlioncore::Ecs(GameMgr).ListSystems(Out, bEntitySystemsOnly);
        return Out;
    }

    // The scheduler's own test (in the copy of the core the set belongs to): update systems also check the exclusive tags, notifiers do not.
    inline bool Matches(const system_ref& S, const component_set& Set) noexcept
    {
        return Set.m_pEcs->SystemMatches(S, Set.m_Guids);
    }

    inline const xecs::system::type::component_access* FindAccess(const system_ref& S, std::uint64_t ComponentGuid) noexcept
    {
        for (auto& A : S.m_pInfo->m_Access)
            if (A.m_ComponentGuid.m_Value == ComponentGuid) return &A;
        return nullptr;
    }

    inline bool HasComponent(const component_set& Set, std::uint64_t ComponentGuid) noexcept
    {
        return Set.Has(ComponentGuid);
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
    inline change WhatIf(const std::vector<system_ref>& Systems, const component_set& Before, const xecs::component::type::info& Info, bool bAdd) noexcept
    {
        component_set After = Before;
        if (bAdd) After.Add(Info.m_Guid);
        else      After.Remove(Info.m_Guid);

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
    inline std::string WhyNotRunning(const system_ref& S, const component_set& Bits) noexcept
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
    inline std::string DescribeChange(const std::vector<system_ref>& Systems, const component_set& Bits, const xecs::component::type::info& Info, bool bAdd) noexcept
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

    // Everything about one entity (Bits = its component types) - the CLI's DescribeEntity section and the
    // Inspector popup's copyable text.
    inline std::string DescribeEntitySystems(xecs::game_mgr::instance& GameMgr, const component_set& Bits, std::span<const xecs::component::type::info* const> Components) noexcept
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
