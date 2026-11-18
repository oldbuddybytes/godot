#include "agent_test.h"

#include "core/config/engine.h"
#include "core/input/input.h"
#include "core/input/input_event.h"
#include "core/io/resource_loader.h"
#include "core/object/class_db.h"
#include "core/object/script_backtrace.h"
#include "core/object/script_language.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/resources/packed_scene.h"

#ifndef _3D_DISABLED
#include "scene/3d/physics/character_body_3d.h"
#endif
#ifndef _2D_DISABLED
#include "scene/2d/physics/character_body_2d.h"
#endif

static constexpr int MAX_TRACE = 200;

void AgentWait::_bind_methods() {
	ADD_SIGNAL(MethodInfo("done", PropertyInfo(Variant::NIL, "value", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NIL_IS_VARIANT)));
}

AgentTest::Wait &AgentTest::_add_wait(int p_frames) {
	Wait w;
	w.id = next_wait_id++;
	w.wait.instantiate();
	w.frames_left = MAX(p_frames, 0);
	waits.push_back(w);
	return waits.write[waits.size() - 1];
}

Signal AgentTest::_signal_of(const Wait &p_wait) const {
	return Signal(p_wait.wait.ptr(), "done");
}

int AgentTest::_frames_for(double p_seconds) const {
	return MAX(1, int(Math::round(p_seconds * Engine::get_singleton()->get_physics_ticks_per_second())));
}

String AgentTest::_describe(const Object *p_object) const {
	if (!p_object) {
		return "null";
	}
	const Node *node = Object::cast_to<Node>(p_object);
	if (node && node->is_inside_tree()) {
		return String(node->get_path()).trim_prefix("/root/");
	}
	return p_object->get_class();
}

void AgentTest::_send_action(const StringName &p_action, bool p_pressed) {
	Ref<InputEventAction> event;
	event.instantiate();
	event->set_action(p_action);
	event->set_pressed(p_pressed);
	event->set_strength(p_pressed ? 1.0 : 0.0);
	Input::get_singleton()->parse_input_event(event);
	trace(vformat("%s %s", p_pressed ? "press" : "release", p_action));
}

// Name of the exact shape node that was hit, e.g. "Level/Door/Blocker", not just the body.
template <typename T>
static String _shape_name(const AgentTest *p_test, ObjectID p_collider, int p_shape, String (AgentTest::*p_describe)(const Object *) const) {
	T *collider = Object::cast_to<T>(ObjectDB::get_instance(p_collider));
	if (!collider) {
		return "unknown collider";
	}
	Object *shape = collider->shape_owner_get_owner(collider->shape_find_owner(p_shape));
	return (p_test->*p_describe)(shape ? shape : collider);
}

void AgentTest::_track_collisions() {
	for (const ObjectID &id : tracked_bodies) {
		Object *obj = ObjectDB::get_instance(id);
		Vector<String> hits;
		String position;
		bool tracked = false;
#ifndef _3D_DISABLED
		if (CharacterBody3D *body = Object::cast_to<CharacterBody3D>(obj)) {
			tracked = true;
			position = String(body->get_global_position());
			for (int i = 0; i < body->get_slide_collision_count(); i++) {
				const PhysicsServer3D::MotionResult result = body->get_slide_collision(i);
				for (int c = 0; c < result.collision_count; c++) {
					const String name = _shape_name<CollisionObject3D>(this, result.collisions[c].collider_id, result.collisions[c].collider_shape, &AgentTest::_describe);
					if (!hits.has(name)) {
						hits.push_back(name);
					}
				}
			}
		}
#endif
#ifndef _2D_DISABLED
		if (CharacterBody2D *body = Object::cast_to<CharacterBody2D>(obj)) {
			tracked = true;
			position = String(body->get_global_position());
			for (int i = 0; i < body->get_slide_collision_count(); i++) {
				const PhysicsServer2D::MotionResult result = body->get_slide_collision(i);
				const String name = _shape_name<CollisionObject2D>(this, result.collider_id, result.collider_shape, &AgentTest::_describe);
				if (!hits.has(name)) {
					hits.push_back(name);
				}
			}
		}
#endif
		if (!tracked) {
			continue;
		}
		const String now = String(", ").join(hits);
		const String *before = last_hits.getptr(id);
		if ((before ? *before : String()) != now) {
			trace(now.is_empty() ? _describe(obj) + " no longer colliding" : vformat("%s hit %s at %s", _describe(obj), now, position));
			last_hits[id] = now;
		}
	}
}

