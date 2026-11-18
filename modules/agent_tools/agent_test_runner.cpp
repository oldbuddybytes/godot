#include "agent_test_runner.h"

#include "agent_common.h"
#include "agent_test.h"

#include "core/input/input.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/math/math_funcs.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/object/script_language.h"
#include "core/os/os.h"
#include "scene/main/window.h"

static constexpr int TRACE_LINES_ON_FAILURE = 8;
static constexpr uint64_t TEST_SEED = 0x5eed;

void AgentTestRunner::_on_error(void *p_self, const char *p_function, const char *p_file, int p_line, const char *p_error, const char *p_message, bool p_editor_notify, ErrorHandlerType p_type) {
	AgentTestRunner *self = static_cast<AgentTestRunner *>(p_self);
	if (p_type == ERR_HANDLER_WARNING || !self->current || self->script_errors.size() >= 3) {
		return;
	}
	String text = String::utf8((p_message && *p_message) ? p_message : p_error);
	if (p_type == ERR_HANDLER_SCRIPT && p_file && *p_file) {
		text = vformat("%s (%s:%d)", text, String::utf8(p_file), p_line);
	}
	self->script_errors.push_back(text);
}

void AgentTestRunner::initialize() {
	SceneTree::initialize();

	String dir = "res://tests/agent";
	String filter;
	const List<String> args = OS::get_singleton()->get_cmdline_args();
	for (const List<String>::Element *E = args.front(); E; E = E->next()) {
		const String &arg = E->get();
		const String next = E->next() ? E->next()->get() : String();
		if (arg == "--agent-test" && !next.is_empty() && !next.begins_with("-")) {
			dir = next;
		} else if (arg == "--json") {
			json = true;
		} else if (arg == "--json-out") {
			json_out = next;
		} else if (arg == "--junit-out") {
			junit_out = next;
		} else if (arg == "--filter") {
			filter = next;
		} else if (arg == "--test-timeout" && next.is_valid_float()) {
			max_frames = MAX(1, int(next.to_float() * Engine::get_singleton()->get_physics_ticks_per_second()));
		}
	}

	Vector<String> files;
	agent_collect_files(dir, { "gd" }, files);
	files.sort();
	for (const String &path : files) {
		Ref<Script> script = ResourceLoader::load(path, "Script");
		if (script.is_null() || !ClassDB::is_parent_class(script->get_instance_base_type(), "AgentTest")) {
			continue;
		}
		List<MethodInfo> methods;
		script->get_script_method_list(&methods);
		Vector<String> names;
		for (const MethodInfo &mi : methods) {
			if (mi.name.begins_with("test_") && !names.has(mi.name) && (filter.is_empty() || mi.name.contains(filter))) {
				names.push_back(mi.name);
			}
		}
		names.sort();
		for (const String &name : names) {
			cases.push_back({ path, name });
		}
	}

	if (cases.is_empty()) {
		agent_write_stdout(vformat("ERROR: No tests found in %s (scripts extending AgentTest with test_* methods).", dir));
		finished = true;
		quit(2);
		return;
	}

	// Errors are reported per test instead of being printed.
	error_handler.errfunc = _on_error;
	error_handler.userdata = this;
	add_error_handler(&error_handler);
	was_printing_errors = Engine::get_singleton()->is_printing_error_messages();
	Engine::get_singleton()->set_print_error_messages(false);
}

void AgentTestRunner::_start_next() {
	if (next_case >= cases.size()) {
		_report();
		finished = true;
		quit(failed_count > 0 ? 1 : 0);
		return;
	}
	const Case c = cases[next_case++];
	current_path = c.path;
	current_method = c.method;
	current_done = false;
	script_errors.clear();
	Math::seed(TEST_SEED);

	Ref<Script> script = ResourceLoader::load(c.path, "Script");
	current = Object::cast_to<AgentTest>(ClassDB::instantiate(script->get_instance_base_type()));
	current->set_script(script);
	current->set_name("AgentTest");
	get_root()->add_child(current);

	Callable::CallError ce;
	const Variant ret = current->callp(c.method, nullptr, 0, ce);
	Object *state = ret.get_type() == Variant::OBJECT ? ret.get_validated_object() : nullptr;
	if (ce.error != Callable::CallError::CALL_OK) {
		current->fail("Could not call " + String(c.method));
		current_done = true;
	} else if (state && state->has_signal(SNAME("completed"))) {
		// The test awaited something: it finishes when its coroutine completes.
		current_state = ret;
		state->connect(SNAME("completed"), callable_mp(this, &AgentTestRunner::_on_test_completed), CONNECT_ONE_SHOT);
	} else {
		current_done = true;
	}
}

