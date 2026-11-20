#include "agent_motion_analyzer.h"

#include "agent_common.h"

#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/io/resource_loader.h"
#include "core/math/math_funcs.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/templates/hash_map.h"
#include "core/templates/sort_array.h"
#include "scene/3d/skeleton_3d.h"
#include "scene/animation/animation_player.h"
#include "scene/resources/animation.h"
#include "scene/resources/animation_library.h"
#include "scene/resources/packed_scene.h"

namespace {

// Tuning for one run. Distances are fractions of the skeleton's scale reference.
struct MotionOptions {
	double sample_rate = 60.0;
	double contact_height = 0.03;
	double slide_speed = 0.15;
	double penetration_depth = 0.03;
	double spike_angle = 30.0; // degrees per sample at 60 Hz
	double seam_angle = 5.0; // degrees
	double bone_length_tolerance = 0.05;
	PackedStringArray feet;
	Node *root_node = nullptr;
	String animation_filter;
	String player_filter;
};

constexpr int MAX_PER_BONE_AND_CODE = 10;
constexpr int SLIDE_MIN_SAMPLES = 3;
constexpr double DRIFT_YAW_DEGREES = 5.0;
constexpr double IN_PLACE_TRAVEL = 0.1;

struct BoneTracks {
	int position = -1;
	int rotation = -1;
	int scale = -1;
};

// One clip being analyzed on one skeleton. Collects diagnostics in order.
struct ClipContext {
	const Skeleton3D *skeleton = nullptr;
	Ref<Animation> animation;
	const MotionOptions *options = nullptr;
	String path;
	String player;
	String animation_name;
	Array *diagnostics = nullptr;
	HashMap<String, int> counts; // "<code>/<bone>" -> emitted so far
	HashMap<String, int> suppressed;
	HashMap<String, int> last_index; // "<code>/<bone>" -> index of the last emitted entry

	void add(const String &p_code, const String &p_severity, const String &p_bone, const String &p_message,
			double p_start_time = -1.0, double p_end_time = -1.0, int p_start_frame = -1, int p_end_frame = -1,
			double p_value = Math::NaN, double p_threshold = Math::NaN) {
		const String key = p_code + "/" + p_bone;
		const int count = counts.has(key) ? counts[key] : 0;
		if (count >= MAX_PER_BONE_AND_CODE) {
			suppressed[key] = (suppressed.has(key) ? suppressed[key] : 0) + 1;
			return;
		}
		counts[key] = count + 1;
		Dictionary d;
		d["code"] = p_code;
		d["severity"] = p_severity;
		if (!path.is_empty()) {
			d["path"] = path;
		}
		if (!player.is_empty()) {
			d["player"] = player;
		}
		d["animation"] = animation_name;
		if (!p_bone.is_empty()) {
			d["bone"] = p_bone;
		}
		if (p_start_time >= 0.0) {
			d["start_time"] = Math::snapped(p_start_time, 0.0001);
			d["end_time"] = Math::snapped(p_end_time, 0.0001);
		}
		if (p_start_frame >= 0) {
			d["start_frame"] = p_start_frame;
			d["end_frame"] = p_end_frame;
		}
		if (!Math::is_nan(p_value)) {
			d["value"] = Math::snapped(p_value, 0.0001);
		}
		if (!Math::is_nan(p_threshold)) {
			d["threshold"] = Math::snapped(p_threshold, 0.0001);
		}
		d["message"] = p_message;
		last_index[key] = diagnostics->size();
		diagnostics->push_back(d);
	}

