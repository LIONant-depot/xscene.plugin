#pragma once

// Scene bookkeeping the scene editing code shares: minting entity and folder ids, folder membership, and releasing a scene.
namespace xscene
{
    // GUID-like rather than sequential (was "Max + 1"): a random id means two branches each creating
    // an unrelated new entity independently essentially never end up minting the same permanent_id, so
    // merging their scene folders afterward doesn't collide two different entities onto one file. Only
    // needs to be unique WITHIN this one scene (checked below) - not globally across all history, so a
    // folded-down 32-bit value is enough entropy for that; xresource::guid_generator::Instance64()
    // already mixes timestamp/thread/machine/random bits, reused here rather than inventing a second
    // id-generation scheme.
    xecs::scene::permanent_id NextFreeEntityId(xecs::scene::instance& Scene) noexcept
    {
        for(;;)
        {
            const auto     Full = xresource::guid_generator::Instance64();
            const auto     Id   = static_cast<xecs::scene::permanent_id>( (Full >> 32) ^ (Full & 0xFFFFFFFFull) );
            if( Id == xecs::scene::invalid_permanent_id_v )                    continue;
            if( Scene.m_LocalToRuntime.contains(Id) )                          continue;
            return Id;
        }
    }

    // Same GUID-like scheme as NextFreeEntityId, checked against the scene's own folder ids instead
    // of its entities - a separate id space, but the same merge-collision reasoning applies.
    xecs::scene::folder_id NextFreeFolderId(xecs::scene::instance& Scene) noexcept
    {
        for(;;)
        {
            const auto Full = xresource::guid_generator::Instance64();
            const auto Id   = static_cast<xecs::scene::folder_id>( (Full >> 32) ^ (Full & 0xFFFFFFFFull) );
            if( Id == xecs::scene::invalid_folder_id_v ) continue;
            if( std::find_if(Scene.m_Folders.begin(), Scene.m_Folders.end(), [&](auto& F) noexcept { return F.m_Id == Id; }) != Scene.m_Folders.end() )
                continue;
            return Id;
        }
    }

    // Forward declarations - FindFolderContaining/PruneEmptyFolderChain are defined below, but
    // ReparentEntityIntoFolder (the one place any entity ever LEAVES a folder) needs both to detect
    // and clean up a folder left empty by that departure.
    xecs::scene::folder_id FindFolderContaining(xecs::scene::instance& Scene, xecs::scene::permanent_id Id) noexcept;
    void PruneEmptyFolderChain(xecs::scene::instance& Scene, xecs::scene::folder_id Id) noexcept;

    // Moves Id into TargetFolder (invalid_folder_id_v = "loose", no folder), removing it from
    // whichever folder currently lists it first - folders own their membership by containment (see
    // xecs_scene.h's folder comment), so "reparent" is just "erase from the old owner, append to the
    // new one" rather than updating any per-entity back-pointer. Whatever folder Id is leaving gets
    // pruned afterward if that departure left it (and, cascading upward, any now-childless ancestor
    // folder) completely empty - direct user report of "fake/empty folders" accumulating, e.g. every
    // time an entity is deleted out of the last-remaining folder that held it.
    void ReparentEntityIntoFolder(xecs::scene::instance& Scene, xecs::scene::permanent_id Id, xecs::scene::folder_id TargetFolder) noexcept
    {
        const auto OldFolder = FindFolderContaining(Scene, Id);

        for( auto& F : Scene.m_Folders )
            std::erase(F.m_Entities, Id);

        if( OldFolder != xecs::scene::invalid_folder_id_v )
            PruneEmptyFolderChain(Scene, OldFolder);

        if( TargetFolder == xecs::scene::invalid_folder_id_v ) return;
        if( auto It = std::find_if(Scene.m_Folders.begin(), Scene.m_Folders.end(), [&](auto& F) noexcept { return F.m_Id == TargetFolder; }); It != Scene.m_Folders.end() )
            It->m_Entities.push_back(Id);
    }

