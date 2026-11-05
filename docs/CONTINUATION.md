# Phase 2 continuation checkpoint

Risk: high — remediation of review-8, and the fix changes which BSP Kurt collides with on the 3DS target.
`mdk_3ds/src/main_3ds.cpp` now refreshes Kurt's solid arena every simulation step instead of pinning it once
at the spawn (F1, High): while the pin is set `move()`, `floorCrossing()`, `crosses()` and `sight()` use it and
only it, so a stale one left Kurt colliding with the arena he was born in after he walked into its neighbour.
Adds a read-only `GameFlow::levelHealth()` so the level's entry health reaches the live `Player` (F3), corrects
two comments that contradicted the code (F4, F5), and adds a gate file. No PROTECTED path, golden or gate
threshold is touched; the emulator run is unchanged because it never exercises the fixed path, which is stated
rather than hidden.

Last updated: 2026-11-05 (**batch 74 — review-8 remediation.** F1 fixed: the step loop refreshes the solid
arena after `follow.update`, as `Viewer.cs:533` does, pinned twice — a new `main_3ds_flow_test.sh` (5 checks, 5
mutation arms red) for the placement, and a `vm_tests` case on real level-7 geometry for the sticky-pin
semantics. F3 fixed but NOT as prescribed: the reviewer's `state().hasCarry` read is dead code because `play()`
consumes the carry first, so the value comes from `GameFlow::levelHealth()`. F1's claim that the spawn pin is
inert is **rebutted** — measured, an unpinned `bottom()` below the world answers false and the fall-out death
never fires. F2/F4/F5 fixed. All gates pass; Mandarine exit 0 at frame 2580 on `:99`. Record:
`docs/PROGRESS_LOG.md`, "Iteration 102"; earlier batches are indexed by number in that file.)