	// Appends "(+N more like this)" to the last entry of each capped code and bone.
	void finish() {
		for (const KeyValue<String, int> &E : suppressed) {
			Dictionary d = (*diagnostics)[last_index[E.key]];
			d["message"] = String(d["message"]) + vformat(" (+%d more like this)", E.value);
		}
	}
};

String _loop_mode_name(Animation::LoopMode p_mode) {
	switch (p_mode) {
		case Animation::LOOP_LINEAR:
			return "linear";
		case Animation::LOOP_PINGPONG:
			return "pingpong";
		default:
			return "none";
	}
}

// Finds a bone by exact name, then by a case-insensitive substring.
int _find_bone_like(const Skeleton3D *p_skeleton, const String &p_exact, const String &p_contains) {
	const int exact = p_skeleton->find_bone(p_exact);
	if (exact >= 0) {
		return exact;
	}
	for (int i = 0; i < p_skeleton->get_bone_count(); i++) {
		if (p_skeleton->get_bone_name(i).to_lower().contains(p_contains)) {
			return i;
		}
	}
	return -1;
}

// Parents always come before their children.
Vector<int> _bone_order(const Skeleton3D *p_skeleton) {
	const int count = p_skeleton->get_bone_count();
	Vector<int> order;
	Vector<bool> placed;
	placed.resize(count);
	placed.fill(false);
	while (order.size() < count) {
		const int before = order.size();
		for (int i = 0; i < count; i++) {
			if (placed[i]) {
				continue;
			}
			const int parent = p_skeleton->get_bone_parent(i);
			if (parent < 0 || placed[parent]) {
				order.push_back(i);
				placed.write[i] = true;
			}
		}
		if (order.size() == before) {
			break; // A parent loop; Skeleton3D refuses these, so this is only a safeguard.
		}
	}
	return order;
}

// Ranges of consecutive true samples, as [first, last] pairs.
Vector<Vector2i> _runs(const Vector<bool> &p_flags, int p_min_length) {
	Vector<Vector2i> out;
	int start = -1;
	for (int i = 0; i <= p_flags.size(); i++) {
		const bool on = i < p_flags.size() && p_flags[i];
		if (on && start < 0) {
			start = i;
		} else if (!on && start >= 0) {
			if (i - start >= p_min_length) {
				out.push_back(Vector2i(start, i - 1));
			}
			start = -1;
		}
	}
	return out;
}

double _median(Vector<double> p_values) {
	if (p_values.is_empty()) {
		return 0.0;
	}
	p_values.sort();
	const int mid = p_values.size() / 2;
	return p_values.size() % 2 ? p_values[mid] : (p_values[mid - 1] + p_values[mid]) * 0.5;
}

double _percentile(Vector<double> p_values, double p_fraction) {
	if (p_values.is_empty()) {
		return 0.0;
	}
	p_values.sort();
	return p_values[CLAMP(int(Math::floor(p_fraction * (p_values.size() - 1))), 0, p_values.size() - 1)];
}

double _yaw(const Basis &p_basis) {
	const Vector3 forward = p_basis.get_column(2);
	return Math::atan2(forward.x, forward.z);
}

double _horizontal_distance(const Vector3 &p_a, const Vector3 &p_b) {
	return Vector2(p_a.x - p_b.x, p_a.z - p_b.z).length();
}

String _frames_text(double p_start, double p_end) {
	return Math::is_equal_approx(p_start, p_end) ? vformat("%.2fs", p_start) : vformat("%.2f-%.2fs", p_start, p_end);
}

// Foot bones: explicit list, then the humanoid profile names, then name matching.
Vector<int> _find_feet(const Skeleton3D *p_skeleton, const MotionOptions &p_options, PackedStringArray &r_missing) {
	Vector<int> feet;
	if (!p_options.feet.is_empty()) {
		for (const String &name : p_options.feet) {
			const int bone = p_skeleton->find_bone(name.strip_edges());
			if (bone >= 0) {
				feet.push_back(bone);
			} else {
				r_missing.push_back(name.strip_edges());
			}
		}
		return feet;
	}
	for (const char *name : { "LeftFoot", "RightFoot", "LeftToes", "RightToes" }) {
		const int bone = p_skeleton->find_bone(name);
		if (bone >= 0) {
			feet.push_back(bone);
		}
	}
	if (!feet.is_empty()) {
		return feet;
	}
	for (int i = 0; i < p_skeleton->get_bone_count(); i++) {
		const String name = p_skeleton->get_bone_name(i).to_lower();
		if (name.contains("foot") || name.contains("toe") || name.contains("ankle")) {
			feet.push_back(i);
		}
	}
	return feet;
}

MotionOptions _parse_options(const Dictionary &p_options) {
	MotionOptions o;
	o.sample_rate = MAX(1.0, double(p_options.get("sample_rate", o.sample_rate)));
	o.contact_height = p_options.get("contact_height", o.contact_height);
	o.slide_speed = p_options.get("slide_speed", o.slide_speed);
	o.penetration_depth = p_options.get("penetration_depth", o.penetration_depth);
	o.spike_angle = p_options.get("spike_angle", o.spike_angle);
	o.seam_angle = p_options.get("seam_angle", o.seam_angle);
	o.bone_length_tolerance = p_options.get("bone_length_tolerance", o.bone_length_tolerance);
	const Variant feet = p_options.get("feet", Variant());
	if (feet.get_type() == Variant::STRING || feet.get_type() == Variant::STRING_NAME) {
		o.feet = String(feet).split(",", false);
	} else if (feet.get_type() == Variant::PACKED_STRING_ARRAY || feet.get_type() == Variant::ARRAY) {
		o.feet = feet;
	}
	o.root_node = Object::cast_to<Node>(p_options.get("root_node", Variant()));
	o.animation_filter = p_options.get("animation", "");
	o.player_filter = p_options.get("player", "");
	return o;
}

// Resolves a track's node. Returns false if the node path doesn't exist.
bool _resolve_track_node(const NodePath &p_path, Node *p_root, const Skeleton3D *p_skeleton, Node *&r_node) {
	const NodePath node_path(p_path.get_names(), p_path.is_absolute());
	if (p_root) {
		r_node = p_root->get_node_or_null(node_path);
	} else {
		r_node = node_path.is_empty() ? const_cast<Skeleton3D *>(p_skeleton) : nullptr;
	}
	return r_node != nullptr;
}

// Samples one clip on one skeleton and runs every check. Adds diagnostics and one summary.
void _analyze_clip(ClipContext &ctx, Node *p_root, bool p_report_missing_nodes, Array &r_summaries) {
	const Skeleton3D *sk = ctx.skeleton;
	const Ref<Animation> &anim = ctx.animation;
	const MotionOptions &o = *ctx.options;
	const int bone_count = sk->get_bone_count();

	// Map tracks to bones; report the ones that point nowhere.
	Vector<BoneTracks> tracks;
	tracks.resize(bone_count);
	int bone_tracks = 0;
	for (int t = 0; t < anim->get_track_count(); t++) {
		const Animation::TrackType type = anim->track_get_type(t);
		if (type != Animation::TYPE_POSITION_3D && type != Animation::TYPE_ROTATION_3D && type != Animation::TYPE_SCALE_3D) {
			continue;
		}
		const NodePath path = anim->track_get_path(t);
		Node *node = nullptr;
		if (!_resolve_track_node(path, p_root, sk, node)) {
			if (p_report_missing_nodes) {
				ctx.add("ANIM_TRACK_UNRESOLVED", "error", path.get_subname_count() ? String(path.get_subname(0)) : String(),
						vformat("track %d points at %s, but node %s doesn't exist", t, String(path), String(NodePath(path.get_names(), path.is_absolute()))));
			}
			continue;
		}
		if (node != sk || path.get_subname_count() == 0) {
			continue; // Another node's transform, or the skeleton node itself.
		}
		const String bone_name = path.get_subname(0);
		const int bone = sk->find_bone(bone_name);
		if (bone < 0) {
			ctx.add("ANIM_TRACK_UNRESOLVED", "error", bone_name, vformat("track %d points at %s, but the skeleton has no bone \"%s\"", t, String(path), bone_name));
			continue;
		}
		BoneTracks &bt = tracks.write[bone];
		(type == Animation::TYPE_POSITION_3D ? bt.position : type == Animation::TYPE_ROTATION_3D ? bt.rotation : bt.scale) = t;
		bone_tracks++;
	}

	const double length = anim->get_length();
	const Animation::LoopMode loop_mode = anim->get_loop_mode();
	Dictionary summary;
	if (!ctx.path.is_empty()) {
		summary["path"] = ctx.path;
	}
	if (!ctx.player.is_empty()) {
		summary["player"] = ctx.player;
	}
	summary["animation"] = ctx.animation_name;
	summary["length"] = Math::snapped(length, 0.0001);
	summary["loop_mode"] = _loop_mode_name(loop_mode);
	summary["sample_rate"] = o.sample_rate;
	if (length <= 0.0 || bone_tracks == 0) {
		ctx.add("ANIM_EMPTY", "error", String(), length <= 0.0 ? String("the clip has zero length") : String("no track moves a bone of this skeleton"));
		r_summaries.push_back(summary);
		return;
	}

	// Rest pose, split into parts the tracks override.
	const Vector<int> order = _bone_order(sk);
	Vector<Vector3> rest_position;
	Vector<Quaternion> rest_rotation;
	Vector<Vector3> rest_scale;
	Vector<Transform3D> global_rest;
	rest_position.resize(bone_count);
	rest_rotation.resize(bone_count);
	rest_scale.resize(bone_count);
	global_rest.resize(bone_count);
	for (int b = 0; b < bone_count; b++) {
		const Transform3D rest = sk->get_bone_rest(b);
		rest_position.write[b] = rest.origin;
		rest_rotation.write[b] = rest.basis.get_rotation_quaternion();
		rest_scale.write[b] = rest.basis.get_scale();
	}
	for (int b : order) {
		const int parent = sk->get_bone_parent(b);
		global_rest.write[b] = parent >= 0 ? global_rest[parent] * sk->get_bone_rest(b) : sk->get_bone_rest(b);
	}

	// Sample times: every 1 / sample_rate, plus the exact end.
	Vector<double> times;
	const int steps = int(Math::floor(length * o.sample_rate + 1e-6));
	for (int i = 0; i <= steps; i++) {
		times.push_back(MIN(i / o.sample_rate, length));
	}
	if (times[times.size() - 1] < length - 1e-6) {
		times.push_back(length);
	}
	const int samples = times.size();

	// Forward kinematics at every sample.
	Vector<Vector3> positions; // [sample * bone_count + bone], skeleton space
	Vector<Quaternion> rotations; // global
	Vector<Quaternion> local_rotations;
	positions.resize(samples * bone_count);
	rotations.resize(samples * bone_count);
	local_rotations.resize(samples * bone_count);
	Vector<Transform3D> global;
	global.resize(bone_count);
	for (int s = 0; s < samples; s++) {
		const double time = times[s];
		for (int b : order) {
			const BoneTracks &bt = tracks[b];
			const Vector3 position = bt.position >= 0 ? anim->position_track_interpolate(bt.position, time) : rest_position[b];
			const Quaternion rotation = bt.rotation >= 0 ? anim->rotation_track_interpolate(bt.rotation, time).normalized() : rest_rotation[b];
			const Vector3 scale = bt.scale >= 0 ? anim->scale_track_interpolate(bt.scale, time) : rest_scale[b];
			const Transform3D local(Basis(rotation, scale), position);
			const int parent = sk->get_bone_parent(b);
			global.write[b] = parent >= 0 ? global[parent] * local : local;
			positions.write[s * bone_count + b] = global[b].origin;
			rotations.write[s * bone_count + b] = global[b].basis.get_rotation_quaternion();
			local_rotations.write[s * bone_count + b] = rotation;
		}
	}
	auto pos = [&](int s, int b) { return positions[s * bone_count + b]; };
	auto rot = [&](int s, int b) { return rotations[s * bone_count + b]; };
	auto lrot = [&](int s, int b) { return local_rotations[s * bone_count + b]; };

	// Scale reference: hips height above the lowest bone at rest, or half the rest height.
	double min_y = 0.0;
	double max_y = 0.0;
	for (int b = 0; b < bone_count; b++) {
		const double y = global_rest[b].origin.y;
		min_y = b == 0 ? y : MIN(min_y, y);
		max_y = b == 0 ? y : MAX(max_y, y);
	}
	int hips = _find_bone_like(sk, "Hips", "hips");
	double scale_ref = hips >= 0 ? global_rest[hips].origin.y - min_y : 0.0;
	if (scale_ref <= CMP_EPSILON) {
		scale_ref = (max_y - min_y) * 0.5;
	}
	if (scale_ref <= CMP_EPSILON) {
		scale_ref = 1.0;
	}
	if (hips < 0) {
		// No hips: use the shallowest bone with a position track, or else the first root bone.
		int best_depth = INT_MAX;
		for (int b : order) {
			int depth = 0;
			for (int p = sk->get_bone_parent(b); p >= 0; p = sk->get_bone_parent(p)) {
				depth++;
			}
			if (tracks[b].position >= 0 && depth < best_depth) {
				best_depth = depth;
				hips = b;
			}
		}
		for (int b = 0; hips < 0 && b < bone_count; b++) {
			if (sk->get_bone_parent(b) < 0) {
				hips = b;
			}
		}
	}
	const double contact_height = o.contact_height * scale_ref;
	const double slide_speed = o.slide_speed * scale_ref;
	const double penetration_depth = o.penetration_depth * scale_ref;
	const double spike_angle = Math::deg_to_rad(o.spike_angle * 60.0 / o.sample_rate);
	const double seam_angle = Math::deg_to_rad(o.seam_angle);

	// Travel and in-place detection, from the hips.
	double max_travel = 0.0;
	for (int s = 0; s < samples; s++) {
		max_travel = MAX(max_travel, _horizontal_distance(pos(s, hips), pos(0, hips)));
	}
	const bool in_place = max_travel < IN_PLACE_TRAVEL * scale_ref;
	const double travel = _horizontal_distance(pos(samples - 1, hips), pos(0, hips));
	summary["in_place"] = in_place;
	summary["travel_distance"] = Math::snapped(travel, 0.0001);
	summary["average_speed"] = Math::snapped(travel / length, 0.0001);

	// Rotation spikes: a bone that turns too far in one sample, relative to its parent and in the world.
	for (int b = 0; b < bone_count; b++) {
		Vector<bool> spike;
		Vector<double> angle;
		spike.resize(samples);
		angle.resize(samples);
		spike.fill(false);
		angle.fill(0.0);
		for (int s = 1; s < samples; s++) {
			const double global_delta = rot(s - 1, b).angle_to(rot(s, b));
			const double local_delta = lrot(s - 1, b).angle_to(lrot(s, b));
			angle.write[s] = global_delta;
			spike.write[s] = global_delta > spike_angle && local_delta > spike_angle;
		}
		for (const Vector2i &run : _runs(spike, 1)) {
			double worst = 0.0;
			for (int s = run.x; s <= run.y; s++) {
				worst = MAX(worst, angle[s]);
			}
			ctx.add("ANIM_ROTATION_SPIKE", "warning", sk->get_bone_name(b),
					vformat("turns %.1f deg in one sample (limit %.1f deg)", Math::rad_to_deg(worst), Math::rad_to_deg(spike_angle)),
					times[run.x - 1], times[run.y], run.x - 1, run.y, Math::rad_to_deg(worst), Math::rad_to_deg(spike_angle));
		}
	}

	// Feet: contact, sliding, penetration.
	PackedStringArray missing_feet;
	const Vector<int> feet = _find_feet(sk, o, missing_feet);
	if (!missing_feet.is_empty()) {
		ctx.add("ANIM_FEET_NOT_FOUND", "info", String(), "the skeleton has no bones named " + String(", ").join(missing_feet));
	}
	Dictionary contact_ratio;
	if (feet.is_empty()) {
		if (missing_feet.is_empty()) {
			ctx.add("ANIM_FEET_NOT_FOUND", "info", String(), "no foot or toe bones found, so foot checks were skipped; pass --feet to name them");
		}
	} else {
		// A foot is in contact when it is within contact_height of its own rest height.
		Vector<Vector<bool>> contact;
		Vector<double> contact_vx;
		Vector<double> contact_vz;
		bool ever_grounded = false;
		for (int f : feet) {
			Vector<bool> c;
			c.resize(samples);
			int count = 0;
			for (int s = 0; s < samples; s++) {
				c.write[s] = pos(s, f).y <= global_rest[f].origin.y + contact_height;
				count += c[s] ? 1 : 0;
			}
			ever_grounded = ever_grounded || count > 0;
			contact_ratio[sk->get_bone_name(f)] = Math::snapped(double(count) / samples, 0.0001);
			for (int s = 1; s < samples; s++) {
				if (c[s] && c[s - 1]) {
					const double dt = times[s] - times[s - 1];
					if (dt > CMP_EPSILON) {
						contact_vx.push_back((pos(s, f).x - pos(s - 1, f).x) / dt);
						contact_vz.push_back((pos(s, f).z - pos(s - 1, f).z) / dt);
					}
				}
			}
			contact.push_back(c);
		}
		// In-place clips move planted feet backward at the treadmill speed; measure slide against it.
		const Vector2 baseline = in_place ? Vector2(_median(contact_vx), _median(contact_vz)) : Vector2();

		for (int i = 0; i < feet.size(); i++) {
			const int f = feet[i];
			const Vector<bool> &c = contact[i];
			Vector<bool> sliding;
			Vector<double> speed;
			sliding.resize(samples);
			speed.resize(samples);
			sliding.fill(false);
			speed.fill(0.0);
			for (int s = 1; s < samples; s++) {
				const double dt = times[s] - times[s - 1];
				if (!c[s] || !c[s - 1] || dt <= CMP_EPSILON) {
					continue;
				}
				const Vector2 v((pos(s, f).x - pos(s - 1, f).x) / dt, (pos(s, f).z - pos(s - 1, f).z) / dt);
				speed.write[s] = (v - baseline).length();
				sliding.write[s] = speed[s] > slide_speed;
			}
			for (const Vector2i &run : _runs(sliding, SLIDE_MIN_SAMPLES)) {
				double worst = 0.0;
				for (int s = run.x; s <= run.y; s++) {
					worst = MAX(worst, speed[s]);
				}
				ctx.add("ANIM_FOOT_SLIDE", "warning", sk->get_bone_name(f),
						vformat("slides at %.3f units/s while planted (limit %.3f)%s", worst, slide_speed, in_place ? ", measured against the in-place treadmill speed" : ""),
						times[run.x - 1], times[run.y], run.x - 1, run.y, worst, slide_speed);
			}

			Vector<bool> below;
			below.resize(samples);
			for (int s = 0; s < samples; s++) {
				below.write[s] = pos(s, f).y < global_rest[f].origin.y - penetration_depth;
			}
			for (const Vector2i &run : _runs(below, 1)) {
				double worst = 0.0;
				for (int s = run.x; s <= run.y; s++) {
					worst = MAX(worst, global_rest[f].origin.y - pos(s, f).y);
				}
				ctx.add("ANIM_GROUND_PENETRATION", "warning", sk->get_bone_name(f),
						vformat("goes %.3f units below its rest height (limit %.3f)", worst, penetration_depth),
						times[run.x], times[run.y], run.x, run.y, worst, penetration_depth);
			}
		}
		if (!ever_grounded) {
			ctx.add("ANIM_NEVER_GROUNDED", "info", String(), "no foot touches the ground at any sample; fine for jumps, falls and swims");
		}
	}
	summary["foot_contact_ratio"] = contact_ratio;

	// Bone length. Parentless bones and their direct children (root, hips) translate on purpose.
	for (int b = 0; b < bone_count; b++) {
		const int parent = sk->get_bone_parent(b);
		if (parent < 0 || sk->get_bone_parent(parent) < 0) {
			continue;
		}
		const double rest_length = global_rest[b].origin.distance_to(global_rest[parent].origin);
		if (rest_length <= CMP_EPSILON) {
			continue;
		}
		Vector<bool> stretched;
		Vector<double> ratio;
		stretched.resize(samples);
		ratio.resize(samples);
		for (int s = 0; s < samples; s++) {
			ratio.write[s] = Math::abs(pos(s, b).distance_to(pos(s, parent)) - rest_length) / rest_length;
			stretched.write[s] = ratio[s] > o.bone_length_tolerance;
		}
		for (const Vector2i &run : _runs(stretched, 1)) {
			double worst = 0.0;
			for (int s = run.x; s <= run.y; s++) {
				worst = MAX(worst, ratio[s]);
			}
			ctx.add("ANIM_BONE_LENGTH", "warning", sk->get_bone_name(b),
					vformat("length differs from rest by %.1f%% (limit %.1f%%)", worst * 100.0, o.bone_length_tolerance * 100.0),
					times[run.x], times[run.y], run.x, run.y, worst, o.bone_length_tolerance);
		}
	}

	// Looping clips: the wrap must not pop, and the loop must not drift.
	if (loop_mode == Animation::LOOP_LINEAR && samples > 2) {
		const int last = samples - 1;
		for (int b = 0; b < bone_count; b++) {
			const double jump = lrot(last, b).angle_to(lrot(0, b));
			if (jump <= seam_angle) {
				continue;
			}
			Vector<double> changes;
			for (int s = 1; s < samples; s++) {
				changes.push_back(lrot(s - 1, b).angle_to(lrot(s, b)));
			}
			if (jump >= 3.0 * _percentile(changes, 0.95)) {
				ctx.add("ANIM_LOOP_SEAM", "warning", sk->get_bone_name(b),
						vformat("jumps %.1f deg where the loop wraps (limit %.1f deg)", Math::rad_to_deg(jump), o.seam_angle),
						length, length, last, last, Math::rad_to_deg(jump), o.seam_angle);
			}
		}
		// Feet relative to the hips' horizontal position, so root travel isn't a jump.
		for (int f : feet) {
			const Vector3 a = pos(last, f) - Vector3(pos(last, hips).x, 0, pos(last, hips).z);
			const Vector3 b = pos(0, f) - Vector3(pos(0, hips).x, 0, pos(0, hips).z);
			const double jump = a.distance_to(b);
			if (jump > contact_height) {
				ctx.add("ANIM_LOOP_SEAM", "warning", sk->get_bone_name(f),
						vformat("jumps %.3f units where the loop wraps (limit %.3f)", jump, contact_height),
						length, length, last, last, jump, contact_height);
			}
		}
		const String hips_name = sk->get_bone_name(hips);
		const double rise = Math::abs(pos(last, hips).y - pos(0, hips).y);
		if (rise > contact_height) {
			ctx.add("ANIM_LOOP_DRIFT", "warning", hips_name, vformat("ends %.3f units higher or lower than it starts (limit %.3f)", rise, contact_height),
					0.0, length, 0, last, rise, contact_height);
		}
		double yaw = Math::abs(_yaw(Basis(rot(last, hips))) - _yaw(Basis(rot(0, hips))));
		yaw = Math::rad_to_deg(MIN(yaw, Math::TAU - yaw));
		if (yaw > DRIFT_YAW_DEGREES) {
			ctx.add("ANIM_LOOP_DRIFT", "warning", hips_name, vformat("ends turned %.1f deg from where it starts (limit %.1f deg)", yaw, DRIFT_YAW_DEGREES),
					0.0, length, 0, last, yaw, DRIFT_YAW_DEGREES);
		}
		if (in_place && travel > contact_height) {
			ctx.add("ANIM_LOOP_DRIFT", "warning", hips_name, vformat("an in-place loop that ends %.3f units from where it starts (limit %.3f)", travel, contact_height),
					0.0, length, 0, last, travel, contact_height);
		}
	}
	r_summaries.push_back(summary);
}

// Counts errors and warnings and builds the result dictionary.
Dictionary _make_result(const Array &p_diagnostics, const Array &p_summaries, const Array &p_skipped) {
	int errors = 0;
	int warnings = 0;
	for (int i = 0; i < p_diagnostics.size(); i++) {
		const String severity = Dictionary(p_diagnostics[i]).get("severity", "");
		errors += severity == "error" ? 1 : 0;
		warnings += severity == "warning" ? 1 : 0;
	}
	Dictionary result;
	result["ok"] = errors == 0 && warnings == 0;
	result["errors"] = errors;
	result["warnings"] = warnings;
	result["clips"] = p_summaries.size();
	result["skipped"] = p_skipped;
	result["clip_summaries"] = p_summaries;
	result["diagnostics"] = p_diagnostics;
	return result;
}

void _collect_players(Node *p_node, Vector<AnimationPlayer *> &r_players) {
	if (AnimationPlayer *player = Object::cast_to<AnimationPlayer>(p_node)) {
		r_players.push_back(player);
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		_collect_players(p_node->get_child(i), r_players);
	}
}

// Drops non-errors if requested and keeps at most p_max diagnostics, noting how many were cut.
Dictionary _filter(const Dictionary &p_result, int p_max, bool p_errors_only) {
	Dictionary out = p_result.duplicate();
	Array kept;
	int cut = 0;
	const Array all = p_result.get("diagnostics", Array());
	for (int i = 0; i < all.size(); i++) {
		const Dictionary d = all[i];
		if (p_errors_only && d.get("severity", "") != "error") {
			continue;
		}
		if (p_max > 0 && kept.size() >= p_max) {
			cut++;
			continue;
		}
		kept.push_back(d);
	}
	out["diagnostics"] = kept;
	if (cut > 0) {
		out["not_shown"] = cut;
	}
	return out;
}

} // namespace

