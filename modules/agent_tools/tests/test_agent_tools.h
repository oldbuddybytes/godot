#pragma once

#include "../agent_common.h"
#include "../agent_inspector.h"
#include "../agent_motion_analyzer.h"
#include "../agent_test.h"
#include "../agent_tools.h"
#include "../agent_unattended.h"
#include "../agent_validator.h"

#include "core/object/class_db.h"
#include "core/object/message_queue.h"
#include "scene/3d/skeleton_3d.h"
#include "scene/resources/animation.h"
#include "scene/gui/dialogs.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "tests/signal_watcher.h"
#include "tests/test_macros.h"

namespace TestAgentTools {

// Expected SignalWatcher arguments for one emission with a single argument.
inline Array one_emission(const Variant &p_value) {
	Array args;
	args.push_back(p_value);
	Array emissions;
	emissions.push_back(args);
	return emissions;
}

TEST_CASE("[AgentTools] Module classes are registered") {
	CHECK(ClassDB::class_exists("AgentTools"));
	CHECK(AgentTools::get_version() == "0.1.0");
}

TEST_CASE("[AgentTools] Validator text report groups and truncates") {
	Dictionary err;
	err["code"] = "SCRIPT_PARSE_ERROR";
	err["severity"] = "error";
	err["path"] = "res://a.gd";
	err["line"] = 5;
	err["column"] = 2;
	err["message"] = "Expected \")\"";
	err["affected"] = PackedStringArray({ "res://1.tscn", "res://2.tscn", "res://3.tscn", "res://4.tscn" });
	Dictionary warn;
	warn["code"] = "INVALID_UID";
	warn["severity"] = "warning";
	warn["path"] = "res://b.tscn";
	warn["message"] = "uid://x is unknown";

	Dictionary result;
	result["ok"] = false;
	result["errors"] = 1;
	result["warnings"] = 1;
	result["files"] = 6;
	result["diagnostics"] = Array({ err, warn });

	const String full = AgentValidator::format_text(result);
	CHECK(full.begins_with("FAIL: 1 error, 1 warning in 6 files"));
	CHECK(full.contains("ERROR res://a.gd:5:2 SCRIPT_PARSE_ERROR"));
	CHECK(full.contains("(affects 4: res://1.tscn, res://2.tscn, res://3.tscn, +1)"));
	CHECK(full.contains("WARNING res://b.tscn INVALID_UID"));

	const String errors_only = AgentValidator::format_text(result, 20, true);
	CHECK_FALSE(errors_only.contains("INVALID_UID"));

	const String capped = AgentValidator::format_text(result, 1);
	CHECK(capped.contains("(+1 more not shown"));
}

TEST_CASE("[AgentTools] Inspector explain report marks the winning layer") {
	Dictionary own;
	own["value"] = "4";
	own["source"] = "res://level.tscn";
	own["kind"] = "instance override";
	Dictionary base;
	base["value"] = "2";
	base["source"] = "res://door.tscn";
	base["kind"] = "base scene";

	Dictionary result;
	result["scene"] = "res://level.tscn";
	result["node"] = "FrontDoor";
	result["property"] = "collision_layer";
	result["value"] = "4";
	result["layers"] = Array({ own, base });

	const String text = AgentInspector::format_explain(result);
	CHECK(text.begins_with("res://level.tscn FrontDoor:collision_layer = 4"));
	CHECK(text.contains("> 4  res://level.tscn (instance override)"));
	CHECK(text.contains("  2  res://door.tscn (base scene)"));

	Dictionary error;
	error["error"] = "Node Nope not found";
	CHECK(AgentInspector::format_explain(error) == "ERROR Node Nope not found");
}

TEST_CASE("[AgentTools] Dependency entries resolve to paths") {
	CHECK(agent_resolve_dependency("res://a.tscn::PackedScene") == "res://a.tscn");
	CHECK(agent_resolve_dependency("uid://unknownuid::Script::res://f.gd") == "res://f.gd");
	CHECK(agent_resolve_dependency("uid://unknownuid::Script") == "");
}

TEST_CASE("[AgentTools][SceneTree] AgentTest waits count physics frames") {
	AgentTest *test = memnew(AgentTest);
	SceneTree::get_singleton()->get_root()->add_child(test);

	Signal signal = test->physics_frames(2);
	Ref<AgentWait> wait = Object::cast_to<AgentWait>(signal.get_object());
	REQUIRE(wait.is_valid());
	SIGNAL_WATCH(wait.ptr(), "done");
	test->notification(Node::NOTIFICATION_PHYSICS_PROCESS);
	SIGNAL_CHECK_FALSE("done");
	test->notification(Node::NOTIFICATION_PHYSICS_PROCESS);
	SIGNAL_CHECK("done", one_emission(true));
	SIGNAL_UNWATCH(wait.ptr(), "done");
	CHECK(test->get_frame() == 2);

	memdelete(test);
}

TEST_CASE("[AgentTools][SceneTree] AgentTest signal_or_timeout fires or times out") {
	AgentTest *test = memnew(AgentTest);
	Node *source = memnew(Node);
	SceneTree::get_singleton()->get_root()->add_child(test);
	SceneTree::get_singleton()->get_root()->add_child(source);

	// Fired: the awaited signal arrives before the timeout.
	Ref<AgentWait> fired = Object::cast_to<AgentWait>(test->signal_or_timeout(source, "renamed", 1.0).get_object());
	SIGNAL_WATCH(fired.ptr(), "done");
	source->set_name("Renamed");
	SIGNAL_CHECK("done", one_emission(true));
	SIGNAL_UNWATCH(fired.ptr(), "done");

	// Timed out: one frame passes without the signal.
	const double one_frame = 1.0 / Engine::get_singleton()->get_physics_ticks_per_second();
	Ref<AgentWait> timed_out = Object::cast_to<AgentWait>(test->signal_or_timeout(source, "renamed", one_frame).get_object());
	SIGNAL_WATCH(timed_out.ptr(), "done");
	test->notification(Node::NOTIFICATION_PHYSICS_PROCESS);
	SIGNAL_CHECK("done", one_emission(false));
	SIGNAL_UNWATCH(timed_out.ptr(), "done");
	CHECK_FALSE(source->is_connected("renamed", Callable(test, "_on_awaited_signal")));
	CHECK_FALSE(test->has_failed());

	// A missing signal fails the test instead of waiting forever.
	ERR_PRINT_OFF;
	test->signal_or_timeout(source, "no_such_signal", 1.0);
	ERR_PRINT_ON;
	CHECK(test->has_failed());

	memdelete(source);
	memdelete(test);
}

TEST_CASE("[AgentTools][SceneTree] AgentTest assertions keep the first failure") {
	AgentTest *test = memnew(AgentTest);
	CHECK(test->assert_eq(3, 3));
	CHECK(test->assert_near(Vector3(0, 0, 0.0005), Vector3(), 0.001));
	CHECK(test->assert_near(1.0, 1.0004, 0.001));
	CHECK_FALSE(test->has_failed());

	CHECK_FALSE(test->assert_eq(1, 2, "count"));
	CHECK_FALSE(test->assert_true(false, "later failure"));
	CHECK(test->has_failed());
	CHECK(test->get_failure() == "count: expected 2, got 1");
	CHECK(test->get_trace().size() == 2);
	CHECK(test->get_trace()[1].ends_with("FAIL later failure"));
	memdelete(test);
}

// One emission of a signal with no arguments.
inline Array no_arg_emission() {
	Array emissions;
	emissions.push_back(Array());
	return emissions;
}

TEST_CASE("[AgentTools] Unattended rules must name the dialog") {
	AgentUnattended::Rule rule;
	CHECK(AgentUnattended::parse_rule("Select lightmap*=OK", rule).is_empty());
	CHECK(rule.pattern == "Select lightmap*");
	CHECK(rule.action == "ok");
	CHECK_FALSE(AgentUnattended::parse_rule("*=ok", rule).is_empty());
	CHECK_FALSE(AgentUnattended::parse_rule("*a?b*=ok", rule).is_empty());
	CHECK_FALSE(AgentUnattended::parse_rule("Probe*=yes", rule).is_empty());
	CHECK_FALSE(AgentUnattended::parse_rule("Probe", rule).is_empty());
	CHECK(AgentUnattended::suggest_rule("Save *all*?", "", "ok") == "--dialog-answer \"Save ?all??*=ok\"");
	CHECK(AgentUnattended::suggest_rule("Please Confirm...", "File \"a.lmbake\" already exists.\nOverwrite?", "ok") == "--dialog-answer \"Please Confirm...*File ?a.lmbake? already exists. Overwrite?*=ok\"");
}

TEST_CASE("[AgentTools][SceneTree] Unattended records dialogs and answers only matching rules") {
	Window *root = SceneTree::get_singleton()->get_root();
	ConfirmationDialog *early = memnew(ConfirmationDialog); // In the tree before setup.
	early->set_title("Probe early");
	root->add_child(early);

	Vector<AgentUnattended::Rule> rules;
	AgentUnattended::Rule rule;
	REQUIRE(AgentUnattended::parse_rule("Probe ok*=ok", rule).is_empty());
	rules.push_back(rule);
	REQUIRE(AgentUnattended::parse_rule("*Probe cancel*=cancel", rule).is_empty());
	rules.push_back(rule);
	AgentUnattended *unattended = memnew(AgentUnattended);
	unattended->setup(SceneTree::get_singleton(), rules, String(), false);

	ConfirmationDialog *ok_dialog = memnew(ConfirmationDialog);
	ok_dialog->set_title("Probe ok");
	ok_dialog->set_text("Continue?");
	root->add_child(ok_dialog);
	ConfirmationDialog *cancel_dialog = memnew(ConfirmationDialog);
	cancel_dialog->set_title("Probe cancel");
	root->add_child(cancel_dialog);

	SIGNAL_WATCH(ok_dialog, "confirmed");
	SIGNAL_WATCH(cancel_dialog, "canceled");
	SIGNAL_WATCH(early, "confirmed");
	SIGNAL_WATCH(early, "canceled");

	ok_dialog->popup_centered();
	MessageQueue::get_singleton()->flush();
	cancel_dialog->popup_centered();
	MessageQueue::get_singleton()->flush();
	SIGNAL_CHECK("confirmed", no_arg_emission());
	SIGNAL_CHECK("canceled", no_arg_emission());
	CHECK_FALSE(unattended->has_failed());
	CHECK(unattended->get_answered_count() == 2);

	// No rule matches: nothing is clicked, and the run is marked failed.
	early->popup_centered();
	MessageQueue::get_singleton()->flush();
	SIGNAL_CHECK_FALSE("confirmed");
	SIGNAL_CHECK_FALSE("canceled");
	CHECK(early->is_visible());
	CHECK(unattended->has_failed());

	Array records = unattended->get_records();
	REQUIRE(records.size() == 3);
	Dictionary first = records[0];
	CHECK(String(first["title"]) == "Probe ok");
	CHECK(String(first["text"]) == "Continue?");
	CHECK(String(first["class"]) == "ConfirmationDialog");
	CHECK(String(first["action"]) == "ok");
	CHECK(int(first["rule"]) == 1);
	Array buttons = first["buttons"];
	REQUIRE(buttons.size() == 2); // The order depends on the platform.
	Dictionary labels;
	for (const Variant &b : buttons) {
		labels[Dictionary(b)["role"]] = Dictionary(b)["label"];
	}
	CHECK(String(labels.get("ok", "")) == "OK");
	CHECK(String(labels.get("cancel", "")) == "Cancel");
	Dictionary last = records[2];
	CHECK(String(last["action"]) == "quit");
	CHECK(last["rule"].get_type() == Variant::NIL);
	CHECK(String(last["suggest"]) == "--dialog-answer \"Probe early*=ok\"");
	CHECK(unattended->get_summary() == "unattended: 3 dialogs, 2 answered by rule, 1 stopped the run");

	SIGNAL_UNWATCH(ok_dialog, "confirmed");
	SIGNAL_UNWATCH(cancel_dialog, "canceled");
	SIGNAL_UNWATCH(early, "confirmed");
	SIGNAL_UNWATCH(early, "canceled");
	memdelete(ok_dialog);
	memdelete(cancel_dialog);
	memdelete(early);
	memdelete(unattended);
}

// A small humanoid: Root at the origin, Hips 1 unit up, a spine and two legs.
// Feet rest at y = 0.1 and toes at y = 0.05, so the scale reference is 1 (times p_scale).
struct TestRig {
	Node3D *root = nullptr;
	Skeleton3D *skeleton = nullptr;
};

inline int add_test_bone(Skeleton3D *p_skeleton, const String &p_name, const String &p_parent, const Vector3 &p_offset) {
	const int bone = p_skeleton->add_bone(p_name);
	if (!p_parent.is_empty()) {
		p_skeleton->set_bone_parent(bone, p_skeleton->find_bone(p_parent));
	}
	p_skeleton->set_bone_rest(bone, Transform3D(Basis(), p_offset));
	return bone;
}

inline TestRig make_test_rig(double p_scale = 1.0) {
	TestRig rig;
	rig.root = memnew(Node3D);
	rig.skeleton = memnew(Skeleton3D);
	rig.skeleton->set_name("Skeleton3D");
	rig.root->add_child(rig.skeleton);
	Skeleton3D *sk = rig.skeleton;
	add_test_bone(sk, "Root", "", Vector3());
	add_test_bone(sk, "Hips", "Root", Vector3(0, 1, 0) * p_scale);
	add_test_bone(sk, "Spine", "Hips", Vector3(0, 0.2, 0) * p_scale);
	add_test_bone(sk, "Head", "Spine", Vector3(0, 0.4, 0) * p_scale);
	for (const String &side : { String("Left"), String("Right") }) {
		const double x = side == "Left" ? 0.1 : -0.1;
		add_test_bone(sk, side + "UpperLeg", "Hips", Vector3(x, 0, 0) * p_scale);
		add_test_bone(sk, side + "LowerLeg", side + "UpperLeg", Vector3(0, -0.45, 0) * p_scale);
		add_test_bone(sk, side + "Foot", side + "LowerLeg", Vector3(0, -0.45, 0) * p_scale);
		add_test_bone(sk, side + "Toes", side + "Foot", Vector3(0, -0.05, 0.1) * p_scale);
	}
	return rig;
}

inline Ref<Animation> make_test_clip(double p_length, Animation::LoopMode p_loop) {
	Ref<Animation> anim;
	anim.instantiate();
	anim->set_name("clip");
	anim->set_length(p_length);
	anim->set_loop_mode(p_loop);
	return anim;
}

inline void add_rotation_keys(const Ref<Animation> &p_anim, const String &p_bone, const Vector<double> &p_times, const Vector<Quaternion> &p_values) {
	const int t = p_anim->add_track(Animation::TYPE_ROTATION_3D);
	p_anim->track_set_path(t, NodePath("Skeleton3D:" + p_bone));
	for (int i = 0; i < p_times.size(); i++) {
		p_anim->rotation_track_insert_key(t, p_times[i], p_values[i]);
	}
}

inline void add_position_keys(const Ref<Animation> &p_anim, const String &p_bone, const Vector<double> &p_times, const Vector<Vector3> &p_values) {
	const int t = p_anim->add_track(Animation::TYPE_POSITION_3D);
	p_anim->track_set_path(t, NodePath("Skeleton3D:" + p_bone));
	for (int i = 0; i < p_times.size(); i++) {
		p_anim->position_track_insert_key(t, p_times[i], p_values[i]);
	}
}

inline Quaternion degrees_about(const Vector3 &p_axis, double p_degrees) {
	return Quaternion(p_axis, Math::deg_to_rad(p_degrees));
}

// Diagnostics with a given code (and bone, if given).
inline Array diagnostics_with(const Dictionary &p_result, const String &p_code, const String &p_bone = String()) {
	Array out;
	const Array all = p_result["diagnostics"];
	for (const Variant &v : all) {
		const Dictionary d = v;
		if (String(d["code"]) == p_code && (p_bone.is_empty() || String(d.get("bone", "")) == p_bone)) {
			out.push_back(d);
		}
	}
	return out;
}

inline Dictionary analyze_test_clip(const TestRig &p_rig, const Ref<Animation> &p_anim, const Dictionary &p_options = Dictionary()) {
	Ref<AgentMotionAnalyzer> analyzer;
	analyzer.instantiate();
	return analyzer->analyze(p_rig.skeleton, p_anim, p_options);
}

TEST_CASE("[AgentTools] Motion analyzer passes a clean looping clip") {
	TestRig rig = make_test_rig();
	Ref<Animation> anim = make_test_clip(1.0, Animation::LOOP_LINEAR);
	add_rotation_keys(anim, "Spine", { 0.0, 0.5, 1.0 }, { Quaternion(), degrees_about(Vector3(1, 0, 0), 10), Quaternion() });
	const Dictionary result = analyze_test_clip(rig, anim);
	CHECK(bool(result["ok"]));
	CHECK(int(result["errors"]) == 0);
	CHECK(int(result["warnings"]) == 0);
	CHECK(int(result["clips"]) == 1);
	const Dictionary summary = Array(result["clip_summaries"])[0];
	CHECK(bool(summary["in_place"]));
	CHECK(String(summary["loop_mode"]) == "linear");
	CHECK(double(Dictionary(summary["foot_contact_ratio"])["LeftFoot"]) == doctest::Approx(1.0));
	memdelete(rig.root);
}

TEST_CASE("[AgentTools] Motion analyzer finds a loop seam") {
	TestRig rig = make_test_rig();
	Ref<Animation> anim = make_test_clip(1.0, Animation::LOOP_LINEAR);
	add_rotation_keys(anim, "Spine", { 0.0, 1.0 }, { Quaternion(), degrees_about(Vector3(1, 0, 0), 20) });
	const Dictionary result = analyze_test_clip(rig, anim);
	const Array seams = diagnostics_with(result, "ANIM_LOOP_SEAM");
	REQUIRE(seams.size() == 1);
	const Dictionary seam = seams[0];
	CHECK(String(seam["bone"]) == "Spine");
	CHECK(double(seam["start_time"]) == doctest::Approx(1.0));
	CHECK(double(seam["value"]) == doctest::Approx(20.0).epsilon(0.01));
	CHECK_FALSE(bool(result["ok"]));
	memdelete(rig.root);
}

TEST_CASE("[AgentTools] Motion analyzer finds one rotation spike") {
	TestRig rig = make_test_rig();
	Ref<Animation> anim = make_test_clip(1.0, Animation::LOOP_NONE);
	const Quaternion turned = degrees_about(Vector3(0, 1, 0), 120);
	add_rotation_keys(anim, "Head", { 0.0, 0.5, 31.0 / 60.0, 1.0 }, { Quaternion(), Quaternion(), turned, turned });
	const Dictionary result = analyze_test_clip(rig, anim);
	const Array spikes = diagnostics_with(result, "ANIM_ROTATION_SPIKE");
	REQUIRE(spikes.size() == 1);
	const Dictionary spike = spikes[0];
	CHECK(String(spike["bone"]) == "Head");
	CHECK(int(spike["start_frame"]) == 30);
	CHECK(int(spike["end_frame"]) == 31);
	CHECK(double(spike["value"]) == doctest::Approx(120.0).epsilon(0.01));
	memdelete(rig.root);
}

// Hips glide forward at 0.5 units/s between 0.25 s and 0.75 s while the feet stay at ground height.
inline Ref<Animation> make_slide_clip(double p_scale) {
	Ref<Animation> anim = make_test_clip(1.0, Animation::LOOP_NONE);
	const Vector3 start = Vector3(0, 1, 0) * p_scale;
	const Vector3 end = Vector3(0, 1, 0.25) * p_scale;
	add_position_keys(anim, "Hips", { 0.0, 0.25, 0.75, 1.0 }, { start, start, end, end });
	return anim;
}

TEST_CASE("[AgentTools] Motion analyzer finds sliding feet") {
	TestRig rig = make_test_rig();
	const Dictionary result = analyze_test_clip(rig, make_slide_clip(1.0));
	const Array slides = diagnostics_with(result, "ANIM_FOOT_SLIDE", "LeftFoot");
	REQUIRE(slides.size() == 1);
	const Dictionary slide = slides[0];
	CHECK(int(slide["start_frame"]) >= 14);
	CHECK(int(slide["start_frame"]) <= 16);
	CHECK(int(slide["end_frame"]) >= 44);
	CHECK(int(slide["end_frame"]) <= 46);
	CHECK(double(slide["value"]) == doctest::Approx(0.5).epsilon(0.01));
	const Dictionary summary = Array(result["clip_summaries"])[0];
	CHECK_FALSE(bool(summary["in_place"]));
	CHECK(double(summary["travel_distance"]) == doctest::Approx(0.25));
	memdelete(rig.root);
}

TEST_CASE("[AgentTools] Motion analyzer thresholds follow the skeleton's scale") {
	TestRig small = make_test_rig(1.0);
	TestRig large = make_test_rig(100.0);
	const Dictionary a = analyze_test_clip(small, make_slide_clip(1.0));
	const Dictionary b = analyze_test_clip(large, make_slide_clip(100.0));
	const Array da = a["diagnostics"];
	const Array db = b["diagnostics"];
	REQUIRE(da.size() == db.size());
	CHECK(da.size() > 0);
	for (int i = 0; i < da.size(); i++) {
		const Dictionary x = da[i];
		const Dictionary y = db[i];
		CHECK(String(x["code"]) == String(y["code"]));
		CHECK(String(x.get("bone", "")) == String(y.get("bone", "")));
		CHECK(x.get("start_frame", -1) == y.get("start_frame", -1));
		CHECK(x.get("end_frame", -1) == y.get("end_frame", -1));
	}
	memdelete(small.root);
	memdelete(large.root);
}

TEST_CASE("[AgentTools] Motion analyzer accepts in-place treadmill feet") {
	TestRig rig;
	rig.root = memnew(Node3D);
	rig.skeleton = memnew(Skeleton3D);
	rig.skeleton->set_name("Skeleton3D");
	rig.root->add_child(rig.skeleton);
	add_test_bone(rig.skeleton, "Root", "", Vector3());
	add_test_bone(rig.skeleton, "Hips", "Root", Vector3(0, 1, 0));
	add_test_bone(rig.skeleton, "LeftFoot", "Root", Vector3(0.1, 0.1, 0));
	add_test_bone(rig.skeleton, "RightFoot", "Root", Vector3(-0.1, 0.1, 0));
	Ref<Animation> anim = make_test_clip(1.0, Animation::LOOP_NONE);
	add_position_keys(anim, "LeftFoot", { 0.0, 1.0 }, { Vector3(0.1, 0.1, 0.25), Vector3(0.1, 0.1, -0.25) });
	add_position_keys(anim, "RightFoot", { 0.0, 1.0 }, { Vector3(-0.1, 0.1, 0.25), Vector3(-0.1, 0.1, -0.25) });
	const Dictionary result = analyze_test_clip(rig, anim);
	CHECK(diagnostics_with(result, "ANIM_FOOT_SLIDE").is_empty());
	const Dictionary summary = Array(result["clip_summaries"])[0];
	CHECK(bool(summary["in_place"]));
	memdelete(rig.root);
}

TEST_CASE("[AgentTools] Motion analyzer finds ground penetration and stretched bones") {
	TestRig rig = make_test_rig();
	Ref<Animation> sunk = make_test_clip(1.0, Animation::LOOP_NONE);
	add_position_keys(sunk, "Hips", { 0.0, 1.0 }, { Vector3(0, 0.9, 0), Vector3(0, 0.9, 0) });
	CHECK(diagnostics_with(analyze_test_clip(rig, sunk), "ANIM_GROUND_PENETRATION", "LeftFoot").size() == 1);

	Ref<Animation> stretched = make_test_clip(1.0, Animation::LOOP_NONE);
	add_position_keys(stretched, "LeftLowerLeg", { 0.0, 1.0 }, { Vector3(0, -0.54, 0), Vector3(0, -0.54, 0) });
	const Array lengths = diagnostics_with(analyze_test_clip(rig, stretched), "ANIM_BONE_LENGTH");
	REQUIRE(lengths.size() == 1);
	CHECK(String(Dictionary(lengths[0])["bone"]) == "LeftLowerLeg");
	CHECK(double(Dictionary(lengths[0])["value"]) == doctest::Approx(0.2).epsilon(0.01));
	memdelete(rig.root);
}

TEST_CASE("[AgentTools] Motion analyzer reports unresolved tracks as errors") {
	TestRig rig = make_test_rig();
	Ref<Animation> anim = make_test_clip(1.0, Animation::LOOP_NONE);
	add_rotation_keys(anim, "Spine", { 0.0 }, { Quaternion() });
	add_rotation_keys(anim, "NoSuchBone", { 0.0 }, { Quaternion() });
	const int t = anim->add_track(Animation::TYPE_ROTATION_3D);
	anim->track_set_path(t, NodePath("Missing:Bone"));
	anim->rotation_track_insert_key(t, 0.0, Quaternion());
	const Dictionary result = analyze_test_clip(rig, anim);
	CHECK_FALSE(bool(result["ok"]));
	CHECK(int(result["errors"]) == 2);
	const Array unresolved = diagnostics_with(result, "ANIM_TRACK_UNRESOLVED");
	REQUIRE(unresolved.size() == 2);
	CHECK(String(Dictionary(unresolved[0])["bone"]) == "NoSuchBone");
	CHECK(String(Dictionary(unresolved[0])["severity"]) == "error");
	memdelete(rig.root);
}

TEST_CASE("[AgentTools] Motion analyzer skips foot checks without feet, unless named") {
	TestRig rig;
	rig.root = memnew(Node3D);
	rig.skeleton = memnew(Skeleton3D);
	rig.skeleton->set_name("Skeleton3D");
	rig.root->add_child(rig.skeleton);
	add_test_bone(rig.skeleton, "A", "", Vector3());
	add_test_bone(rig.skeleton, "B", "A", Vector3(0, 1, 0));
	add_test_bone(rig.skeleton, "C", "B", Vector3(0, -0.9, 0));
	Ref<Animation> anim = make_test_clip(1.0, Animation::LOOP_NONE);
	add_position_keys(anim, "B", { 0.0, 1.0 }, { Vector3(0, 1, 0), Vector3(0, 1, 0.5) });

	const Dictionary without = analyze_test_clip(rig, anim);
	const Array notes = diagnostics_with(without, "ANIM_FEET_NOT_FOUND");
	REQUIRE(notes.size() == 1);
	CHECK(String(Dictionary(notes[0])["severity"]) == "info");
	CHECK(diagnostics_with(without, "ANIM_FOOT_SLIDE").is_empty());
	CHECK(bool(without["ok"])); // Info entries don't fail a clip.

	Dictionary options;
	options["feet"] = PackedStringArray({ "C" });
	const Dictionary with = analyze_test_clip(rig, anim, options);
	CHECK(diagnostics_with(with, "ANIM_FEET_NOT_FOUND").is_empty());
	CHECK(diagnostics_with(with, "ANIM_FOOT_SLIDE", "C").size() == 1);
	memdelete(rig.root);
}

TEST_CASE("[AgentTools] Motion analyzer text report") {
	Dictionary slide;
	slide["code"] = "ANIM_FOOT_SLIDE";
	slide["severity"] = "warning";
	slide["path"] = "res://guard.tscn";
	slide["animation"] = "walk";
	slide["bone"] = "LeftFoot";
	slide["start_time"] = 1.2;
	slide["end_time"] = 1.45;
	slide["message"] = "slides at 0.310 units/s while planted (limit 0.150)";
	Dictionary unresolved;
	unresolved["code"] = "ANIM_TRACK_UNRESOLVED";
	unresolved["severity"] = "error";
	unresolved["path"] = "res://guard.tscn";
	unresolved["animation"] = "walk";
	unresolved["bone"] = "Tail";
	unresolved["message"] = "track 3 points at Skeleton3D:Tail, but the skeleton has no bone \"Tail\"";

	Dictionary result;
	result["ok"] = false;
	result["errors"] = 1;
	result["warnings"] = 1;
	result["clips"] = 2;
	result["skipped"] = Array();
	result["diagnostics"] = Array({ slide, unresolved });

	const String full = AgentMotionAnalyzer::format_text(result);
	CHECK(full.begins_with("FAIL: 1 error, 1 warning in 2 clips"));
	CHECK(full.contains("WARNING res://guard.tscn [walk] LeftFoot 1.20-1.45s ANIM_FOOT_SLIDE slides at 0.310"));
	CHECK(full.contains("ERROR res://guard.tscn [walk] Tail ANIM_TRACK_UNRESOLVED"));
	CHECK_FALSE(AgentMotionAnalyzer::format_text(result, 20, true).contains("ANIM_FOOT_SLIDE"));
	CHECK(AgentMotionAnalyzer::format_text(result, 1).contains("(+1 more not shown; use --max-diagnostics 0 for all)"));

	Dictionary clean;
	clean["errors"] = 0;
	clean["warnings"] = 0;
	clean["clips"] = 1;
	clean["diagnostics"] = Array();
	CHECK(AgentMotionAnalyzer::format_text(clean) == "OK: 1 clip");
}

} // namespace TestAgentTools
