# Phase 2 continuation checkpoint

Risk: high — the batch adds a new 3DS platform subsystem (the HID -> `Input` mapping, a new interface and
a new state owner for the accumulated mouse), changes the 3DS frontend's frame loop so the poll is the only
`hidScanInput`, and changes a `GameFlow` state transition (`play()` now installs a fresh level result, which
is review-5 F1). No golden, no gate, no PROTECTED path and no `mdk_core` numeric path is touched, and every
new number is hand-derived and re-derived twice; the two parts worth a reviewer's eye are the control scheme
(it is this target's, not the C#'s) and the `play()` reset.

Last updated: 2026-11-05 (**batch 72** — owner-directive item 4 is done: libctru HID fills `mdk::Input`
(`mdk_3ds/src/platform/input_3ds.{h,cpp}` + `input_3ds_hid.cpp`), pinned by `PASS Input3dsMap` and proved
on-target by the guest log's `input selftest: fwd=1 fire=1 look=20.0000`, which is the host golden's
0x41A00000. review-5 F1 is fixed and pinned (the `play()` reset), F3 is answered in this batch's frame
sidecar, F4 stays closed by batch 71, and F2 is rebutted in part and deferred in part. The record is
`docs/PROGRESS_LOG.md`, "Iteration 100"; earlier batches are indexed by number in that file.).

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
5. **NEXT — wire the frontend.** `main_3ds.cpp` drives the real game: input → player → the ported `FollowCamera`
   (not the framing camera) → Kurt's sprite → the HUD, through `GameFlow` — whose `handle(Event)` returns the
   `Screen` to build and whose header states the tick order. It now *polls* the controls but still reads only
   START/SELECT as a quit chord and drives no game object.
6. **ndsp audio backend.** The control plane in `mdk_core/src/audio/` is ported; the sample plane is not
   (`sound_device.h:13`). Adapt `../forsaken/sound_3ds.c` and `../t2k/t2k_3ds/src/audio/sfx_3ds.cpp`, then
   set the mixer on both frontends.
7. **Citro3D alpha blend.** No `C3D_AlphaBlend`, so `Pass::Blended` glass is opaque. Blocked on a named
   cause: the palette expansion forces alpha 255 and the vertex shader writes `outclr = ones`.

**Cut from the sprint (deferred with cause):** menu/stats/save (`Menu/` ≈ 1,820 C# lines — boot straight
into a level), rides/snowboard/stream-fall view/sniper/cutscene camera, the 27 deferred opcodes, the
Old-3DS optimisation pass (take a first capture; do not optimise against a corpus that does not exist).

## NEXT (batch 73)

**Item 5 — wire the frontend.** `main_3ds.cpp` drives the real game: `mdk::Input` from `input3dsPoll` →
`Player` → the ported `FollowCamera` (not the framing camera at `main_3ds.cpp`'s `view.bounds()` block) →
Kurt's sprite → the HUD, through `GameFlow`. What the frontend must get right and does not do today: call
`handle(Event)` and show the `Screen` it returns, drain the mouse once per simulation step (the game takes
it — `Player::mouseLook`, `FollowCamera::mouseLook` — so the frontend must not), and use
`GameFlow::stepFor`/`elapsedFor` instead of its own clamped wall delta. `reportLevel` is now optional (batch
72 made `play()` stand a fresh result in its place), but a frontend that skips it reports a level that fired
nothing, so call it.

**From review-5's open questions, for when the screens land:** `FlowOptions` must grow three things
`handle()` cannot carry yet — `MenuPage Page` (`Game.cs:404`), `StatsScreen.Phase` (`Game.cs:408/412`) and
the fall's `ViewerOptions(0, …)` vs `_options.Level` (`Game.cs:466`). `firstLevel()` already exists for the
third; the header does not record that the first two will be needed. Also deferred from batch 72:
`FlowOptions::setCounts(values, count)` keeps its raw pointer + count (review-5 F2) and the CLI that first
calls it owns the length.

Two measured facts carry forward, both unchanged by batch 72:

* **The 3DS picture is a red clear with a sliver of geometry.** The guest logs 3 draws/frame with real
  vertex data (`view: 154 batches, 1188 vertices, 399 triangles`) and the frame counter advances at 60 fps,
  so the frame path runs; what reaches the screen is not a level. Item 7's neighbourhood, and AGENTS P2's
  "all GPU state every frame" list.
* **`draws issued: 0` is printed while draws happen.** `main_3ds.cpp:277-281` reads
  `mdk_render_stats(renderer, &draws, nullptr)` at `frame == 3` and gets 0 while the same frame logs three
  `draw N:` lines. The instrument, not the game: name the cause before quoting that number anywhere.

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
  rule 4, owner-protected). Builder `litellm/qwen`, reviewer `litellm/deepseek-v4.1-flash`, fallbacks
  `litellm/minimax-m3.1-flash-nvidia litellm/luna`. A same-family reviewer is refused, and if no eligible
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
