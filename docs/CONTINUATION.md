# Phase 2 continuation checkpoint

Risk: medium — this batch changes no behaviour and no pixel: it retracts a measurement with a clean-build A/B, archives the evidence pair, and adds gate checks (3 checks + 4 arms in `c3d_frame_test.sh`, one matrix case) plus comment corrections. The emulator A/B is 0 px on the panel; the only diff is the emulator's own overlay text.

**Batch 83's headline measurement was taken against a build that is not its parent.** A clean `f47d8fc` worktree build, run through the same pinned harness (`DISPLAY=:99`, Mandarine `0d2adab6`, exit 0, guest frame 1680 — the same frame and the same static camera), differs from batch 83's `before.png` by **33795 px** of the top panel and from its `after.png` by **0**. So the "the opaque passes were blending at six percent" story is retracted, and with it the "`(255,0,0)` landed at `(30,30,45)`" example and the 33909/68001→58127 table: every visible fragment in the start arena has alpha 255, which is what a replace pair and a `SRC_ALPHA` pair agreeing pixel for pixel across 96000 pixels means. The blend state and the alpha test stay — they are the oracle's state (`Renderer.cs:565-576`) and AGENTS P2's "write every GPU state explicitly" — but they carry **no runtime evidence in either direction**: the start arena is 154 all-`Solid` batches, so `Pass::Blended`, `Pass::Overlay` and `Blend::Add` are unreached by any capture, and the frame gate plus `PASS PicaMatrix` are what hold them now. Evidence, with commands and hashes: `docs/evidence/2026-11-06-L7-blend-ab-{parent,current}.{png,txt}` — the two crops are byte-identical, and that equality is the finding. Review-3 F1–F5 all closed; narrative `docs/PROGRESS_LOG.md` Iteration 112.

**The census was never inflated, so do not re-run it.** The exact clear-colour pixels `(16,32,48)` of the 400x240 panel are **58464 in the parent and 58464 in this build**, so the 61 % figure stands and this file's previous "re-census first" instruction is withdrawn. The one-sided view, stated in panel columns (review-3 F4 — "column 640 of the 400-wide panel" mixed capture pixels with panel columns): geometry covers panel columns **200..399** of 400, the right half of the top screen, and the census instrument is honest.