Dictionary AgentMotionAnalyzer::analyze(Skeleton3D *p_skeleton, const Ref<Animation> &p_animation, const Dictionary &p_options) {
	Array diagnostics;
	Array summaries;
	ERR_FAIL_NULL_V_MSG(p_skeleton, _make_result(diagnostics, summaries, Array()), "A Skeleton3D is required.");
	ERR_FAIL_COND_V_MSG(p_animation.is_null(), _make_result(diagnostics, summaries, Array()), "An Animation is required.");
	const MotionOptions options = _parse_options(p_options);
	ClipContext ctx;
	ctx.skeleton = p_skeleton;
	ctx.animation = p_animation;
	ctx.options = &options;
	ctx.animation_name = p_animation->get_name().is_empty() ? String("animation") : p_animation->get_name();
	ctx.diagnostics = &diagnostics;
	Node *root = options.root_node ? options.root_node : p_skeleton->get_parent();
	_analyze_clip(ctx, root, true, summaries);
	ctx.finish();
	return _make_result(diagnostics, summaries, Array());
}

Dictionary AgentMotionAnalyzer::analyze_scene(const String &p_path, const Dictionary &p_options) {
	Array diagnostics;
	Array summaries;
	Array skipped;
	const MotionOptions options = _parse_options(p_options);

	Ref<PackedScene> scene;
	Node *instance = nullptr;
	Vector<String> load_errors;
	{
		ErrorCapture capture;
		if (ResourceLoader::exists(p_path)) {
			scene = ResourceLoader::load(p_path, "PackedScene");
		}
		if (scene.is_valid()) {
			instance = scene->instantiate(PackedScene::GEN_EDIT_STATE_DISABLED);
		}
		load_errors = capture.messages;
	}
	if (!instance) {
		Dictionary result = _make_result(diagnostics, summaries, skipped);
		result["error"] = load_errors.is_empty() ? vformat("Cannot load %s as a scene", p_path) : vformat("Cannot load %s: %s", p_path, load_errors[0]);
		return result;
	}
	for (const String &message : load_errors) {
		Dictionary d;
		d["code"] = "ANIM_LOAD_ERROR";
		d["severity"] = "warning";
		d["path"] = p_path;
		d["message"] = message;
		diagnostics.push_back(d);
	}

	Vector<AnimationPlayer *> players;
	_collect_players(instance, players);
	for (AnimationPlayer *player : players) {
		const String player_path = String(instance->get_path_to(player));
		if (!options.player_filter.is_empty() && player_path != options.player_filter) {
			continue;
		}
		Node *root = player->get_node_or_null(player->get_root_node());
		LocalVector<StringName> names;
		player->get_animation_list(&names);
		Vector<String> sorted;
		for (const StringName &name : names) {
			sorted.push_back(name);
		}
		sorted.sort();
		for (const String &name : sorted) {
			const String short_name = name.get_slice("/", name.get_slice_count("/") - 1);
			if (!options.animation_filter.is_empty() && name != options.animation_filter && short_name != options.animation_filter) {
				continue;
			}
			const Ref<Animation> anim = player->get_animation(name);
			Dictionary skip;
			skip["path"] = p_path;
			skip["player"] = player_path;
			skip["animation"] = name;
			if (short_name == "RESET") {
				skip["reason"] = "RESET animation";
				skipped.push_back(skip);
				continue;
			}

			// Group the clip's bone tracks by the skeleton they move.
			Vector<Skeleton3D *> skeletons;
			bool has_transform_tracks = false;
			bool has_missing_nodes = false;
			for (int t = 0; t < anim->get_track_count(); t++) {
				const Animation::TrackType type = anim->track_get_type(t);
				if (type != Animation::TYPE_POSITION_3D && type != Animation::TYPE_ROTATION_3D && type != Animation::TYPE_SCALE_3D) {
					continue;
				}
				has_transform_tracks = true;
				const NodePath path = anim->track_get_path(t);
				Node *node = root ? root->get_node_or_null(NodePath(path.get_names(), path.is_absolute())) : nullptr;
				Skeleton3D *skeleton = Object::cast_to<Skeleton3D>(node);
				if (!node) {
					has_missing_nodes = true;
				} else if (skeleton && path.get_subname_count() > 0 && !skeletons.has(skeleton)) {
					skeletons.push_back(skeleton);
				}
			}
			if (skeletons.is_empty()) {
				if (has_missing_nodes) {
					ClipContext ctx;
					ctx.path = p_path;
					ctx.player = player_path;
					ctx.animation_name = name;
					ctx.diagnostics = &diagnostics;
					ctx.add("ANIM_SKELETON_NOT_FOUND", "error", String(), "the clip's bone tracks point at nodes that don't exist, so no Skeleton3D was found");
					Dictionary summary;
					summary["path"] = p_path;
					summary["player"] = player_path;
					summary["animation"] = name;
					summary["length"] = Math::snapped(anim->get_length(), 0.0001);
					summary["loop_mode"] = _loop_mode_name(anim->get_loop_mode());
					summary["sample_rate"] = options.sample_rate;
					summaries.push_back(summary);
				} else {
					skip["reason"] = has_transform_tracks ? "moves nodes, not skeleton bones" : "no position, rotation or scale tracks";
					skipped.push_back(skip);
				}
				continue;
			}
			for (int i = 0; i < skeletons.size(); i++) {
				ClipContext ctx;
				ctx.skeleton = skeletons[i];
				ctx.animation = anim;
				ctx.options = &options;
				ctx.path = p_path;
				ctx.player = player_path;
				ctx.animation_name = name;
				ctx.diagnostics = &diagnostics;
				_analyze_clip(ctx, root, i == 0, summaries);
				ctx.finish();
			}
		}
	}
	memdelete(instance);
	return _make_result(diagnostics, summaries, skipped);
}

