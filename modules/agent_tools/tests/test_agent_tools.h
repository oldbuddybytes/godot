#pragma once

#include "../agent_common.h"
#include "../agent_inspector.h"
#include "../agent_test.h"
#include "../agent_tools.h"
#include "../agent_unattended.h"
#include "../agent_validator.h"

#include "core/object/class_db.h"
#include "core/object/message_queue.h"
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

} // namespace TestAgentTools
