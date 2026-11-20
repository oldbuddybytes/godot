#pragma once

#include "core/object/ref_counted.h"
#include "core/variant/dictionary.h"

class Animation;
class Node;
class Skeleton3D;

// Samples skeletal animation clips on their rest skeleton and reports motion
// defects (sliding feet, ground penetration, loop pops, rotation spikes, bone
// stretch, broken tracks) as diagnostics with bone, time and frame ranges.
// Used by `--analyze-animation` and from scripts as
// `AgentMotionAnalyzer.new().analyze(skeleton, animation)`.
class AgentMotionAnalyzer : public RefCounted {
	GDCLASS(AgentMotionAnalyzer, RefCounted);

protected:
	static void _bind_methods();

public:
	// Analyzes one clip on one skeleton. Track paths resolve relative to
	// options.root_node, or to the skeleton's parent.
	Dictionary analyze(Skeleton3D *p_skeleton, const Ref<Animation> &p_animation, const Dictionary &p_options = Dictionary());

	// Loads a scene and analyzes every clip of every AnimationPlayer in it.
	Dictionary analyze_scene(const String &p_path, const Dictionary &p_options = Dictionary());

	// Formats a result as a short human-readable report.
	static String format_text(const Dictionary &p_result, int p_max_diagnostics = 20, bool p_errors_only = false);

	// Entry point for `--analyze-animation`. Returns the process exit code
	// (0 clean, 1 errors or warnings, 2 could not run).
	static int run_cli();
};
