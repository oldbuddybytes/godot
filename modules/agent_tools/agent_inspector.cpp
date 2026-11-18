#include "agent_inspector.h"

#include "agent_common.h"

#include "core/io/json.h"
#include "core/object/class_db.h"
#include "core/object/script_language.h"
#include "core/os/os.h"
#include "scene/main/node.h"
#include "scene/property_utils.h"
#include "scene/resources/packed_scene.h"

namespace {

const Vector<String> SCENE_EXTS = { "tscn", "scn" };
const Vector<String> RESOURCE_EXTS = { "tscn", "scn", "tres", "res" };

// "./Door/Shape" -> "Door/Shape"; "." stays ".".
String _clean_path(const String &p_path) {
	String path = p_path;
	if (path.begins_with("./")) {
		path = path.substr(2);
	}
	return path.is_empty() ? "." : path;
}

// Short, single-line text for a value. Resources show their file instead of their contents.
String _value_text(const Variant &p_value) {
	if (p_value.get_type() == Variant::OBJECT) {
		Object *obj = p_value;
		if (!obj) {
			return "null";
		}
		Resource *res = Object::cast_to<Resource>(obj);
		if (res) {
			const String path = res->get_path();
			if (path.is_empty()) {
				return vformat("<%s>", res->get_class());
			}
			if (path.contains("::")) {
				return vformat("<%s sub-resource in %s>", res->get_class(), path.get_slice("::", 0));
			}
			return path;
		}
		return vformat("<%s>", obj->get_class());
	}
	String text = p_value.get_construct_string().replace("\n", " ");
	if (text.length() > 120) {
		text = text.substr(0, 117) + "...";
	}
	return text;
}

Dictionary _error(const String &p_message) {
	Dictionary d;
	d["error"] = p_message;
	return d;
}

Dictionary _layer(const Variant &p_value, const String &p_source, const String &p_kind) {
	Dictionary d;
	d["value"] = _value_text(p_value);
	d["source"] = p_source;
	d["kind"] = p_kind;
	return d;
}

String _resolve_path(const String &p_path) {
	if (p_path.begins_with("uid://")) {
		return agent_resolve_dependency(p_path);
	}
	return p_path;
}

Ref<PackedScene> _load_scene(const String &p_path) {
	ErrorCapture capture;
	return ResourceLoader::load(p_path, "PackedScene");
}

PackedStringArray _property_list(const Ref<SceneState> &p_state, int p_idx) {
	PackedStringArray props;
	for (int i = 0; i < p_state->get_node_property_count(p_idx); i++) {
		props.push_back(vformat("%s=%s", p_state->get_node_property_name(p_idx, i), _value_text(p_state->get_node_property_value(p_idx, i))));
	}
	return props;
}

Dictionary _entry(const String &p_file, const String &p_node, const String &p_kind, const PackedStringArray &p_details) {
	Dictionary d;
	d["file"] = p_file;
	d["node"] = p_node;
	d["kind"] = p_kind;
	d["details"] = p_details;
	return d;
}

} // namespace

Dictionary AgentInspector::explain_scene_property(const String &p_scene, const NodePath &p_node_path, const StringName &p_property) {
	const String scene_path = _resolve_path(p_scene);
	Ref<PackedScene> packed = _load_scene(scene_path);
	if (packed.is_null()) {
		return _error("Could not load scene " + p_scene);
	}

	Node *root = nullptr;
	{
		ErrorCapture capture;
		root = packed->instantiate(PackedScene::GEN_EDIT_STATE_INSTANCE);
	}
	if (!root) {
		return _error("Could not instantiate scene " + scene_path);
	}

	const String node_path = _clean_path(String(p_node_path));
	Node *node = node_path == "." ? root : root->get_node_or_null(NodePath(node_path));
	if (!node) {
		memdelete(root);
		return _error(vformat("Node %s not found in %s", node_path, scene_path));
	}
	bool valid = false;
	const Variant effective = node->get(p_property, &valid);
	if (!valid) {
		const String cls = node->get_class();
		memdelete(root);
		return _error(vformat("%s (%s) has no property %s", node_path, cls, p_property));
	}

	Array layers;

	// 1. Value saved in this scene file for this node.
	const Ref<SceneState> own = packed->get_state();
	for (int i = 0; i < own->get_node_count(); i++) {
		if (_clean_path(String(own->get_node_path(i))) != node_path) {
			continue;
		}
		bool found = false;
		bool deferred = false;
		const Variant value = own->get_property_value(i, p_property, found, deferred);
		if (found) {
			layers.push_back(_layer(value, scene_path, own->get_node_instance(i).is_valid() ? "instance override" : "this scene"));
		}
		break;
	}

	// 2. Values from inherited and instanced scenes, nearest first.
	const Vector<SceneState::PackState> stack = PropertyUtils::get_node_states_stack(node, root);
	for (const SceneState::PackState &ps : stack) {
		bool found = false;
		bool deferred = false;
		const Variant value = ps.state->get_property_value(ps.node, p_property, found, deferred);
		if (found) {
			layers.push_back(_layer(value, ps.state->get_path(), "base scene"));
		}
	}

	// 3. Script export default, then 4. class default.
	Ref<Script> script = node->get_script();
	if (script.is_valid()) {
		Variant value;
		if (script->get_property_default_value(p_property, value)) {
			layers.push_back(_layer(value, script->get_path(), "script default"));
		}
	}
	bool class_valid = false;
	const Variant class_default = ClassDB::class_get_default_property_value(node->get_class_name(), p_property, &class_valid);
	if (class_valid) {
		layers.push_back(_layer(class_default, node->get_class(), "class default"));
	}

	Dictionary result;
	result["scene"] = scene_path;
	result["node"] = node_path;
	result["property"] = String(p_property);
	result["value"] = _value_text(effective);
	result["layers"] = layers;
	PackedStringArray notes;
	if (script.is_valid()) {
		notes.push_back("Node has a script (" + script->get_path() + "); it may change this value at runtime.");
	}
	if (effective.get_type() == Variant::OBJECT) {
		const Resource *res = Object::cast_to<Resource>(effective.get_validated_object());
		if (res && !res->get_path().is_empty() && !res->get_path().contains("::")) {
			notes.push_back("Shared resource file: editing it affects every user (see --resource-users).");
		}
	}
	if (!notes.is_empty()) {
		result["notes"] = notes;
	}
	memdelete(root);
	return result;
}

