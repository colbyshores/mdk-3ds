# Phase 2 continuation checkpoint

Risk: medium — review-8 remediation: **no code byte moves** (the clean out-of-tree build's `mdk.elf` is md5 `75b1edb06d0d2c58aecafb6f7b008421`, identical to batch 88's, and the in-tree build agrees), so the diff is comments in `mdk_3ds/src/audio/*`, one gate, and prose. It is not `low` because a gate was edited in the same batch — `ndsp_sink_test.sh`'s clock check went from presence-only to structural and gained a mutation arm (16 checks/11 arms → **16/12**, every arm adjudicated red against the check it names) — and because the shipped comments describe target-specific timing that a later hardware pass is told to read. The reviewer's check is whether the strengthened gate really fails on a substituted value.

**Review-5 (run-20261106-062909) closed: seven findings, none rebutted** (Iteration 114). Its two durable rules are standing facts below — **a build's identity is `mdk.elf`, never `mdk.3dsx`**, and **the panel crop is `tools/panel_crop.py`**. Its F2 was the second could-not-fail check in a row; its F3 struck a false mechanism (`DrawQueue::add` clamps an out-of-range bucket to 0, so a seventh pass folds into `Solid`).

**Review-6 (same run) closed: four findings, none rebutted, three fixed, one owner-only** (Iteration 115). Its F1 was the loop's own record lying about itself, and batch 87 annotates such blocks **in place**: an append-only log is corrected by annotation, never by rewriting, because the audit trail is what made the error findable. Its F3 is a standing fact below (the alpha-test corner's **two independent triggers**). F2 is owner-only (the reviewer holds a qwen slot).

**Review-2 (same run) F2–F5 closed by batch 88, then corrected by batch 89** (Iterations 116, 117). **F2**'s `dspframes` unit was wrong twice over ("240-sample blocks at 48 kHz"; ndsp is neither); the unit is **derived** from a disassembly of `libctru.a` (`ndspThreadMain` adds 1 to `.bss.frameCount` once per iteration, after the `svcSignalEvent` that kicks the DSP), and the DSP rate is `NDSP_SAMPLE_RATE = SYSCLOCK_SOC/512 = 32728.5 Hz` — batch 88 printed 32768 and review-8 F1 struck it. **F3**'s denominator was a frame count standing in for a clock, exact only at 60 fps, so the frontend accumulates the `elapsed` the mixer was actually given and prints `secs=` on the ledger line: the test is `submitted/(secs x 50)`, measured 0.9967 at frame 1800 with the guest at 59.64 fps. **F4**: `inDsp == 4` with `dropped` climbing means *nothing retired*, not *the queue is jammed* — the pre-`status = FREE` run read `submitted=4 done=0 dropped=2579 inDsp=4` with ndsp having taken nothing. **F5**: `submitted - done == inDsp` is an **open-sink** identity (`audioClose()` zeroes `inDsp` and not the other two).

