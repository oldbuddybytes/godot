#pragma once

// Helpers shared by the agent_tools classes.

#include "core/config/engine.h"
#include "core/error/error_macros.h"
#include "core/io/dir_access.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_uid.h"
#include "core/os/os.h"
#include "core/templates/vector.h"

#include <cstdio>

// Captures engine errors printed while it is alive, and keeps them off the console.
struct ErrorCapture {
	ErrorHandlerList handler;
	Vector<String> messages;
	bool was_printing = true;

	static void _on_error(void *p_self, const char *p_function, const char *p_file, int p_line, const char *p_error, const char *p_message, bool p_editor_notify, ErrorHandlerType p_type) {
		if (p_type == ERR_HANDLER_WARNING) {
			return;
		}
		ErrorCapture *self = static_cast<ErrorCapture *>(p_self);
		if (self->messages.size() < 5) {
			self->messages.push_back(String::utf8((p_message && *p_message) ? p_message : p_error));
		}
	}

	ErrorCapture() {
		handler.errfunc = _on_error;
		handler.userdata = this;
		add_error_handler(&handler);
		was_printing = Engine::get_singleton()->is_printing_error_messages();
		Engine::get_singleton()->set_print_error_messages(false);
	}

	~ErrorCapture() {
		remove_error_handler(&handler);
		Engine::get_singleton()->set_print_error_messages(was_printing);
	}
};

// Lists files under p_dir (recursively) whose extension is in p_exts, skipping
// hidden folders, folders with a .gdignore file and res://script_templates.
inline void agent_collect_files(const String &p_dir, const Vector<String> &p_exts, Vector<String> &r_out) {
	Ref<DirAccess> da = DirAccess::open(p_dir);
	if (da.is_null() || da->file_exists(".gdignore")) {
		return;
	}
	for (const String &file : da->get_files()) {
		if (p_exts.has(file.get_extension().to_lower())) {
			r_out.push_back(p_dir.path_join(file));
		}
	}
	for (const String &dir : da->get_directories()) {
		const String path = p_dir.path_join(dir);
		if (!dir.begins_with(".") && path != "res://script_templates") {
			agent_collect_files(path, p_exts, r_out);
		}
	}
}

// Resolves a dependency entry from ResourceLoader::get_dependencies()
// ("<path or uid>::<type>[::<fallback>]") to a res:// path, or "" if unresolvable.
inline String agent_resolve_dependency(const String &p_dep) {
	const Vector<String> parts = p_dep.split("::");
	if (!parts[0].begins_with("uid://")) {
		return parts[0];
	}
	const ResourceUID::ID id = ResourceUID::get_singleton()->text_to_id(parts[0]);
	if (id != ResourceUID::INVALID_ID && ResourceUID::get_singleton()->has_id(id)) {
		return ResourceUID::get_singleton()->get_id_path(id);
	}
	return parts.size() > 2 ? parts[2] : String();
}

// Writes a line straight to stdout, so it still appears with --quiet.
inline void agent_write_stdout(const String &p_text) {
	CharString utf8 = (p_text + "\n").utf8();
	fwrite(utf8.get_data(), 1, utf8.length(), stdout);
	fflush(stdout);
}

// Output file paths given on the command line are relative to the directory the
// command was run from. Godot changes into the project folder at startup, so
// resolve them against the shell's $PWD instead.
inline String agent_output_path(const String &p_path) {
	if (p_path.is_empty() || p_path.is_absolute_path() || p_path.contains("://")) {
		return p_path;
	}
	const String pwd = OS::get_singleton()->get_environment("PWD");
	return pwd.is_empty() ? p_path : pwd.path_join(p_path).simplify_path();
}
