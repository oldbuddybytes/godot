// AgentUnattended: --unattended, --dialog-answer and --dialog-log.

#include "agent_unattended.h"

#include "agent_common.h"

#include "core/config/engine.h"
#include "core/io/dir_access.h"
#include "core/io/json.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/object/script_backtrace.h"
#include "core/object/script_language.h"
#include "core/os/os.h"
#include "scene/gui/button.h"
#include "scene/gui/dialogs.h"
#include "scene/gui/file_dialog.h"
#include "scene/main/scene_tree.h"
#include "scene/scene_string_names.h"

#ifdef TOOLS_ENABLED
#include "editor/settings/editor_settings.h"
#endif

AgentUnattended *AgentUnattended::singleton = nullptr;

namespace {

// Straight to stderr, so it still appears with --quiet and never mixes with --json on stdout.
void _write_stderr(const String &p_text) {
	CharString utf8 = (p_text + "\n").utf8();
	fwrite(utf8.get_data(), 1, utf8.length(), stderr);
	fflush(stderr);
}

String _one_line(const String &p_text, int p_max = 160) {
	String s = p_text.replace("\n", " ").strip_edges();
	return s.length() > p_max ? s.substr(0, p_max) + "..." : s;
}

} // namespace

String AgentUnattended::parse_rule(const String &p_text, Rule &r_rule) {
	const int eq = p_text.rfind("=");
	if (eq < 0) {
		return vformat("--dialog-answer \"%s\": expected \"<pattern>=ok\" or \"<pattern>=cancel\".", p_text);
	}
	r_rule.pattern = p_text.substr(0, eq).strip_edges();
	r_rule.action = p_text.substr(eq + 1).strip_edges().to_lower();
	if (r_rule.action != "ok" && r_rule.action != "cancel") {
		return vformat("--dialog-answer \"%s\": the answer must be ok or cancel, not \"%s\".", p_text, r_rule.action);
	}
	// A pattern must name the dialog, so a catch-all like "*=ok" can't accept every prompt.
	if (r_rule.pattern.replace("*", "").replace("?", "").strip_edges().length() < 4) {
		return vformat("--dialog-answer \"%s\": the pattern is too broad. Include at least 4 characters of the dialog's title or text.", p_text);
	}
	return String();
}

String AgentUnattended::suggest_rule(const String &p_title, const String &p_detail, const String &p_action) {
	// Title plus the start of the text or file path, so generic titles like "Please Confirm..." only match this dialog.
	const auto clean = [](const String &p_text, int p_max) {
		String s = p_text.replace("\n", " ").strip_edges().substr(0, p_max);
		return s.replace("*", "?").replace("\"", "?").replace("=", "?").replace("\\", "?");
	};
	String pattern = clean(p_title, 60) + "*";
	if (!p_detail.strip_edges().is_empty()) {
		pattern += clean(p_detail, 60) + "*";
	}
	return vformat("--dialog-answer \"%s=%s\"", pattern, p_action);
}

String AgentUnattended::install_from_cmdline(SceneTree *p_tree) {
	Vector<Rule> rules;
	String log_path;
	const List<String> args = OS::get_singleton()->get_cmdline_args();
	for (const List<String>::Element *E = args.front(); E; E = E->next()) {
		if ((E->get() == "--dialog-answer" || E->get() == "--dialog-log") && !E->next()) {
			return E->get() + " needs a value.";
		}
		if (E->get() == "--dialog-answer") {
			E = E->next();
			Rule rule;
			const String error = parse_rule(E->get(), rule);
			if (!error.is_empty()) {
				return error;
			}
			rules.push_back(rule);
		} else if (E->get() == "--dialog-log") {
			E = E->next();
			log_path = agent_output_path(E->get());
		}
	}

	singleton = memnew(AgentUnattended);
	singleton->setup(p_tree, rules, log_path, true);

	bool native = OS::get_singleton()->is_sandboxed();
#ifdef TOOLS_ENABLED
	if (EditorSettings::get_singleton() && EditorSettings::get_singleton()->has_setting("interface/editor/appearance/use_native_file_dialogs")) {
		native = native || bool(EditorSettings::get_singleton()->get_setting("interface/editor/appearance/use_native_file_dialogs"));
	}
#endif
	if (native) {
		_write_stderr("unattended: warning: native OS file dialogs are on, and --unattended can't see them.");
	}
	return String();
}

