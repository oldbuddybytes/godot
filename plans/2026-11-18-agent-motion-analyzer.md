---
id: 2026-11-18-agent-motion-analyzer
title: Add --analyze-animation motion analyzer to agent_tools
status: Planned             # Planned | Agreed | In Progress | In Review | Changes Requested | Done | Blocked
baseline_sha: d0efd68fcee1fd0a2e353eff8bdbebd3ab9e3576
planner: Claude Code (Opus 5.5)
plan_checker: Codex Desktop (GPT 5.5)
executor: Cursor (Composer)
reviewer: Claude Desktop or Codex Desktop (whichever did not plan-check)
created: 2026-11-18
updated: 2026-11-18
parent:
---

# Add --analyze-animation motion analyzer to agent_tools

## Context

AI agents working in this Godot fork can already validate a project
(`--validate-project`), trace where a property value comes from
(`--explain-property`, `--find-overrides`, `--resource-users`) and drive gameplay
tests (`--agent-test`). None of that covers skeletal animation. An agent that
imports, retargets or generates a clip (Mixamo, NVIDIA Kimodo, procedural code)
has no way to tell whether the motion is broken: it only sees still screenshots.

The common defects are measurable: feet sliding while planted, feet going
through the floor, a pop where a loop wraps, a bone snapping half a turn between
two frames (typical of bad retargets), bones stretching, and tracks pointing at
bones that don't exist. This task adds a headless analyzer that samples a clip
on its skeleton and reports those defects as structured diagnostics with times,
frame ranges and bone names, so an agent can fix and re-check a clip without a
human watching it.

Desired outcome: `godot --headless --path <project> --analyze-animation
res://chars/guard.tscn --json` lists every defect found in every clip in that
scene and exits non-zero when there are errors or warnings, in the same style as
`--validate-project`.

**Precondition:** the working tree at `baseline_sha` has uncommitted
agent_tools changes (`agent_common.h`, `agent_test*.{h,cpp}`,
`agent_test_runner.cpp`, `agent_validator.cpp`, `config.py`,
`tests/test_agent_tools.h`, and untracked `doc_classes/`). Those must be
committed to the `agent-tools` branch first. The planner then re-records
`baseline_sha`, so this task's diff contains only this task's changes.

## Approach

### New class: `AgentMotionAnalyzer` (RefCounted)

New files `modules/agent_tools/agent_motion_analyzer.{h,cpp}`. Registered in
`register_types.cpp`, listed in `config.py` `get_doc_classes()`, documented in
`doc_classes/AgentMotionAnalyzer.xml`.

Scripting API (bound, so `AgentTest` scripts and editor plugins can call it):

- `analyze(skeleton: Skeleton3D, animation: Animation, options: Dictionary = {}) -> Dictionary`.
  Analyzes one clip on one skeleton. Track paths are resolved relative to the
  skeleton's parent by default, or relative to `options.root_node` (a Node).
- `analyze_scene(path: String, options: Dictionary = {}) -> Dictionary`. Loads a
  PackedScene (`.tscn`, `.scn`, or an imported `.glb`/`.gltf`/`.fbx`/`.blend`),
  instantiates it without adding it to the tree, finds every `AnimationPlayer`,
  and analyzes each clip in its libraries against the `Skeleton3D` its tracks
  target. Frees the instance afterwards.
- `static format_text(result: Dictionary, max_diagnostics := 20, errors_only := false) -> String`.
- `static run_cli() -> int`: the `--analyze-animation` entry point.

The result shape mirrors `AgentValidator`, so agents parse both the same way:

```
{ ok, errors, warnings, clips,
  clip_summaries: [ { path, player, animation, length, loop_mode, sample_rate,
                      in_place, travel_distance, average_speed,
                      foot_contact_ratio: {bone: ratio} } ],
  diagnostics: [ { code, severity, path, player, animation, bone,
                   start_time, end_time, start_frame, end_frame,
                   value, threshold, message } ],
  not_shown? }
```

`start_frame`/`end_frame` are sample indices at `sample_rate`. `value` and
`threshold` are the measured number and the limit it broke. Fields that don't
apply are omitted, not null.

### How poses are computed (key decision)

The analyzer does its own forward kinematics from the `Animation` and the
skeleton's rest poses. It does not play the clip through `AnimationPlayer` /
`AnimationMixer` in a SceneTree.

- For each sample time, each bone's local transform is its rest, with
  position/rotation/scale overridden by the clip's `position_3d` /
  `rotation_3d` / `scale_3d` track for that bone. Values come from the engine's
  own `*_track_interpolate`, so interpolation and loop wrapping match playback.
