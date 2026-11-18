#pragma once

#include "core/object/ref_counted.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/variant/typed_array.h"

// Checks a project's scripts, scene/resource dependencies and resource loading,
// and reports diagnostics grouped by root cause. Used by `--validate-project`
// and from scripts as `AgentValidator.new().validate_project()`.
class AgentValidator : public RefCounted {
	GDCLASS(AgentValidator, RefCounted);

	struct Diagnostic {
		String code;
		bool is_error = true;
		String path;
		int line = -1;
		int column = -1;
		String message;
		Vector<String> affected;
	};

	Vector<Diagnostic> diagnostics;
	HashMap<String, HashSet<String>> dependents; // dependency -> files that reference it
	HashSet<String> failed_files; // files with at least one error of their own
	int files_checked = 0;

	void _collect_files(const String &p_dir, Vector<String> &r_scripts, Vector<String> &r_resources) const;
	void _add(const String &p_code, bool p_is_error, const String &p_path, const String &p_message, int p_line = -1, int p_column = -1);
	void _check_script(const String &p_path);
	void _check_dependencies(const String &p_path);
	void _check_load(const String &p_path);
	void _collect_affected(const String &p_path, HashSet<String> &r_out) const;
	Dictionary _run(const Vector<String> &p_scripts, const Vector<String> &p_resources);

protected:
	static void _bind_methods();

public:
	Dictionary validate_project();
	Dictionary validate_paths(const PackedStringArray &p_paths);

	// Formats a result as a short human-readable report.
	static String format_text(const Dictionary &p_result, int p_max_diagnostics = 20, bool p_errors_only = false);

	// Entry point for `--validate-project`. Reads its options from the command line,
	// prints the report and returns the process exit code (0 clean, 1 errors, 2 could not run).
	static int run_cli();
};
