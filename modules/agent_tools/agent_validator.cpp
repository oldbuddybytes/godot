#include "agent_validator.h"

#include "core/config/engine.h"
#include "core/error/error_list.h"
#include "core/error/error_macros.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_uid.h"
#include "core/object/class_db.h"
#include "core/object/script_language.h"
#include "core/os/os.h"

#include <cstdio>

namespace {

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

bool _is_resource_ext(const String &p_ext) {
	return p_ext == "tscn" || p_ext == "scn" || p_ext == "tres" || p_ext == "res";
}

ScriptLanguage *_language_for_ext(const String &p_ext) {
	for (int i = 0; i < ScriptServer::get_language_count(); i++) {
		ScriptLanguage *lang = ScriptServer::get_language(i);
		if (lang->get_extension() == p_ext) {
			return lang;
		}
	}
	return nullptr;
}

bool _exists(const String &p_path) {
	return ResourceLoader::exists(p_path) || FileAccess::exists(p_path);
}

void _write_stdout(const String &p_text) {
	CharString utf8 = (p_text + "\n").utf8();
	fwrite(utf8.get_data(), 1, utf8.length(), stdout);
	fflush(stdout);
}

} // namespace

void AgentValidator::_collect_files(const String &p_dir, Vector<String> &r_scripts, Vector<String> &r_resources) const {
	Ref<DirAccess> da = DirAccess::open(p_dir);
	if (da.is_null() || da->file_exists(".gdignore")) {
		return;
	}
	for (const String &file : da->get_files()) {
		const String path = p_dir.path_join(file);
		const String ext = file.get_extension().to_lower();
		if (_is_resource_ext(ext)) {
			r_resources.push_back(path);
		} else if (_language_for_ext(ext)) {
			r_scripts.push_back(path);
		}
	}
	for (const String &dir : da->get_directories()) {
		const String path = p_dir.path_join(dir);
		if (dir.begins_with(".") || path == "res://script_templates") {
			continue;
		}
		_collect_files(path, r_scripts, r_resources);
	}
}

void AgentValidator::_add(const String &p_code, bool p_is_error, const String &p_path, const String &p_message, int p_line, int p_column) {
	Diagnostic d;
	d.code = p_code;
	d.is_error = p_is_error;
	d.path = p_path;
	d.line = p_line;
	d.column = p_column;
	d.message = p_message.strip_edges();
	diagnostics.push_back(d);
	if (p_is_error) {
		failed_files.insert(p_path);
	}
}

void AgentValidator::_check_script(const String &p_path) {
	ScriptLanguage *lang = _language_for_ext(p_path.get_extension().to_lower());
	if (!lang) {
		return;
	}
	Error err = OK;
	const String source = FileAccess::get_file_as_string(p_path, &err);
	if (err != OK) {
		_add("READ_FAILED", true, p_path, vformat("Could not read file (%s)", error_names[err]));
		return;
	}

	List<ScriptLanguage::ScriptError> errors;
	List<ScriptLanguage::Warning> warnings;
	{
		ErrorCapture capture;
		lang->validate(source, p_path, nullptr, &errors, &warnings);
	}
	// Later parse errors in the same file are usually caused by the first one,
	// so report only the first and count the rest.
	const ScriptLanguage::ScriptError *first = nullptr;
	int more = 0;
	for (const ScriptLanguage::ScriptError &e : errors) {
		if (!e.path.is_empty() && e.path != p_path) {
			// The error is in another script this one uses: report it there, not here.
			dependents[e.path].insert(p_path);
			continue;
		}
		if (first) {
			more++;
		} else {
			first = &e;
		}
	}
	if (first) {
		const String suffix = more > 0 ? vformat(" (+%d more in this file, likely caused by this one)", more) : String();
		_add("SCRIPT_PARSE_ERROR", true, p_path, first->message + suffix, first->line, first->column);
	}
	for (const ScriptLanguage::Warning &w : warnings) {
		_add("SCRIPT_WARNING_" + w.string_code, false, p_path, w.message, w.start_line);
	}
}