**Review-8 (same run) closed by batch 89: six findings, none rebutted, all six fixed — one of them in four of its five named files.** Its High was that batch 88's corrected unit was itself wrong: `SYSCLOCK_SOC` is `16756991u` (`os.h:9`) and `NDSP_SAMPLE_RATE` is `(SYSCLOCK_SOC / 512.0)` (`ndsp/ndsp.h:9`), so the DSP runs at **32728.5 Hz**, the value Iteration 111 had right before batch 88 "corrected" it to a power of two. The re-measurement this batch took (a longer ledger window, frame 1800) shows the counter is **not a rate instrument at all**: 204.71/s at 30 s, bounded to (204.67, 204.74] by the 2-dp clock, which excludes both 32728.5/160 = 204.55 and 32768/160 = 204.80 — so the emulator confirms neither model, the 160-sample quantum is an assumption nothing installed names, and the console's mixer rate stays unmeasured. Its F2 was the third could-not-fail check in a row (the `secs=` check pinned the field's presence, not its value: printing `(double)delta` passed all 16 checks); the check is now structural and has an arm for exactly that substitution. Its F3 was an inverted comparison row (`501/500 = 1.0020`, not `0.9980`), which reversed the direction of its own argument. F4/F5/F6 were the gate header still stating the superseded rate rule, a `main_3ds.cpp:439` citation that is now `:449`, and a disassembly immediate (`#340` → `#260`). Narrative Iteration 117.

**Two historical corrections that stay load-bearing.** Batch 83's headline measurement was taken against a build that is not its parent (batch 84's retraction stands), and with review-5 F1 the pattern was three consecutive batches trusting a number whose provenance was asserted rather than reproduced — what is different now is that anyone can reproduce both halves with `git archive`/`git ls-files` + `make` in a throwaway tree, and the identity field is the ELF. And **the census was never inflated, so do not re-run it**: the exact clear-colour pixels `(16,32,48)` of the 400x240 panel are **58464 in the parent build and 58464 in this one**, and that equality is the whole argument (review-4 F8: do not restate it as "the 61 % figure stands" — the census's 61 % was 58522 near-clear pixels of a 95361-px panel at a different frame with a different tolerance, a different quantity). The old "re-census first" instruction stays withdrawn. Coverage is stated in panel columns (review-3 F4): geometry covers panel columns **200..399** of 400, the right half of the top screen.

Last updated: 2026-11-06 (**batch 89 — review-8 closed: the ndsp DSP rate is 32728.5 Hz and not 32768, the frame counter is now documented as a liveness signal rather than a rate instrument because a longer ledger window excludes both candidate rates, the `secs=` gate check pins the VALUE and not just the field, and the inverted comparison row is corrected in place in both records**).

**The audio-docs batch is DONE (88), and its own correction is DONE (89).** The corrections are in `mdk_3ds/src/audio/audio_3ds.{h,cpp}` and `mdk_3ds/src/main_3ds.cpp`; the text of each is `docs/PROGRESS_LOG.md` Iterations 116 and 117. Two durable facts are in the standing list below: **the ndsp frame counter counts mixer frames, not samples**, and **its rate is not measured — the kind is derived, the quantum is assumed**.


**Owner action (review F2, new):** `ralph/builder.md`'s "Search scope and bounded commands" block forbids sibling reads, while `AGENTS.md` §12 requires them ("**Proven 3DS platform code in sibling ports — adapt, do not re-derive**", naming `../forsaken/sound_3ds.c`, `../t2k`'s ndsp audio, `../perfect_dark/tools/emurun.sh`) and the 2026-11-05 directive names `../forsaken/sound_3ds.c` for this subsystem — it is where the `nsamples` convention came from. One of the two must be amended by the owner; the builder may not edit either. Combined with the item below about the same uncommitted file.
**Owner action (review F3, from run-20261106-035708):** `SPEC.md:809` says the 3DS "voice pool shrinks to 24 and the queue to 40 ms to fit the 3DS's DSP budget". The port ships the oracle's 64 (`src/Mdk.Engine/Audio/AudioDevice.cs:73 MaxVoices = 64`, no platform branch) and an 80 ms queue, and `audio_3ds.h` reads §8's 24 as the software mixer's knob. That reading is a deviation from a protected document and the builder cannot resolve it: either amend SPEC.md §8 or authorise a per-target cap. Unmeasured either way — the hot-path corpus is empty, and nothing in the current build exceeds a handful of voices.
**Owner action:** `ralph-pi.sh` worktree mode is 664 (index 100755); this uid cannot chmod it, so systemd restart fails `203/EXEC`. Owner task 18: restore 0755 and restart `ralph-pi-mdk.service`.
**Owner action (review-4 F3 / review-2 F6):** uncommitted operator edits in `ralph/builder.md` (PROTECTED) plus routing changes in `ralph-pi.sh`, `ralph/LOOP_CONTEXT.md`, and `tools/runner/test_review_state.sh` conflict with the committed owner directive. Owner must approve/commit or revert; builder must not stage these or set `MDK_OWNER_APPROVED=1`. Stage batch paths by name only. **Attribution, which review-4's open question 1 asked for: these six files are operator-authored, not builder work.** The content is harness policy no batch writes — reviewer/worker chains, `ITER_TIMEOUT`/`REVIEW_TIMEOUT`, `PI_CMD=/mnt/nas/Development/qwen-slot-pi.sh`, the watchdog's phase limits, and a "Search scope" section in the builder's own contract; batches 83, 84 and 85 each staged none of them, and batch 85 staged its paths by name and did not use the add-all commit path. So review-4 F2 is documentation hygiene plus a gate gap, not a protected-contract breach.
**Owner action (review-4 of run-20261106-062909, F2):** `tools/check.sh:175` runs `gates_batch_integrity.py` with neither `--staged` nor `--base`, so a PROTECTED edit sitting in the worktree or the index is invisible to the loop's own gate and an add-all commit lands it first and fails the gate second. The two files that would carry the fix — `tools/check.sh` and `tools/runner/gates_batch_integrity.py` — are BOTH in `PROTECTED`, so this is an owner edit: run the gate with `--staged` when the index is non-empty (or `--base HEAD`). Also restore `tools/runner/test_review_state.sh`'s mode bit (100755→100644 in the same uncommitted set; this uid does not own the file and cannot chmod it).
**Owner action (review-4 of run-20261106-062909, F3):** three texts and one sibling repository disagree on whether a same-family review exception exists, and only the owner can pick a winner. The uncommitted `docs/OWNER_DIRECTIVE_2026-11-05.md` says "the `-th` -> `flash` pair is the single operator-approved same-family exception"; `ralph-pi.sh` says "No independence exception appears anywhere in this file: none is needed, and one must not be added"; `tools/runner/test_review_state.sh` still pins `REJECT` for a same-family pair. The same edit also asserts `tetrisphere-godot` carries the same chains, which its own `tools/ralph-pi.sh:120` falsifies (`WORKER_MODELS="litellm/qwen litellm/luna"`). Either the directive loses that sentence or the exception becomes a code change in `reviewer_reject_reason` plus a rewritten test case — neither is a builder batch.
**Owner action (review-4 of run-20261106-062909, F4):** the uncommitted `ralph-pi.sh` `guard()` replaced `FATAL: pi write probe failed on every route / exit 3` with an unbounded `while :; … sleep 300`, and it rewrites `phase route-health` before every round, so the watchdog's `route-health|startup|verify) limit=900` arm compares against a refreshed epoch and can never fire: a loop with no live route parks silently, with `logs/ralph/disabled` already touched. The wait is the operator's decision (the file says so); bounding it is not builder-owned. Suggested: after N failed rounds write a phase no watchdog arm covers (e.g. `route-dead`) and stop refreshing the epoch on a failed round. **Relabelled per review-5 F7: this is an operator decision that `PROTECTED` does not enforce** — neither `ralph-pi.sh` nor `tools/ralph_pi_watchdog.sh` is in the `PROTECTED` set, so the boundary here is ownership, not a gate, and a reviewer should not re-derive it as a gate gap.
**Owner action (review-2 F3 / review-4 F3):** batch frames duplicate an older capture (`sha256 6646adf1…`) whenever a batch moves no pixels — batch 79's, 80's, 81's and 82's do, and each sidecar says so and names the measurement. Rejecting duplicates and requiring emulator verdicts needs owner-protected gate tooling; recorded, not changed.
**Owner action (review dispatch):** `logs/ralph-pi/run-20261105-142911/review-6.md` is **0 bytes** — the reviewer was dispatched and returned nothing, and no `.rc` was written. That is a failed dispatch, not an all-clear; the range stays owed.
**Owner action (review-6 of run-20261106-062909, F2):** the reviewer spawn at `ralph-pi.sh:1081-1090` runs through `PI_CMD`, which the same uncommitted edit defaults to `/mnt/nas/Development/qwen-slot-pi.sh` — a gate whose only stated purpose is the two local qwen KV slots, bypassing prompts under 4000 chars. A review prompt is ~10.8 KB, so every hosted DeepSeek review holds a builder-class slot for its whole duration (measured 16 min 04 s, iter=5: invoked 06:29:17, collected 06:45:21), and with `QWEN_SLOTS=2` across three loops a review can starve a builder for up to `QWEN_SLOT_WAIT`. Fix: add `QWEN_SLOT_BYPASS=1` to that spawn's `env` list (or give the review arm its own `PI_CMD`). The builder cannot land it: the `PI_CMD` default line and the spawn block are both inside the operator's uncommitted hunk.
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
    `player.died()`, draws Kurt after the level, refreshes the solid arena per step, and (batch 75) clears
    the right colour. Narratives "Iteration 101"…"Iteration 104".
