#pragma once

#include "core/io/file_access.h"
#include "core/object/object.h"
#include "core/templates/hash_set.h"
#include "core/variant/array.h"

class AcceptDialog;
class Node;
class SceneTree;

// --unattended: makes scripted runs fail fast instead of waiting forever on a
// dialog nobody will click. Every AcceptDialog (which includes confirmation and
// file dialogs) is watched. When one opens, it is recorded and either answered
// by a --dialog-answer rule or, with no matching rule, left alone while the run
// quits with exit code 3.
class AgentUnattended : public Object {
	GDCLASS(AgentUnattended, Object);

public:
	struct Rule {
		String pattern;
		String action; // "ok" or "cancel"
	};

private:
	static AgentUnattended *singleton;

	SceneTree *tree = nullptr;
	Vector<Rule> rules;
	bool quit_on_unexpected = true;
	Ref<FileAccess> log;
	Array records;
	HashSet<ObjectID> watched;
	bool failed = false;
	int answered = 0;

	void _on_node_added(Node *p_node);
	void _watch(AcceptDialog *p_dialog);
	void _on_visibility_changed(ObjectID p_dialog);
	void _answer(ObjectID p_dialog, const String &p_action);

protected:
	static void _bind_methods();

public:
	// Parses "<pattern>=ok|cancel". Returns an error message, or an empty string.
	static String parse_rule(const String &p_text, Rule &r_rule);
	// A rule built from the dialog's title and text (or file path), for the "suggest:" line.
	static String suggest_rule(const String &p_title, const String &p_detail, const String &p_action);

	// Reads --dialog-answer and --dialog-log from the command line and installs the
	// global instance. Returns an error message if a rule is invalid.
	static String install_from_cmdline(SceneTree *p_tree);
	// Prints the summary and closes the log. Returns true if a dialog stopped the run.
	static bool finish();
	// Frees the global instance, after the scene tree is gone.
	static void cleanup();
	static AgentUnattended *get_singleton() { return singleton; }

	void setup(SceneTree *p_tree, const Vector<Rule> &p_rules, const String &p_log_path, bool p_quit_on_unexpected);
	bool has_failed() const { return failed; }
	int get_answered_count() const { return answered; }
	Array get_records() const { return records; }
	String get_summary() const;

	~AgentUnattended();
};