void AgentValidator::_check_dependencies(const String &p_path) {
	List<String> deps;
	{
		ErrorCapture capture;
		ResourceLoader::get_dependencies(p_path, &deps, true);
	}
	ResourceUID *uids = ResourceUID::get_singleton();
	for (const String &dep : deps) {
		// Format: "<path or uid>::<type>[::<fallback path>]".
		const Vector<String> parts = dep.split("::");
		String target = parts[0];
		const String fallback = parts.size() > 2 ? parts[2] : String();

		if (target.begins_with("uid://")) {
			const ResourceUID::ID id = uids->text_to_id(target);
			if (id != ResourceUID::INVALID_ID && uids->has_id(id)) {
				target = uids->get_id_path(id);
			} else if (!fallback.is_empty() && _exists(fallback)) {
				_add("INVALID_UID", false, p_path, vformat("%s is unknown; loads only via fallback path %s", parts[0], fallback));
				target = fallback;
			} else {
				_add("MISSING_DEPENDENCY", true, p_path, vformat("%s not found%s", parts[0], fallback.is_empty() ? String() : " (fallback " + fallback + " missing too)"));
				continue;
			}
		}
		if (!_exists(target)) {
			_add("MISSING_DEPENDENCY", true, p_path, vformat("%s not found", target));
			continue;
		}
		dependents[target].insert(p_path);
	}
}

void AgentValidator::_check_load(const String &p_path) {
	Error err = OK;
	Ref<Resource> res;
	Vector<String> messages;
	{
		ErrorCapture capture;
		res = ResourceLoader::load(p_path, "", ResourceFormatLoader::CACHE_MODE_REUSE, &err);
		messages = capture.messages;
	}
	if (res.is_null() || err != OK) {
		_add("LOAD_FAILED", true, p_path, messages.is_empty() ? vformat("Failed to load (%s)", error_names[err]) : messages[0]);
	} else if (!messages.is_empty()) {
		_add("LOAD_ERROR", true, p_path, messages[0]);
	}
}

void AgentValidator::_collect_affected(const String &p_path, HashSet<String> &r_out) const {
	const HashSet<String> *users = dependents.getptr(p_path);
	if (!users) {
		return;
	}
	for (const String &user : *users) {
		if (!r_out.has(user)) {
			r_out.insert(user);
			_collect_affected(user, r_out);
		}
	}
}

Dictionary AgentValidator::_run(const Vector<String> &p_scripts, const Vector<String> &p_resources) {
	diagnostics.clear();
	dependents.clear();
	failed_files.clear();
	files_checked = p_scripts.size() + p_resources.size();

	for (const String &path : p_scripts) {
		_check_script(path);
	}
	for (const String &path : p_scripts) {
		_check_dependencies(path);
	}
	for (const String &path : p_resources) {
		_check_dependencies(path);
	}

	// Only load files whose problems are not already explained by a broken dependency.
	HashSet<String> tainted;
	for (const String &path : failed_files) {
		_collect_affected(path, tainted);
	}
	for (const String &path : p_resources) {
		if (failed_files.has(path) || tainted.has(path)) {
			continue;
		}
		_check_load(path);
		if (failed_files.has(path)) {
			_collect_affected(path, tainted);
		}
	}

	int error_count = 0;
	int warning_count = 0;
	for (Diagnostic &d : diagnostics) {
		if (!d.is_error) {
			warning_count++;
			continue;
		}
		error_count++;
		HashSet<String> affected;
		_collect_affected(d.path, affected);
		affected.erase(d.path);
		for (const String &path : affected) {
			d.affected.push_back(path);
		}
		d.affected.sort();
	}

	struct Order {
		bool operator()(const Diagnostic &a, const Diagnostic &b) const {
			if (a.is_error != b.is_error) {
				return a.is_error;
			}
			if (a.affected.size() != b.affected.size()) {
				return a.affected.size() > b.affected.size();
			}
			if (a.path != b.path) {
				return a.path < b.path;
			}
			return a.line < b.line;
		}
	};
	diagnostics.sort_custom<Order>();

	Array list;
	for (const Diagnostic &d : diagnostics) {
		Dictionary item;
		item["code"] = d.code;
		item["severity"] = d.is_error ? "error" : "warning";
		item["path"] = d.path;
		if (d.line > 0) {
			item["line"] = d.line;
		}
		if (d.column > 0) {
			item["column"] = d.column;
		}
		item["message"] = d.message;
		if (!d.affected.is_empty()) {
			PackedStringArray affected;
			for (const String &path : d.affected) {
				affected.push_back(path);
			}
			item["affected"] = affected;
		}
		list.push_back(item);
	}

	Dictionary result;
	result["ok"] = error_count == 0;
	result["errors"] = error_count;
	result["warnings"] = warning_count;
	result["files"] = files_checked;
	result["diagnostics"] = list;
	return result;
}

Dictionary AgentValidator::validate_project() {
	Vector<String> scripts;
	Vector<String> resources;
	_collect_files("res://", scripts, resources);
	return _run(scripts, resources);
}

