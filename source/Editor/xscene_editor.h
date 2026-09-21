#ifndef XSCENE_EDITOR_H
#define XSCENE_EDITOR_H
#pragma once

// The scene editor: editing the entities of a scene through undoable commands, prefab authoring and instancing, and the
// Entity Properties and Add Component panels. The headers are meant to be included in this order in one translation unit.
// Needs xECSV2, xeditor and xresource_pipeline_v2/source/editor; knows nothing about levels or Play.
#include "plugins/xscene.plugin/source/Editor/xscene_name.h"
#include "plugins/xscene.plugin/source/Editor/xscene_context.h"
#include "plugins/xscene.plugin/source/Editor/xscene_component_display.h"
#include "plugins/xscene.plugin/source/Editor/xscene_scene_ops.h"
#include "plugins/xscene.plugin/source/Editor/xscene_dependencies.h"
#include "plugins/xscene.plugin/source/Editor/xscene_prefab_overrides.h"
#include "plugins/xscene.plugin/source/Editor/xscene_prefab_authoring.h"
#include "plugins/xscene.plugin/source/Editor/xscene_command_context.h"
#include "plugins/xscene.plugin/source/Editor/xscene_create_menu.h"
#include "plugins/xscene.plugin/source/Editor/xscene_commands_property_edit.h"
#include "plugins/xscene.plugin/source/Editor/xscene_commands_entity_reference.h"
#include "plugins/xscene.plugin/source/Editor/xscene_commands_component_edit.h"
#include "plugins/xscene.plugin/source/Editor/xscene_commands_entity_lifecycle.h"
#include "plugins/xscene.plugin/source/Editor/xscene_commands_selection.h"
#include "plugins/xscene.plugin/source/Editor/xscene_commands_scene_organization.h"
#include "plugins/xscene.plugin/source/Editor/xscene_commands_apply_overrides.h"
#include "plugins/xscene.plugin/source/Editor/xscene_commands_make_prefab.h"
#include "plugins/xscene.plugin/source/Editor/xscene_entity_inspector_bridge.h"
#include "plugins/xscene.plugin/source/Editor/xscene_panel_component_selector.h"
#include "plugins/xscene.plugin/source/Editor/xscene_panel_entity_properties.h"

#endif // XSCENE_EDITOR_H
