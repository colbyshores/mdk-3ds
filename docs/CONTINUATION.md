# Phase 2 continuation checkpoint

Risk: high — batch 69 changes the loop's evidence chain in three places at once (the self-staleness verdict
becomes positive and is re-sampled per artifact, an unreadable hash stops reading as "fresh", and an
unclaimable process now counts as foreign for FUSE mount release) plus the emulator instrument's INI
section-ownership rule — i.e. who may certify a harness fix, whether a mount may be detached, and whether a
forced config key may be reported as forced. No game code changed. Emulator evidence is attached; hardware
evidence is not.

Last updated: 2026-11-05 (**batch 69** — review-3 remediated, all six findings confirmed and fixed, none
rebutted: the staleness stamp is now positive (`harness: CURRENT sha=…`) so its absence is signal, it is
re-sampled after the worker commits, an unreadable self-hash is its own verdict, an inert INI key in the wrong
section is moved and reported as moved instead of being claimed as a forced override, and a process this uid
cannot read is now foreign so the emulator lane leaves the mount alone — which fired for real on this batch's
emulator run. The record is `docs/PROGRESS_LOG.md`, "Iteration 97"; batch 68's (the stale-loop guard) is
"Iteration 96", batch 67's (the guest boots) "Iteration 95", batch 66's (the harness instrument) "Iteration
94", batch 65's "Iteration 93", batch 64's "Iteration 92", batch 63's "Iteration 91", batch 62's "Iteration
90"; earlier batches are indexed by number in that file.).

**Owner action still open (blocks the loop's own fixes taking effect):** the worktree `ralph-pi.sh` is mode
`664` while the index says `100755`, this uid cannot `chmod` it (owner task 18), and the systemd unit execs it
directly — so a restart today fails `203/EXEC` and the loop keeps running a pre-fix copy. Restore `0755` and
restart `ralph-pi-mdk.service`. **Until that restart, no review artifact carries any `harness:` line at all**
(review-3 F1, confirmed: `review-3.identity.txt` has none, nor even the `independence:` line batch 68 added).
Batch 68's wording here claimed every artifact was stamped `harness: STALE`; that was false and is corrected —
the guard that writes the stamp lives in the copy that is not running. So **the absence of a `harness:` line
means nothing**, and must not be read as "the running loop is current". From the first restart onward the
stamp is positive (`harness:  CURRENT sha=…` on every artifact), which is the only form that survives being
read by a loop too old to write it.

## PRIORITY ORDER (owner directive 2026-11-05, in force until withdrawn)

**Goal: a CIA that plays a level end-to-end, verified in Mandarine.** This supersedes the older numbered
checkpoint below; the 1-ULP fidelity work is not thrown away but it is no longer first, and no new 1-ULP
work opens unless a listed item needs it.

1. **DONE (batch 67) — the emulator harness.** `mdk_3ds/tools/emurun.sh` runs `mdk.3dsx` on `DISPLAY=:99`
   against pinned Mandarine r1.2.1 and prints a verdict: display, emulator md5, frames reached, the forced
   instrument read back after the run, and the guest-log lines. Exit 3 = stale log, 4 = emulator died,
   5 = too short, 6 = wrong emulator build. Every 3DS batch carries its verdict from here on (AGENTS P8).
2. **NEXT — game-flow glue.** Port `src/Mdk.Game/Flow/Game.cs` (689 lines): level start, the tick order,
   level end, death/restart. Transliteration, not research.
3. **3DS input.** Fill `mdk_core/src/game/input.h` from libctru HID. Adapt `../forsaken/input_3ds.c`; heed
   the double-swap and axis-exists traps in `../forsaken/3DS_PORT_NOTES.md`.
4. **Wire the frontend.** `main_3ds.cpp` drives the real game: input → player → the ported `FollowCamera`
   (not the framing camera) → Kurt's sprite → the HUD. It reads only START/SELECT today.
5. **ndsp audio backend.** The control plane in `mdk_core/src/audio/` is ported; the sample plane is not
   (`sound_device.h:13`). Adapt `../forsaken/sound_3ds.c` and `../t2k/t2k_3ds/src/audio/sfx_3ds.cpp`, then
   set the mixer on both frontends.
6. **Citro3D alpha blend.** No `C3D_AlphaBlend`, so `Pass::Blended` glass is opaque. Blocked on a named
   cause: the palette expansion forces alpha 255 and the vertex shader writes `outclr = ones`.

**Cut from the sprint (deferred with cause):** menu/stats/save (`Menu/` ≈ 1,820 C# lines — boot straight
into a level), rides/snowboard/stream-fall view/sniper/cutscene camera, the 27 deferred opcodes, the
Old-3DS optimisation pass (take a first capture; do not optimise against a corpus that does not exist).

## NEXT (batch 70)

Item 1 stays closed and item 2 (`Game.cs`) is next — batch 69 changed no game code. Two measured facts to
carry into it, both from a real emulator run and both unchanged by this batch:

* **The 3DS picture is a red clear with a sliver of geometry.** The guest logs 3 draws/frame with real
  vertex data (`view: 154 batches, 1188 vertices, 399 triangles`) and the frame counter advances at 60 fps,
  so the frame path runs; what reaches the screen is not a level. That is item 6's neighbourhood (alpha/colour
  source) and AGENTS P2's "all GPU state every frame" list, and it is a visible, measurable defect instead of
  an inference. Capture: `/tmp/kilo-pi/batch69-3ds.png` (emulator window, top screen).
* **`draws issued: 0` is printed while draws happen.** `main_3ds.cpp:277-281` reads
  `mdk_render_stats(renderer, &draws, nullptr)` at `frame == 3` and gets 0 while the same frame logs three
  `draw N:` lines. Either the counter is reset before it is read or it counts a different path. It is the
  instrument, not the game: name the cause before quoting that number anywhere.

## Standing facts a batch must not re-derive

* **Never `git add -A`.** Owner-authored paths sit uncommitted in the worktree and at least one is
  PROTECTED (`ralph/builder.md`); staging a protected path halts the loop at exit 6. Stage your own paths
  by name. Do not trust a list of them — read `git status --porcelain` in the batch that needs to know
  (review-2 F7: the list kept here had been stale for a batch and sent a reader hunting for files that were
  no longer dirty).
* **Editing `ralph-pi.sh` does not change the running loop** (review-2 F1). bash reads a script incrementally,
  so a harness fix is only true once the process restarts, and the loop now enforces that: it hashes its own
  file at startup, re-checks every iteration, and exits 8 when the tree has moved past it — unless the file is
  not executable, where a restart would fail `203/EXEC` and stopping would be worse than a labelled stale run.
  So: never claim a harness fix is in effect from a commit alone, and expect a restart to be required.
  Two refinements from review-3: the verdict is **re-sampled before each review artifact** (F3, so the batch
  that edits the harness is labelled in the artifact that certifies it) and an **unreadable** self-hash is
  itself a verdict rather than a skipped guard (F6). Neither is in effect until the restart, for the reason
  in the paragraph above.
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
* **A cross-uid co-tenant running `mandarine.AppImage` on this box is real, not hypothetical** (review-3 F4
  open question, answered by measurement on batch 69's run: `/proc/<pid>/environ` → `Permission denied`). So
  an unreadable `/proc` entry is a normal condition an emulator lane must survive, and `emurun.sh` now treats
  it as *foreign* — the lane leaves the mount alone and says so. Do not "simplify" that back to a bare
  `readlink` check.
* **`test_review_state.sh` is not wired into `tools/check.sh`** (check.sh is owner-protected, so wiring it is
  an owner task). Run `bash tools/runner/test_review_state.sh` by hand with the gates; a green
  `tools/check.sh` says nothing about the review state machine.
* **`/tmp/kilo` is `museagent`-owned and not writable by this uid**; captures go to `/tmp/kilo-pi` (P7's rule
  is "capture to files, never to the operator's display", which this satisfies).

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
