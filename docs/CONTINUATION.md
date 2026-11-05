# Phase 2 continuation checkpoint

Risk: medium — one-shot census instrumentation plus two real fixes (per-draw log vertex, diagnostic vertices moved into linear memory) in the target-specific Citro3D renderer; no shipped draw behaviour changed, but the diff sits on the render path under investigation.

**Level-7 start is faithful:** Kurt lands on a floor at z=0.01; oracle and C++ agree at z=0.0002 with the same states. `golden/L7_spawn_fall.txt` pins it; camera eye `(0,-4,4.5)` is inside DANT_1. Details: `docs/PROGRESS_LOG.md`, Iteration 104.

Last updated: 2026-11-05 (**batch 78 — the level-7 fan is not the cull, not the depth, not the queue**).

**Owner action:** `ralph-pi.sh` worktree mode is 664 (index 100755); this uid cannot chmod it, so systemd restart fails `203/EXEC`. Owner task 18: restore 0755 and restart `ralph-pi-mdk.service`.
**Owner action (review-4 F3 / review-2 F6):** uncommitted operator edits in `ralph/builder.md` (PROTECTED) plus routing changes in `ralph-pi.sh`, `ralph/LOOP_CONTEXT.md`, and `tools/runner/test_review_state.sh` conflict with the committed owner directive. Owner must approve/commit or revert; builder must not stage these or set `MDK_OWNER_APPROVED=1`. Stage batch paths by name only.
**Owner action (review-2 F3):** batches 75/76 frame PNGs duplicate an older capture (`sha256 6646adf1…`). Rejecting duplicates and requiring emulator verdicts needs owner-protected gate tooling; recorded, not changed.

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

## NEXT (batch 79)

**The fan is not the cull, not the depth test, not the queue, and not a sheared transfer — all four were
measured and retired (Iteration 106).** The census of a real Mandarine frame: 136 draws, 852 triangles,
**825 inside the clip square**, `areaset=33.28` (eight screens of triangle area), yet the panel is 67.7%
clear colour. Forcing `GPU_CULL_NONE` moves the panel by 13 px of 96000; forcing the Solid depth test off
moves it by 58 px. Neither is hiding the room.

**The narrowed question.** Draw the scene with `GPU_PRIMARY_COLOR` instead of the texture and the panel is
**exactly half white** (47845 px), split by a straight vertical edge through the panel's centre. So the
geometry that reaches the rasterizer covers half the view, the split is a straight line and not a
silhouette, and the texture path then discards a further ~18%. Two separate questions, both smaller than
"why a fan":

1. **Why does the geometry stop at a straight line through the view's centre?** In the rotated portrait
   target that line is one NDC axis at 0. Diff the C++ frame against the **oracle's own landed frame**:
   `mdk_sdl --level=7 --frames=<dir>,30,6` (driver at `/tmp/kilo-pi/orc_frames.sh`, reference frame
   `frame029.bmp`, step 174) shows a symmetric corridor — bright floor across the bottom third, walls
   converging to a centre vanishing point, a dark ceiling band. The oracle's view is NOT one-sided, so
   suspect the rotated axes or the aspect: the projection's quarter turn and the 240x400 target are a
   matched pair, and `kScreenWidth`/`kScreenHeight` also feed `mdk_render_canvas_width`.
   **A landscape 400x240 target with the rotation deleted was tried in batch 78 and is worse** — the panel
   goes black except a 407x9 strip of clear colour — so do not re-try it as a one-line swap.
2. **Why does the texture path discard ~18% of what is painted?** That is the index-0-transparent palette
   expansion meeting a PICA path that never calls `C3D_AlphaTest`. It is the same alpha source owner item
   7 names as blocked; the two are now known to be the same defect, not two.

**The diagnostic triangle still does not appear**, with its vertices in linear memory, both windings,
`GPU_CULL_NONE`, the depth test off and vertices covering the whole clip square — not one pixel moves, while
the scene paints half the panel with the same program and viewport. The linear-memory fix is kept because a
`.bss` pointer is not a GPU address, not because it worked. Open, and it is a diagnostic, not the game.