**Owner action still open (blocks the loop's own fixes taking effect):** the worktree `ralph-pi.sh` is mode
`664` while the index says `100755`, this uid cannot `chmod` it (owner task 18), and the systemd unit execs it
directly — so a restart today fails `203/EXEC` and the loop keeps running a pre-fix copy. Restore `0755` and
restart `ralph-pi-mdk.service`. **Until that restart no review artifact carries any `harness:` line at all**
(review-3 F1), so its absence is not evidence the running loop is current.

**Owner action still open (review-4 F3):** `ralph/builder.md` carries an uncommitted operator-authored edit (a
"Search scope and bounded commands" section, and the reviewer alias corrected to
`litellm/deepseek-v4.1-flash`) and that path is in `gates_batch_integrity.py`'s `PROTECTED` set. Commit it with
`MDK_OWNER_APPROVED=1` and a record entry, or revert it. A builder must not stage it (exit 6) and must not set
the approval marker, which records the owner's approval. Until then the worktree is permanently dirty and every
batch stages by name. A batch that edits `ralph-pi.sh` must also re-assert the bit with
`git update-index --chmod=+x ralph-pi.sh` after staging, or the commit records `100644`.

## PRIORITY ORDER (owner directive 2026-11-05, in force until withdrawn)

**Goal: a CIA that plays a level end-to-end, verified in Mandarine.** The 1-ULP fidelity work is not thrown
away but it is no longer first, and no new 1-ULP work opens unless a listed item needs it.

1. **DONE (batch 67) — the emulator harness.** `mdk_3ds/tools/emurun.sh` runs `mdk.3dsx` on `DISPLAY=:99`
   against pinned Mandarine r1.2.1 and prints a verdict. Exit 3 = stale log, 4 = emulator died, 5 = too
   short, 6 = wrong emulator build. Every 3DS batch carries its verdict from here on (AGENTS P8).
2. **DONE (batch 70) — game-flow glue.** `GameFlow`/`GameState`/`LevelFlow` in `mdk_core/src/game/`. What is
   *not* there is listed as deferred-with-cause in `docs/PROGRESS_LOG.md` "Iteration 98": the `SaveGame`
   methods (save is cut), the screens' construction (`Menu/` is cut), and the stream's RNG seed.
3. **DONE (batch 71) — the emulator harness's mount guard (review-4 F1 + F5).** `emurun_mount_foreign` asked
   whether a process was *unreadable* before asking whether it executes out of the mount under test, so every
   lane leaked its own mount; and `pgrep -f` matches *command lines*, which here means other agents' wrappers
   quoting the AppImage path. Both fixed: the mount question comes first, a vanished pid is not an unreadable
   live one, and a `pgrep -f` hit must be confirmed by comm or by an exe under `.mount_mandar*`. Re-pinned with
   a real-EACCES fixture (`PASS EmurunHarness (68 checks)`).
4. **DONE (batch 72) — 3DS input.** `mdk_3ds/src/platform/input_3ds.{h,cpp}` maps HID to `mdk::Input` and
   `input_3ds_hid.cpp` is the one `hidScanInput` per frame; scheme in the header, pinned rows and derivations in
   `tests/scripts/input_3ds_tests.cpp`, narrative "Iteration 100". The C-Stick is deferred with a cause.
5a. **DONE (batch 73) — wire the frontend.** `main_3ds.cpp` builds the level from `flow.state().level`, spawns
   `Player` at the DTI start `+ StartDrop`, drains the mouse per frame, steps `Player` then `FollowCamera` in
   fixed steps, respawns on `player.died()`, and draws Kurt after the level. The framing camera is deleted.
   Batch 74 added the per-step solid-arena refresh it was missing (`Viewer.cs:533`) and the level's entry
   health. Narrative: "Iteration 101", then "Iteration 102".
5b. **OPEN — the HUD half of item 5.** Blocked on a named cause, not on effort:
   `mdk_render_canvas_quad` is a no-op on this backend (`renderer_c3d.cpp:1854`), so a HUD call would draw
   nothing while reading as a ported HUD. The canvas seam has to land first.
6. **NEXT — ndsp audio backend.** The control plane in `mdk_core/src/audio/` is ported; the sample plane is not
   (`sound_device.h:13`). Adapt `../forsaken/sound_3ds.c` and `../t2k/t2k_3ds/src/audio/sfx_3ds.cpp`, then
   set the mixer on both frontends.
7. **Citro3D alpha blend.** No `C3D_AlphaBlend`, so `Pass::Blended` glass is opaque. Blocked on a named
   cause: the palette expansion forces alpha 255 and the vertex shader writes `outclr = ones`.
8. **The 3DS picture is still a red clear, and it gates the directive's goal.** Builder-added to the owner's
   order in batch 73 and still awaiting an owner line, so it is the builder's reading of the goal, not owner text.
   Batch 73 confirmed by screenshot that the panel shows the red clear with a sliver of magenta geometry while
   the simulation and the frame path both run — the guest logs 3 draws/frame with real vertex data (`view: 135
   batches, 2544 vertices, 870 triangles`) at 60 fps with the follow camera on Kurt. So it is blocked on the
   render path, not on the game glue, and batch 74 touched no render path. Named suspects: AGENTS P2's "all GPU
   state / all uniforms set every frame" list, and the depth map the guest log prints (`map z*1.0+1.0
   near=0.0000 far=1.0000`).

**Cut from the sprint (deferred with cause):** menu/stats/save (`Menu/` ≈ 1,820 C# lines — boot straight into a
level), rides/snowboard/stream-fall view/sniper/cutscene camera, the 27 deferred opcodes, and the Old-3DS
optimisation pass (take a first capture; do not optimise against a corpus that does not exist).

## NEXT (batch 75)

**Item 8 — the red clear.** It is the only thing between this loop and the directive's goal, and the first
unfinished item not blocked on a cut subsystem: 5b waits on the canvas seam, and audio cannot make a picture
appear. Start with AGENTS P2's list — every GPU state and every uniform written every frame — and the depth map
the guest log prints, the value a D3D-derived port is most likely to have carried over wrong. Evidence here is a
screenshot off `:99` that shows a level, not a frame counter.

**Then item 6 — the ndsp audio backend.** `mdk_core/src/audio/sound_device.h:13` is the sample-plane gap:
nothing sets a `Player`'s mixer on either frontend. Adapt `../forsaken/sound_3ds.c` and
`../t2k/t2k_3ds/src/audio/sfx_3ds.cpp` (AGENTS §12: adapt, do not re-derive), then set the mixer on both
frontends. R1 applies to the audio worker's entry (the FPSCR helper is the shipped pattern) and §10's
small-stack rule applies to it.

**Two things the next batches must not lose:**

* **The collision world is per-step, not per-spawn.** `ArenaSpace::setSolid` is sticky and *replaces* the
  arenas around the feet, so anything that moves Kurt without refreshing the pin leaves him colliding with the
  wrong BSP. `main_3ds.cpp` refreshes it after the camera step; a new frontend or loop must too, and
  `main_3ds_flow_test.sh` is the gate that says so.
* **The frontend does not call `reportLevel` yet.** It is optional (`play()` stands a fresh result in its
  place, and batch 74 made the level's entry health reach the live `Player` through it), but a frontend that
  skips it reports a level that fired nothing. It matters when the statistics screens exist, not before.
* **`FlowOptions` must grow three things `handle()` cannot carry yet,** from review-5's open questions: `MenuPage
  Page` (`Game.cs:404`), `StatsScreen.Phase` (`Game.cs:408/412`) and the fall's `ViewerOptions(0, …)` vs
  `_options.Level` (`Game.cs:466`); `firstLevel()` already exists for the third. Also from batch 72:
  `FlowOptions::setCounts(values, count)` keeps its raw pointer + count (review-5 F2) and the CLI that first
  calls it owns the length.

One measured fact carries forward:

* **`emurun.sh --shot` silently produces nothing on this box** (no ImageMagick `import` in the lane; it warns
  and continues). Grab the display with `DISPLAY=:99 ffmpeg -f x11grab -video_size 1280x1024 -i :99
  -frames:v 1 <file>` while a lane is live, and say which tool produced a 3DS picture.

## Standing facts a batch must not re-derive

* **Exactly one `input3dsPoll()` per frame, and it is the only `hidScanInput()`.** libctru recomputes its edge
  bits on every scan, so a second scan in the same frame discards the first one's presses (the "Input
  Double-Swap" class, `../forsaken/3DS_PORT_NOTES.md`). The poll RETURNS the held mask, and does not drain the
  mouse — the game does. Scheme in `mdk_3ds/src/platform/input_3ds.h`; the C-Stick is absent by decision
  (`irrstCstickRead`, a second service, New 3DS only), not by oversight.
* **Never `git add -A`.** Owner-authored paths sit uncommitted in the worktree and at least one is PROTECTED
  (`ralph/builder.md`); staging a protected path halts the loop at exit 6. Stage your own paths by name, and
  read `git status --porcelain` rather than trusting a list of them.
* **Editing `ralph-pi.sh` does not change the running loop** (review-2 F1): bash reads a script incrementally,
  so a harness fix is only true once the process restarts — the loop enforces that (self-hash, re-checked every
  iteration, exit 8 when the tree moved past it, unless the file is not executable, where stopping would be
  worse than a labelled stale run). Never claim a harness fix from a commit alone.
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
  `/tmp/mdk-pi/build_probe.sh` rebuilds it. `cmp` the scratch `src/Mdk.Game/**` against the repository's before
  believing a row. A `flow` mode exists (`mdkprobe 3 1 1 flow`).

## Verification commands

```
make -C tests/scripts                        # goldens ("All cases match the oracle")
bash tools/check.sh                          # all contract, parity, frame and integrity gates
make -C mdk_3ds                              # devkitARM target (needs DEVKITPRO/DEVKITARM set)
bash tools/runner/test_review_state.sh       # the review-record state machine
bash tests/scripts/emurun_harness_test.sh    # the emulator harness's instrument rules
python3 tools/runner/model_identity.py selftest --live
DISPLAY=:99 mdk_3ds/tools/emurun.sh --secs 30 --shot /tmp/kilo-pi/3ds.png
tools/publish_frame.sh <label>               # the pinned 320x240 frame pair
```

`make -C mdk_3ds` and `bash tools/check.sh` need `DEVKITPRO=/opt/devkitpro
DEVKITARM=/opt/devkitpro/devkitARM`; the loop exports them, a bare shell does not.