    // Which folder (if any) currently lists Id as a member - invalid_folder_id_v if Id is loose or
    // parented (folders own membership by containment, so this is a linear scan, not a lookup).
    xecs::scene::folder_id FindFolderContaining(xecs::scene::instance& Scene, xecs::scene::permanent_id Id) noexcept
    {
        if( auto It = std::find_if(Scene.m_Folders.begin(), Scene.m_Folders.end(), [&](auto& F) noexcept { return std::find(F.m_Entities.begin(), F.m_Entities.end(), Id) != F.m_Entities.end(); }); It != Scene.m_Folders.end() )
            return It->m_Id;
        return xecs::scene::invalid_folder_id_v;
    }

    // Deletes Id if it's now completely empty (no member entities AND no child folder still parented
    // under it), then repeats for its own parent, walking up until a non-empty/non-childless folder
    // is hit or the root is reached - keeps the tree free of folders left behind purely because
    // whatever used to justify their existence (an entity, a now-pruned child folder) is gone.
    void PruneEmptyFolderChain(xecs::scene::instance& Scene, xecs::scene::folder_id Id) noexcept
    {
        while( Id != xecs::scene::invalid_folder_id_v )
        {
            auto It = std::find_if(Scene.m_Folders.begin(), Scene.m_Folders.end(), [&](auto& F) noexcept { return F.m_Id == Id; });
            if( It == Scene.m_Folders.end() ) return;

            if( It->m_Entities.empty() == false ) return;

            const bool bHasChildFolder = std::any_of(Scene.m_Folders.begin(), Scene.m_Folders.end(), [&](auto& F) noexcept { return F.m_Parent == Id; });
            if( bHasChildFolder ) return;

            const auto ParentId = It->m_Parent;
            Scene.m_Folders.erase(It);
            Id = ParentId;
        }
    }

    // Folders are purely organizational - deleting one must never delete gameplay content. Its
    // member entities and any child folders are promoted up to ITS OWN parent (which may itself be
    // root/invalid) rather than being deleted or left dangling.
    void DeleteFolder(xecs::scene::instance& Scene, xecs::scene::folder_id Id) noexcept
    {
        auto It = std::find_if(Scene.m_Folders.begin(), Scene.m_Folders.end(), [&](auto& F) noexcept { return F.m_Id == Id; });
        if( It == Scene.m_Folders.end() ) return;

        const auto ParentId       = It->m_Parent;
        const auto EntitiesToMove = std::move(It->m_Entities);
        Scene.m_Folders.erase(It);

        for( auto& F : Scene.m_Folders )
            if( F.m_Parent == Id ) F.m_Parent = ParentId;

        if( ParentId != xecs::scene::invalid_folder_id_v )
        {
            if( auto ParentIt = std::find_if(Scene.m_Folders.begin(), Scene.m_Folders.end(), [&](auto& F) noexcept { return F.m_Id == ParentId; }); ParentIt != Scene.m_Folders.end() )
                for( auto EId : EntitiesToMove ) ParentIt->m_Entities.push_back(EId);
        }
        // ParentId == invalid: entities are already "loose" simply by not being listed anywhere.
    }

    // Releases one specific scene's residency and removes it from State.m_OpenScenes - unlike the old
    // single-current-scene design, this never gets called implicitly when another scene opens; only
    // when a scene is explicitly removed from the Level (any number of OTHER scenes stay open).
    void CloseScene(xecs::game_mgr::instance& GameMgr, scene_state& State, xecs::scene::guid Guid) noexcept
    {
        auto It = std::find(State.m_OpenScenes.begin(), State.m_OpenScenes.end(), Guid);
        if (It == State.m_OpenScenes.end()) return;

        GameMgr.m_SceneMgr.ReleaseLoad(Guid);
        State.m_OpenScenes.erase(It);

        if (State.m_SelectedEntityScene == Guid)
        {
            State.m_SelectedEntityId    = xecs::scene::invalid_permanent_id_v;
            State.m_SelectedEntity      = {};
            State.m_SelectedEntityScene = {};
        }

        // The primary selection was already scrubbed above, but the MULTI-select set/order (a
        // completely separate pair of fields, see their own comment) never was - every id in it
        // belonged to a scene that just got fully unloaded, so a later Make-Prefab grouping could
        // silently pick up stale, now-nonexistent ids. Whole-scene close means every one of those ids
        // is gone regardless of which entity it was, so this just clears the set outright rather than
        // checking survival one id at a time.
        if (State.m_MultiSelectScene == Guid)
        {
            State.m_MultiSelectedEntityIds.clear();
            State.m_MultiSelectOrder.clear();
        }
    }
}