String AgentMotionAnalyzer::format_text(const Dictionary &p_result, int p_max_diagnostics, bool p_errors_only) {
	const Dictionary result = _filter(p_result, p_max_diagnostics, p_errors_only);
	const int errors = result.get("errors", 0);
	const int warnings = result.get("warnings", 0);
	const int clips = result.get("clips", 0);
	const int skipped = Array(result.get("skipped", Array())).size();
	String text;
	if (errors == 0 && warnings == 0) {
		text = vformat("OK: %d clip%s", clips, clips == 1 ? "" : "s");
	} else {
		text = vformat("FAIL: %d error%s, %d warning%s in %d clip%s", errors, errors == 1 ? "" : "s", warnings, warnings == 1 ? "" : "s", clips, clips == 1 ? "" : "s");
	}
	if (skipped > 0) {
		text += vformat(" (%d skipped)", skipped);
	}

	const Array list = result["diagnostics"];
	for (int i = 0; i < list.size(); i++) {
		const Dictionary d = list[i];
		PackedStringArray parts;
		parts.push_back(String(d["severity"]).to_upper());
		if (d.has("path")) {
			parts.push_back(d["path"]);
		}
		if (d.has("animation")) {
			parts.push_back("[" + String(d["animation"]) + "]");
		}
		if (d.has("bone")) {
			parts.push_back(d["bone"]);
		}
		if (d.has("start_time")) {
			parts.push_back(_frames_text(d["start_time"], d["end_time"]));
		}
		parts.push_back(d["code"]);
		parts.push_back(d["message"]);
		text += "\n" + String(" ").join(parts);
	}
	if (result.has("not_shown")) {
		text += vformat("\n(+%d more not shown; use --max-diagnostics 0 for all)", int(result["not_shown"]));
	}
	return text;
}