void AgentTestRunner::_on_test_completed(const Variant &p_return) {
	current_done = true;
}

void AgentTestRunner::_finish_current() {
	if (!script_errors.is_empty()) {
		current->fail("Script error: " + script_errors[0]);
	}
	Dictionary r;
	r["file"] = current_path;
	r["test"] = String(current_method);
	r["passed"] = !current->has_failed();
	r["frames"] = current->get_frame();
	if (current->has_failed()) {
		failed_count++;
		r["message"] = current->get_failure();
		if (current->get_failure_line() > 0) {
			r["line"] = current->get_failure_line();
		}
		const Vector<String> trace = current->get_trace();
		PackedStringArray tail;
		for (int i = MAX(0, trace.size() - TRACE_LINES_ON_FAILURE); i < trace.size(); i++) {
			tail.push_back(trace[i]);
		}
		r["trace"] = tail;
	}
	results.push_back(r);

	Object *state = current_state.get_validated_object();
	if (state && state->is_connected(SNAME("completed"), callable_mp(this, &AgentTestRunner::_on_test_completed))) {
		state->disconnect(SNAME("completed"), callable_mp(this, &AgentTestRunner::_on_test_completed));
	}
	current_state = Variant();
	current->free_loaded();
	current->queue_free();
	current = nullptr;
	Input::get_singleton()->release_pressed_events();
	cooldown = 2; // Let queued frees run so the next test starts with an empty tree.
}

bool AgentTestRunner::process(double p_time) {
	const bool quit_requested = SceneTree::process(p_time);
	if (finished) {
		return quit_requested;
	}
	if (current) {
		if (!script_errors.is_empty()) {
			current_done = true;
		} else if (!current_done && current->get_frame() >= max_frames) {
			current->fail(vformat("Timed out after %d physics frames", max_frames));
			current_done = true;
		}
		if (current_done) {
			_finish_current();
		}
	} else if (cooldown > 0) {
		cooldown--;
	} else {
		_start_next();
	}
	return quit_requested;
}

void AgentTestRunner::_report() {
	String text;
	int frames = 0;
	for (int i = 0; i < results.size(); i++) {
		const Dictionary r = results[i];
		frames += int(r["frames"]);
		if (bool(r["passed"])) {
			continue;
		}
		text += vformat("FAIL %s::%s%s: %s\n", r["file"], r["test"], r.has("line") ? vformat(" (line %d)", int(r["line"])) : String(), r["message"]);
		const PackedStringArray trace = r["trace"];
		for (const String &line : trace) {
			text += "  " + line + "\n";
		}
	}
	const int passed = results.size() - failed_count;
	text += vformat("%s: %d test%s, %d passed, %d failed (%d physics frames)", failed_count == 0 ? "OK" : "FAIL",
			results.size(), results.size() == 1 ? "" : "s", passed, failed_count, frames);

	Dictionary summary;
	summary["ok"] = failed_count == 0;
	summary["passed"] = passed;
	summary["failed"] = failed_count;
	summary["tests"] = results;
	const String json_text = JSON::stringify(summary, "", false);
	if (!json_out.is_empty()) {
		Ref<FileAccess> f = FileAccess::open(agent_output_path(json_out), FileAccess::WRITE);
		if (f.is_valid()) {
			f->store_string(json_text);
		}
	}
	if (!junit_out.is_empty()) {
		Ref<FileAccess> f = FileAccess::open(agent_output_path(junit_out), FileAccess::WRITE);
		if (f.is_valid()) {
			String xml = vformat("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<testsuite name=\"agent-test\" tests=\"%d\" failures=\"%d\">\n", results.size(), failed_count);
			for (int i = 0; i < results.size(); i++) {
				const Dictionary r = results[i];
				xml += vformat("  <testcase classname=\"%s\" name=\"%s\">", String(r["file"]).xml_escape(true), String(r["test"]).xml_escape(true));
				if (!bool(r["passed"])) {
					xml += vformat("<failure message=\"%s\">%s</failure>", String(r["message"]).xml_escape(true), String("\n").join(PackedStringArray(r["trace"])).xml_escape());
				}
				xml += "</testcase>\n";
			}
			f->store_string(xml + "</testsuite>\n");
		}
	}
	agent_write_stdout(json ? json_text : text);
}

void AgentTestRunner::finalize() {
	if (error_handler.errfunc) {
		remove_error_handler(&error_handler);
		Engine::get_singleton()->set_print_error_messages(was_printing_errors);
		error_handler.errfunc = nullptr;
	}
	SceneTree::finalize();
}