Variant AgentTest::_on_awaited_signal(const Variant **p_args, int p_argcount, Callable::CallError &r_error) {
	r_error.error = Callable::CallError::CALL_OK;
	if (p_argcount < 1) {
		return Variant();
	}
	const int id = *p_args[p_argcount - 1]; // Bound wait id comes last.
	for (int i = 0; i < waits.size(); i++) {
		if (waits[i].id != id) {
			continue;
		}
		const Wait w = waits[i];
		waits.remove_at(i);
		trace(vformat("signal %s.%s received", _describe(ObjectDB::get_instance(w.signal_source)), w.signal_name));
		w.wait->emit_signal(SNAME("done"), true);
		break;
	}
	return Variant();
}

void AgentTest::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			set_process_mode(PROCESS_MODE_ALWAYS);
			set_physics_process(true);
		} break;
		case NOTIFICATION_PHYSICS_PROCESS: {
			frame++;
			_track_collisions();
			// Finish due waits after the loop: resuming a test can add new waits.
			Vector<Wait> due;
			for (int i = waits.size() - 1; i >= 0; i--) {
				if (--waits.write[i].frames_left <= 0) {
					due.insert(0, waits[i]);
					waits.remove_at(i);
				}
			}
			for (const Wait &w : due) {
				Variant value = true;
				if (w.signal_name != StringName()) {
					Object *source = ObjectDB::get_instance(w.signal_source);
					if (source && source->is_connected(w.signal_name, w.signal_callable)) {
						source->disconnect(w.signal_name, w.signal_callable);
					}
					trace(vformat("timed out waiting for %s.%s", _describe(source), w.signal_name));
					value = false;
				}
				if (w.release_action != StringName()) {
					_send_action(w.release_action, false);
				}
				w.wait->emit_signal(SNAME("done"), value);
			}
		} break;
	}
}

Node *AgentTest::load_scene(const String &p_path) {
	Ref<PackedScene> packed = ResourceLoader::load(p_path, "PackedScene");
	Node *node = packed.is_valid() ? packed->instantiate() : nullptr;
	if (!node) {
		fail("Could not load scene " + p_path);
		return nullptr;
	}
	get_tree()->get_root()->add_child(node);
	loaded.push_back(node->get_instance_id());
	trace("loaded " + p_path);
	return node;
}

Signal AgentTest::press_action(const StringName &p_action, double p_seconds) {
	_send_action(p_action, true);
	Wait &w = _add_wait(_frames_for(p_seconds));
	w.release_action = p_action;
	return _signal_of(w);
}

void AgentTest::hold_action(const StringName &p_action) {
	_send_action(p_action, true);
}

void AgentTest::release_action(const StringName &p_action) {
	_send_action(p_action, false);
}

Signal AgentTest::physics_frames(int p_frames) {
	return _signal_of(_add_wait(p_frames));
}

Signal AgentTest::seconds(double p_seconds) {
	return _signal_of(_add_wait(_frames_for(p_seconds)));
}

Signal AgentTest::signal_or_timeout(Object *p_object, const StringName &p_signal, double p_timeout) {
	Wait &w = _add_wait(_frames_for(p_timeout));
	if (!p_object || !p_object->has_signal(p_signal)) {
		fail(vformat("%s has no signal %s", _describe(p_object), p_signal));
		w.frames_left = 1;
		return _signal_of(w);
	}
	w.signal_source = p_object->get_instance_id();
	w.signal_name = p_signal;
	w.signal_callable = Callable(this, "_on_awaited_signal").bind(w.id);
	p_object->connect(p_signal, w.signal_callable, CONNECT_ONE_SHOT);
	return _signal_of(w);
}

void AgentTest::fail(const String &p_message) {
	trace("FAIL " + p_message);
	if (failed) {
		return;
	}
	failed = true;
	failure = p_message;
	for (const Ref<ScriptBacktrace> &bt : ScriptServer::capture_script_backtraces(false)) {
		if (bt.is_valid() && !bt->is_empty()) {
			failure_file = bt->get_frame_file(0);
			failure_line = bt->get_frame_line(0);
			break;
		}
	}
}

