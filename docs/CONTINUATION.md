# Phase 2 continuation checkpoint

Risk: medium — a new oracle-derived golden plus three comment corrections; no behaviour changed, but the
batch retires a recorded premise about the level-7 spawn and rewrites what the pin rule is, so a reviewer should
check the measurement rather than trust the narrative.

**The level-7 start is faithful, and the "camera films the level from outside it" diagnosis was wrong.** Kurt
lands on a real floor at `z = 0.01` in DANT_1 — walls at x = ±25.1, y = −16.74 and 36.89, no ceiling — and the
C# oracle, run the same way, lands him on the same step at the same `0.0002` with the same state sequence.
`golden/L7_spawn_fall.txt` (61 lines, byte-identical to the oracle's `fall` mode) and `tick1 dump fall 7 400`
pin it. The follow camera's eye `(0,-4,4.5)` is *inside* that room. Record: `docs/PROGRESS_LOG.md`,
"Iteration 104"; earlier batches are indexed by number in that file.

Last updated: 2026-11-05 (**batch 76 — the spawn lands on a floor; the oracle says so too**).

**Owner action still open (blocks the loop's own fixes taking effect):** the worktree `ralph-pi.sh` is mode
`664` while the index says `100755`, this uid cannot `chmod` it (owner task 18), and the systemd unit execs it
directly — so a restart today fails `203/EXEC` and the loop keeps running a pre-fix copy. Restore `0755` and
restart `ralph-pi-mdk.service`.
**Owner action still open (review-4 F3, review-2 F6):** `ralph/builder.md` carries an uncommitted
operator-authored edit and that path is in `gates_batch_integrity.py`'s `PROTECTED` set; the worktree's
`ralph-pi.sh` / `ralph/LOOP_CONTEXT.md` / `tools/runner/test_review_state.sh` routing now contradicts the
*committed* `OWNER_DIRECTIVE_2026-11-05.md` "Loop routing" block. Commit them with `MDK_OWNER_APPROVED=1` and a
record entry, or revert them. A builder must not stage them (exit 6) and must not set the approval marker, so the
worktree stays permanently dirty and **every batch stages by name, never `git add -A`**.

**Owner action new (review-2 F3):** the "exactly one new frame PNG" rule is satisfiable by a byte-identical
duplicate of an earlier capture — **batches 75 and 76 are each such a duplicate** (`sha256 6646adf1…`), because
nothing in the level-3 host frame's path changed. The fix (reject a new frame whose sha256 already exists, and
require the `emurun.sh` verdict in the sidecar) is an edit to owner-protected gate tooling, so it is recorded
rather than fixed.


## PRIORITY ORDER (owner directive 2026-11-05, in force until withdrawn)

**Goal: a CIA that plays a level end-to-end, verified in Mandarine.** The 1-ULP fidelity work is not thrown
away but it is no longer first, and no new 1-ULP work opens unless a listed item needs it.

1. **DONE (67) — the emulator harness.** `mdk_3ds/tools/emurun.sh` runs `mdk.3dsx` on `DISPLAY=:99` against
   pinned Mandarine r1.2.1 and prints a verdict (exit 3 stale log, 4 emulator died, 5 too short, 6 wrong build).
2. **DONE (70) — game-flow glue.** `GameFlow`/`GameState`/`LevelFlow` in `mdk_core/src/game/`; what is *not*
   there is deferred-with-cause in `docs/PROGRESS_LOG.md` "Iteration 98".
3. **DONE (71) — the harness's mount guard (review-4 F1+F5).** Mount question first, a vanished pid is not an
   unreadable live one, and a `pgrep -f` hit needs comm or an exe under `.mount_mandar*`
   (`PASS EmurunHarness (68 checks)`).
4. **DONE (72) — 3DS input.** `mdk_3ds/src/platform/input_3ds.{h,cpp}` maps HID to `mdk::Input`;
   `input_3ds_hid.cpp` is the one `hidScanInput` per frame. The C-Stick is deferred with a cause.
5a. **DONE (73-75) — wire the frontend.** `main_3ds.cpp` builds the level from `flow.state().level`, spawns
   `Player` at the DTI start `+ StartDrop`, steps `Player` then `FollowCamera` in fixed steps, respawns on
   `player.died()`, draws Kurt after the level, and refreshes the solid arena per step; the framing camera is
   gone and batch 75 fixed the clear colour. Narratives "Iteration 101"…"Iteration 104".
5b. **OPEN — the HUD half of item 5.** Blocked on a named cause, not on effort:
   `mdk_render_canvas_quad` is a no-op on this backend (`renderer_c3d.cpp:1854`), so a HUD call would draw
   nothing while reading as a ported HUD. The canvas seam has to land first.
6. **NEXT — ndsp audio backend.** The control plane in `mdk_core/src/audio/` is ported; the sample plane is not
   (`sound_device.h:13`). Adapt `../forsaken/sound_3ds.c` and `../t2k/t2k_3ds/src/audio/sfx_3ds.cpp`, then set
   the mixer on both frontends. R1 applies to the audio worker's entry; §10's small-stack rule applies to it.
7. **Citro3D alpha blend.** No `C3D_AlphaBlend`, so `Pass::Blended` glass is opaque. Blocked on a named
   cause: the palette expansion forces alpha 255 (`renderer_c3d.cpp` `expandIndices`, `pixel[3] = 255`) and the
   vertex shader writes `outclr = ones`.
8. **CLOSED (batch 75) — the red clear.** It was the clear colour's byte order, not the render path. See the
   header of this file.

## NEXT (batch 77)

**The room's triangles draw as a fan off one point, and that is the named blocker — a Citro3D vertex/projection
question, not a camera or gameplay one.** The Mandarine capture of this build (`DISPLAY=:99`, exit 0, frame 1740)
is the navy panel with magenta triangles converging below the screen's centre, while the camera's eye `(0,-4,4.5)`
is inside the room and Kurt stands on its floor at `z = 0.0002`, exactly where the oracle puts him. So the picture
is wrong about geometry that IS in front of the camera, not about where the camera is. Batch 76 proved the spawn,
the drop, the fall and the landing faithful with `golden/L7_spawn_fall.txt`, so **do not re-open them**. Two
measured facts to start from: the room around the spawn is walls at x = ±25.1, y = −16.74 and 36.89 with a floor at
z = 0, and a `+Z` ray from the spawn hits nothing within 4000, so do not blame the sky or the far plane first.
Dump what the backend is actually given for one DANT_1 triangle (world vertex, the matrix it was multiplied by,
the clip-space result) before changing a rule.

**Then item 6 — the ndsp audio backend.** `mdk_core/src/audio/sound_device.h:13` is the sample-plane gap:
nothing sets a `Player`'s mixer on either frontend. Adapt `../forsaken/sound_3ds.c` and
`../t2k/t2k_3ds/src/audio/sfx_3ds.cpp` (AGENTS §12: adapt, do not re-derive), then set the mixer on both
frontends.

**Review-2's queue, after this batch.** F2 is first and its precondition is met: the oracle's pin does NOT follow
Kurt down a fall column (`ScriptRuntime.cs:917-931` — `ArenaAt` only while `CurrentArena` is empty, then
`CrossedArena` or a teleport), so the frontend's geometric per-step refresh should stop re-pinning while he is
unsupported — with its own walk-out-of-`DANT_1` golden, because that is the case batch 74 exists for. F1's code
half (the runtime-less `mdk_linux` path pins once at the spawn) needs an owner line and is outside AGENTS §8; its
record half is fixed here. F4 needs an input hook in `emurun.sh`. F3 and F6 are owner actions, above; F5 is fixed.

**Three things the next batches must not lose:**

* **The collision world is per-step, not per-spawn — but the oracle's pin is not the geometric one.**
  `ArenaSpace::setSolid` is sticky and *replaces* the arenas around the feet, so anything that moves Kurt without
  refreshing the pin leaves him colliding with the wrong BSP. `main_3ds.cpp` refreshes it after the camera step; a
  new frontend or loop must too, and `main_3ds_flow_test.sh` is the gate that says so. What the oracle actually
  pins is the SCRIPTS' drawn arenas, which move only on a doorway connection or a teleport and therefore do NOT
  follow a fall out of the world (`ScriptRuntime.cs:917-931`, `CrossedArena` :791; measured, see Iteration 104).
  The geometric refresh is a stand-in for the unported connection crossing, not the rule.
* **The frontend does not call `reportLevel` yet.** It is optional (`play()` stands a fresh result in its
  place, and batch 74 made the level's entry health reach the live `Player` through it), but a frontend that
  skips it reports a level that fired nothing. It matters when the statistics screens exist, not before.
* **`FlowOptions` must grow three things `handle()` cannot carry yet** (review-5): `MenuPage Page`
  (`Game.cs:404`), `StatsScreen.Phase` (`Game.cs:408/412`) and the fall's `ViewerOptions(0, …)` vs
  `_options.Level` (`Game.cs:466`); `firstLevel()` covers the third. `FlowOptions::setCounts(values, count)`
  keeps its raw pointer + count (review-5 F2) and the CLI that first calls it owns the length.

One measured fact carries forward:

* **`emurun.sh --shot` works for this uid** (it writes the PNG; batch 76 got
  `/tmp/kilo-pi/b76-3ds.png` from it). If it ever produces nothing, the fallback
  is `DISPLAY=:99 ffmpeg -f x11grab -video_size 1280x1024 -i :99
  -frames:v 1 <file>` while a lane is live, and say which tool produced a 3DS
  picture. **Analyse the capture by pixel histogram, not by eye** — that is how
  the byte order was found, and `python3 -c` with PIL is available. The shot is
  the whole 1280x1024 display, so crop to the game panel before reading colours.

## Standing facts a batch must not re-derive

* **Level 7's start is inside DANT_1, on a floor at z = 0, and the oracle drops him there too.** The DTI start is
  `(0,4,9)` at angle 90, `+ StartDrop` lifts it to 12, and a BSP ray down hits a floor at `z = 0.01`; the room is
  walls at x = ±25.1, y = −16.74 and 36.89 with no ceiling, and the follow camera's eye is `(0,-4,4.5)`. An
  arena's first vertex says nothing about where its start is (DANT_1 spans
  `(-172,-17,-27)..(263,1188,279)`). `golden/L7_spawn_fall.txt` is the oracle's own 61-line run of this and
  `tick1 dump fall <level> <steps>` is the twin. Do not re-ask "why does Kurt stop at z=0".
* **The render seam's `clear_colour` is `0xAARRGGBB`; the PICA's clear word is `0xRRGGBBAA`, red in the TOP
  byte.** The Citro3D backend converts via `mdk::pica::clearColour`. Assert colour contracts as **bit fields,
  not memory bytes**: on a little-endian machine offset 0 is the low byte, which here is the alpha, so a
  byte-indexed test encodes the opposite of what the hardware sees (this cost two red drafts in batch 75 over a
  fix the emulator had already confirmed).
* **Exactly one `input3dsPoll()` per frame, and it is the only `hidScanInput()`.** libctru recomputes its edge
  bits on every scan, so a second scan in the same frame discards the first one's presses (the "Input
  Double-Swap" class, `../forsaken/3DS_PORT_NOTES.md`). The poll RETURNS the held mask, and does not drain the
  mouse — the game does. Scheme in `mdk_3ds/src/platform/input_3ds.h`; the C-Stick is absent by decision
  (`irrstCstickRead`, a second service, New 3DS only), not by oversight.
* **Never `git add -A`.** Owner-authored paths sit uncommitted in the worktree and at least one is PROTECTED
  (`ralph/builder.md`); staging a protected path halts the loop at exit 6. Stage your own paths by name, and
  read `git status --porcelain` rather than trusting a list of them.
* **Editing `ralph-pi.sh` does not change the running loop** (review-2 F1): bash reads a script incrementally,
  so a harness fix is only true once the process restarts — the loop enforces that (self-hash, exit 8 when the
  tree moved past it, unless the file is not executable, where stopping would be worse than a labelled stale
  run). Never claim a harness fix from a commit alone.
* **Review independence is the resolved upstream and family, never the alias** (SPEC-COMPLETION harness rule 4,
  owner-protected, pinned by `test_review_state.sh`). Current chain: builder `litellm/qwen` (fallback
  `litellm/luna`), reviewer `litellm/deepseek-v4.1-flash-th` (fallback `litellm/deepseek-v4.1-flash`) — both
  reviewer routes cross-family. A same-family reviewer is refused; if none resolves, BLOCKED and stop. An
  allowlist is an owner edit to a PROTECTED file, never a batch decision.
* **Emulator-green is not hardware-pass.** Mandarine masks FCRAM/VRAM exhaustion, `GPU_CLAMP_TO_EDGE`
  leniency and FCRAM smear across soft relaunches. Old-3DS timing stays **UNMEASURED** and the Citro3D
  picture stays **UNVERIFIED** against the oracle (AGENTS P6/P8).
* **A cross-uid co-tenant whose `/proc/<pid>/environ` answers EACCES is normal here, and those processes are
  not emulators** — `pgrep -f` matches command lines, and other agents' wrappers quote the AppImage path. Since
  batch 71 the rule is precise: **a failed environ read is the absence of proof, never proof of foreignness**;
  only a process BOTH unclaimable and unlocatable holds the mount. Do not "simplify" it back to a bare
  `readlink` (review-4 F1; vault
  `30-architecture/patterns/emulator-isolation-on-a-shared-multi-agent-machine.md`).
* **`test_review_state.sh` is not wired into `tools/check.sh`** (check.sh is owner-protected, so wiring it is
  an owner task). Run `bash tools/runner/test_review_state.sh` by hand with the gates.
* **`/tmp/kilo` is `museagent`-owned and not writable by this uid**; captures go to `/tmp/kilo-pi` (P7's rule
  is "capture to files, never to the operator's display", which this satisfies).
* **The C# oracle is runnable here, so measure instead of reasoning.** `/tmp/kilo/dotnet10/dotnet` works, the
  scratch tree is `/tmp/mdk-pi/obj3` (a `Probe` project referencing `Mdk.Game`), and
  `/tmp/mdk-pi/build_probe.sh` rebuilds it. A `flow` mode exists (`mdkprobe 3 1 1 flow`). `cmp` the scratch
  `src/Mdk.Game/**` against the repository's before believing a row.
* **The level data is readable with 10 lines of Python, and it settles parse arguments outright.** For a DTI,
  block *i* is at `4 + u32(20 + 4*i)`, and block 0 is `u32 startArena, vec3 startPosition, f32 startAngle`.
  Batch 75 ended a "the parser must be broken" hypothesis in one command this way.

## Verification commands

```
make -C tests/scripts                        # goldens ("All cases match the oracle")
bash tools/check.sh                          # all contract, parity, frame and integrity gates
make -C mdk_3ds                              # devkitARM target (needs DEVKITPRO/DEVKITARM set)
bash tools/runner/test_review_state.sh       # the review-record state machine
sh tests/scripts/c3d_frame_test.sh           # the Citro3D frame's source-shape invariants
bash tests/scripts/emurun_harness_test.sh    # the emulator harness's instrument rules
python3 tools/runner/model_identity.py selftest --live
DISPLAY=:99 mdk_3ds/tools/emurun.sh --secs 30 --shot /tmp/kilo-pi/3ds.png
tools/publish_frame.sh <label>               # the pinned 320x240 frame pair
```

`make -C mdk_3ds` and `bash tools/check.sh` need `DEVKITPRO=/opt/devkitpro
DEVKITARM=/opt/devkitpro/devkitARM`; the loop exports them, a bare shell does not.
