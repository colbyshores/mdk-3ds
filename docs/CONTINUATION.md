# Phase 2 continuation checkpoint

Risk: medium — one behavioural line on the 3DS render path (the frame's clear colour), bounded and measured
twice on the pinned instrument, pinned by a host test and a mutation arm. Not high: the root cause is proven,
not suspected, and the blast radius is one colour value.
The fix is `mdk::pica::clearColour()` in `mdk_3ds/src/render/pica_matrix.{h,cpp}`: the seam carries `0xAARRGGBB`
and the PICA's clear word is `0xRRGGBBAA`, so the backend's pass-through painted every frame with the alpha byte
as red. Measured on Mandarine r1.2.1 / `:99`: the panel went from `#ff1020` to `#102030`, the value
`main_3ds.cpp:495` actually asked for. **Owner item 8 ("the red clear") is therefore closed, and it was never a
render-path failure** — neither of its two named suspects (AGENTS P2's GPU-state list, the depth map) is
implicated; the guest log still prints the same `map z*1.0+1.0 near=0.0000 far=1.0000 clear=0xffffff
test=LEQUAL` it printed before.

Last updated: 2026-11-05 (**batch 75 — the red clear was a byte order.** Record: `docs/PROGRESS_LOG.md`,
"Iteration 103"; earlier batches are indexed by number in that file.)

**Owner action still open (blocks the loop's own fixes taking effect):** the worktree `ralph-pi.sh` is mode
`664` while the index says `100755`, this uid cannot `chmod` it (owner task 18), and the systemd unit execs it
directly — so a restart today fails `203/EXEC` and the loop keeps running a pre-fix copy. Restore `0755` and
restart `ralph-pi-mdk.service`. **Until that restart no review artifact carries any `harness:` line at all**
(review-3 F1), so its absence is not evidence the running loop is current.
**Owner action still open (review-4 F3, review-2 F6):** `ralph/builder.md` carries an uncommitted
operator-authored edit and that path is in `gates_batch_integrity.py`'s `PROTECTED` set; the worktree's
`ralph-pi.sh` / `ralph/LOOP_CONTEXT.md` / `tools/runner/test_review_state.sh` routing now contradicts the
*committed* `OWNER_DIRECTIVE_2026-11-05.md` "Loop routing" block (review-2 F6). Commit them with
`MDK_OWNER_APPROVED=1` and a record entry, or revert them, and make the directive agree with the chain that is
actually running. A builder must not stage them (exit 6) and must not set the approval marker, so the worktree
stays permanently dirty and every batch stages by name. A batch that edits `ralph-pi.sh` must also re-assert the
bit with `git update-index --chmod=+x ralph-pi.sh` after staging, or the commit records `100644`.

**Owner action new (review-2 F3):** the batch-integrity rule "exactly one new frame PNG with sidecar" is
satisfiable by a byte-identical duplicate of an earlier capture, and **batch 75 is itself such a duplicate**
(`sha256 6646adf1…`, identical to four earlier pairs) because nothing in the host frame's path changed. The
reviewer's fix — reject a new frame whose sha256 equals an existing one, and require the `emurun.sh` verdict in
the sidecar — is an edit to `tools/runner/gates_batch_integrity.py`, which is gate tooling and owner-protected.
A builder may not touch a gate to clear a finding, so this is recorded instead of fixed.

## PRIORITY ORDER (owner directive 2026-11-05, in force until withdrawn)

**Goal: a CIA that plays a level end-to-end, verified in Mandarine.** The 1-ULP fidelity work is not thrown
away but it is no longer first, and no new 1-ULP work opens unless a listed item needs it.

1. **DONE (batch 67) — the emulator harness.** `mdk_3ds/tools/emurun.sh` runs `mdk.3dsx` on `DISPLAY=:99`
   against pinned Mandarine r1.2.1 and prints a verdict. Exit 3 = stale log, 4 = emulator died, 5 = too
   short, 6 = wrong emulator build. Every 3DS batch carries its verdict from here on (AGENTS P8).
2. **DONE (batch 70) — game-flow glue.** `GameFlow`/`GameState`/`LevelFlow` in `mdk_core/src/game/`. What is
   *not* there is listed as deferred-with-cause in `docs/PROGRESS_LOG.md` "Iteration 98".
3. **DONE (batch 71) — the emulator harness's mount guard (review-4 F1 + F5).** The mount question now comes
   before the unreadable-process question, a vanished pid is not an unreadable live one, and a `pgrep -f` hit
   must be confirmed by comm or by an exe under `.mount_mandar*`. Re-pinned with a real-EACCES fixture
   (`PASS EmurunHarness (68 checks)`).
4. **DONE (batch 72) — 3DS input.** `mdk_3ds/src/platform/input_3ds.{h,cpp}` maps HID to `mdk::Input` and
   `input_3ds_hid.cpp` is the one `hidScanInput` per frame; scheme in the header, pinned rows in
   `tests/scripts/input_3ds_tests.cpp`, narrative "Iteration 100". The C-Stick is deferred with a cause.
5a. **DONE (batches 73-75) — wire the frontend.** `main_3ds.cpp` builds the level from `flow.state().level`,
   spawns `Player` at the DTI start `+ StartDrop`, drains the mouse per frame, steps `Player` then
   `FollowCamera` in fixed steps, respawns on `player.died()`, and draws Kurt after the level. The framing
   camera is deleted. Batch 74 added the per-step solid-arena refresh (`Viewer.cs:533`) and the level's entry
   health; batch 75 fixed the clear colour it was asking for. Narrative: "Iteration 101"…"Iteration 103".
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

## NEXT (batch 76)

**The picture is now the follow camera filming the level from outside it, and that is the next named blocker.**
After batch 75 the panel is the intended navy with legible triangles, but they form a fan off one point: the
guest log ends with Kurt at `feet=(0.0,4.0,0.0)` while the level's first vertex is `(11.0,438.0,25.0)`, so the
camera is nowhere in the level. **Do not "fix" the spawn**: `spawn: feet=(0.0,4.0,12.0)` looks like an
uninitialised default but `MDK/TRAVERSE/LEVEL7/LEVEL7.DTI` block 0 really says `pos=(0,4,9) angle=90`, so
`Dti::parse` and `+ StartDrop` are faithful. The open question is why Kurt falls to z=0 and stops there instead
of landing on a floor — which is a `Player`/`ArenaSpace` question on level-7 geometry, and the host harness can
walk it without an emulator. Take the oracle's answer before changing a rule.

**Then item 6 — the ndsp audio backend.** `mdk_core/src/audio/sound_device.h:13` is the sample-plane gap:
nothing sets a `Player`'s mixer on either frontend. Adapt `../forsaken/sound_3ds.c` and
`../t2k/t2k_3ds/src/audio/sfx_3ds.cpp` (AGENTS §12: adapt, do not re-derive), then set the mixer on both
frontends.

**Review-2's queue, still open after this batch** (all Medium; the verdict was COMPLETE): F1 the runtime-less
`mdk_linux` path pins the solid arena once at the spawn and batch 74's record claimed otherwise — needs its own
batch, and `mdk_linux/**` is outside AGENTS §8, so it also needs an owner line on whether the degraded install is
supported; F2 the per-step pin can change mid-fall on 4 of 1681 probed columns — needs the oracle's
`_drawnArenas` behaviour during a fall-out before the rule moves; F4 no on-target run crosses an arena boundary,
because `emurun.sh` has no input injection; F5 `main_3ds_flow_test.sh` check 1 matches loop text including
comments. F3 and F6 are owner actions, listed above.

**Three things the next batches must not lose:**

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
  -frames:v 1 <file>` while a lane is live, and say which tool produced a 3DS picture. **Analyse the capture by
  pixel histogram, not by eye** — that is how the byte order was found, and `python3 -c` with PIL is available.

## Standing facts a batch must not re-derive

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