Last updated: 2026-11-06 (**batch 84 — review-3 F1–F5 closed; batch 83's before/after table retracted, the census premise with it**).

**Next small batch (review-2 F2–F5, all confirmed, all comment-only, all in `mdk_3ds/src/audio/audio_3ds.{h,cpp}`):** the `dspframes` unit is DSP sound frames at `SYSCLOCK_SOC/512 = 32728.5 Hz` ≈ 204.5/s (not 240-sample blocks at 48 kHz); the `frames × 735/882` denominator is exact only at 60 fps; `inDsp` pinned at 4 is also the signature of "nothing was ever submitted"; and `submitted - done == inDsp` holds for an **open** sink. Full text of each in `docs/PROGRESS_LOG.md`, Iteration 111.


**Owner action (review F2, new):** `ralph/builder.md`'s "Search scope and bounded commands" block forbids sibling reads, while `AGENTS.md` §12 requires them ("**Proven 3DS platform code in sibling ports — adapt, do not re-derive**", naming `../forsaken/sound_3ds.c`, `../t2k`'s ndsp audio, `../perfect_dark/tools/emurun.sh`) and the 2026-11-05 directive names `../forsaken/sound_3ds.c` for this subsystem — it is where the `nsamples` convention came from. One of the two must be amended by the owner; the builder may not edit either. Combined with the item below about the same uncommitted file.
**Owner action (review F3, from run-20261106-035708):** `SPEC.md:809` says the 3DS "voice pool shrinks to 24 and the queue to 40 ms to fit the 3DS's DSP budget". The port ships the oracle's 64 (`src/Mdk.Engine/Audio/AudioDevice.cs:73 MaxVoices = 64`, no platform branch) and an 80 ms queue, and `audio_3ds.h` reads §8's 24 as the software mixer's knob. That reading is a deviation from a protected document and the builder cannot resolve it: either amend SPEC.md §8 or authorise a per-target cap. Unmeasured either way — the hot-path corpus is empty, and nothing in the current build exceeds a handful of voices.
**Owner action:** `ralph-pi.sh` worktree mode is 664 (index 100755); this uid cannot chmod it, so systemd restart fails `203/EXEC`. Owner task 18: restore 0755 and restart `ralph-pi-mdk.service`.
**Owner action (review-4 F3 / review-2 F6):** uncommitted operator edits in `ralph/builder.md` (PROTECTED) plus routing changes in `ralph-pi.sh`, `ralph/LOOP_CONTEXT.md`, and `tools/runner/test_review_state.sh` conflict with the committed owner directive. Owner must approve/commit or revert; builder must not stage these or set `MDK_OWNER_APPROVED=1`. Stage batch paths by name only.
**Owner action (review-2 F3 / review-4 F3):** batch frames duplicate an older capture (`sha256 6646adf1…`) whenever a batch moves no pixels — batch 79's, 80's, 81's and 82's do, and each sidecar says so and names the measurement. Rejecting duplicates and requiring emulator verdicts needs owner-protected gate tooling; recorded, not changed.
**Owner action (review dispatch):** `logs/ralph-pi/run-20261105-142911/review-6.md` is **0 bytes** — the reviewer was dispatched and returned nothing, and no `.rc` was written. That is a failed dispatch, not an all-clear; the range stays owed.
**Owner action (session config, AGENTS P9):** neither `obsidian_*` nor `cognee_*` tools are exposed in this session, so the vault lookup is the degraded disk-only path and Cognee's graph search was not available at all. Reported, not worked around silently.

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
6b. **DONE (80, corrected by 81) — the sinks and the mixer.** `audio_sdl.cpp` (SDL F32LE stream: the C#'s
   `SDL_OpenAudioDeviceStream`/`PutAudioStreamData`/`GetAudioStreamQueued`) and `audio_3ds.cpp` (ndsp chn0,
   stereo PCM16 44100, 4 x 20 ms linear wave buffers, float→PCM16 at the sink, the drained-channel
   clear+reset the vault's Forsaken finding warns about). The seam grew the C#'s second branch: a sink that
   can answer `queued` gets `2646 - queued`, one that cannot keeps the golden's `owed` accounting
   (`PASS AudioSinkSeam`). **No audio worker thread exists** — the sink has nothing to read, so R1's audio row
   is satisfied by `vfpThreadFzDn()` at the audio entry with its read-back logged, and §10's small-stack rule
   has nothing to bite on. **The memory question is answered: `preload()` is not called on the 3DS**; the bank
   decodes on first use (level 3 = 12.34 MB, level 4 = 15.03 MB, level 8 = 19.08 MB of float against 16 MB of
   FCRAM, so a full decode was never possible). What that does NOT do is bound the total — see NEXT.
   Its emulator evidence (`plays=1`) was an over-claim, corrected by batch 81 and re-corrected by 82;
   the ledger line is the evidence now. Narrative "Iteration 108", corrections "Iteration 109" and
   "Iteration 110".
6c. **OPEN — the sample store is still float, and the bank still has no bound.** The follow-up 6b names:
   an int16 sample store halves all three numbers with **no change to a single sample value** (both PCM scales
   are powers of two: `(byte-128)*256 * (1/32768)` is the same float as `(byte-128)*(1/128)`, and the 16-bit
   path is already exact), at the cost of one `vcvt` per sample read in the mix loop — which needs its own
   pin and a re-run of `golden/L3_audio_mix.txt` to prove the claim. Budget-and-evict is the alternative.
6d. **OPEN — the 3DS frontend has no `ScriptRuntime`, so script sounds cannot play.** The mixer on the console
   is standalone and carries Kurt's own set (footsteps, chute, guns, `LAND` — measured). Ambient and object
   sounds come from opcodes and need the runtime wired on the console, which is the same missing piece as the
   object pass and the camera's script inputs.
7. **DONE (83, measurement corrected by 84) — Citro3D alpha blend** (owner directive item **6** — this file's
   own numbering drifted, review F6). The blend state is installed per pass from the oracle, and the shader's
   index-0 discard is the PICA's alpha test. The named cause was wrong: citro3d's own initialiser had been
   blending every pass on the oracle's colour pair, and the flat-colour path already carries the material's
   alpha. What batch 84 corrected is the *evidence*: its before/after table was measured against a build that
   is not its parent, so the change is verified as the oracle's state and as AGENTS P2 compliance, and is
   **not** verified as a visible fix — it moves 0 px of the panel, because every fragment there has alpha 255.
8. **CLOSED (batch 75) — the red clear.** It was the clear colour's byte order, not the render path. See the
   header of this file.

## NEXT (batch 85)
**The one-sided view.** Geometry covers panel columns **200..399** of the 400-wide top
screen — the right half — and nothing else has changed: the census's "61 % of the panel
is clear colour" was **not** inflated by a transparency bug (batch 84 measured 58464
exact clear-colour pixels in both the parent build and this one), so the one-frame
census does **not** need re-running first. Its instrument is honest and its probes were
never read through a bug: `GPU_CULL_NONE` moves 69 px, the depth test off 201 px, and
the census's own mapping probe fails, so it cannot localise — `logCensus` says so on
its own output. The live question is why coverage stops at a straight vertical line at
the panel's middle. Do not re-derive what Iteration 106 retired (the cull, the depth
triple, the queue, the MVP, a deleted rotation). The next useful capture is **census
and `GPU_PRIMARY_COLOR` in the same run**, with the panel crop published, so the
comparison is frame-identical. The audio-docs batch (F2–F5 above) is the cheap
alternative if the view is the longer job.
Item 5b (the HUD) stays blocked on the same canvas seam (`mdk_render_canvas_quad`
is a no-op on this backend).

