# Phase 2 continuation checkpoint

Risk: high — the 3DS frontend stops being a level viewer and becomes a game driver: it now owns a `Player`,
a `FollowCamera` and a `KurtSprite`, replaces its frame loop with the C#'s tick order (per-frame mouse drain,
a fixed 1/60 step with a ten-step pending clamp, per-step camera placement), deletes the framing camera, and
changes a `GameFlow` state transition (`play()` now seeds the fresh level result from the fall's carry and
consumes it — review-7 F1). Target-specific, state-transition-bearing, and the on-target picture is unchanged
for a reason this batch names rather than fixes. No gate, no golden and no PROTECTED path is touched; the one
core change is pinned by rows that were checked to fail without the fix.

Last updated: 2026-11-05 (**batch 73** — owner-directive item 5 is done: `mdk_3ds/src/main_3ds.cpp` drives
`GameFlow` → `Player` → the ported `FollowCamera` → Kurt's sprite, in the C#'s tick order, and the framing
camera is gone. Proved on-target by a fresh Mandarine run (exit 0, frame 3480) whose guest log reads
`flow: screen=4 level=7`, `spawn: feet=(0.0,4.0,12.0)`, `game: player=1 sprite=1` and
`input selftest: ... lookbits=41A00000`. review-7 F1 is fixed and pinned, F2 and F3 are fixed, F4 and F5 are
owner actions. **The on-target picture is still a red clear, recorded as the next blocker.** The record is
`docs/PROGRESS_LOG.md`, "Iteration 101"; earlier batches are indexed by number in that file.)

