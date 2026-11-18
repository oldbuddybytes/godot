#pragma once

#include "core/error/error_macros.h"
#include "scene/main/scene_tree.h"

class AgentTest;

// Main loop for --agent-test: runs every `test_*` method of scripts extending
// AgentTest in a folder, one at a time in a fresh tree, and reports the results.
class AgentTestRunner : public SceneTree {
	GDCLASS(AgentTestRunner, SceneTree);

	struct Case {
		String path;
		StringName method;
	};

	Vector<Case> cases;
	int next_case = 0;
	AgentTest *current = nullptr;
	String current_path;
	StringName current_method;
	Variant current_state;
	bool current_done = false;
	int cooldown = 0;
	bool finished = false;

	int max_frames = 3600;
	bool json = false;
	String json_out;
	String junit_out;
	String screenshot_dir = "agent_screenshots";
	double screenshot_scale = 1.0;

	Array results;
	int failed_count = 0;

	ErrorHandlerList error_handler;
	Vector<String> script_errors;
	bool was_printing_errors = true;
	bool loading = false; // Collect errors while loading test scripts.

	static void _on_error(void *p_self, const char *p_function, const char *p_file, int p_line, const char *p_error, const char *p_message, bool p_editor_notify, ErrorHandlerType p_type);
	void _start_next();
	void _finish_current();
	void _on_test_completed(const Variant &p_return);
	void _report();

protected:
	static void _bind_methods() {}

public:
	virtual void initialize() override;
	virtual bool process(double p_time) override;
	virtual void finalize() override;
};