Dictionary AgentInspector::explain_property(Node *p_node, const StringName &p_property) {
	ERR_FAIL_NULL_V(p_node, _error("Node is null"));
	// The topmost owner with a scene file is the scene this node was saved in.
	Node *owner = p_node;
	while (owner->get_owner()) {
		owner = owner->get_owner();
	}
	if (owner->get_scene_file_path().is_empty()) {
		return _error("Node is not part of a saved scene");
	}
	Dictionary result = explain_scene_property(owner->get_scene_file_path(), owner->get_path_to(p_node), p_property);
	if (!result.has("error")) {
		const String runtime = _value_text(p_node->get(p_property));
		result["runtime_value"] = runtime;
		result["changed_at_runtime"] = runtime != String(result["value"]);
	}
	return result;
}

Array AgentInspector::find_overrides(const String &p_scene) {
	const String target = _resolve_path(p_scene);
	Vector<String> files;
	agent_collect_files("res://", SCENE_EXTS, files);
	Array out;
	for (const String &file : files) {
		if (file == target) {
			continue;
		}
		Ref<PackedScene> packed = _load_scene(file);
		if (packed.is_null()) {
			continue;
		}
		const Ref<SceneState> state = packed->get_state();
		const bool inherits = state->get_base_scene_state().is_valid() && state->get_base_scene_state()->get_path() == target;
		Vector<String> instance_paths;
		for (int i = 0; i < state->get_node_count(); i++) {
			const String path = _clean_path(String(state->get_node_path(i)));
			const Ref<PackedScene> instance = state->get_node_instance(i);
			if (i == 0 && inherits) {
				// The root of an inherited scene is stored as an instance of its base scene.
				out.push_back(_entry(file, path, "inherits", _property_list(state, i)));
			} else if (instance.is_valid() && instance->get_path() == target) {
				instance_paths.push_back(path);
				out.push_back(_entry(file, path, "instance", _property_list(state, i)));
			} else if (instance.is_null() && state->get_node_type(i) == StringName() && state->get_node_property_count(i) > 0) {
				// A typeless node overrides a node that comes from an instance or the base scene.
				bool from_target = inherits;
				for (const String &ip : instance_paths) {
					from_target = from_target || path.begins_with(ip + "/");
				}
				if (from_target) {
					out.push_back(_entry(file, path, "child override", _property_list(state, i)));
				}
			}
		}
	}
	return out;
}