**Audio, in priority order after the renderer items.** (a) 6c — the int16 sample store, which halves the
bank's footprint with no change to a sample value, pinned by a re-run of `golden/L3_audio_mix.txt`. (b) 6d —
the console has no `ScriptRuntime`, so ambient and object sounds cannot play; only Kurt's own set exists.
(c) the bank still has no eviction bound (6b). (d) the ndsp sink's `nsamples` unit is now *measured* in the
emulator's HLE DSP; the Old-3DS check stays open (AGENTS P6/P8). What to read on hardware
is the ledger line's `submitted` against `frame x 735/882` — **not** `done` against
`submitted`, which is an identity and proves nothing (batch 82, review F1).

**Deferred review findings:** review-4 F1/F2 (the geometric solid-arena pin stands in for the unported
DTI connection crossing; the fall probe hard-codes the start and omits the camera stage) need a
placed-fall oracle in full order plus a walk-out golden, and are deferred to a collision batch.
Original-3DS behaviour remains unmeasured.

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
* **ndsp's wave-buffer contract, both halves measured in batch 81:** `wave.status` MUST be
  `NDSP_WBUF_FREE` at the `ndspChnWaveBufAdd` call — libctru will not link a buffer whose status is
  QUEUED or PLAYING, it sets QUEUED itself — and `nsamples` is in **frames** for a stereo format, not
  interleaved halfwords. `libctru/include/3ds/ndsp/ndsp.h:66` says "PCM16=halfwords" and
  `examples/3ds/audio/ogg-vorbis-decoding` follows it; four other stereo examples and the hardware-proven
  `../forsaken/sound_3ds.c` say frames, and the emulator's rate measurement settles it. Pinned by
  `tests/scripts/ndsp_sink_test.sh`. Do not re-derive either from the header comment.
* **The sink's ledger is a balance, not a rate (batch 82, review F1):** `submitted - done
  == inDsp <= 4` holds by construction in every state, healthy or broken, so no reading of
  `done / submitted` means anything — batch 81's broken build measured 0.996. The rate is
  `submitted / (frame x 735/882)`, which is why the ledger line prints the frame; the three
  measured builds give 0.002 (nothing queued), 0.502 (the wrong `nsamples`) and 1.003
  (correct). `dropped` counts rejected FILLS, not buffers. Pinned by `ndsp_sink_test.sh`'s
  `ledger-balances` and `counts-done-at-the-sink-recheck`.
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
* **An A/B capture must name the revision it was built from, or it is not evidence (batch 84).** Batch 83's
  `before.png` was a real capture of a real build — the wrong one: a clean `f47d8fc` worktree differs from it
  by 33795 px of the panel and from batch 83's `after` by 0. AGENTS P6 requires the revision in an emulator
  record and `emurun.sh` names only the `.3dsx` path, so an A/B pair records each `.3dsx` md5 and its commit
  and archives both in `docs/evidence/`. Vault: `date-your-evidence-against-your-tree.md` ("a measurement is
  a statement about the TREE THAT PRODUCED IT") and
  `relative-vs-absolute-oracle-for-emulated-subsystems.md` (a relative A/B proves an effect, never
  correctness — say which kind a pair is). Noise floor while you do: Mandarine draws its own `Frame: N.NN ms`
  overlay on the bottom screen, so two runs of the SAME build differ by ~64-112 px at x 753..767,
  y 744..752 of the 1280x1024 window; compare the top-panel crop `(440,253)-(839,492)`, 400x240, and state it.
* **Every visible fragment of the level-7 start arena has alpha 255 — measured, not read.** The parent
  (citro3d's `SRC_ALPHA` blend) and batch 83 onward (the oracle's replace pair) draw byte-identical 400x240
  panels. Do not re-open "the opaque passes were blending" as a visible defect; the register state was wrong
  and is now the oracle's, and the pixel effect is nil until a scene with a real material alpha on an
  unblended pass exists. `docs/evidence/2026-11-06-L7-blend-ab-{parent,current}.txt`.
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