**Owner action still open (blocks the loop's own fixes taking effect):** the worktree `ralph-pi.sh` is mode
`664` while the index says `100755`, this uid cannot `chmod` it (owner task 18), and the systemd unit execs it
directly — so a restart today fails `203/EXEC` and the loop keeps running a pre-fix copy. Restore `0755` and
restart `ralph-pi-mdk.service`. **Until that restart, no review artifact carries any `harness:` line at all**
(review-3 F1, confirmed), so the absence of a `harness:` line means nothing and must not be read as "the
running loop is current". From the first restart onward the stamp is positive (`harness:  CURRENT sha=…`).

**Owner action still open (review-4 F3):** `ralph/builder.md` carries an uncommitted operator-authored edit
(a "Search scope and bounded commands" section, and the reviewer alias corrected to
`litellm/deepseek-v4.1-flash`) and that path is in `gates_batch_integrity.py`'s `PROTECTED` set. Commit it
with `MDK_OWNER_APPROVED=1` and a record entry, or revert it. A builder must not stage it (exit 6) and must
not set the approval marker itself — the marker records the owner's approval, so setting it from a builder
would forge the thing it attests. Until then the worktree is permanently dirty and every batch must stage by
name.

## PRIORITY ORDER (owner directive 2026-11-05, in force until withdrawn)

**Goal: a CIA that plays a level end-to-end, verified in Mandarine.** The 1-ULP fidelity work is not thrown
away but it is no longer first, and no new 1-ULP work opens unless a listed item needs it.

1. **DONE (batch 67) — the emulator harness.** `mdk_3ds/tools/emurun.sh` runs `mdk.3dsx` on `DISPLAY=:99`
   against pinned Mandarine r1.2.1 and prints a verdict. Exit 3 = stale log, 4 = emulator died, 5 = too
   short, 6 = wrong emulator build. Every 3DS batch carries its verdict from here on (AGENTS P8).
2. **DONE (batch 70) — game-flow glue.** `GameFlow`/`GameState`/`LevelFlow` in `mdk_core/src/game/`. What is
   *not* there is listed as deferred-with-cause in `docs/PROGRESS_LOG.md` "Iteration 98": the `SaveGame`
   methods (the save subsystem is cut), the screens' construction (`Menu/` is cut), and the stream's RNG seed.
3. **DONE (batch 71) — the emulator harness's mount guard (review-4 F1 + F5).** `emurun_mount_foreign` asked
   whether a process was *unreadable* before it asked whether that process executes out of the mount under
   test, so every lane leaked its own mount and printed a NOTE the guard never established; and
   `pgrep -f 'mandarine\.AppImage|…'` matches *command lines*, which on this box means other agents'
   `timeout`/`sudo` wrappers quoting the AppImage path. Both fixed: the mount question now comes first, a
   vanished pid is no longer mistaken for an unreadable live one, and a `pgrep -f` hit is a candidate that must
   be confirmed as an emulator by comm or by an exe under `.mount_mandar*`. Re-pinned with a real-EACCES
   fixture (`PASS EmurunHarness (68 checks)`), and the batch carries emulator evidence.
4. **DONE (batch 72) — 3DS input.** `mdk_3ds/src/platform/input_3ds.{h,cpp}` maps HID to `mdk::Input` and
   `input_3ds_hid.cpp` is the one `hidScanInput` per frame; the scheme and its causes are in the header, the
   pinned rows and their derivations are in `tests/scripts/input_3ds_tests.cpp`, and the narrative is
   "Iteration 100". The C-Stick is deferred with a cause (its read is `irrstCstickRead`, a second service,
   on a New 3DS-only path).
5. **DONE (batch 73) — wire the frontend.** `main_3ds.cpp` builds the level from `flow.state().level`, spawns
   `Player` at the DTI start `+ StartDrop`, drains the mouse per frame, steps `Player` then `FollowCamera` in
   fixed steps, respawns on `player.died()`, and draws Kurt after the level. The framing camera is deleted.
   The HUD is **not** drawn, with its cause: `mdk_render_canvas_quad` is a no-op on this backend
   (`renderer_c3d.cpp:1854`), so a HUD call would draw nothing while reading as a ported HUD. Narrative:
   "Iteration 101".
6. **NEXT — ndsp audio backend.** The control plane in `mdk_core/src/audio/` is ported; the sample plane is not
   (`sound_device.h:13`). Adapt `../forsaken/sound_3ds.c` and `../t2k/t2k_3ds/src/audio/sfx_3ds.cpp`, then
   set the mixer on both frontends.
7. **Citro3D alpha blend.** No `C3D_AlphaBlend`, so `Pass::Blended` glass is opaque. Blocked on a named
   cause: the palette expansion forces alpha 255 and the vertex shader writes `outclr = ones`.
8. **NEW, and it gates the directive's goal — the 3DS picture is still a red clear.** Batch 73 confirmed by
   screenshot that the panel still shows the red clear with a sliver of magenta geometry even though the
   simulation and the frame path both run (3 draws/frame, 60 fps, follow camera placed per step). So "a CIA
   that plays a level end-to-end" is blocked on the render path, not on the game glue any more. Named
   suspects: AGENTS P2's "all GPU state / all uniforms set every frame" list, and the depth map the guest log
   prints (`map z*1.0+1.0 near=0.0000 far=1.0000`).

**Cut from the sprint (deferred with cause):** menu/stats/save (`Menu/` ≈ 1,820 C# lines — boot straight
into a level), rides/snowboard/stream-fall view/sniper/cutscene camera, the 27 deferred opcodes, the
Old-3DS optimisation pass (take a first capture; do not optimise against a corpus that does not exist).

## NEXT (batch 74)

**Item 6 — the ndsp audio backend.** `mdk_core/src/audio/sound_device.h:13` is the sample-plane gap: the
control plane is ported and nothing sets a `Player`'s mixer on either frontend. Adapt `../forsaken/sound_3ds.c`
and `../t2k/t2k_3ds/src/audio/sfx_3ds.cpp` (AGENTS §12: adapt, do not re-derive), then set the mixer on both
frontends. R1 applies to the audio worker's entry (the FPSCR helper is already the shipped pattern) and §10's
small-stack rule applies to it.

**Two things the next batches must not lose:**

* **The frontend does not call `reportLevel` yet.** It is optional (`play()` stands a fresh result in its
  place, and batch 73 made that result honour the fall's carry), but a frontend that skips it reports a level
  that fired nothing. It matters when the statistics screens exist, not before.
* **`FlowOptions` must grow three things `handle()` cannot carry yet,** from review-5's open questions: `MenuPage
  Page` (`Game.cs:404`), `StatsScreen.Phase` (`Game.cs:408/412`) and the fall's `ViewerOptions(0, …)` vs
  `_options.Level` (`Game.cs:466`). `firstLevel()` already exists for the third. Also deferred from batch 72:
  `FlowOptions::setCounts(values, count)` keeps its raw pointer + count (review-5 F2) and the CLI that first
  calls it owns the length.

Two measured facts carry forward:

* **The 3DS picture is a red clear with a sliver of geometry — re-confirmed by screenshot in batch 73, with
  the game running.** The guest logs 3 draws/frame with real vertex data (`view: 135 batches, 2544 vertices,
  870 triangles`), the frame counter advances at 60 fps, and the follow camera now moves with Kurt, and the
  panel still does not show a level. Priority-order item 8. AGENTS P2's "all GPU state / all uniforms every
  frame" list is the first place to look.
* **`emurun.sh --shot` silently produces nothing on this box** (no ImageMagick `import` in the lane; it warns
  and continues). Grab the display with `DISPLAY=:99 ffmpeg -f x11grab -video_size 1280x1024 -i :99
  -frames:v 1 <file>` while a lane is live, and say which tool produced a 3DS picture.

## Standing facts a batch must not re-derive

* **Exactly one `input3dsPoll()` per frame, and it is the only `hidScanInput()`.** libctru recomputes its
  edge bits on every scan, so a second scan in the same frame discards the first one's presses (the same
  class as `../forsaken/3DS_PORT_NOTES.md` "Input Double-Swap"). The poll RETURNS the held mask so a
  frontend never needs its own scan, and it does not drain the mouse — the game does. The control scheme is
  documented in `mdk_3ds/src/platform/input_3ds.h`; the C-Stick is absent by decision (`irrstCstickRead`, a
  second service, New 3DS only), not by oversight.
* **Never `git add -A`.** Owner-authored paths sit uncommitted in the worktree and at least one is
  PROTECTED (`ralph/builder.md`); staging a protected path halts the loop at exit 6. Stage your own paths
  by name. Do not trust a list of them — read `git status --porcelain` in the batch that needs to know.
* **Editing `ralph-pi.sh` does not change the running loop** (review-2 F1): bash reads a script
  incrementally, so a harness fix is only true once the process restarts, and the loop enforces that (it
  hashes its own file, re-checks every iteration, exits 8 when the tree moved past it — unless the file is
  not executable, where stopping would be worse than a labelled stale run). Never claim a harness fix is in
  effect from a commit alone. Two review-3 refinements, also not in effect until the restart: the verdict is
  re-sampled before each review artifact (F3) and an unreadable self-hash is itself a verdict (F6).
* **`ralph-pi.sh` worktree mode is `664` and this uid cannot `chmod` it** (owner task 18; the file is
  `coleshores`-owned, index says `100755`). A batch that edits it must re-assert the bit with
  `git update-index --chmod=+x ralph-pi.sh` after staging, or the commit records `100644` and the systemd
  `ExecStart` fails `203/EXEC`.
* **Review independence is the resolved upstream and family, never the alias** (SPEC-COMPLETION harness
  rule 4, owner-protected). Current chain (the operator's uncommitted `ralph-pi.sh` edit, and what this run
  reports): builder `litellm/qwen` (family `qwen`) with `litellm/luna` fallback, reviewer
  `litellm/deepseek-v4.1-flash-th` (family `deepseek`) with `litellm/deepseek-v4.1-flash` fallback — both
  reviewer routes cross-family from both builder routes. A same-family reviewer is refused, and if no eligible
  reviewer resolves the loop records BLOCKED and stops. There is no allowlist and one must not be added:
  that is an owner edit to a PROTECTED file, not a batch decision (`test_review_state.sh` pins it).
* **Emulator-green is not hardware-pass.** Mandarine masks FCRAM/VRAM exhaustion, `GPU_CLAMP_TO_EDGE`
  leniency and FCRAM smear across soft relaunches. Old-3DS timing stays **UNMEASURED** and the Citro3D
  picture stays **UNVERIFIED** against the oracle (AGENTS P6/P8).
* **A cross-uid co-tenant whose `/proc/<pid>/environ` answers EACCES is normal on this box**, and the
  processes that trip it are *not* emulators: `pgrep -f 'mandarine\.AppImage|AppRun\.wrapped'` matches
  command lines, and other agents' wrappers quote that path (re-measured 2026-11-05: a `timeout` at uid 1000
  and a `sudo` at uid 0, both EACCES on environ AND on exe, neither an emulator). So an unreadable `/proc`
  entry is a condition an emulator lane must survive — but since batch 71 the rule is precise: **a failed
  environ read is the absence of proof, never proof of foreignness.** The guard asks whether the process
  executes out of the mount under test, and only a process that is BOTH unclaimable and unlocatable holds the
  mount. Do not "simplify" it back to a bare `readlink`, and do not answer "foreign" about a mount the process
  was never asked about (review-4 F1; vault
  `30-architecture/patterns/emulator-isolation-on-a-shared-multi-agent-machine.md`, "the co-tenant guard that
  LEAKS").
* **`test_review_state.sh` is not wired into `tools/check.sh`** (check.sh is owner-protected, so wiring it is
  an owner task). Run `bash tools/runner/test_review_state.sh` by hand with the gates.
* **`/tmp/kilo` is `museagent`-owned and not writable by this uid**; captures go to `/tmp/kilo-pi` (P7's rule
  is "capture to files, never to the operator's display", which this satisfies).
* **The C# oracle is runnable here, so measure instead of reasoning.** `/tmp/kilo/dotnet10/dotnet` works, the
  scratch tree is `/tmp/mdk-pi/obj3` (a `Probe` project referencing `Mdk.Game`), and
  `/tmp/mdk-pi/build_probe.sh` rebuilds it (`shaders ok`, `build ok`). `cmp` the scratch `src/Mdk.Game/**`
  against the repository's before believing a row. A `flow` mode now exists (`mdkprobe 3 1 1 flow`).

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
