#pragma once

// The shared `name` component every entity/prefab/tree label relies on.
// Split out of xscene_entity_inspector_bridge.h; included from there at the position this code used to occupy.
namespace xscene
{
    //---------------------------------------------------------------------------
    // Shared starter component - every entity the Level tree/prefab machinery below labels, names,
    // or searches by relies on THIS component being present (see e.g. ResolveEntityReference,
    // CreatePrefabFromGroupRoot, DetermineGroupRoot's synthetic-root naming, the tree row label
    // logic) - promoted from "just E29's own demo content" into the kit itself for that reason. A
    // future editor built on this kit registers it exactly like any other component
    // (GameMgr.RegisterComponents<xscene::name, ...>()).
    //---------------------------------------------------------------------------

    struct name
    {
        // The guid is the one the component had as e29::name (derived from that name), which saved scenes refer to.
        constexpr static auto typedef_v = xecs::component::type::data{ .m_Guid = xecs::component::type::guid{ 0xA063D0D3500FA785ull }, .m_pName = "Name" };

        std::string m_Value = "Entity";

        XPROPERTY_DEF
        ( "Name", name
        , obj_member<"Value", &name::m_Value>
        )
    };
    XPROPERTY_REG(name)

}
