# Phase 2 continuation checkpoint

Risk: high — batch 67 changes the shipped Citro3D shader-create path (the create-time
`shaderProgramConfigure` is deleted, so the microcode upload moves to the in-frame bind) and rewrites the
loop's review-independence guard (same-family allowlist removed, cross-family reviewer chain restored).
Both are behaviour and both are the loop's evidence chain; a wrong call on either is a silent false PASS.
Emulator evidence is attached, hardware evidence is not.

Last updated: 2026-11-05 (**batch 67** — owner-directive item 1 is DONE: the Mandarine harness prints a
real verdict and the guest boots. The hang was the create-time `shaderProgramConfigure`; dropping it made
`backend: PICA200 Citro3D` appear and the run reach frame 1080 at 60 fps. Review-1's F1 (Critical) and F2
(High) are fixed — the same-family reviewer allowlist is gone and the identity artifact now states whether
independence held — and F3–F8 are fixed too. The record is `docs/PROGRESS_LOG.md`, "Iteration 95".).
Batch 66's record (the harness instrument, and the hang named) is "Iteration 94"; batch 65's (review-6's
camera min/max class) is "Iteration 93"; batch 64's (review-5's claim corrections) is "Iteration 92";
batch 63's (review-4's camera pins) is "Iteration 91"; batch 62's (the follow camera's between-step blend)
is "Iteration 90"; earlier batches are indexed by number in that file.

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

## NEXT (batch 68)

Item 1 is closed, so item 2 (`Game.cs`) is next. Two measured facts to carry into it, both from the first
real emulator run (Iteration 95):

* **The 3DS picture is a red clear with a sliver of geometry.** The guest logs 3 draws/frame with real
  vertex data and the frame counter advances at 60 fps, so the frame path runs; what reaches the screen is
  not a level. That is item 6's neighbourhood (alpha/colour source) and AGENTS P2's "all GPU state every
  frame" list, and it is now a visible, measurable defect instead of an inference. Capture:
  `/tmp/kilo-pi/batch67-3ds-final.png` (emulator window, top screen).
* **`draws issued: 0` is printed while draws happen.** `main_3ds.cpp:277-281` reads
  `mdk_render_stats(renderer, &draws, nullptr)` at `frame == 3` and gets 0 while the same frame logs three
  `draw N:` lines. Either the counter is reset before it is read or it counts a different path. It is the
  instrument, not the game: name the cause before quoting that number anywhere.

## Standing facts a batch must not re-derive

* **Never `git add -A`.** Owner-authored paths sit uncommitted in the worktree and at least one is
  PROTECTED (`ralph/builder.md`); staging a protected path halts the loop at exit 6. Stage your own paths
  by name. Currently uncommitted and owner-owned: `AGENTS.md`, `ralph/builder.md`,
  `docs/OWNER_DIRECTIVE_2026-11-05.md`, `tools/runner/gates_batch_integrity.py`, `.kilo/agent/*`,
  `ralph/LOOP_CONTEXT.md`, `docs/frames/2026-11-02-ralph-self-heal.{png,txt}`.
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
* `/tmp/kilo` is `museagent`-owned and not writable by this uid; captures go to `/tmp/kilo-pi` (P7's rule
  is "capture to files, never to the operator's display", which this satisfies).

## Verification commands

```
make -C tests/scripts                        # goldens ("All cases match the oracle")
bash tools/check.sh                          # all contract, parity, frame and integrity gates
make -C mdk_3ds                              # devkitARM target (needs DEVKITPRO/DEVKITARM set)
bash tools/runner/test_review_state.sh       # the review-record state machine
python3 tools/runner/model_identity.py selftest --live
DISPLAY=:99 mdk_3ds/tools/emurun.sh --secs 30 --shot /tmp/kilo-pi/3ds.png
tools/publish_frame.sh <label>               # the pinned 320x240 frame pair
```