- Global transforms are built parent-first, using `get_bone_parent`.
- Track path resolution: `NodePath` of node plus subname bone, relative to the
  player's `root_node`. This is the same resolution `AnimationMixer` uses.
- **Why not the AnimationMixer:** offline evaluation is deterministic, fast, needs
  no SceneTree, and measures the clip itself. Playing through the mixer would
  also apply SkeletonModifiers (IK, LookAt) and blending, which hides defects in
  the source clip. Modifier and AnimationTree effects are out of scope.
- Value and method tracks, blend shapes and Bezier tracks are ignored: they don't
  affect bone poses.

### Scale and thresholds

All distance thresholds are fractions of a per-skeleton **scale reference**:
the height of the `Hips` bone in the global rest pose if a bone with that name
exists, otherwise half the vertical extent of all bone origins in the global
rest pose. The same defaults then work for any unit scale. Defaults (every
default can be overridden through the `options` dictionary in the scripting
API):

| Option | Default | Used by |
|---|---|---|
| `sample_rate` | 60 Hz | all |
| `contact_height` | 0.03 × scale reference above ground | foot checks |
| `slide_speed` | 0.15 × scale reference per second, sustained ≥ 3 samples | `ANIM_FOOT_SLIDE` |
| `penetration_depth` | 0.03 × scale reference below ground | `ANIM_GROUND_PENETRATION` |
| `spike_angle` | 30° within one sample at 60 Hz (scaled to sample rate) | `ANIM_ROTATION_SPIKE` |
| `seam_angle` | 5°, and ≥ 3× the bone's 95th-percentile per-sample change elsewhere in the clip | `ANIM_LOOP_SEAM` |
| `bone_length_tolerance` | 5% of rest length | `ANIM_BONE_LENGTH` |

**Ground** is the lowest foot or toe bone origin in the global rest pose. The
skeleton is assumed to stand on the ground at rest.

**Foot bones:** use `options.feet` (CLI `--feet A,B,...`) if given. Otherwise
use the SkeletonProfileHumanoid names `LeftFoot`, `RightFoot`, `LeftToes`,
`RightToes`. Otherwise fall back to a case-insensitive match on names containing
`foot`, `toe` or `ankle`, for example `mixamorig:LeftFoot`. If no foot bones are
found, the foot checks are skipped and an `ANIM_FEET_NOT_FOUND` info entry is
added. The fallback is a heuristic; the `--feet` override exists for rigs it
misses.

**In-place clips:** a clip is `in_place` when the hips (or the root bone, if
there is no hips bone) travel less than 0.1 × scale reference horizontally over
the clip. For in-place clips, a planted foot legitimately moves backward at the
treadmill speed. In that case slide is measured as the foot's deviation from the
clip's median contact velocity, not from zero.

### Checks and codes

| Code | Severity | Condition |
|---|---|---|
| `ANIM_EMPTY` | error | Clip has zero length or no tracks that affect bones. |
| `ANIM_TRACK_UNRESOLVED` | error | A position/rotation/scale track points at a node or bone that doesn't exist. Reported once per track. |
| `ANIM_SKELETON_NOT_FOUND` | error | `analyze_scene`: a player's tracks resolve to no `Skeleton3D`. |
| `ANIM_ROTATION_SPIKE` | warning | A bone's global rotation changes more than `spike_angle` between consecutive samples. Consecutive spike samples merge into one ranged diagnostic. |
| `ANIM_LOOP_SEAM` | warning | Looping clip only: the per-sample change across the wrap (last sample → first sample after wrapping) breaks `seam_angle` for any bone, or a foot or the hips jump by more than `contact_height` across the wrap. |
| `ANIM_FOOT_SLIDE` | warning | A foot bone is in contact (within `contact_height` of ground) and its horizontal speed (or deviation, for in-place clips) is above `slide_speed` for ≥ 3 consecutive samples. One diagnostic per contiguous range. |
| `ANIM_GROUND_PENETRATION` | warning | A foot or toe bone origin is more than `penetration_depth` below ground. Ranged. |
| `ANIM_BONE_LENGTH` | warning | A non-root bone's distance to its parent differs from its rest distance by more than `bone_length_tolerance`. Ranged. |
| `ANIM_LOOP_DRIFT` | warning | Looping clip only: the hips' height, or the yaw of the root (or hips) bone, differs between start and end by more than `contact_height` or 5°. For in-place looping clips, horizontal displacement counts as drift too. |
| `ANIM_NEVER_GROUNDED` | info | Foot bones exist but none is in contact at any sample. This is legitimate for falls and swims. |
| `ANIM_FEET_NOT_FOUND` | info | See above. |

