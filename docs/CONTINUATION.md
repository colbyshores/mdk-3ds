# Phase 2 continuation checkpoint

Risk: high — new audio sample-plane subsystem in `mdk_core`: the bank now owns decoded samples shared by pointer into `Entry`/`Voice`, the copy is deleted, a new sink interface lands, and the mix loops are reclassified hot path with new `@vfp-exempt` pins.

**Audio sample plane is ported and bit-exact:** `golden/L3_audio_mix.txt` and
`golden/L3_audio_meta.txt` are the C# `AudioDevice`'s own mix and decode, digested as bit patterns, and the C++ is byte-identical to both. Details: `docs/PROGRESS_LOG.md`, Iteration 107.

Last updated: 2026-11-06 (**batch 79 — the software mix is ported, and it is bit-for-bit the C# mixer**).

**Owner action:** `ralph-pi.sh` worktree mode is 664 (index 100755); this uid cannot chmod it, so systemd restart fails `203/EXEC`. Owner task 18: restore 0755 and restart `ralph-pi-mdk.service`.
**Owner action (review-4 F3 / review-2 F6):** uncommitted operator edits in `ralph/builder.md` (PROTECTED) plus routing changes in `ralph-pi.sh`, `ralph/LOOP_CONTEXT.md`, and `tools/runner/test_review_state.sh` conflict with the committed owner directive. Owner must approve/commit or revert; builder must not stage these or set `MDK_OWNER_APPROVED=1`. Stage batch paths by name only.
**Owner action (review-2 F3 / review-4 F3):** batch frames duplicate an older capture (`sha256 6646adf1…`) whenever a batch moves no pixels — batch 79's does too, and its sidecar says so. Rejecting duplicates and requiring emulator verdicts needs owner-protected gate tooling; recorded, not changed.
**Owner action (review dispatch):** `logs/ralph-pi/run-20261105-142911/review-6.md` is **0 bytes** — the reviewer was dispatched and returned nothing, and no `.rc` was written. That is a failed dispatch, not an all-clear; the range stays owed.

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
6a. **DONE (79) — the sample plane.** `mdk_core/src/audio/sound.{h,cpp}`, `limiter.{h,cpp}` and the mix half of
   `sound_device.{h,cpp}`: PCM decode of both widths, interpolation, the DirectSound pan law, the music
   low-pass, the bus/master gains, the limiter, and a `Sink` seam (NULL = the oracle's `Output.Muted`).
   Pinned bit-for-bit by two new goldens. Narrative "Iteration 107".
6b. **NEXT — the sinks and the mixer.** No shipped frontend calls `SoundDevice::update()`, so nothing is
   audible on either target: the SDL sink (Linux) and the ndsp sink (3DS) are unwritten, and no `Player` has a
   mixer on either frontend. Adapt `../forsaken/sound_3ds.c` and `../t2k/t2k_3ds/src/audio/sfx_3ds.cpp`
   (AGENTS §12: adapt, do not re-derive). R1 applies to the audio worker's entry; §10's small-stack rule
   applies to it. **The memory problem is measured and it is real:** level 3's bank is 72 sounds =
   3,234,976 samples = **12.34 MB of float** from 4,390,060 bytes on disk, against 16 MB of FCRAM, so the 3DS
   cannot `preload()` the bank as `mdk_linux/src/main.cpp:239` does. Keep the file's 8/16-bit bytes and
   convert on read, or budget and evict, or stream — and say which.
7. **Citro3D alpha blend.** No `C3D_AlphaBlend`, so `Pass::Blended` glass is opaque. Blocked on a named
   cause: the palette expansion forces alpha 255 (`renderer_c3d.cpp` `expandIndices`, `pixel[3] = 255`) and the
   vertex shader writes `outclr = ones`.
8. **CLOSED (batch 75) — the red clear.** It was the clear colour's byte order, not the render path. See the
   header of this file.

## NEXT (batch 80)

**Item 6b: make the game audible on both targets.** The mix is ported and pinned; the plumbing that would
hear it is not written. Land the SDL sink on Linux and the ndsp sink on 3DS, call `SoundDevice::update()` from
each frame, and give the `Player` a mixer so the script opcodes that play sounds reach a device. The 3DS half
must answer the 12.34 MB question above before it can preload anything, must take R1's FZ/DN helper at the
worker's entry, and must respect the audio worker's small stack (no recursion, no large locals). A batch that
changes `mdk_3ds/**` carries an `emurun.sh` verdict on `DISPLAY=:99`.

**In parallel, the level-7 view is still one-sided (Iteration 106 retired four causes by measurement).** The
census of a real Mandarine frame: 136 draws, 852 triangles, **825 inside the clip square**, `areaset=33.28`,
yet the panel is 67.7% clear colour; forcing `GPU_CULL_NONE` moves it 13 px of 96000 and the depth test off
moves it 58 px. Painting with `GPU_PRIMARY_COLOR` instead of the texture gives **exactly half** a white panel
split by a straight vertical edge — so the question is "why does coverage stop at a straight line", and the
texture path then discards a further ~18%, which is owner item 7's alpha source, not a second defect. The
oracle's landed frame (`frame029.bmp`, step 174) is a symmetric corridor, so the C++ view genuinely differs;
suspect the rotated axes or the aspect. **A landscape 400x240 target with the rotation deleted was tried and
is worse.** Do not re-derive: the frame queues everything the view built, the MVP matches the logged eye
(batch 77), the depth triple is not rejecting near geometry, and the panel is not a sheared linear copy.

**Review-4 medium findings:** F1/F2 (the geometric solid-arena pin is a stand-in for the unported DTI
connection crossing; the fall probe hard-codes the start and omits the camera stage) need a placed-fall oracle
in full order plus a walk-out golden, and are deferred to a collision batch. F3/F4 remain owner actions; no
protected path was staged. Original-3DS behaviour remains unmeasured.

## Standing facts a batch must not re-derive

* **Level 7's start is inside DANT_1 on a floor at z = 0 and the oracle drops him there too** — DTI start
  `(0,4,9)` at angle 90, `+ StartDrop` lifts it to 12, a BSP ray down hits `z = 0.01`, follow-camera eye
  `(0,-4,4.5)`. `golden/L7_spawn_fall.txt` is the oracle's own run of this. Do not re-ask "why does Kurt stop
  at z=0".
* **The audio mix's four constants are verified, not assumed:** `far(pan 0.5)=3B4F3E37`, `ceiling=3F71ADF9`,
  `release=396DC000`, `music alpha=3F131C9A`, identical from C `powf/expf` and the oracle's `MathF`, and
  `3.14159265f` is the same float as `MathF.PI`. The mix loop is hot path by AGENTS 2's second criterion
  (735 iterations a frame, no capture needed) and carries its pins; do not integerise the position or solve
  its wrap without a capture and a fidelity verdict.
* **The render seam's `clear_colour` is `0xAARRGGBB`; the PICA's clear word is `0xRRGGBBAA`, red in the TOP
  byte** (`mdk::pica::clearColour`). Assert colour contracts as **bit fields, not memory bytes**.
* **Exactly one `input3dsPoll()` per frame, and it is the only `hidScanInput()`.** libctru recomputes its edge
  bits on every scan. The poll RETURNS the held mask. The C-Stick is absent by decision, not oversight.
* **Never `git add -A`.** Owner-authored paths sit uncommitted in the worktree and at least one is PROTECTED
  (`ralph/builder.md`); staging a protected path halts the loop at exit 6. Stage your own paths by name.
* **Editing `ralph-pi.sh` does not change the running loop** (review-2 F1): bash reads a script incrementally,
  so a harness fix is only true once the process restarts. Never claim a harness fix from a commit alone.
* **Review independence is the resolved upstream and family, never the alias** (owner-protected, pinned by
  `test_review_state.sh`, which is NOT wired into `tools/check.sh` and must be run by hand). Builder
  `litellm/qwen`, reviewer `litellm/deepseek-v4.1-flash-th` — cross-family; a same-family reviewer is refused.
  A cross-uid co-tenant whose `/proc/<pid>/environ` answers EACCES is normal here: a failed environ read is
  the absence of proof, never proof of foreignness (review-4 F1).
* **Emulator-green is not hardware-pass.** Mandarine masks FCRAM/VRAM exhaustion, `GPU_CLAMP_TO_EDGE`
  leniency and FCRAM smear across soft relaunches. Old-3DS timing stays **UNMEASURED** and the Citro3D
  picture stays **UNVERIFIED** against the oracle (AGENTS P6/P8).
* **The C# oracle is runnable here, so measure instead of reasoning.** `/tmp/kilo/dotnet10/dotnet` works; the
  scratch tree is `/tmp/mdk-pi/obj3`. `Probe/` (the level/VM modes) currently does **not** build — its
  `src/` lost the `ScriptVm.ProbeTick` / `ObjectView.DumpDraw` / `MaterialResolver.TextureIds` hooks — so
  batch 79 added a separate `MixProbe/` project that builds and runs the audio oracle
  (`mdkmix <level> <updates> mix|mixmeta|bits|size`). `cmp` the scratch `src/**` against the repository's
  before believing a row; batch 79 verified `AudioDevice.cs`, `Limiter.cs`, `SoundBank.cs`, `SoundMixer.cs`,
  `Wav.cs`, `Sni.cs` identical.
* **The level data is readable with 10 lines of Python, and it settles parse arguments outright.** For a DTI,
  block *i* is at `4 + u32(20 + 4*i)`, and block 0 is `u32 startArena, vec3 startPosition, f32 startAngle`.

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
