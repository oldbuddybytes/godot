#pragma once

#include "core/object/ref_counted.h"
#include "core/variant/callable.h"
#include "scene/main/node.h"

// Emits `done(value)` when an AgentTest wait finishes. Tests `await` this signal.
class AgentWait : public RefCounted {
	GDCLASS(AgentWait, RefCounted);

protected:
	static void _bind_methods();
};

// Base class for gameplay tests run by --agent-test. Test scripts extend it and
// define `test_*` methods, which can `await` the waits below. Every wait counts
// physics frames, so runs are repeatable.
class AgentTest : public Node {
	GDCLASS(AgentTest, Node);

	struct Wait {
		int id = 0;
		Ref<AgentWait> wait;
		int frames_left = 0;
		StringName release_action;
		ObjectID signal_source;
		StringName signal_name;
		Callable signal_callable;
	};

	Vector<Wait> waits;
	int next_wait_id = 1;
	int frame = 0;
	Vector<ObjectID> loaded;
	Vector<ObjectID> tracked_bodies;
	HashMap<ObjectID, String> last_hits;
	Vector<String> trace_log;
	bool failed = false;
	String failure;
	String failure_file;
	int failure_line = -1;
	String test_name = "test";
	String screenshot_dir = "agent_screenshots";
	double screenshot_scale = 1.0;
	int screenshot_count = 0;

	Wait &_add_wait(int p_frames);
	Signal _signal_of(const Wait &p_wait) const;
	int _frames_for(double p_seconds) const;
public:
	String _describe(const Object *p_object) const;

private:
	void _send_action(const StringName &p_action, bool p_pressed);
	void _track_collisions();
	Variant _on_awaited_signal(const Variant **p_args, int p_argcount, Callable::CallError &r_error);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	// Scene and input helpers.
	Node *load_scene(const String &p_path);
	Signal press_action(const StringName &p_action, double p_seconds = 0.1);
	void hold_action(const StringName &p_action);
	void release_action(const StringName &p_action);

	// Waits.
	Signal physics_frames(int p_frames);
	Signal seconds(double p_seconds);
	Signal signal_or_timeout(Object *p_object, const StringName &p_signal, double p_timeout = 5.0);

	// Assertions: the first failure is reported; the test keeps running.
	bool assert_true(bool p_condition, const String &p_message = String());
	bool assert_false(bool p_condition, const String &p_message = String());
	bool assert_eq(const Variant &p_actual, const Variant &p_expected, const String &p_message = String());
	bool assert_near(const Variant &p_actual, const Variant &p_expected, double p_tolerance = 0.001, const String &p_message = String());
	void fail(const String &p_message);

	// Looking at the rendered game (needs --offscreen, not --headless).
	String screenshot(const String &p_name = String());
	Dictionary pick(const Vector2 &p_position);

	// Tracing: input sent, signals awaited and collisions of tracked bodies are recorded automatically.
	void track_body(Node *p_body);
	void trace(const String &p_text);
	int get_frame() const { return frame; }

	// Used by the runner.
	bool has_failed() const { return failed; }
	String get_failure() const { return failure; }
	String get_failure_file() const { return failure_file; }
	int get_failure_line() const { return failure_line; }
	Vector<String> get_trace() const { return trace_log; }
	void free_loaded();
	void configure(const String &p_test_name, const String &p_screenshot_dir, double p_screenshot_scale);
};
