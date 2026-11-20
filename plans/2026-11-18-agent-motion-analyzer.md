---
id: 2026-11-18-agent-motion-analyzer
title: Add --analyze-animation motion analyzer to agent_tools
status: In Progress         # Planned | Agreed | In Progress | In Review | Changes Requested | Done | Blocked
baseline_sha: 95d5f0b1cb31ce0ed685e0a63b0d21ae44d95406
planner: Claude Code (Opus 5.5)
plan_checker: Human (project owner)
executor: Claude Code (Opus 5.5)
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

`baseline_sha` is `95d5f0b1cb` ("Add a usage guide for agent_tools"). At that
commit the working tree is clean and `agent-tools` matches `origin`, so the
task's diff contains only this task's changes. Since the first draft of this
plan, the module gained `--offscreen` rendering, `screenshot()`/`pick()`,
`--unattended` and `modules/agent_tools/README.md`. The analyzer has to fit
alongside them, including in that README.

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

### Documentation

The feature isn't done until it's documented in all three places agents and
people look:

1. **`modules/agent_tools/README.md`**, the usage guide:
   - Add a row to the "I want to..." table: "Check a skeletal animation for
     sliding feet, loop pops and broken tracks" → `--analyze-animation`.
   - Add a `## Check animations: --analyze-animation` section after
     "Gameplay tests" and before "Screenshots and picking". Match the existing
     sections: one-paragraph summary, a `$GODOT ...` command, a real text
     output sample copied from a fixture run (not invented), the options, and a
     table of codes with severity and a one-line meaning. Explain how "ground"
     and foot bones are found, the in-place behaviour, the scale reference,
     and the `--feet` override.
   - Add "From GDScript": `AgentMotionAnalyzer.new().analyze(skeleton, animation, options)`
     and `.analyze_scene(path, options)`, plus a short `AgentTest` example
     that asserts a clip has no warnings.
   - Update the exit-code table. Row 2 currently says only `--validate-project`
     couldn't run; it must cover `--analyze-animation` too (unreadable scene,
     no clips matched, bad arguments).
   - Add a Gotchas bullet: results describe the clip on its rest skeleton, so
     IK modifiers, AnimationTree blending and `RESET` aren't applied.
2. **`doc_classes/AgentMotionAnalyzer.xml`**: generated with `--doctool`, then
   every description is filled in. That covers the class, each method, each
   `options` key with its default, and the result dictionary's keys. `<` and
   `>` are escaped as the README's "Developing the module" section says. No
   other XML in `doc_classes/` or `doc/classes/` may change.
3. **`godot --help`**: one `print_help_option` line for `--analyze-animation`,
   placed with the other agent_tools options in `main/main.cpp`, listing its
   options and exit codes in the same style as `--validate-project`.

### Files touched

Inside this repo:
- `modules/agent_tools/agent_motion_analyzer.h` (new)
- `modules/agent_tools/agent_motion_analyzer.cpp` (new)
- `modules/agent_tools/agent_tools.cpp`: dispatch `--analyze-animation` to `AgentMotionAnalyzer::run_cli()`
- `modules/agent_tools/register_types.cpp`, `modules/agent_tools/config.py`: register and document the class
- `modules/agent_tools/doc_classes/AgentMotionAnalyzer.xml` (new)
- `modules/agent_tools/tests/test_agent_tools.h`: unit tests
- `modules/agent_tools/README.md`: usage guide section, table row, exit codes, gotcha
- `main/main.cpp`: add `--analyze-animation` to the existing `MODULE_AGENT_TOOLS_ENABLED` flag branch (same `editor`/`cmdline_tool`/`wait_for_import`/`quit_after` setup as `--validate-project`) and one `print_help_option` line
- `plans/2026-11-18-agent-motion-analyzer.md` (this file)

