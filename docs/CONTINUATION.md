# Phase 2 continuation checkpoint

Risk: medium — one-shot diagnostic logging in the 3DS renderer; no draw behavior or public interface changed, but the target-specific trace measures clip-space geometry on the path under investigation.

**Level-7 start is faithful:** Kurt lands on a floor at z=0.01; oracle and C++ agree at z=0.0002 with the same states. `golden/L7_spawn_fall.txt` pins it; camera eye `(0,-4,4.5)` is inside DANT_1. Details: `docs/PROGRESS_LOG.md`, Iteration 104.

Last updated: 2026-11-05 (**batch 77 — first DANT_1 triangle traced through Citro3D**).

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

## NEXT (batch 78)

**The first-triangle clip trace does not support a broken matrix or a camera outside the room, but it does not yet explain the fan.** From the actual DANT_1 draw on Mandarine (`DISPLAY=:99`, pinned r1.2.1): first triangle vertices `(11,438,25)`, `(0,438,36)`, `(14,438,32)` produce clip W ≈ 440 and NDC around `(0.12,-0.02,-0.001)`, `(0.16,0,-0.001)`, `(0.14,-0.027,-0.001)` under the logged MVP; projected signed area is `-0.00066111`. This is a far-distance triangle, and the signed winding should be checked against actual PICA cull semantics before changing the cull enum. The trace computes the actual per-draw world*MVP (not just the frame matrix) and logs once per process. Next sample triangles near the spawn and compare the vertices/MVP/clip values with host-derived geometry; inspect the PICA winding and depth/cull behavior before modifying renderer state. Do not declare the room's fan fixed from one triangle.

Batch 76 proved the level-7 spawn, fall and landing faithful with `golden/L7_spawn_fall.txt`; **do not re-open them**. Standing geometry around the spawn is walls at x = ±25.1, y = −16.74 and 36.89 with a floor at z = 0, and a `+Z` ray from spawn hits nothing within 4000.

**Then item 6 — the ndsp audio backend.** `mdk_core/src/audio/sound_device.h:13` is the sample-plane gap:
nothing sets a `Player`'s mixer on either frontend. Adapt `../forsaken/sound_3ds.c` and
`../t2k/t2k_3ds/src/audio/sfx_3ds.cpp` (AGENTS §12: adapt, do not re-derive), then set the mixer on both
frontends.

**Review-4 medium findings:** F1/F2 need a placed-fall C# oracle/C++ twin in full order and a walk-out golden before changing collision pin behavior; deferred to a collision batch. F3/F4 remain owner actions; no protected paths staged. Original-3DS behavior remains unmeasured.

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
