#pragma once

#include "core/object/ref_counted.h"

class Node;

// Explains where saved property values come from and who uses scenes/resources.
// Used by --explain-property, --find-overrides and --resource-users.
class AgentInspector : public RefCounted {
	GDCLASS(AgentInspector, RefCounted);

protected:
	static void _bind_methods();

public:
	// Every layer that sets p_property on the node at p_node_path in p_scene, highest priority first:
	// this scene, then inherited/instanced scenes, then the script default, then the class default.
	static Dictionary explain_scene_property(const String &p_scene, const NodePath &p_node_path, const StringName &p_property);
	// Same, for a live node: explains its owner scene's saved values and adds the current runtime value.
	static Dictionary explain_property(Node *p_node, const StringName &p_property);
	// Every place in the project that instances or inherits p_scene, with the properties it overrides.
	static Array find_overrides(const String &p_scene);
	// Every scene or resource file that references p_resource, with the node properties using it.
	static Array resource_users(const String &p_resource);

	// Saves the last rendered frame of the main window as a PNG (plus a .json with frame,
	// camera and renderer info) and returns that info. Needs real rendering (--offscreen, not --headless).
	static Dictionary capture(const String &p_path, double p_scale = 1.0);
	// What is under a pixel of a viewport (the main window if null): the UI control,
	// following SubViewportContainers into their SubViewports, then the 3D or 2D physics shape.
	static Dictionary pick(const Vector2 &p_position, Object *p_viewport = nullptr);

	static String format_explain(const Dictionary &p_result);
	static String format_list(const Array &p_entries, const String &p_what);

	// Entry point for the CLI flags; returns the process exit code.
	static int run_cli(const String &p_command);
};