Outside this repo, so the reviewer must check them separately, not through
`git diff`:
- `../test-project/anim/`: fixture scenes, plus `make_fixtures.gd`, the script that generates them
- `../check.sh`: new `phase6` (phases 1–5 exist), added to the usage line and to `all`

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
- [ ] Documentation, as described in Approach → Documentation:
  - [ ] `README.md` has the table row, the `--analyze-animation` section with a
        real output sample, the GDScript and `AgentTest` usage, the updated
        exit-code table and the gotcha;
  - [ ] every code the analyzer can emit appears in the README's code table and
        in `AgentMotionAnalyzer.xml`, and no code is listed that the analyzer
        can't emit;
  - [ ] `AgentMotionAnalyzer.xml` has no empty `<description>` elements;
  - [ ] after running `--doctool .`, `git status` shows no changes outside
        this task's files;
  - [ ] `godot --help` lists `--analyze-animation` with its options.
- [ ] `./check.sh all` passes, including the existing phase1–5 checks, which
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
  (`--offscreen` and `screenshot()` now exist, so a filmstrip is the natural
  next task after this one.)
- `AgentTest` helpers such as `track_bone`; scripts can call
  `AgentMotionAnalyzer` directly in this task.
- Automatically fixing clips. The analyzer only reports.
- Adding the missing workflow files (`CLAUDE.md`, `plans/_template.md`) to this repo.

## Commands / verification

Known useful commands in this repo (run from `/Users/jason/projects/godot-engine`):
- `./check.sh`: incremental build (`scons platform=macos target=editor dev_build=yes tests=yes vulkan=no -j8`) plus `[AgentTools]` unit tests
- `./check.sh phase6`: build, unit tests, and the new CLI checks
- `./check.sh all`: every phase
- `godot/bin/godot.macos.editor.dev.arm64 --test --test-case="*[AgentTools]*"`: unit tests only
- `godot/bin/godot.macos.editor.dev.arm64 --headless --path test-project --script res://anim/make_fixtures.gd`: regenerate fixtures
- `(cd godot && bin/godot.macos.editor.dev.arm64 --doctool .)`: regenerate class XML after changing bound methods

Expected verification for this task:
- [ ] `./check.sh all` passes (build ok, unit tests pass, phase1–6 ok).
- [ ] Manual run: `godot/bin/godot.macos.editor.dev.arm64 --headless --quiet --path test-project --analyze-animation res://anim/broken_walk.tscn`. Read the text output and confirm each diagnostic names a bone, a time range and value > threshold.
- [ ] Manual run of the same command with `--json`, piped to `python3 -m json.tool`. Confirm it parses.
- [ ] Manual run on a real imported humanoid (any Mixamo or Kimodo-derived `.glb` placed in `test-project/anim/`, not committed). Confirm it completes in under 2 seconds and the findings are plausible. Note the result in the Execution log.
- [ ] Run `--doctool .`, then `git -C godot status`: no changes outside "Files touched".
- [ ] Read the new README section start to finish. Run its `$GODOT` example as written and confirm the output matches the sample.
- [ ] `godot/bin/godot.macos.editor.dev.arm64 --help | grep -A1 analyze-animation` shows the option.
- [ ] `git -C godot diff --stat <baseline_sha>..HEAD` lists only files from "Files touched".

## Child tasks (optional)

None.

## Acknowledgements
Each role ticks its box ONLY after reading the planner-owned sections above
(Context, Approach, Acceptance criteria, Out of scope, Commands / verification)
AND writing a one-sentence restatement in its own words.

- [x] **Planner** (Claude Code, Opus 5.5) — 2026-11-18 — "Add a headless `--analyze-animation` command and scriptable `AgentMotionAnalyzer` class that samples each skeletal clip on its rest skeleton with its own forward kinematics. It reports foot slide, ground penetration, loop seams and drift, rotation spikes, bone stretch and unresolved tracks as validator-style diagnostics with times, frames and bones, so agents can check motion they can't watch. It's documented in the module README, the class XML and `--help`."
- [x] **Plan checker** (Human, project owner) — 2026-11-18 — Approved in chat ("I'll approve it myself") after the revision that added the Documentation section and phase6; no separate restatement was written.
- [x] **Executor** (Claude Code, Opus 5.5) — 2026-11-18 — "I'll build `AgentMotionAnalyzer` and `--analyze-animation`, which evaluate each clip's bone tracks on the rest skeleton with offline forward kinematics. They report the listed ANIM_* codes with bone, time and frame ranges in the validator's result shape, and I'll prove it with in-memory unit tests, generated fixture scenes and check.sh phase6. I'll document it in the README, the class XML and --help, touching only the files listed."
- [ ] **Reviewer** (<tool/model>) — <date> — "<restatement after seeing the diff>"