**Do not re-derive these** (all measured off the pinned r1.2.1 on `:99`): the frame queues everything the
view built; the MVP is consistent with the logged eye (batch 77); `GPU_CULL_BACK_CCW` discards the faces
that look counter-clockwise on the panel; the depth map/test/clear triple is not rejecting near geometry;
the panel is not a raw linear copy of the portrait target (un-shearing produces a comb, not a corridor).

Batch 76 proved the level-7 spawn, fall and landing faithful with `golden/L7_spawn_fall.txt`; **do not re-open them**. Standing geometry around the spawn: walls at x = ±25.1, y = −16.74 and 36.89, a floor at z = 0, and a `+Z` ray from spawn hits nothing within 4000.

**Then item 6 — the ndsp audio backend.** `mdk_core/src/audio/sound_device.h:13` is the sample-plane gap:
nothing sets a `Player`'s mixer on either frontend. Adapt `../forsaken/sound_3ds.c` and
`../t2k/t2k_3ds/src/audio/sfx_3ds.cpp` (AGENTS §12: adapt, do not re-derive), then set the mixer on both
frontends.

**Review-4 medium findings:** F1/F2 need a placed-fall C# oracle/C++ twin in full order and a walk-out golden before changing collision pin behavior; deferred to a collision batch. F3/F4 remain owner actions; no protected paths staged. Original-3DS behavior remains unmeasured.

**Three things the next batches must not lose:**

* **The collision world is per-step, not per-spawn — but the oracle's pin is not the geometric one.**
  `ArenaSpace::setSolid` is sticky and replaces the arenas around the feet, so anything that moves Kurt without
  refreshing the pin collides with the wrong BSP; `main_3ds.cpp` refreshes it after the camera step and
  `main_3ds_flow_test.sh` is the gate. What the oracle pins is the SCRIPTS' drawn arenas, which move only on a
  doorway connection or teleport (`ScriptRuntime.cs:917-931`, `CrossedArena` :791) — the geometric refresh is a
  stand-in for the unported connection crossing, not the rule.
* **The frontend does not call `reportLevel` yet** — optional (`play()` stands a fresh result in its place, and
  batch 74 made the level's entry health reach the live `Player`), but a frontend that skips it reports a level
  that fired nothing. It matters when the statistics screens exist, not before.
* **`FlowOptions` must grow three things `handle()` cannot carry** (review-5): `MenuPage Page`,
  `StatsScreen.Phase`, and the fall's `ViewerOptions(0, …)` vs `_options.Level`; `firstLevel()` covers the
  third, and `setCounts(values, count)` keeps its raw pointer + count (review-5 F2).

**Reading a capture.** Analyse it by **pixel histogram, not by eye** — that is how the byte order was found —
and `emurun.sh --shot` writes the whole 1280x1024 display, so crop to the game panel first. Captures go to
`/tmp/kilo-pi` (`/tmp/kilo` is another uid's and is not writable here).

## Standing facts a batch must not re-derive

* **Level 7's start is inside DANT_1 on a floor at z = 0 and the oracle drops him there too** — DTI start
  `(0,4,9)` at angle 90, `+ StartDrop` lifts it to 12, a BSP ray down hits `z = 0.01`, follow-camera eye
  `(0,-4,4.5)`. An arena's first vertex says nothing about where its start is (DANT_1 spans
  `(-172,-17,-27)..(263,1188,279)`). `golden/L7_spawn_fall.txt` is the oracle's own run of this and
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
* **Review independence is the resolved upstream and family, never the alias** (owner-protected, pinned by
  `test_review_state.sh`, which is NOT wired into `tools/check.sh` and must be run by hand). Builder
  `litellm/qwen`, reviewer `litellm/deepseek-v4.1-flash-th` — cross-family; a same-family reviewer is refused.
  Editing `ralph-pi.sh` does not change the running loop (review-2 F1). A cross-uid co-tenant whose
  `/proc/<pid>/environ` answers EACCES is normal here and is not an emulator: a failed environ read is the
  absence of proof, never proof of foreignness (review-4 F1).
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