Dictionary AgentValidator::validate_paths(const PackedStringArray &p_paths) {
	Vector<String> scripts;
	Vector<String> resources;
	for (const String &path : p_paths) {
		const String ext = path.get_extension().to_lower();
		if (_is_resource_ext(ext)) {
			resources.push_back(path);
		} else if (_language_for_ext(ext)) {
			scripts.push_back(path);
		}
	}
	return _run(scripts, resources);
}

// Drops warnings if requested and keeps at most p_max diagnostics, noting how many were cut.
static Dictionary _filter(const Dictionary &p_result, int p_max, bool p_errors_only) {
	Dictionary out = p_result.duplicate();
	Array kept;
	int skipped = 0;
	const Array all = p_result.get("diagnostics", Array());
	for (int i = 0; i < all.size(); i++) {
		const Dictionary d = all[i];
		if (p_errors_only && d.get("severity", "") != "error") {
			continue;
		}
		if (p_max > 0 && kept.size() >= p_max) {
			skipped++;
			continue;
		}
		kept.push_back(d);
	}
	out["diagnostics"] = kept;
	if (skipped > 0) {
		out["not_shown"] = skipped;
	}
	return out;
}

String AgentValidator::format_text(const Dictionary &p_result, int p_max_diagnostics, bool p_errors_only) {
	const Dictionary result = _filter(p_result, p_max_diagnostics, p_errors_only);
	const int errors = result.get("errors", 0);
	const int warnings = result.get("warnings", 0);
	String text = vformat("%s: %d error%s, %d warning%s in %d files", errors == 0 ? "OK" : "FAIL",
			errors, errors == 1 ? "" : "s", warnings, warnings == 1 ? "" : "s", int(result.get("files", 0)));

	const Array list = result["diagnostics"];
	for (int i = 0; i < list.size(); i++) {
		const Dictionary d = list[i];
		String location = d["path"];
		if (d.has("line")) {
			location += ":" + itos(d["line"]);
			if (d.has("column")) {
				location += ":" + itos(d["column"]);
			}
		}
		String line = vformat("%s %s %s %s", String(d["severity"]).to_upper(), location, d["code"], d["message"]);
		if (d.has("affected")) {
			const PackedStringArray affected = d["affected"];
			String names;
			for (int j = 0; j < MIN(affected.size(), 3); j++) {
				names += (j > 0 ? ", " : "") + affected[j];
			}
			if (affected.size() > 3) {
				names += vformat(", +%d", affected.size() - 3);
			}
			line += vformat(" (affects %d: %s)", affected.size(), names);
		}
		text += "\n" + line;
	}
	if (result.has("not_shown")) {
		text += vformat("\n(+%d more not shown; use --max-diagnostics 0 for all)", int(result["not_shown"]));
	}
	return text;
}

int AgentValidator::run_cli() {
	bool json = false;
	String json_out;
	int max_diagnostics = 20;
	bool errors_only = false;

	const List<String> args = OS::get_singleton()->get_cmdline_args();
	for (const List<String>::Element *E = args.front(); E; E = E->next()) {
		const String &arg = E->get();
		const bool has_next = E->next() != nullptr;
		if (arg == "--json") {
			json = true;
		} else if (arg == "--json-out" && has_next) {
			json_out = E->next()->get();
		} else if (arg == "--max-diagnostics" && has_next) {
			max_diagnostics = E->next()->get().to_int();
		} else if (arg == "--only" && has_next && E->next()->get() == "errors") {
			errors_only = true;
		}
	}

	Ref<AgentValidator> validator;
	validator.instantiate();
	const Dictionary result = validator->validate_project();
	if (int(result["files"]) == 0) {
		_write_stdout("ERROR: No project files found to validate.");
		return 2;
	}

	const Dictionary filtered = _filter(result, max_diagnostics, errors_only);
	if (!json_out.is_empty()) {
		Ref<FileAccess> f = FileAccess::open(json_out, FileAccess::WRITE);
		if (f.is_null()) {
			_write_stdout("ERROR: Could not write " + json_out);
			return 2;
		}
		f->store_string(JSON::stringify(filtered, "", false));
	}
	_write_stdout(json ? JSON::stringify(filtered, "", false) : format_text(result, max_diagnostics, errors_only));
	return bool(result["ok"]) ? 0 : 1;
}

void AgentValidator::_bind_methods() {
	ClassDB::bind_method(D_METHOD("validate_project"), &AgentValidator::validate_project);
	ClassDB::bind_method(D_METHOD("validate_paths", "paths"), &AgentValidator::validate_paths);
	ClassDB::bind_static_method("AgentValidator", D_METHOD("format_text", "result", "max_diagnostics", "errors_only"), &AgentValidator::format_text, DEFVAL(20), DEFVAL(false));
}