If your restatement contradicts the previous tool's, DO NOT tick the box. Add a
note starting with `MISMATCH:` describing the disagreement, and hand back to the
planner.

## Execution log
Append-only. Executor writes here. One bullet per meaningful step, commit, or
deviation. No rewrites.

- 2026-11-18 — Plan approved by the project owner in chat (status Planned → Agreed). Executor changed from Cursor to Claude Code at the owner's request. Status → In Progress.

## Review
Filled in by reviewer at the end.

- Diff command: `git diff <baseline_sha>..HEAD`
- Acceptance criteria met: yes | no | partial — <notes>
- Unrelated changes detected: <none | list>
- Result: Approved | Changes Requested | Blocked
- Notes:
- Reviewer: <tool/model> — <date>
- 2026-11-18 — Added `AgentMotionAnalyzer` (`agent_motion_analyzer.{h,cpp}`), registered it, dispatched `--analyze-animation` from `agent_tools.cpp`, and added the flag and `--help` line to `main/main.cpp`. Built clean.
- 2026-11-18 — Added 10 `[AgentTools]` unit tests covering each acceptance case. 19/19 pass (66 assertions in the new cases). Confirmed they bite by breaking one expectation on purpose: it failed, then I restored it.
- 2026-11-18 — Deviation (detection detail, flagged for review): contact and penetration are measured against each foot bone's own rest height, not one ground plane at the lowest foot or toe. With a single ground, ankles sit above the toes and are never planted, so heel slides would be missed.
- 2026-11-18 — Deviation (detection detail, flagged for review): `ANIM_BONE_LENGTH` skips root bones and their direct children. Hips translate relative to the root in almost all mocap, so checking them would flag every walk.
- 2026-11-18 — Clarification: `ANIM_ROTATION_SPIKE` needs the change to break the limit both relative to the parent and in skeleton space, so one snapping bone isn't also reported for every child. `ANIM_LOOP_SEAM` compares local rotations, and checks feet relative to the hips' horizontal position so root travel isn't a jump. Seam and drift are checked for `LOOP_LINEAR` only (pingpong can't pop). With no hips bone, travel uses the shallowest bone that has a position track.
- 2026-11-18 — Deviation (output format): text output uses ASCII ("20.0 deg", "0.25-0.75s") instead of the plan's "°" and en dash. Godot reads C++ string literals as Latin-1, and the first fixture run printed mojibake.
- 2026-11-18 — Additions within scope: results include a `skipped` list (RESET clips, clips that move no bones), counted in the text header. `ANIM_LOAD_ERROR` is a warning.
- 2026-11-18 — Added `../test-project/anim/make_fixtures.gd`, generated `good_walk.tscn` (clean `idle` plus `RESET`) and `broken_walk.tscn` (`slide`, `seam`, `spike`, `unresolved`), and added `anim/tests/test_analyzer_in_tests.gd`, the README's AgentTest example. Added `phase6` to `../check.sh` (fixtures, exit codes 0/1/2, every expected code, no raw engine errors, identical repeat run, JSON keys, AgentTest use, `--help`). `./check.sh phase6` passed, before the agent-test and help checks were added.
- 2026-11-18 — Docs: ran `--doctool .`; only `AgentMotionAnalyzer.xml` appeared and nothing else changed. Filled in every description. README: table row, `--analyze-animation` section with real output from the fixture command, GDScript and AgentTest usage, exit-code table, gotcha, `phase6` in the developer notes. The README's AgentTest example passes (2/2) with the current binary.
- 2026-11-18 — Blocked on disk space: the disk is 99% full (2.3 GB free), and the relink after the XML change fails with `ranlib: error writing file 'bin/obj/editor/libeditor.macos.editor.dev.arm64.a'`. Still to run: the final `./check.sh all`, the real-humanoid `.glb` manual check, and the diff-scope check. Status stays In Progress.