5b. **OPEN — the HUD half of item 5.** Blocked on a named cause, not on effort:
   `mdk_render_canvas_quad` is a no-op on this backend (`renderer_c3d.cpp:1854`), so a HUD call would draw
   nothing while reading as a ported HUD. The canvas seam has to land first.
6a. **DONE (79) — the sample plane.** `mdk_core/src/audio/sound.{h,cpp}`, `limiter.{h,cpp}` and the mix half of
    `sound_device.{h,cpp}`: PCM decode of both widths, interpolation, the DirectSound pan law, the music
    low-pass, the bus/master gains, the limiter, and a `Sink` seam (NULL = the oracle's `Output.Muted`).
    Pinned bit-for-bit by two new goldens. Narrative "Iteration 107".
6b. **DONE (80, corrected by 81/82) — the sinks and the mixer.** `audio_sdl.cpp` (SDL F32LE stream) and
    `audio_3ds.cpp` (ndsp chn0, stereo PCM16 44100, 4 x 20 ms linear wave buffers, float→PCM16 at the sink,
    the drained-channel clear+reset the vault's Forsaken finding warns about). The seam grew the C#'s second
    branch: a sink that can answer `queued` gets `2646 - queued`, one that cannot keeps the golden's `owed`
    accounting (`PASS AudioSinkSeam`). **No audio worker thread exists** — the sink has nothing to read, so
    R1's audio row is satisfied by `vfpThreadFzDn()` at the audio entry with its read-back logged. **The
    memory question is answered: `preload()` is not called on the 3DS**; the bank decodes on first use (level
    3 = 12.34 MB, level 4 = 15.03 MB, level 8 = 19.08 MB of float against 16 MB of FCRAM, so a full decode
    was never possible) — which bounds nothing, see NEXT. Its `plays=1` emulator evidence was an over-claim
    corrected twice; the ledger line is the evidence now. Narratives "Iteration 108"–"Iteration 110".
6c. **OPEN — the sample store is still float, and the bank still has no bound.** The follow-up 6b names:
   an int16 sample store halves all three numbers with **no change to a single sample value** (both PCM scales
   are powers of two: `(byte-128)*256 * (1/32768)` is the same float as `(byte-128)*(1/128)`, and the 16-bit
   path is already exact), at the cost of one `vcvt` per sample read in the mix loop — which needs its own
   pin and a re-run of `golden/L3_audio_mix.txt` to prove the claim. Budget-and-evict is the alternative.
6d. **OPEN — the 3DS frontend has no `ScriptRuntime`, so script sounds cannot play.** The mixer on the console
   is standalone and carries Kurt's own set (footsteps, chute, guns, `LAND` — measured). Ambient and object
   sounds come from opcodes and need the runtime wired on the console, which is the same missing piece as the
   object pass and the camera's script inputs.
7. **DONE (83, evidence corrected by 84) — Citro3D alpha blend** (owner directive item **6** — this file's
   own numbering drifted, review F6). The blend state is installed per pass from the oracle, and the shader's
   index-0 discard is the PICA's alpha test. Batch 84 corrected the *evidence*, not the change: it is verified
   as the oracle's state and as AGENTS P2 compliance, and is **not** verified as a visible fix — it moves 0 px
   of the panel, because every fragment there has alpha 255.
8. **CLOSED (batch 75) — the red clear.** It was the clear colour's byte order, not the render path. See the
   header of this file.

*(Items 1-8 are closed work and each points at its narrative. This file is over its ~150-line budget and stays
so deliberately: the owner-action block and the standing-facts list are the parts a batch must not lose, and
the closed items above are the only cheap material. Trimming further is an owner edit, not a batch.)*

## NEXT (batch 90)
**The one-sided view, still first and still unadvanced.** Geometry covers panel columns **200..399** of the 400-wide top
screen — the right half — and the live question is why coverage stops at a straight
vertical line at the panel's middle. The census instrument is honest (header: 58464
exact clear pixels in BOTH builds, `GPU_CULL_NONE` 69 px, depth-off 201 px, and its own
mapping probe fails, so it cannot localise), so do not re-run it first and do not
re-derive what Iteration 106 retired (the cull, the depth triple, the queue, the MVP, a
deleted rotation). The next useful capture is **census and `GPU_PRIMARY_COLOR` in the
same run**, cropped with `tools/panel_crop.py` so the comparison is frame-identical.
Batches 88 and 89 both took sanctioned alternatives instead of this, so there is no
cheap item queued behind it: if the view cannot be localised with a new hypothesis,
**say so in the batch** rather than shipping a sixth measurement with the same
instrument. What batch 89 brings to it is a habit, not a datum — review-8 F1 is a
unit that was "corrected" by rounding to the number that made the measurement fit,
and "the census box covers the panel" is the same shape of claim.
Item 5b (the HUD) stays blocked on the same canvas seam (`mdk_render_canvas_quad`
is a no-op on this backend). The second constraint batch 85 wrote onto it is
**downgraded, not carried** (review-5 F5): the frame-wide alpha test and the
oracle's `Pass.Overlay` blending are equivalent for an alpha-0 texel — Overlay is
`SRC_ALPHA / ONE_MINUS_SRC_ALPHA` (`Renderer.cs:565,571-573`), which leaves the
target exactly as a discard does — and a partial alpha is above the `> 0`
reference, so it survives the test. The canvas batch must NOT go scoping the
alpha test per pass on the strength of the old note; it only has to re-read
`renderer_c3d.cpp`'s corner (3) if it ever puts a canvas draw on a pass that is
**unblended or depth-writing** — review-6 F3: those are two independent triggers, not one
conjunction, and an unblended canvas pass diverges on colour alone.

**Audio, in priority order after the renderer items.** (a) 6c — the int16 sample store, which halves the
bank's footprint with no change to a sample value, pinned by a re-run of `golden/L3_audio_mix.txt`. (b) 6d —
the console has no `ScriptRuntime`, so ambient and object sounds cannot play; only Kurt's own set exists.
(c) the bank still has no eviction bound (6b). (d) the ndsp sink's `nsamples` unit is *measured* in the
emulator's HLE DSP and the frame counter's **kind** is *derived*, but its **rate is not known**:
`NDSP_SAMPLE_RATE` is 32728.5 Hz from the headers, the 160-sample quantum is an assumption, and the
emulator's 204.71/s fits neither that nor a power-of-two 32768 Hz (review-8 F1). The Old-3DS check stays
open (AGENTS P6/P8). What to read on hardware is the ledger line's `submitted` against
`secs x 50` — the seconds the mixer was actually given, printed on the line since
batch 88 — and **not** `done` against `submitted`, which is an identity and proves
nothing (batch 82, review F1). The older `frame x 735/882` form of that division is
the same number only at exactly 60 fps (review-2 F3), and it **over**-reads below 60
fps, not under (review-8 F3). Reading `dspframes` off a hardware run is a liveness
check only: no installed header names the frame quantum, so it cannot be turned into
a rate without a firmware-level timestamp.

**Deferred review findings:** review-4 of run-20261105-142911 F1/F2 (the geometric solid-arena pin stands in for the unported
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
  == inDsp <= 4` holds by construction in every state, healthy or broken — and only while
  the sink is OPEN (review-2 F5: `audioClose()` zeroes `inDsp` and not the other two), so
  read it behind `audioIsOpen()`. No reading of `done / submitted` means anything — batch
  81's broken build measured 0.996. The rate is `submitted / (secs x 50)`, the seconds the
  mixer was actually given, which the ledger line prints as `secs=` since batch 88; the
  older `frame x 735/882` form is the same number only at exactly 60 fps (review-2 F3), and
  a half-rate sink at 30 fps scores 1.00 against it. The three measured builds give 0.002
  (nothing queued), 0.502 (the wrong `nsamples`) and 1.003 (correct). `dropped` counts
  rejected FILLS, not buffers, and `inDsp == 4` with `dropped` climbing means only that
  NOTHING RETIRED — the run where ndsp accepted nothing read `submitted=4 done=0
  dropped=2579 inDsp=4` (review-2 F4). Pinned by `ndsp_sink_test.sh`'s `ledger-balances`,
  `counts-done-at-the-sink-recheck`, `frontend-logs-the-clock-with-the-ledger`.
* **`ndspGetFrameCount()` counts ndsp MIXER frames, not samples (batch 88, review-2 F2; rate corrected by batch 89, review-8 F1).**
  Derived from the linked `libctru.a`: `.bss.frameCount` is zeroed by `ndspInitialize`,
  gets exactly one `add` per `ndspThreadMain` iteration after the `svcSignalEvent`
  that kicks the DSP, and `ndspGetFrameCount` is its only reader — that is the
  counter's KIND, and it is proof. The **rate is not**: `NDSP_SAMPLE_RATE =
  SYSCLOCK_SOC/512 = 16756991/512 = 32728.5 Hz` (`os.h:9`, `ndsp/ndsp.h:9`) and has
  nothing to do with this channel's 44100, but the DSP frame quantum is named by
  nothing installed, so a 160-sample frame predicts 204.55/s while the emulator reads
  204.75/s at 19 s and 204.71/s at 30 s — a window that excludes both that and a
  power-of-two 32768/160 = 204.80. **It is a liveness signal, not a rate, and the
  console's mixer rate is unmeasured.** Batch 88 printed 32768 Hz because it made the
  measurement fit; the claim it replaced ("240-sample blocks at 48 kHz") fitted the
  same magnitude at 200/s. A magnitude must never be accepted as a mechanism, and
  rounding a constant to reach one is the same error wearing a derivation.
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
  **Use `python3 tools/panel_crop.py` for that crop** — it is the only place the
  rectangle is written down, it refuses to write or compare a crop that is not
  exactly 400x240, and it is the fix for two consecutive batches shipping a 399x239
  crop under a sidecar that said 400x240 (review-5 F4).
* **A build's identity is `mdk.elf`, not `mdk.3dsx` (batch 86, review-5 F1).** The
  `.3dsx` is ELF + SMDH + RomFS and `3dsxtool` walks the staged RomFS in readdir
  order, so the container hash follows the filesystem it was staged on: the same
  tree gives `567a85a0…` from `/tmp` (ext4) and `34eff8e6…` from this NFS tree,
  with the same `mdk.elf` `73bfe104…`. Cite the ELF md5 in a sidecar; treat a
  `.3dsx` md5 as a container note, and never as proof that a capture came from a
  given commit.
* **`mdk::Pass`'s cardinality is compiler-owned (batch 85, corrected by 86).**
  `Pass::Count` is a sentinel and never a pass: `DrawQueue::add` clamps an
  out-of-range bucket to 0, so a `Count` passed as a pass would be *folded into
  `Solid`*, not rejected (review-5 F3),
  `mdk::kDrawQueuePassCount` is derived from it rather than hand-counted, and
  `pica_matrix_tests.cpp` static_asserts the enum's size, the sampled array's size
  and the opaque array's size. A seventh pass BEFORE the sentinel is a build
  failure, not a silently inherited `Solid` state — demonstrated by adding one and
  reading the three failures. It must stay the LAST enumerator: a member appended
  after it moves none of those asserts and neither build uses `-Werror`, so only a
  `-Wswitch` warning would notice (review-5 F6). The two exhaustive `switch`es over
  `Pass` (`pica_matrix.cpp`
  `depthStateForPass`, `tick1.cpp passName`) name `Count` on purpose so `-Wswitch`
  stays the backstop; do not "tidy" them into a `default`.
* **`BlendState::enable` is read by the renderer, and the disable is a named pair
  (batch 85, review-4 F6).** `installBlendState` installs
  `mdk::pica::kBlendReplace` — `(ONE, ZERO)` on both channels — for any state whose
  `enable` is false, whatever factors it carries, so the type cannot say "disabled"
  and mix anyway. It changes no register value any pass installs today, and the A/B
  says so: 0 px of the panel against a clean build of the parent
  (`docs/evidence/2026-11-06-L7-blend-enable-{parent,current}.txt`). Do not read
  that pair as evidence about `Pass::Blended`: the scene has none.
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
