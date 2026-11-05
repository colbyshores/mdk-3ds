# Phase 2 continuation checkpoint

Risk: medium — the batch changes the emulator harness's mount-ownership decision (a lane now releases its own
mount where it used to leak it) and re-pins two rows of `emurun_harness_test.sh` that had been certifying the
defect; no game code, no core source, no golden, no gate and no PROTECTED path is touched. The re-pin is the
part worth a reviewer's eye, and the new rows are proved non-vacuous against the pre-fix harness.

Last updated: 2026-11-05 (**batch 71** — owner-directive item 3 is done: `mdk_3ds/tools/emurun.sh` no longer
answers "foreign" about a mount it never asked the process about, a vanished pid is not an unreadable process,
and a `pgrep -f` hit is a candidate rather than a verdict. review-4 F1 and F5 are fixed and pinned, review-5
F4 is fixed, review-5 F3 is answered in the frame sidecar, and review-5 F1 and F2 are confirmed and deferred
with a cause. The record is `docs/PROGRESS_LOG.md`, "Iteration 99"; batch 70's is "Iteration 98", batch 69's
"Iteration 97", batch 68's "Iteration 96", batch 67's "Iteration 95", batch 66's "Iteration 94", batch 65's
"Iteration 93", batch 64's "Iteration 92", batch 63's "Iteration 91"; earlier batches are indexed by number
in that file.).

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
4. **NEXT — 3DS input.** Fill `mdk_core/src/game/input.h` from libctru HID. Adapt `../forsaken/input_3ds.c`; heed
   the double-swap and axis-exists traps in `../forsaken/3DS_PORT_NOTES.md`.
5. **Wire the frontend.** `main_3ds.cpp` drives the real game: input → player → the ported `FollowCamera`
   (not the framing camera) → Kurt's sprite → the HUD, through `GameFlow` — whose `handle(Event)` returns the
   `Screen` to build and whose header states the tick order. It reads only START/SELECT today.
6. **ndsp audio backend.** The control plane in `mdk_core/src/audio/` is ported; the sample plane is not
   (`sound_device.h:13`). Adapt `../forsaken/sound_3ds.c` and `../t2k/t2k_3ds/src/audio/sfx_3ds.cpp`, then
   set the mixer on both frontends.
7. **Citro3D alpha blend.** No `C3D_AlphaBlend`, so `Pass::Blended` glass is opaque. Blocked on a named
   cause: the palette expansion forces alpha 255 and the vertex shader writes `outclr = ones`.

**Cut from the sprint (deferred with cause):** menu/stats/save (`Menu/` ≈ 1,820 C# lines — boot straight
into a level), rides/snowboard/stream-fall view/sniper/cutscene camera, the 27 deferred opcodes, the
Old-3DS optimisation pass (take a first capture; do not optimise against a corpus that does not exist).

## NEXT (batch 72)

**Take review-5 F1 first — it is a confirmed defect, three lines and a test row, and item 5's frontend will be
written against the code it is wrong about.** `GameFlow::play()` never resets `level_`/`hasLevel_`, so a level
that ends without calling `reportLevel` streams the *previous* level's stats and health; the C# replaces
`_viewer` with a fresh `Viewer` inside `Play()` (`Game.cs:488`), so `_viewer != null` holds from `Play` onward
and its stats are always the current level's. Fix: `level_ = LevelResult(); hasLevel_ = true;` in `play()`,
correct the `_viewer != null` claim at `game_flow.h:181`, and add the two-level row (Play L7 → report → Play
L6 → no report → LevelEnded) that `flow_tests.cpp` does not have.

Then item 4 (3DS input) is a clean slice: `mdk_core/src/game/input.h` is a latched `Input` with 13 actions and
a relative mouse, and libctru HID has to fill it.

Two measured facts carry forward, both unchanged by batch 71:

* **The 3DS picture is a red clear with a sliver of geometry.** The guest logs 3 draws/frame with real
  vertex data (`view: 154 batches, 1188 vertices, 399 triangles`) and the frame counter advances at 60 fps,
  so the frame path runs; what reaches the screen is not a level. Item 7's neighbourhood, and AGENTS P2's
  "all GPU state every frame" list.
* **`draws issued: 0` is printed while draws happen.** `main_3ds.cpp:277-281` reads
  `mdk_render_stats(renderer, &draws, nullptr)` at `frame == 3` and gets 0 while the same frame logs three
  `draw N:` lines. The instrument, not the game: name the cause before quoting that number anywhere.

## Standing facts a batch must not re-derive

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