int AgentMotionAnalyzer::run_cli() {
	PackedStringArray scenes;
	Dictionary options;
	bool json = false;
	String json_out;
	int max_diagnostics = 20;
	bool errors_only = false;

	const List<String> args = OS::get_singleton()->get_cmdline_args();
	bool collecting = false;
	for (const List<String>::Element *E = args.front(); E; E = E->next()) {
		const String &arg = E->get();
		const String next = E->next() ? E->next()->get() : String();
		if (arg == "--analyze-animation") {
			collecting = true;
			continue;
		}
		if (collecting && !arg.begins_with("--")) {
			scenes.push_back(arg);
			continue;
		}
		collecting = false;
		if (arg == "--json") {
			json = true;
		} else if (arg == "--json-out" && !next.is_empty()) {
			json_out = next;
		} else if (arg == "--max-diagnostics" && !next.is_empty()) {
			max_diagnostics = next.to_int();
		} else if (arg == "--only" && next == "errors") {
			errors_only = true;
		} else if (arg == "--animation" && !next.is_empty()) {
			options["animation"] = next;
		} else if (arg == "--player" && !next.is_empty()) {
			options["player"] = next;
		} else if (arg == "--feet" && !next.is_empty()) {
			options["feet"] = next;
		} else if (arg == "--sample-rate" && next.is_valid_float()) {
			options["sample_rate"] = next.to_float();
		}
	}
	if (scenes.is_empty()) {
		agent_write_stdout("Usage: --analyze-animation <scene> [<scene> ...] [--animation <name>] [--player <node path>] [--feet A,B] [--sample-rate <hz>] [--json] [--json-out <file>] [--max-diagnostics <n>] [--only errors]");
		return 2;
	}

	Ref<AgentMotionAnalyzer> analyzer;
	analyzer.instantiate();
	Array diagnostics;
	Array summaries;
	Array skipped;
	for (const String &scene : scenes) {
		const Dictionary part = analyzer->analyze_scene(scene, options);
		if (part.has("error")) {
			agent_write_stdout("ERROR: " + String(part["error"]));
			return 2;
		}
		diagnostics.append_array(part["diagnostics"]);
		summaries.append_array(part["clip_summaries"]);
		skipped.append_array(part["skipped"]);
	}
	if (summaries.is_empty()) {
		agent_write_stdout(options.has("animation") ? vformat("ERROR: No skeletal animation named \"%s\" found.", options["animation"]) : String("ERROR: No skeletal animation clips found."));
		return 2;
	}

	const Dictionary result = _make_result(diagnostics, summaries, skipped);
	const Dictionary filtered = _filter(result, max_diagnostics, errors_only);
	if (!json_out.is_empty()) {
		Ref<FileAccess> f = FileAccess::open(agent_output_path(json_out), FileAccess::WRITE);
		if (f.is_null()) {
			agent_write_stdout("ERROR: Could not write " + json_out);
			return 2;
		}
		f->store_string(JSON::stringify(filtered, "", false));
	}
	agent_write_stdout(json ? JSON::stringify(filtered, "", false) : format_text(result, max_diagnostics, errors_only));
	return bool(result["ok"]) ? 0 : 1;
}

void AgentMotionAnalyzer::_bind_methods() {
	ClassDB::bind_method(D_METHOD("analyze", "skeleton", "animation", "options"), &AgentMotionAnalyzer::analyze, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("analyze_scene", "path", "options"), &AgentMotionAnalyzer::analyze_scene, DEFVAL(Dictionary()));
	ClassDB::bind_static_method("AgentMotionAnalyzer", D_METHOD("format_text", "result", "max_diagnostics", "errors_only"), &AgentMotionAnalyzer::format_text, DEFVAL(20), DEFVAL(false));
}
