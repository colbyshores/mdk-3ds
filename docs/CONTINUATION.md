# Phase 2 continuation checkpoint

Risk: high — the sink seam changes core audio behaviour (a second, optional queue query re-drives how many frames a mix makes), both targets gain a new backend file, the 3DS gains an ndsp wave-buffer queue in linear memory, and the console's sample-memory policy is decided here.

**The game is audible on both targets.** `mdk_linux/src/audio/audio_sdl.cpp` (SDL F32LE stream, the C#'s own calls) and `mdk_3ds/src/audio/audio_3ds.cpp` (one ndsp channel, stereo PCM16 at 44100, four 20 ms linear wave buffers, the float→PCM16 conversion at the sink) are written, both frontends call `mixer.update()` and `device().update()` per frame, and the 3DS build played the level-7 landing sound through ndsp in the emulator (`plays=1`). Details: `docs/PROGRESS_LOG.md`, Iteration 108.

Last updated: 2026-11-06 (**batch 80 — the sinks and the mixer: the mix now reaches a speaker on both targets**).

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
6b. **DONE (80) — the sinks and the mixer.** `audio_sdl.cpp` (SDL F32LE stream: the C#'s
   `SDL_OpenAudioDeviceStream`/`PutAudioStreamData`/`GetAudioStreamQueued`) and `audio_3ds.cpp` (ndsp chn0,
   stereo PCM16 44100, 4 x 20 ms linear wave buffers, float→PCM16 at the sink, the drained-channel
   clear+reset the vault's Forsaken finding warns about). The seam grew the C#'s second branch: a sink that
   can answer `queued` gets `2646 - queued`, one that cannot keeps the golden's `owed` accounting
   (`PASS AudioSinkSeam`). **No audio worker thread exists** — the sink has nothing to read, so R1's audio row
   is satisfied by `vfpThreadFzDn()` at the audio entry with its read-back logged, and §10's small-stack rule
   has nothing to bite on. **The memory question is answered: `preload()` is not called on the 3DS**; the bank
   decodes on first use (level 3 = 12.34 MB, level 4 = 15.03 MB, level 8 = 19.08 MB of float against 16 MB of
   FCRAM, so a full decode was never possible). What that does NOT do is bound the total — see NEXT.
   Emulator evidence: `plays=1`, the level-7 `LAND` sound reached ndsp. Narrative "Iteration 108".
6c. **OPEN — the sample store is still float, and the bank still has no bound.** The follow-up 6b names:
   an int16 sample store halves all three numbers with **no change to a single sample value** (both PCM scales
   are powers of two: `(byte-128)*256 * (1/32768)` is the same float as `(byte-128)*(1/128)`, and the 16-bit
   path is already exact), at the cost of one `vcvt` per sample read in the mix loop — which needs its own
   pin and a re-run of `golden/L3_audio_mix.txt` to prove the claim. Budget-and-evict is the alternative.
6d. **OPEN — the 3DS frontend has no `ScriptRuntime`, so script sounds cannot play.** The mixer on the console
   is standalone and carries Kurt's own set (footsteps, chute, guns, `LAND` — measured). Ambient and object
   sounds come from opcodes and need the runtime wired on the console, which is the same missing piece as the
   object pass and the camera's script inputs.
7. **Citro3D alpha blend.** No `C3D_AlphaBlend`, so `Pass::Blended` glass is opaque. Blocked on a named
   cause: the palette expansion forces alpha 255 (`renderer_c3d.cpp` `expandIndices`, `pixel[3] = 255`) and the
   vertex shader writes `outclr = ones`.
8. **CLOSED (batch 75) — the red clear.** It was the clear colour's byte order, not the render path. See the
   header of this file.

## NEXT (batch 81)

**Owner item 7: Citro3D alpha blend**, the first unblocked renderer item. `C3D_AlphaBlend` is absent, so
`Pass::Blended` glass is opaque, and the named cause is upstream of the blend state: the palette expansion
forces alpha 255 (`renderer_c3d.cpp` `expandIndices`, `pixel[3] = 255`) and the vertex shader writes
`outclr = ones`. Fix the alpha source first, then the blend state, and take a Mandarine frame of a level with
blended glass in the same run so the two changes are separable. Item 5b (the HUD) stays blocked on the same
canvas seam (`mdk_render_canvas_quad` is a no-op on this backend), so if the canvas work is the cheaper way
into that code, it is the same neighbourhood.

**The level-7 view is still one-sided, and batch 80 corrected the numbers it was being planned with**
(review-1 F1/F2, re-measured from `/tmp/kilo-pi/b78-*-panel.png`; Iteration 106's figures are superseded, not
edited): the censused panel is **61.0 % clear** (58,522 of 96,000 px), not 67.7 % — the old figure counted a
scene colour (`0xe12a81`, 6,463 px) as clear; `GPU_CULL_NONE` moves it **69** px and the depth test off **201**
px (the record said 13 and 58); the "texture path discards a further ~18 %" is **withdrawn** (it compared two
different runs: non-clear 47,845 px white vs 37,478 px textured, a 10.8-point gap inside a larger coverage
change); and the census's own mapping probe, applied, **fails** — the ndc box maps outside the target on three
sides, so it covers the panel while the panel is 61 % clear, which a union of bboxes with no per-pixel coverage
and no depth test cannot localise. `logCensus` now prints the pixel box, its coverage verdict, and a `depthout`
counter (this frame: `onscreen=825 depthout=0`, so nothing was lost past the depth range).

So the live question is unchanged and the instrument is honest now: **why does coverage stop at a straight
vertical line at the panel's middle** (the `GPU_PRIMARY_COLOR` run: 0 white px for x<200, 97 at x=200, 240
from x=202). Do not re-derive what Iteration 106 retired (the cull, the depth triple, the queue, the MVP, a
deleted rotation). The next useful capture is **census and `GPU_PRIMARY_COLOR` in the same run**, with the
panel crop published, so the comparison is frame-identical.

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
* **A private emulator lane has no DSP firmware unless `emurun.sh` seeds one.** Isolating `XDG_DATA_HOME`
  also isolates `sysdata/dspfirm.cdc`, which is what Mandarine auto-populates the *shared* sdmc from, so a
  lane that reads its own log silently loses ndsp: `ndspInit` would fail and the game mutes with one log line
  as the only trace. `emurun.sh` copies the operator's dump into the lane and prints a `dspfirm :` line;
  `EMURUN_DSPFIRM=/dev/null forces the missing-firmware path. A verdict without that line is an older build.
  The blob is never committed and never shipped in a CIA.
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