Info entries appear in `diagnostics` but don't count toward `errors` or
`warnings` and don't affect `ok`. For each code, at most 10 diagnostics are
emitted per bone per clip; the rest are summarized in the last entry's message.

### CLI

```
godot --headless --path <project> --analyze-animation <scene> [<scene> ...]
      [--animation <name>] [--player <node path>] [--feet A,B,...]
      [--sample-rate <hz>] [--json] [--json-out <file>]
      [--max-diagnostics <n>] [--only errors]
```

- Exit codes: `0` no errors or warnings; `1` errors or warnings found; `2` usage
  error, unreadable scene, or no clips matched.
- `--json-out` paths resolve via `agent_output_path`. stdout goes through
  `agent_write_stdout`, as in the validator.
- Text output starts with `OK: N clips` or `FAIL: E errors, W warnings in N clips`,
  then one line per diagnostic:
  `WARNING res://guard.tscn [walk] LeftFoot 1.20–1.45s ANIM_FOOT_SLIDE 0.31 m/s > 0.15`.
- Engine errors raised while loading are captured with `ErrorCapture` and
  reported as `ANIM_LOAD_ERROR`, never printed raw.

### Files touched

Inside this repo:
- `modules/agent_tools/agent_motion_analyzer.h` (new)
- `modules/agent_tools/agent_motion_analyzer.cpp` (new)
- `modules/agent_tools/agent_tools.cpp`: dispatch `--analyze-animation` to `AgentMotionAnalyzer::run_cli()`
- `modules/agent_tools/register_types.cpp`, `modules/agent_tools/config.py`: register and document the class
- `modules/agent_tools/doc_classes/AgentMotionAnalyzer.xml` (new)
- `modules/agent_tools/tests/test_agent_tools.h`: unit tests
- `main/main.cpp`: add `--analyze-animation` to the existing `MODULE_AGENT_TOOLS_ENABLED` flag branch (same `editor`/`cmdline_tool`/`wait_for_import`/`quit_after` setup as `--validate-project`) and one `print_help_option` line
- `plans/2026-11-18-agent-motion-analyzer.md` (this file)