bool AgentTest::assert_true(bool p_condition, const String &p_message) {
	if (!p_condition) {
		fail(p_message.is_empty() ? "expected true" : p_message);
	}
	return p_condition;
}

bool AgentTest::assert_false(bool p_condition, const String &p_message) {
	if (p_condition) {
		fail(p_message.is_empty() ? "expected false" : p_message);
	}
	return !p_condition;
}

bool AgentTest::assert_eq(const Variant &p_actual, const Variant &p_expected, const String &p_message) {
	const bool ok = p_actual == p_expected;
	if (!ok) {
		const String detail = vformat("expected %s, got %s", p_expected.get_construct_string(), p_actual.get_construct_string());
		fail(p_message.is_empty() ? detail : p_message + ": " + detail);
	}
	return ok;
}

bool AgentTest::assert_near(const Variant &p_actual, const Variant &p_expected, double p_tolerance, const String &p_message) {
	double distance = 0.0;
	switch (p_actual.get_type()) {
		case Variant::INT:
		case Variant::FLOAT:
			distance = Math::abs(double(p_actual) - double(p_expected));
			break;
		case Variant::VECTOR2:
			distance = Vector2(p_actual).distance_to(Vector2(p_expected));
			break;
		case Variant::VECTOR3:
			distance = Vector3(p_actual).distance_to(Vector3(p_expected));
			break;
		default:
			return assert_eq(p_actual, p_expected, p_message);
	}
	const bool ok = distance <= p_tolerance;
	if (!ok) {
		const String detail = vformat("expected %s ± %s, got %s", p_expected.get_construct_string(), p_tolerance, p_actual.get_construct_string());
		fail(p_message.is_empty() ? detail : p_message + ": " + detail);
	}
	return ok;
}

void AgentTest::track_body(Node *p_body) {
	ERR_FAIL_NULL(p_body);
	tracked_bodies.push_back(p_body->get_instance_id());
}

void AgentTest::trace(const String &p_text) {
	trace_log.push_back(vformat("f%d %s", frame, p_text));
	if (trace_log.size() > MAX_TRACE) {
		trace_log.remove_at(0);
	}
}

void AgentTest::free_loaded() {
	for (const ObjectID &id : loaded) {
		Node *node = Object::cast_to<Node>(ObjectDB::get_instance(id));
		if (node) {
			node->queue_free();
		}
	}
	loaded.clear();
}

void AgentTest::_bind_methods() {
	ClassDB::bind_method(D_METHOD("load_scene", "path"), &AgentTest::load_scene);
	ClassDB::bind_method(D_METHOD("press_action", "action", "seconds"), &AgentTest::press_action, DEFVAL(0.1));
	ClassDB::bind_method(D_METHOD("hold_action", "action"), &AgentTest::hold_action);
	ClassDB::bind_method(D_METHOD("release_action", "action"), &AgentTest::release_action);
	ClassDB::bind_method(D_METHOD("physics_frames", "frames"), &AgentTest::physics_frames);
	ClassDB::bind_method(D_METHOD("seconds", "seconds"), &AgentTest::seconds);
	ClassDB::bind_method(D_METHOD("signal_or_timeout", "object", "signal", "timeout"), &AgentTest::signal_or_timeout, DEFVAL(5.0));
	ClassDB::bind_method(D_METHOD("assert_true", "condition", "message"), &AgentTest::assert_true, DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("assert_false", "condition", "message"), &AgentTest::assert_false, DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("assert_eq", "actual", "expected", "message"), &AgentTest::assert_eq, DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("assert_near", "actual", "expected", "tolerance", "message"), &AgentTest::assert_near, DEFVAL(0.001), DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("fail", "message"), &AgentTest::fail);
	ClassDB::bind_method(D_METHOD("track_body", "body"), &AgentTest::track_body);
	ClassDB::bind_method(D_METHOD("trace", "text"), &AgentTest::trace);
	ClassDB::bind_method(D_METHOD("get_frame"), &AgentTest::get_frame);
	ClassDB::bind_vararg_method(METHOD_FLAGS_DEFAULT, "_on_awaited_signal", &AgentTest::_on_awaited_signal, MethodInfo("_on_awaited_signal"));
}
