# agent_tools

Command-line tools that let an AI agent (or a script, or CI) check a Godot project, explain where
values come from, play the game and look at it, and run editor jobs without anyone at the keyboard.
Everything prints short text by default and has machine-readable output where it matters.

Built into this fork of Godot 4.7.2 (`modules/agent_tools`). The full reference for each class is
in the editor's help (search for `Agent`), and `godot --help` lists every flag.

| I want to... | Use |
|---|---|
| Find broken scripts, scenes and references | [`--validate-project`](#check-a-project---validate-project) |
| Know why a node property has the value it has | [`--explain-property`, `--find-overrides`, `--resource-users`](#where-a-value-comes-from) |
| Play the game and check what happens | [`--agent-test`](#gameplay-tests---agent-test) |
| See what the game rendered, or what's under a pixel | [`--offscreen`, `screenshot()`, `pick()`](#screenshots-and-picking---offscreen) |
| Run an editor job (bake, export, import) from a script | [`--unattended`](#unattended-runs---unattended) |

```bash
GODOT=/path/to/godot-engine/godot/bin/godot.macos.editor.arm64
```

Use this optimized build to run the tools. The `.dev` build that `check.sh` uses is for working on
the module: it rebuilds in seconds but runs 3–5× slower.

## Exit codes

| Code | Meaning |
|---|---|
| 0 | Clean: no errors, all tests passed |
| 1 | Problems found: errors in the project, failing tests, or a bad `--dialog-answer` rule |
| 2 | `--validate-project` couldn't run (for example, an unreadable project) |
| 3 | `--unattended`: a dialog with no matching rule stopped the run |

## Check a project: `--validate-project`

Imports the project, then checks every script, every dependency and every scene or resource load.
Errors are grouped by root cause, so one broken script used by ten scenes is one error, not eleven.

```bash
$GODOT --headless --quiet --path . --validate-project
```

```
FAIL: 3 errors, 1 warning in 32 files
ERROR res://broken/shared_broken.gd:5:8 SCRIPT_PARSE_ERROR Expected closing ")" after call arguments. (+2 more in this file, likely caused by this one) (affects 3: res://broken/scene_a.tscn, res://broken/scene_b.tscn, res://broken/scene_c.tscn)
ERROR res://broken/missing_texture.tscn MISSING_DEPENDENCY res://broken/does_not_exist.png not found
WARNING res://broken/bad_uid.tscn INVALID_UID uid://bogusuid0000 is unknown; loads only via fallback path res://good/player.gd
```

Options: `--json` (print JSON instead), `--json-out <file>`, `--max-diagnostics <n>` (default 20,
0 for all), `--only errors`.

Errors Godot itself prints while the editor starts up are hidden, because the report already
covers them. If the check can't run (exit 2), they're printed as `startup error:` lines.

Codes: `SCRIPT_PARSE_ERROR`, `MISSING_DEPENDENCY`, `INVALID_UID` (warning), `LOAD_FAILED`, `LOAD_ERROR`,
and `SCRIPT_WARNING_<NAME>` for GDScript warnings, such as `SCRIPT_WARNING_STANDALONE_TERNARY`.

From GDScript: `AgentValidator.new().validate_project()` or `.validate_paths(paths)` return the same
result as a Dictionary.

## Where a value comes from

These read saved files only. A value a script sets at runtime isn't traced; for a node with a
script, the output adds a note that the script may change the value at runtime.

**`--explain-property <scene> <node> <property>`**: every layer that sets the property, from the
value that wins down to the class default. `<node>:<property>` also works.

```bash
$GODOT --headless --quiet --path . --explain-property res://prov/level.tscn FrontDoor collision_layer
```

```
res://prov/level.tscn FrontDoor:collision_layer = 4
  > 4  res://prov/level.tscn (instance override)
    2  res://prov/door_variant.tscn (base scene)
    1  StaticBody3D (class default)
```

**`--find-overrides <scene>`**: every scene that instances or inherits it, with what each changes.

```
3 places instance or inherit res://prov/door_variant.tscn
  res://prov/level.tscn FrontDoor (instance): collision_layer=4
  res://prov/level.tscn BackDoor (instance): no overrides
  res://prov/level.tscn BackDoor/CollisionShape3D (child override): disabled=true
```

**`--resource-users <path>`**: every scene or resource that uses a file, and which property uses it.

```
1 files use res://prov/shared_box.tres
  res://prov/door_base.tscn (uses): CollisionShape3D:shape
```

Add `--json` to any of them. From GDScript, `AgentInspector.explain_property(node, property)` also
works in a running game, and adds the live `runtime_value` to compare with the saved one.

## Gameplay tests: `--agent-test`

Runs the game (not the editor) and every `test_*` method of scripts that extend `AgentTest`.
Each test gets a fresh scene tree and the same random seed, and time runs at a fixed 60 physics
frames per second, as fast as the machine allows. The same test gives the same result every run.

```bash
$GODOT --headless --quiet --path . --agent-test               # every test in res://tests/agent
$GODOT --headless --quiet --path . --agent-test res://tests/door --filter walks
```

Options: `[dir]` (default `res://tests/agent`), `--filter <text>` (test names containing it),
`--test-timeout <seconds>` (default 60, in game time), `--json`, `--json-out <file>`,
`--junit-out <file>`, `--screenshot-dir <dir>`, `--screenshot-scale <factor>`.

### Writing a test

```gdscript
extends AgentTest


func test_player_walks_through_open_door() -> void:
	var level := load_scene("res://game/door_level.tscn")
	var player: CharacterBody3D = level.get_node("Player")
	track_body(player)                         # record what it bumps into
	await press_action("interact")             # press for 0.1 s, then release
	var opened: bool = await signal_or_timeout(level.get_node("Door"), "opened", 2.0)
	assert_true(opened, "door should open")
	await press_action("move_forward", 2.0)
	assert_true(player.position.z < -1.0, "player should pass the doorway (z = %.2f)" % player.position.z)
```

| Kind | Methods |
|---|---|
| Scenes | `load_scene(path)` returns the root node |
| Input actions | `press_action(name, seconds = 0.1)`, `hold_action(name)`, `release_action(name)` |
| Physical keys | `press_key(KEY_W, seconds = 0.1)`, `hold_key(key)`, `release_key(key)`. Use these when the game reads keys with `Input.is_physical_key_pressed()` instead of input actions. |
| Waits (all `await`-able) | `physics_frames(n)`, `seconds(s)`, `signal_or_timeout(object, signal, timeout = 5.0)`, which returns `true` if the signal came |
| Checks | `assert_true`, `assert_false`, `assert_eq`, `assert_near(actual, expected, tolerance = 0.001)`, `fail(message)`. The first failure is reported; the test keeps running. |
| Tracing | `track_body(body)`, `trace(text)`, `get_trace()`, `get_frame()` |
| Looking | `screenshot(name)`, `pick(position)`; see below |

Input sent, signals awaited and collisions of tracked bodies are recorded automatically. A passing
test prints nothing; a failing one prints its failure and the last few trace lines, so it explains
itself:

```
FAIL res://tests/agent_failing/test_door.gd::test_player_walks_through_open_door (line 12): player should pass the doorway (z = 0.50)
  f30 signal Level/Door.opened received
  f30 press move_forward
  f61 Level/Player hit Level/Door/Blocker at (0.0, 0.0, 0.500086)
  f150 release move_forward
  f150 FAIL player should pass the doorway (z = 0.50)
FAIL: 2 tests, 1 passed, 1 failed (155 physics frames)
```

`fNN` is the physics frame. A script error in a test fails that test, and a test script that
doesn't parse is reported as a failing `(load)` test, not skipped.

## Screenshots and picking: `--offscreen`

`--headless` renders nothing. `--offscreen` (macOS) renders real frames with no window, so tests
can take screenshots. It works with `--agent-test` and with `--editor`.

```bash
$GODOT --offscreen --quiet --path . --agent-test res://tests/visual --screenshot-dir shots
```

- `screenshot(name)` saves `<dir>/<test>_<name>.png` and a `.json` next to it with the frame,
  size, renderer and camera (position, direction, field of view). It returns the PNG path.
  Under `--headless` it fails and asks for `--offscreen`.
- `pick(Vector2(x, y))` says what is under a pixel, as a Dictionary:
  - `control`: the UI control there, if any;
  - `screen`: a `TextureRect` that shows a viewport (for example a low-resolution 3D world scaled
    up to the window), even if it ignores the mouse;
  - `inside`: the result of picking inside a `SubViewport` or a viewport shown by a `TextureRect`;
  - `object`, `hit_position`, `distance`: the 3D or 2D collision shape hit, with its node path.
    `object` is `null` if nothing is there.

From GDScript outside tests: `AgentInspector.capture(path, scale)` and `AgentInspector.pick(position, viewport)`.

## Unattended runs: `--unattended`

Scripted editor runs hang forever when a dialog opens that nobody will click. With `--unattended`,
every dialog is printed, and one with no matching rule makes Godot quit with exit code 3 instead
of waiting. Nothing is clicked unless you wrote a rule for it.

```bash
$GODOT --editor --path . --unattended --dialog-log dialogs.json -- --my-plugin-flag
```

```
DIALOG "Select lightmap bake file:" "res://scenes/room.lmbake" [Cancel, Save] from res://addons/bake/plugin.gd:38 -> quit (no rule)
suggest: --dialog-answer "Select lightmap bake file:*res://scenes/room.lmbake*=ok" (or =cancel). Check what the button does before adding it.
unattended: 1 dialog, 0 answered by rule, 1 stopped the run
```

- **`--dialog-answer "<pattern>=ok|cancel"`** answers a dialog you know is safe. It can be given
  more than once; the first matching rule wins. The pattern is a case-insensitive wildcard (`*`, `?`)
  matched against the title and text (or a file dialog's path). It must name the dialog: a
  catch-all like `*=ok` is refused. Custom buttons such as "Don't Save" are never pressed.
- **`--dialog-log <file>`** writes one JSON line per dialog: `title`, `text`, `class`, `node`,
  `buttons` (each with `label` and `role`: `ok`, `cancel` or `custom`), `file_path`,
  `script_backtrace`, `frame`, `action` (`ok`, `cancel` or `quit`), `rule` and `suggest`.
- Dialogs can't be listed ahead of time, because many get their text only when they open. Run
  once, read the log, and add a rule for each dialog you've checked.
- Pressing OK can lead to another dialog. Accepting a save dialog for a file that exists opens
  "already exists. Do you want to overwrite it?", which needs its own rule. That's deliberate:
  overwriting is never allowed by accident.

A full lightmap bake with no window, as tested on a real project:

```bash
$GODOT --offscreen --rendering-driver metal --editor --path . --unattended \
  --dialog-answer "Select lightmap bake file:*=ok" \
  --dialog-answer 'Please Confirm...*.lmbake" already exists*=ok' \
  -- --claude-bake
```

From GDScript: `AgentUnattended.get_active()` returns the running instance (or `null`), with
`get_records()`, `has_failed()` and `get_summary()`.

## Gotchas

- **`--headless` vs `--offscreen`.** Use `--headless` for checks and tests that don't need pixels,
  and `--offscreen` for screenshots, `pick()` on rendered views and editor bakes.
- **Vulkan projects.** This fork is built without Vulkan. If a project sets
  `rendering_device/driver.macos="vulkan"`, add `--rendering-driver metal`, or the engine crashes
  at startup.
- **Relative output paths** (`--json-out`, `--junit-out`, `--dialog-log`, `--screenshot-dir`)
  resolve against the folder you ran the command from, not the project folder.
- **`--quiet`** hides GDScript `print()`. The tools' own output still shows; in tests, use
  `printerr()` or `trace()`.
- **Name nodes created in code.** Traces and `pick()` show node paths, and an unnamed collider is
  `@CollisionShape3D@173`. Setting `.name` makes failures readable.
- **`ERROR: Plugin is not attached to debugger.`** It appears when a run is stopped with a dialog
  open. Stock Godot prints it too, and it's harmless.

## Developing the module

From the folder above the engine clone:

```bash
./check.sh          # incremental build + unit tests (godot --test --test-case="*[AgentTools]*")
./check.sh all      # also every phase against test-project/ and test-unattended/
```

Output is kept short; full logs are in `build.log`, `test.log` and `run.log`. Build with
`scons platform=macos target=editor dev_build=yes tests=yes vulkan=no` while developing, and
`scons platform=macos target=editor tests=no vulkan=no` for the optimized `godot.macos.editor.arm64`
(a full build; the two keep separate object files). Class docs live in
`doc_classes/`; after changing bound methods run `bin/godot... --doctool .` and escape `<` and `>`
as `&lt;` and `&gt;` in descriptions.