bool AgentUnattended::finish() {
	if (!singleton) {
		return false;
	}
	_write_stderr(singleton->get_summary());
	singleton->log.unref();
	return singleton->failed;
}

void AgentUnattended::cleanup() {
	if (singleton) {
		memdelete(singleton);
	}
}

void AgentUnattended::setup(SceneTree *p_tree, const Vector<Rule> &p_rules, const String &p_log_path, bool p_quit_on_unexpected) {
	tree = p_tree;
	rules = p_rules;
	quit_on_unexpected = p_quit_on_unexpected;
	if (!p_log_path.is_empty()) {
		DirAccess::make_dir_recursive_absolute(p_log_path.get_base_dir());
		log = FileAccess::open(p_log_path, FileAccess::WRITE);
		if (log.is_null()) {
			_write_stderr("unattended: warning: could not write " + p_log_path);
		}
	}
	tree->connect("node_added", callable_mp(this, &AgentUnattended::_on_node_added));
	// Dialogs already in the tree.
	List<Node *> stack;
	stack.push_back(tree->get_root());
	while (!stack.is_empty()) {
		Node *node = stack.front()->get();
		stack.pop_front();
		_on_node_added(node);
		for (int i = 0; i < node->get_child_count(); i++) {
			stack.push_back(node->get_child(i));
		}
	}
}

void AgentUnattended::_on_node_added(Node *p_node) {
	if (AcceptDialog *dialog = Object::cast_to<AcceptDialog>(p_node)) {
		_watch(dialog);
	}
}

void AgentUnattended::_watch(AcceptDialog *p_dialog) {
	if (watched.has(p_dialog->get_instance_id())) {
		return;
	}
	watched.insert(p_dialog->get_instance_id());
	p_dialog->connect(SceneStringName(visibility_changed), callable_mp(this, &AgentUnattended::_on_visibility_changed).bind(p_dialog->get_instance_id()));
	if (p_dialog->is_visible()) {
		_on_visibility_changed(p_dialog->get_instance_id());
	}
}

