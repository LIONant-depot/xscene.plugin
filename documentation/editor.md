# source/Editor - the scene editor

Editing the entities of a scene: what a level editor, a prefab editor or any other editor that opens scenes shares. It
compiles into the editor application. It knows nothing about levels, Play or the game module: those belong to the editor
that composes it.

An editor includes `plugins/xscene.plugin/source/Editor/xscene_editor.h`, which pulls in every header in the order they
need. The headers are not standalone. They need xECSV2, `xeditor` and `xresource_pipeline_v2/source/editor`.

## The context

`xscene::scene_context` is what an editor hands to the scene code: its `scene_state` (open scenes, selection), the
`unique_ptr` that owns its world, and the `xundo::system` of its document. The world is recreated on every Game.dll
reload, so the context reads it through its owner each time (`World()`).

The editor provides the context to `xeditor::host` (`provide<xscene::scene_context>`) so code with no session of its own,
such as a drag-drop handler, can find it. An editor with more state derives from the context and from `scene_state`; the
scene code only ever sees the base.

## Commands

Every edit is an undoable `xundo` command, so the AI/CLI can do everything the UI can. The commands derive from
`xscene::commands::scene_command` and read the scene context through it. The editor that builds the command set must give
them a `scene_context*` as their database.

| Header | Commands |
|---|---|
| `xscene_commands_selection.h` | `Select`, `ToggleMultiSelect`, `ClearSelection` |
| `xscene_commands_property_edit.h` | `SetProperty`, `RevertOverride` |
| `xscene_commands_component_edit.h` | `AddComponent`, `RemoveComponent` |
| `xscene_commands_entity_lifecycle.h` | `CreateEntity`, `DeleteEntity` |
| `xscene_commands_entity_reference.h` | `SetEntityReference` |
| `xscene_commands_scene_organization.h` | `CreateFolder`, `DeleteFolder`, `MoveToFolder`, `InstantiatePrefab` |
| `xscene_commands_apply_overrides.h` | `ApplyOverrides`, `RevertHierarchyOverrides`, `RevertAllOverrides` |
| `xscene_commands_make_prefab.h` | `MakePrefab`, `MakePrefabVariant` |

Guids and ids travel on the command line as hex, and free text as base64 (`xscene_command_context.h`).

## The rest

| Header | What it is |
|---|---|
| `xscene_name.h` | the `Name` component every entity is labelled with. Its guid is fixed: saved scenes refer to it |
| `xscene_context.h` | `scene_state`, `scene_context` |
| `xscene_component_display.h` | each loaded component's category and priority, filled by the editor from the game module |
| `xscene_scene_ops.h` | minting entity and folder ids, folder membership, releasing a scene |
| `xscene_dependencies.h` | opening a scene with its parents, and refusing dependency cycles |
| `xscene_prefab_overrides.h`, `xscene_prefab_authoring.h` | prefab lookup and override bookkeeping; creating, instancing and deleting prefabs |
| `xscene_create_menu.h` | the New Entity / New Folder menu items |
| `xscene_entity_inspector_bridge.h` | wires the property inspector to the override and entity-reference commands |
| `xscene_panel_entity_properties.h`, `xscene_panel_component_selector.h` | the Entity Properties panel and its Add Component popup |

## Known coupling to clean up

- Component types get their guid from their C++ name (`__FUNCSIG__`) unless they set one. Moving a component to another
  namespace changes its guid and breaks every saved scene, so `Name` carries an explicit guid.