Outside this repo, so the reviewer must check them separately, not through
`git diff`:
- `../test-project/anim/`: fixture scenes, plus `make_fixtures.gd`, the script that generates them
- `../check.sh`: new `phase4` (the usage line already mentions `phase4`; the `case` doesn't handle it yet), added to `all`

### Ruled out

- **Playing through AnimationPlayer:** see "How poses are computed".
- **Exposing every threshold as a CLI flag:** the scripting `options` dictionary
  covers tuning; the CLI stays small. Add flags later if agents need them.
- **Hand-writing fixture `.tscn` files:** they're generated by `make_fixtures.gd`,
  so they're reproducible and easy to review.

## Acceptance criteria

- [ ] `AgentMotionAnalyzer` is registered, documented (every method and its
      options have descriptions in `AgentMotionAnalyzer.xml`) and callable from
      GDScript.
- [ ] Unit tests in `tests/test_agent_tools.h` (tagged `[AgentTools]`) build a
      skeleton and animations in memory and check each of these:
  - [ ] a clean looping clip (feet planted while in contact, matching first and
        last poses) produces zero errors and warnings and `ok == true`;
  - [ ] a clip whose last pose differs from its first by 20° on one bone produces
        `ANIM_LOOP_SEAM` naming that bone, with `start_time` at the wrap;
  - [ ] a 120° snap between two keys one sample apart produces exactly one
        `ANIM_ROTATION_SPIKE` with the right bone and frame;
  - [ ] a foot moving horizontally at 0.5 × scale reference per second while at
        ground height produces `ANIM_FOOT_SLIDE` with a frame range covering
        the sliding samples (±1 sample);
  - [ ] an in-place walk whose planted foot moves backward at a constant speed
        produces no `ANIM_FOOT_SLIDE`, and its summary has `in_place == true`;
  - [ ] a foot 0.1 × scale reference below ground produces `ANIM_GROUND_PENETRATION`;
  - [ ] a position track stretching a shin by 20% produces `ANIM_BONE_LENGTH`;
  - [ ] a track to a non-existent bone produces an `ANIM_TRACK_UNRESOLVED` error and `ok == false`;
  - [ ] a skeleton with no recognizable foot bones produces `ANIM_FEET_NOT_FOUND`
        (info) and still runs the other checks; passing `feet` in options
        enables the foot checks;
  - [ ] thresholds scale: the same slide clip on a skeleton scaled ×100 gives
        the same codes and frame ranges;
  - [ ] `format_text` produces the `OK:` / `FAIL:` header and per-diagnostic
        lines described in Approach, and respects `max_diagnostics` and `errors_only`.
- [ ] `--analyze-animation` on the fixture project:
  - [ ] the clean fixture exits 0 and prints `OK:`;
  - [ ] the broken fixture exits 1 and its output contains `ANIM_FOOT_SLIDE`,
        `ANIM_LOOP_SEAM`, `ANIM_ROTATION_SPIKE` and `ANIM_TRACK_UNRESOLVED`;
  - [ ] a missing scene path exits 2, and so does `--animation` with an unknown name;
  - [ ] `--json` output parses as JSON and has the keys listed in Approach;
  - [ ] two runs on the same input give byte-identical output.
- [ ] No raw engine error lines appear in the CLI output for the broken fixture.
- [ ] `./check.sh all` passes, including the existing phase1–3 checks, which
      are unchanged.
- [ ] Every changed file in `git diff <baseline_sha>..HEAD` is in the "Files
      touched" list.

## Out of scope

- AnimationTree and state machine validation (a separate planned task).
- Joint limits and hyperextension (knees or elbows bending backward), and jitter
  or high-frequency noise detection. These need per-rig anatomy or tuning, so
  they're a follow-up.
- Effects of SkeletonModifier3D (IK, LookAt, retarget modifiers), AnimationTree
  blending, and the `RESET` animation.
- 2D skeletons (`Skeleton2D`), blend shapes, value/method/Bezier tracks.
- Filmstrip or contact-sheet rendering, BVH import, animation mirroring.
- `AgentTest` helpers such as `track_bone`; scripts can call
  `AgentMotionAnalyzer` directly in this task.
- Automatically fixing clips. The analyzer only reports.
- Adding the missing workflow files (`CLAUDE.md`, `plans/_template.md`) to this repo.

## Commands / verification

Known useful commands in this repo (run from `/Users/jason/projects/godot-engine`):
- `./check.sh`: incremental build (`scons platform=macos target=editor dev_build=yes tests=yes vulkan=no -j8`) plus `[AgentTools]` unit tests
- `./check.sh phase4`: build, unit tests, and the new CLI checks
- `./check.sh all`: every phase
- `godot/bin/godot.macos.editor.dev.arm64 --test --test-case="*[AgentTools]*"`: unit tests only
- `godot/bin/godot.macos.editor.dev.arm64 --headless --path test-project --script res://anim/make_fixtures.gd`: regenerate fixtures

Expected verification for this task:
- [ ] `./check.sh all` passes (build ok, unit tests pass, phase1–4 ok).
- [ ] Manual run: `godot/bin/godot.macos.editor.dev.arm64 --headless --quiet --path test-project --analyze-animation res://anim/broken_walk.tscn`. Read the text output and confirm each diagnostic names a bone, a time range and value > threshold.
- [ ] Manual run of the same command with `--json`, piped to `python3 -m json.tool`. Confirm it parses.
- [ ] Manual run on a real imported humanoid (any Mixamo or Kimodo-derived `.glb` placed in `test-project/anim/`, not committed). Confirm it completes in under 2 seconds and the findings are plausible. Note the result in the Execution log.
- [ ] `git -C godot diff --stat <baseline_sha>..HEAD` lists only files from "Files touched".

## Child tasks (optional)

None.

## Acknowledgements
Each role ticks its box ONLY after reading the planner-owned sections above
(Context, Approach, Acceptance criteria, Out of scope, Commands / verification)
AND writing a one-sentence restatement in its own words.

- [x] **Planner** (Claude Code, Opus 5.5) — 2026-11-18 — "Add a headless `--analyze-animation` command and scriptable `AgentMotionAnalyzer` class that samples each skeletal clip on its rest skeleton with its own forward kinematics. It reports foot slide, ground penetration, loop seams and drift, rotation spikes, bone stretch and unresolved tracks as validator-style diagnostics with times, frames and bones, so agents can check motion they can't watch."
- [ ] **Plan checker** (<tool/model>) — <date> — "<restatement / explicit agreement>"
- [ ] **Executor** (<tool/model>) — <date> — "<restatement before starting code>"
- [ ] **Reviewer** (<tool/model>) — <date> — "<restatement after seeing the diff>"

If your restatement contradicts the previous tool's, DO NOT tick the box. Add a
note starting with `MISMATCH:` describing the disagreement, and hand back to the
planner.

## Execution log
Append-only. Executor writes here. One bullet per meaningful step, commit, or
deviation. No rewrites.

## Review
Filled in by reviewer at the end.

- Diff command: `git diff <baseline_sha>..HEAD`
- Acceptance criteria met: yes | no | partial — <notes>
- Unrelated changes detected: <none | list>
- Result: Approved | Changes Requested | Blocked
- Notes:
- Reviewer: <tool/model> — <date>