Array AgentInspector::resource_users(const String &p_resource) {
	const String target = _resolve_path(p_resource);
	Vector<String> files;
	agent_collect_files("res://", RESOURCE_EXTS, files);
	Array out;
	for (const String &file : files) {
		if (file == target) {
			continue;
		}
		List<String> deps;
		{
			ErrorCapture capture;
			ResourceLoader::get_dependencies(file, &deps, true);
		}
		bool uses = false;
		for (const String &dep : deps) {
			uses = uses || agent_resolve_dependency(dep) == target;
		}
		if (!uses) {
			continue;
		}
		PackedStringArray props;
		if (SCENE_EXTS.has(file.get_extension().to_lower())) {
			Ref<PackedScene> packed = _load_scene(file);
			if (packed.is_valid()) {
				const Ref<SceneState> state = packed->get_state();
				for (int i = 0; i < state->get_node_count(); i++) {
					for (int j = 0; j < state->get_node_property_count(i); j++) {
						const Variant value = state->get_node_property_value(i, j);
						const Resource *res = Object::cast_to<Resource>(value.get_validated_object());
						if (res && res->get_path() == target) {
							props.push_back(_clean_path(String(state->get_node_path(i))) + ":" + String(state->get_node_property_name(i, j)));
						}
					}
					const Ref<PackedScene> instance = state->get_node_instance(i);
					if (instance.is_valid() && instance->get_path() == target) {
						props.push_back(_clean_path(String(state->get_node_path(i))) + " (instance)");
					}
				}
			}
		}
		out.push_back(_entry(file, "", "uses", props));
	}
	return out;
}

String AgentInspector::format_explain(const Dictionary &p_result) {
	if (p_result.has("error")) {
		return "ERROR " + String(p_result["error"]);
	}
	String text = vformat("%s %s:%s = %s", p_result["scene"], p_result["node"], p_result["property"], p_result["value"]);
	const Array layers = p_result["layers"];
	for (int i = 0; i < layers.size(); i++) {
		const Dictionary l = layers[i];
		text += vformat("\n  %s %s  %s (%s)", i == 0 ? ">" : " ", l["value"], l["source"], l["kind"]);
	}
	if (p_result.has("runtime_value")) {
		text += "\n  runtime value: " + String(p_result["runtime_value"]);
	}
	const PackedStringArray notes = p_result.get("notes", PackedStringArray());
	for (const String &note : notes) {
		text += "\n  note: " + note;
	}
	return text;
}

String AgentInspector::format_list(const Array &p_entries, const String &p_what) {
	String text = vformat("%d %s", p_entries.size(), p_what);
	for (int i = 0; i < p_entries.size(); i++) {
		const Dictionary e = p_entries[i];
		const PackedStringArray details = e["details"];
		String line = "\n  " + String(e["file"]);
		if (!String(e["node"]).is_empty()) {
			line += " " + String(e["node"]);
		}
		line += " (" + String(e["kind"]) + ")";
		line += details.is_empty() ? String(e["kind"]) == "uses" ? "" : ": no overrides" : ": " + String(", ").join(details);
		text += line;
	}
	return text;
}

int AgentInspector::run_cli(const String &p_command) {
	Vector<String> params;
	bool json = false;
	const List<String> args = OS::get_singleton()->get_cmdline_args();
	bool collecting = false;
	for (const String &arg : args) {
		if (arg == "--json") {
			json = true;
		} else if (arg == p_command) {
			collecting = true;
		} else if (collecting && !arg.begins_with("--")) {
			params.push_back(arg);
		} else {
			collecting = false;
		}
	}

	Variant result;
	String text;
	int code = 0;
	if (p_command == "--explain-property") {
		// Accept "<scene> <node> <property>" or "<scene> <node>:<property>".
		if (params.size() == 2 && params[1].contains(":")) {
			params.push_back(params[1].get_slice(":", 1));
			params.write[1] = params[1].get_slice(":", 0);
		}
		if (params.size() < 3) {
			agent_write_stdout("Usage: --explain-property <scene> <node path> <property>");
			return 2;
		}
		const Dictionary d = explain_scene_property(params[0], NodePath(params[1]), StringName(params[2]));
		code = d.has("error") ? 1 : 0;
		result = d;
		text = format_explain(d);
	} else {
		if (params.is_empty()) {
			agent_write_stdout(vformat("Usage: %s <res:// path>", p_command));
			return 2;
		}
		const bool overrides = p_command == "--find-overrides";
		const Array list = overrides ? find_overrides(params[0]) : resource_users(params[0]);
		result = list;
		text = format_list(list, overrides ? "places instance or inherit " + params[0] : "files use " + params[0]);
	}
	agent_write_stdout(json ? JSON::stringify(result, "", false) : text);
	return code;
}

void AgentInspector::_bind_methods() {
	ClassDB::bind_static_method("AgentInspector", D_METHOD("explain_scene_property", "scene", "node_path", "property"), &AgentInspector::explain_scene_property);
	ClassDB::bind_static_method("AgentInspector", D_METHOD("explain_property", "node", "property"), &AgentInspector::explain_property);
	ClassDB::bind_static_method("AgentInspector", D_METHOD("find_overrides", "scene"), &AgentInspector::find_overrides);
	ClassDB::bind_static_method("AgentInspector", D_METHOD("resource_users", "resource"), &AgentInspector::resource_users);
	ClassDB::bind_static_method("AgentInspector", D_METHOD("format_explain", "result"), &AgentInspector::format_explain);
}