void AgentUnattended::_on_visibility_changed(ObjectID p_dialog) {
	AcceptDialog *dialog = ObjectDB::get_instance<AcceptDialog>(p_dialog);
	if (!dialog || !dialog->is_visible()) {
		return;
	}

	Dictionary record;
	const String title = dialog->get_title();
	const String text = dialog->get_text();
	record["title"] = title;
	record["text"] = text;
	record["class"] = dialog->get_class();
	record["node"] = String(dialog->get_path()).trim_prefix("/root/");
	String subject = title + "\n" + text;
	if (FileDialog *file_dialog = Object::cast_to<FileDialog>(dialog)) {
		record["file_path"] = file_dialog->get_current_path();
		subject += "\n" + file_dialog->get_current_path();
	}

	Array buttons;
	PackedStringArray labels;
	ConfirmationDialog *confirm = Object::cast_to<ConfirmationDialog>(dialog);
	Button *ok = dialog->get_ok_button();
	Button *cancel = confirm ? confirm->get_cancel_button() : nullptr;
	if (ok && ok->get_parent()) {
		Node *box = ok->get_parent();
		for (int i = 0; i < box->get_child_count(); i++) {
			Button *button = Object::cast_to<Button>(box->get_child(i));
			if (!button || !button->is_visible()) {
				continue;
			}
			Dictionary b;
			b["label"] = button->get_text();
			b["role"] = button == ok ? "ok" : (button == cancel ? "cancel" : "custom");
			buttons.push_back(b);
			labels.push_back(button->get_text());
		}
	}
	record["buttons"] = buttons;

	Array backtrace;
	for (const Ref<ScriptBacktrace> &bt : ScriptServer::capture_script_backtraces(false)) {
		if (bt.is_null()) {
			continue;
		}
		for (int i = 0; i < bt->get_frame_count(); i++) {
			backtrace.push_back(vformat("%s:%d in %s", bt->get_frame_file(i), bt->get_frame_line(i), bt->get_frame_function(i)));
		}
	}
	record["script_backtrace"] = backtrace;
	record["frame"] = Engine::get_singleton()->get_process_frames();

	int rule_index = -1;
	for (int i = 0; i < rules.size(); i++) {
		if (subject.matchn(rules[i].pattern) || title.matchn(rules[i].pattern)) {
			rule_index = i;
			break;
		}
	}
	const String action = rule_index >= 0 ? rules[rule_index].action : String("quit");
	record["action"] = action;
	record["rule"] = rule_index >= 0 ? Variant(rule_index + 1) : Variant();
	const String suggestion = suggest_rule(title, record.has("file_path") ? String(record["file_path"]) : text, "ok");
	if (rule_index < 0) {
		record["suggest"] = suggestion;
	}
	records.push_back(record);
	if (log.is_valid()) {
		log->store_line(JSON::stringify(record, "", false));
		log->flush();
	}

	String line = vformat("DIALOG \"%s\"", _one_line(title, 80));
	const String detail = record.has("file_path") ? String(record["file_path"]) : text;
	if (!detail.is_empty()) {
		line += vformat(" \"%s\"", _one_line(detail));
	}
	line += " [" + String(", ").join(labels) + "]";
	if (!backtrace.is_empty()) {
		line += " from " + String(backtrace[0]).get_slice(" in ", 0);
	}
	line += rule_index >= 0 ? vformat(" -> %s (rule %d)", action, rule_index + 1) : String(" -> quit (no rule)");
	_write_stderr(line);

	if (rule_index >= 0) {
		answered++;
		// Answer on the next frame, not while the dialog is still opening.
		callable_mp(this, &AgentUnattended::_answer).call_deferred(p_dialog, action);
		return;
	}
	_write_stderr("suggest: " + suggestion + " (or =cancel). Check what the button does before adding it.");
	failed = true;
	if (quit_on_unexpected && tree) {
		// Deferred, so the dialog finishes opening before the editor starts shutting down.
		callable_mp(tree, &SceneTree::quit).call_deferred(3);
	}
}

void AgentUnattended::_answer(ObjectID p_dialog, const String &p_action) {
	AcceptDialog *dialog = ObjectDB::get_instance<AcceptDialog>(p_dialog);
	if (!dialog || !dialog->is_visible()) {
		return;
	}
	if (p_action == "ok") {
		dialog->get_ok_button()->emit_signal(SceneStringName(pressed));
	} else {
		// The same path as closing the window: emits "canceled" and hides the dialog.
		dialog->notification(Window::NOTIFICATION_WM_CLOSE_REQUEST);
	}
}

String AgentUnattended::get_summary() const {
	int stopped = 0;
	for (const Variant &r : records) {
		if (String(Dictionary(r)["action"]) == "quit") {
			stopped++;
		}
	}
	return vformat("unattended: %d dialog%s, %d answered by rule, %d stopped the run", records.size(), records.size() == 1 ? "" : "s", answered, stopped);
}

void AgentUnattended::_bind_methods() {
	ClassDB::bind_static_method("AgentUnattended", D_METHOD("get_active"), &AgentUnattended::get_singleton);
	ClassDB::bind_method(D_METHOD("has_failed"), &AgentUnattended::has_failed);
	ClassDB::bind_method(D_METHOD("get_records"), &AgentUnattended::get_records);
	ClassDB::bind_method(D_METHOD("get_summary"), &AgentUnattended::get_summary);
}

AgentUnattended::~AgentUnattended() {
	if (singleton == this) {
		singleton = nullptr;
	}
}
