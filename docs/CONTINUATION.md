# Phase 2 continuation checkpoint

Risk: high — **memory safety.** `Mve::decodeAudio` wrote a predictor past the end of its own
sample array when a compressed audio frame declared less than `2 * channels_` bytes, and this
batch adds the bounds guard that makes it `FrameStatus::Broken` (the C#'s `IndexOutOfRange`
answer, per `mve.h`). It also widens a sanitizer gate: `tests/scripts/ubsan_decode_test.sh` now
compiles `-fsanitize=undefined,address`, because the two shapes of this one bug need two
instruments and UBSan alone walks over the stereo one. Both halves were proven to fire, each on
the shape only it can see. Eight new tests cover the compressed path for the first time. The
shipped binary is nevertheless byte-identical — `mdk.elf` md5 `c8295e96663ca33be481df2e77613e46`,
`.text 0x057ba0`, unchanged from batches 95/96 — because **no `Mve` symbol is linked into the ELF
at all** (`nm | grep -ci Mve` → 0; `mdk.map` shows `build/mve.o (__aeabi_idiv)`), so the defect
is real, pinned, and not reachable in today's build.

**Review-3 (run-20261108-195620) closed by batch 97: five findings, no Critical, one High — two
fixed, three owner-only, none rebutted on merit** (Iteration 125). Its open question 2 was the
batch: the compressed (DPCM) half of the MVE audio decode and `SilenceFrame` had never been
executed by a test, and **the first time anything executed that branch it found a
heap-buffer-overflow** — `count = size / 2` sizes the sample array, and the predictor-seed loop
fills `channels_` slots before any loop condition consults `count`, so a frame declaring less
than `2 * channels_` bytes wrote past the end; the guard that was there, `if (count < 0)`, could
not fire because `size` is a `u16` and `count` is therefore never negative. Two shapes, two
instruments, each measured: mono/declared-1 segfaults the plain suite (exit 139) and trips UBSan
(`reference binding to null pointer of type 'value_type'`), while stereo/declared-2 is silent in
both and only AddressSanitizer sees it (`heap-buffer-overflow … WRITE of size 4`) — which is why
the sanitizer script now runs both. Seven mutations were run: six redden a value assertion and
name themselves (a delta entry, the clamp bound, the alternation start, the silence divisor, the
version rule, seeding only channel 0) and the seventh is the guard itself, which reports by
crashing and by sanitizer report rather than by a FAIL line. **F2 (Low)** was the fixture's
comment claiming the init-audio flags were the upper half of a frame-time `u32` — opcode `0x03`
reads `U16(p+2)`/`U16(p+4)` and never touches `+0`; the `u32` belongs to `0x02`, so those two
bytes are unread padding, and the comment now says so. **F3 (Low)** was a struck verdict still
standing in prose: "a fully decoded level 8 **fits**" survived in the batch-95 tail at `:32`, and
the same verb was also still at `:7`, which the reviewer did not name; both now read *bank* with
the "nothing measures the level's own cost" clause. **F1 (High)** is the watchdog heal, confirmed
live and owner-only as before (root-owned installed copy, no commit contains the hunk). **F4**
(the directive's false fleet-wide chain claim) and **F5** (`test_review_state.sh`'s mode bit) are
inside the operator's uncommitted set and a file this uid cannot `chmod`; both confirmed, both
untouched. **And review-3's open question 1 is answered:** the harness set is operator-authored,
this batch staged its paths **by name**, and `ralph/builder.md` was not committed because it is
`PROTECTED` and `ralph-pi.sh` runs the integrity gate without `--owner-approved`, so staging it
would stop the loop at exit 6 rather than warn.


**Review-2 (run-20261108-195620) closed by batch 96: nine findings, no Critical, one High — eight dispositioned here, F1 confirmed and escalated owner-only** (Iteration 124). **F2 (Medium)** was the **ninth** could-not-see clause: batch 95 replaced the guard walk's line-count bound with a structural one and left a second bound in the same function — `HEADER_STOP = r"[;}:]\s*$"` stops the walk at any line ending in `;`, and a **wrapped `for` header** puts exactly that on a continuation line, so `for (int32_t extra = 0;` read as "the statement above finished", and a clock accumulated three times per frame adjudicated PASS behind a three-line header while the one-line spelling was red. A terminator now only terminates at **paren depth 0** (`paren_depth`, measured from the walk's existing `floor`), which is a fact about where a `;` can end a statement and not another budget on formatting; **33 arms red, 9 probes green**. **F4 (Low)** was that batch 95 fixed the same UB in **two** decodes and pinned only one — nothing in `tests/` could reach `Mve::decodePcm`. `tests/scripts/mve_audio_tests.cpp` now builds its own one-chunk movie (the C# twin loads the real `MDKBZK.MVE` from the gitignored tree, which the oracle boundary forbids and which would skip without the tree) and asserts the oracle's arithmetic over all 256 byte values, the stereo pair order and the 16-bit branch; it caught two of this batch's own errors on its first run. **And the class a value test cannot pin is now committed**: the multiply and the shift emit the same `lsl #8`, so batch 95's UBSan run — done in `/tmp`, never committed, the asserted-provenance shape again — is replaced by `tests/scripts/ubsan_decode_test.sh` on the `test` target, with both reversions measured (`mve.cpp:319`, `sound.cpp:67`, each `left shift of negative value -128`). **F1 (High) is confirmed and NOT fixed** — see the owner-action block: it is live on the host and no batch can land it. **F3/F5/F6/F8/F9** were the record again: a fit verdict drawn from a measurement of a different quantity (the **bank** fits; nothing measures the level's own cost), a header comment still calling the pre-scale a shift, a fourth "16 MB of FCRAM", a duplicated `HEADER_STOP`, and a SPEC quotation that dropped the inner quotes around `"system"`. **F7** is immutable by design and carried forward in this batch's sidecar.

**Review-1 (run-20261108-195620) closed by batch 95: eight findings, no Critical, no High, none rebutted on merit — all eight fixed** (Iteration 123). Its scope `96cfe849..5bf0c61c` also covered `96cfe849..953369da` (batch 93), so the range review-13's timed-out dispatch owed is now covered and no longer owed. **F1 (Medium)** was the **eighth** could-not-see clause in a row and it struck this file's own fix: batch 93's guard walk was capped at `HEADER_WALK = 6` non-blank lines, so a braceless `if` whose condition wrapped over **seven** lines — the reviewer's mutant, reproduced — adjudicated PASS, i.e. the hole reopened one wrapped line further out, for the third batch running. The cap is **deleted**, not raised: the walk's bound is now the `{` of the block that owns the statement (already computed by `guard_of`), which is a structural fact about where a guard can live and not a budget on how long a condition may be. Seven correct-code probes are **committed** this time (batch 93 ran its four by hand and lost them with the terminal), and one of them is F5's. **30 arms red, 7 probes green.** **F2 (Medium)** is the UB above, and it was in **two** decodes: `mve.cpp:311` had the same shift, fixed the same way, deliberately not factored into a shared helper because one constant does not justify a `formats → audio` dependency. **F3 (Medium)** struck a false premise this file has been repeating: **there is no 16 MB of FCRAM in this contract** — the built ELF's crt0 header asks for `__heap_size = 0x01800000` (**24 MiB**) and `__linear_heap_size = 0x02000000` (32 MiB), and libctru `os.h` gives `OS_OLD_FCRAM_SIZE = 0x8000000` (**128 MiB**, 256 MiB New). A fully decoded level 8 **bank** (15.82 MB) therefore **fits** the request with 9.34 MB left **for** the level, so the eviction item is about unbounded growth and not about a ceiling that does not exist — and that remainder is a budget and not a verdict, because nothing in the tree measures the level's own cost (review-2 F3); the places that said 16 MB are corrected below. **F4** was a branch no test reached (every shipped sound is mono), which is exactly how a signed `frame * 2` survived in it. **F5** was an alias regex whose `\s*` matched a newline, so a correct wrapped condition that merely *named* the clock read as an address-taken alias — a gate redding correct code, which is the failure mode this file keeps naming. **F6/F7/F8** were a pin back-dated two days before the code existed, a cost comment that omitted the `vmov` its own contract prices, and `15.83` where `6,469,952 + 5,821,432 = 15,823,516` is `15.82`.

**Review-5 (run-20261106-062909) closed: seven findings, none rebutted** (Iteration 114). Its two durable rules are standing facts below — **a build's identity is `mdk.elf`, never `mdk.3dsx`**, and **the panel crop is `tools/panel_crop.py`**. Its F2 was the second could-not-fail check in a row; its F3 struck a false mechanism (`DrawQueue::add` clamps an out-of-range bucket to 0, so a seventh pass folds into `Solid`).

**Review-6 (same run) closed: four findings, none rebutted, three fixed, one owner-only** (Iteration 115). Its F1 was the loop's own record lying about itself, and batch 87 annotates such blocks **in place**: an append-only log is corrected by annotation, never by rewriting, because the audit trail is what made the error findable. Its F3 is a standing fact below (the alpha-test corner's **two independent triggers**). F2 is owner-only (the reviewer holds a qwen slot).

**Review-2 (same run) F2–F5 closed by batch 88, then corrected by batch 89** (Iterations 116, 117). **F2**'s `dspframes` unit was wrong twice over ("240-sample blocks at 48 kHz"; ndsp is neither); the unit is **derived** from a disassembly of `libctru.a` (`ndspThreadMain` adds 1 to `.bss.frameCount` once per iteration, after the `svcSignalEvent` that kicks the DSP), and the DSP rate is `NDSP_SAMPLE_RATE = SYSCLOCK_SOC/512 = 32728.5 Hz` — batch 88 printed 32768 and review-8 F1 struck it. **F3**'s denominator was a frame count standing in for a clock, exact only at 60 fps, so the frontend accumulates the `elapsed` the mixer was actually given and prints `secs=` on the ledger line: the test is `submitted/(secs x 50)`, measured 0.9967 at frame 1800 with the guest at 59.64 fps. **F4**: `inDsp == 4` with `dropped` climbing means *nothing retired*, not *the queue is jammed* — the pre-`status = FREE` run read `submitted=4 done=0 dropped=2579 inDsp=4` with ndsp having taken nothing. **F5**: `submitted - done == inDsp` is an **open-sink** identity (`audioClose()` zeroes `inDsp` and not the other two).

**Review-8 (same run) closed by batch 89: six findings, none rebutted, all six fixed — one of them in four of its five named files.** Its High was that batch 88's corrected unit was itself wrong: `SYSCLOCK_SOC` is `16756991u` (`os.h:9`) and `NDSP_SAMPLE_RATE` is `(SYSCLOCK_SOC / 512.0)` (`ndsp/ndsp.h:9`), so the DSP runs at **32728.5 Hz**, the value Iteration 111 had right before batch 88 "corrected" it to a power of two. The re-measurement this batch took (a longer ledger window, frame 1800) shows the counter is **not a rate instrument at all**: 204.71/s at 30 s, bounded to (204.67, 204.74] by the 2-dp clock, which excludes both 32728.5/160 = 204.55 and 32768/160 = 204.80 — so the emulator confirms neither model, the 160-sample quantum is an assumption nothing installed names, and the console's mixer rate stays unmeasured. **[Superseded by review-9 F2 / batch 90: the exclusion is conditional on `secs` being exact, and a second run of a byte-identical ELF reads 204.628/s, so the residual is run jitter and excludes nothing. The conclusion stands; the argument for it does not.]** Its F2 was the third could-not-fail check in a row (the `secs=` check pinned the field's presence, not its value: printing `(double)delta` passed all 16 checks); the check is now structural and has an arm for exactly that substitution. Its F3 was an inverted comparison row (`501/500 = 1.0020`, not `0.9980`), which reversed the direction of its own argument. F4/F5/F6 were the gate header still stating the superseded rate rule, a `main_3ds.cpp:439` citation that is now `:449`, and a disassembly immediate (`#340` → `#260`). Narrative Iteration 117.

**Review-9 (same run) closed by batch 90: five findings, no Critical, no High, none rebutted on merit — three fixed, one impossible for a batch, two owner-only** (Iteration 118). Its Medium was the **fourth consecutive could-not-fail check**: the `secs=` check's third clause matched `audioClock\s*\+=\s*elapsed` as a **prefix**, so `audioClock += elapsed * 0.5f;` and `audioClock += elapsed; audioClock *= 0.999;` both adjudicated PASS — a ledger clock at half speed printed a rate of 0.5x and the gate certified it. The clause is now structural: between the declaration and the ledger printf `audioClock` must occur **exactly once**, as the bare statement `audioClock += elapsed;`, and two arms perform both substitutions (**16 checks, 14 arms**, all red). Its F2 was a shipped comment that presented an exclusion as a derived fact and then named the error source that voids it — and a second emulator run of the same ELF proves the reviewer right: 204.628/s at 43.22 s against batch 89's 204.705/s at 30.18 s, a **0.078/s run-to-run spread** that is larger than the residual being used as an exclusion. The counter is a liveness signal because **nothing installed names the frame quantum**, not because of that arithmetic. Its F3 (a sidecar with no "why the picture did not move") cannot be remediated as asked: `gates_batch_integrity.py` makes historical frame `.txt` files immutable and is itself `PROTECTED`, so the gap in batch 89's record is permanent and the convention is carried forward instead. F4/F5 are confirmed against the files and are owner-only (the uncommitted watchdog and `guard()` comment).

**Review-10 (same run) closed by batch 91: eight findings, no Critical, one High, none rebutted on merit — four fixed, four owner-only** (Iteration 119). Its **F1 (High)** falsified batch 90's own falsification: a third run of the byte-identical ELF reproduces **both** cited rows out of **one** trajectory (3921@19.15 s, 6178@30.18 s, 8844@43.22 s), so there was never a 0.078/s run-to-run spread — the instrument is deterministic frame for frame and the leftover is a constant ~20 ms **start offset** that a single-row `dpf/secs` cannot separate from a rate error. **[Superseded in its strong form by batch 92: a THIRD run of the byte-identical ELF reads `dspframes` one lower on 25 of 48 rows, so the counter is reproducible to a frame and not frame for frame. What survives is the slope estimator, now with three samples — 204.544 / 204.562 / 204.566/s, a 0.021/s spread that is exactly ±1 count over 47.134 s, the estimator's own quantum. The header model sits inside it (−0.009..+0.013/s) and a power-of-two sits 0.234–0.256/s outside it, so the quantum conclusion is stronger than before and its error bar is named.]** `secs` now prints at 3 dp because the 2-dp rounding (±0.043/s per slope) was the same size as that scatter. **F2** compared a percentage with a per-second rate (0.074 % of 204.553 is 0.152/s, twice the 0.078/s it was called small against); **F3** was a false comparative contradicted inside the same document set — its sidecar half cannot be fixed, see the immutability item below. **F4** was the **fifth** could-not-fail clause in a row: the clock check's window stopped at the printf's argument, so a decay placed *after* the ledger printf adjudicated PASS, and "the same elapsed the device got" was prose, so feeding the device `elapsed * 0.5f` adjudicated PASS too; the window is now the whole function body, the count is of *writes* (the printf's argument is a read), and a new check pins that the one `audioClock += X;` and both `mixer.update(X)` / `mixer.device().update(X)` calls name the same identifier. **F5–F8** are the operator's uncommitted harness set (below).

**Review-11 (same run) closed by batch 92: five findings, no Critical, no High, none rebutted on merit — all five fixed** **[Superseded in its closure claim by review-12 R11-1 / batch 93: four of the five closed as claimed; R10-2's guard class was closed for two of its three brace styles and the braceless and Allman forms stayed green until batch 93.]** (Iteration 120). **F1 (Medium)** was the **sixth** could-not-fail clause in a row: the write count only sees a write that spells `audioClock`, so `{ double& ac = audioClock; ac *= 0.999; }` — a ledger clock decaying 0.1 % per frame — adjudicated PASS, reproduced before the fix. The gate now forbids the alias itself (`&audioClock` and `T& x = audioClock`), which is what makes the single-write rule a bound on the value and not on one spelling of it. **F2 (Medium)**: a call count cannot tell an unconditional feed from a guarded one, so `if (frame & 1u) mixer.device().update(elapsed);` adjudicated PASS while the scaled-feed mutant was caught. The three statements (the accumulate and both feeds) must now each own their line, and `guard_of()` walks back to the enclosing block's opener so the block form `if (c) { audioClock += elapsed; }` — which owns its line and was a hole the reviewer's mutant did not reach — is caught too; the frame loop is whitelisted by name so the check cannot red correct code. **F3** struck "drifts monotonically by −0.059 s" (it rises on 9 of 47 steps on one run, 32 of 47 on another; the drift stands). **F4** struck "two runs of this build" (one run was the parent ELF at 2 dp) and the re-measurement that checked it produced F1's falsification above. **F5** struck "the next integer is 0.6/s away": 0.6/s is the distance to the 160.5 **halfway point** (32728.5/160.5 = 203.916/s ⇒ 0.63/s, not the review's 203.88/0.66), while the nearest other integer quantum 161 is **1.26/s** away.

**Review-12 (same run) closed by batch 93: five findings, no Critical, no High, none rebutted on merit — four fixed, one owner-only** (Iteration 121). Its **F1 (Medium)** was the **seventh** could-not-see clause in a row: batch 92's guard test read a guard from the statement's own line and from the opener of its enclosing block, so a **braceless** `if (c)` with the body below it and the same guard in **Allman** style — where the `{` sits alone and the keyword is one line higher — both adjudicated PASS, reproduced with the reviewer's mutants and two more of this batch's own (a braceless mixer feed, and a guard whose condition wraps). The guard is now read from **around** the statement: its own line's prefix, a walk up from the statement, the enclosing block's opener, and a walk up from the brace when that opener is alone. The walk stops at `;`, `}` or `:` — deliberately the **negative** of the suggested "must end in `{`/`}`/`;`", because the positive form reds a statement after a `case`/`default`/`public:` label, and because that predicate as written would not have closed this finding anyway: in Allman style the line above the statement *is* the `{`. Four correct-code probes stay green (accumulate first in the frame loop, the frame loop itself in Allman style, a plain Allman block, a `default:` label) and a comment between a guard and its body stays red, because `code()` blanks comments. **F2/F3** were digits that did not recompute (`32728.5/204.562 = 159.993`, not the `160.001` printed; `1.26/0.021 = 60`, not the `25x` printed against a scatter the same paragraph had just defined as 0.021/s); the quanta are now printed **as divisions by the slope they come from**. **F4** was batch 92 writing "Four new arms" and listing six — annotated in place, per the append-only convention. **F5** is owner-only and untouched.

**Two historical corrections that stay load-bearing.** Batch 83's headline measurement was taken against a build that is not its parent (batch 84's retraction stands), and with review-5 F1 the pattern was three consecutive batches trusting a number whose provenance was asserted rather than reproduced — what is different now is that anyone can reproduce both halves with `git archive`/`git ls-files` + `make` in a throwaway tree, and the identity field is the ELF — and a `.3dsx` md5 is not even reproducible from the tree that built it (batch 90: two consecutive in-tree builds gave `48931bcd…` then `23c5354f…` from one byte-identical `mdk.elf`). And **the census was never inflated, so do not re-run it**: the exact clear-colour pixels `(16,32,48)` of the 400x240 panel are **58464 in the parent build and 58464 in this one**, and that equality is the whole argument (review-4 F8: do not restate it as "the 61 % figure stands" — the census's 61 % was 58522 near-clear pixels of a 95361-px panel at a different frame with a different tolerance, a different quantity). The old "re-census first" instruction stays withdrawn. Coverage is stated in panel columns (review-3 F4): geometry covers panel columns **200..399** of 400, the right half of the top screen.

Last updated: 2026-11-09 (**batch 97 — review-3 of run-20261108-195620 closed, 2 fixed and 3
owner-only: covering the compressed MVE audio path for the first time found a
**heap-buffer-overflow** in `Mve::decodeAudio` — the predictor-seed loop filled `channels_` slots
before any loop condition consulted `count = size / 2`, so a frame declaring less than
`2 * channels_` bytes wrote past its sample array, and the guard that existed (`count < 0`) could
not fire because `size` is a `u16`. Now `count < channels_` answers `FrameStatus::Broken`, which
is the C#'s `IndexOutOfRange` answer, and the sanitizer gate runs UBSan **and** ASan because the
mono shape is a null reference and the stereo shape is invisible to UBSan. `mdk.elf` is
byte-identical to batch 95's and 96's — `c8295e96…`, `.text` `0x057ba0` — for the gc-sections
reason in the risk line above, not because nothing was compiled.** Superseded tail of the batch-96
line: review-2 closed, 8 of 9 dispositioned and F1 escalated: the guard walk's stop test read a
wrapped `for` header's `;` as a statement terminator, so a clock accumulated three times per
frame adjudicated PASS; a terminator now only terminates at paren depth 0. The MVE half of batch
95's UB fix got the test it never had, and the sanitizer run batch 95 asserted in `/tmp` is now a
committed gate that catches both reversions.** Superseded tail of the batch-95 line: review-1 of run-20261108-195620 closed, 8/8: the 8-bit decode's signed left shift was undefined behaviour and is now `* 256`, the stereo pair is formed in `size_t` and is executed by a test for the first time, and this file's own "16 MB of FCRAM" premise was false — the crt0 header asks for a 24 MiB heap and libctru says 128 MiB of FCRAM, so a fully decoded level 8 **bank** fits with 9.34 MB left for the level — nothing measures the level's own cost — and the eviction item is about growth, not a ceiling. `mdk.elf` `5437e57c…` → `c8295e96…`, `.text` +48 B from `sample()` inlining**).

**The audio-docs batch is DONE (88), its first correction is DONE (89), its second is DONE (90), its third is DONE (91), its fourth is DONE (92), and its fifth is DONE (93).** The corrections are in `mdk_3ds/src/audio/audio_3ds.{h,cpp}` and `mdk_3ds/src/main_3ds.cpp`; the text of each is `docs/PROGRESS_LOG.md` Iterations 116–121. Two durable facts are in the standing list below: **the ndsp frame counter counts mixer frames, not samples**, and **its rate is measured in the emulator (204.544 twice, 204.562, 204.566/s ⇒ a 160-sample frame at 32728.5 Hz) by a SLOPE between two ledger rows, never by one row's `dpf/secs`, which carries a constant ~20 ms start offset; the slope's error bar is ±0.02/s and comes from TWO sources — a ±1 dspframe count and a 4 ms movement of the measured window — while the console's rate stays unmeasured**.


**Owner action (review-2 F1 of run-20261108-195620, HIGH, CONFIRMED, LIVE, NOT LANDABLE BY A BATCH):** the
uncommitted `tools/ralph_pi_watchdog.sh` heals **any** unit in `ActiveState=failed` with
`systemctl reset-failed` + `systemctl start` every 5 minutes, which inverts the shipped unit's
`RestartPreventExitStatus=2 6 7` (`tools/install_ralph_pi_service.sh:75`) — the loop's deliberate
fail-closed stops: 2 = another loop alive (`ralph-pi.sh:355`, `:479`), 6 = `:805`, `:855`, 7 = reviews
lost without a verdict (`:621`, `:688`, `:695`, `:1247`) — and `reset-failed` clears the start-rate
counters, so `StartLimitBurst=3`/`StartLimitIntervalSec=30min` never trips and a stop becomes an
unbounded 5-minute restart storm. Three facts a batch cannot act on: (1) **no commit contains it**
(`git show HEAD:tools/ralph_pi_watchdog.sh` has no heal block); (2) **it is live anyway** —
`/usr/local/libexec/ralph-pi-mdk-watchdog` is md5-identical to the worktree file (`241add59…`), so the
installer has already been re-run and the timer is executing it now; (3) **a repo-only fix cannot take
effect**, because the installer does `install -D -o root -g root -m 0755 … "$WATCHDOG_BIN"`, so what
systemd runs is a root-owned copy. And a batch may not stage the file: the heal lines are new lines
*inside* the operator's uncommitted hunk, so editing them is inseparable from committing that hunk (the
laundering review-4 F2 / review-10 F8 forbid). The vault already prescribes the correct rule —
`50-tooling/ralph-pi-supervision-and-wedge-prevention.md:237`, "heal `failed` (**skip the unit's
fail-closed codes**; skip `ExecMainStatus=0`)" — and the block implements that sentence with the
parenthetical dropped. The patch is in `docs/PROGRESS_LOG.md` Iteration 124: gate the heal on
`systemctl show -p ExecMainStatus --value` against the prevent list (or on `Result` being
`core-dump`/`signal`/`timeout`) and do not `reset-failed` on a prevented exit.
**Owner action (review F2, new):** `ralph/builder.md`'s "Search scope and bounded commands" block forbids sibling reads, while `AGENTS.md` §12 requires them ("**Proven 3DS platform code in sibling ports — adapt, do not re-derive**", naming `../forsaken/sound_3ds.c`, `../t2k`'s ndsp audio, `../perfect_dark/tools/emurun.sh`) and the 2026-11-05 directive names `../forsaken/sound_3ds.c` for this subsystem — it is where the `nsamples` convention came from. One of the two must be amended by the owner; the builder may not edit either. Combined with the item below about the same uncommitted file.
**Owner action (review F3, from run-20261106-035708):** `SPEC.md:809` says the 3DS "voice pool shrinks to 24 and the queue to 40 ms to fit the 3DS's DSP budget". The port ships the oracle's 64 (`src/Mdk.Engine/Audio/AudioDevice.cs:73 MaxVoices = 64`, no platform branch) and an 80 ms queue, and `audio_3ds.h` reads §8's 24 as the software mixer's knob. That reading is a deviation from a protected document and the builder cannot resolve it: either amend SPEC.md §8 or authorise a per-target cap. Unmeasured either way — the hot-path corpus is empty, and nothing in the current build exceeds a handful of voices.
**Owner action:** `ralph-pi.sh` worktree mode is 664 (index 100755); this uid cannot chmod it, so systemd restart fails `203/EXEC`. Owner task 18: restore 0755 and restart `ralph-pi-mdk.service`.
**Owner action (review-4 F3 / review-2 F6):** uncommitted operator edits in `ralph/builder.md` (PROTECTED) plus routing changes in `ralph-pi.sh`, `ralph/LOOP_CONTEXT.md`, and `tools/runner/test_review_state.sh` conflict with the committed owner directive. Owner must approve/commit or revert; builder must not stage these or set `MDK_OWNER_APPROVED=1`. Stage batch paths by name only. **Attribution, which review-4's open question 1 asked for: these six files are operator-authored, not builder work.** The content is harness policy no batch writes — reviewer/worker chains, `ITER_TIMEOUT`/`REVIEW_TIMEOUT`, `PI_CMD=/mnt/nas/Development/qwen-slot-pi.sh`, the watchdog's phase limits, and a "Search scope" section in the builder's own contract; batches 83, 84 and 85 each staged none of them, and batch 85 staged its paths by name and did not use the add-all commit path. So review-4 F2 is documentation hygiene plus a gate gap, not a protected-contract breach.
**Owner action (review-4 of run-20261106-062909, F2):** `tools/check.sh:175` runs `gates_batch_integrity.py` with neither `--staged` nor `--base`, so a PROTECTED edit sitting in the worktree or the index is invisible to the loop's own gate and an add-all commit lands it first and fails the gate second. The two files that would carry the fix — `tools/check.sh` and `tools/runner/gates_batch_integrity.py` — are BOTH in `PROTECTED`, so this is an owner edit: run the gate with `--staged` when the index is non-empty (or `--base HEAD`). Also restore `tools/runner/test_review_state.sh`'s mode bit (100755→100644 in the same uncommitted set; this uid does not own the file and cannot chmod it).
**Owner action (review-4 of run-20261106-062909, F3):** three texts and one sibling repository disagree on whether a same-family review exception exists, and only the owner can pick a winner. The uncommitted `docs/OWNER_DIRECTIVE_2026-11-05.md` says "the `-th` -> `flash` pair is the single operator-approved same-family exception"; `ralph-pi.sh` says "No independence exception appears anywhere in this file: none is needed, and one must not be added"; `tools/runner/test_review_state.sh` still pins `REJECT` for a same-family pair. The same edit also asserts `tetrisphere-godot` carries the same chains, which its own `tools/ralph-pi.sh:120` falsifies (`WORKER_MODELS="litellm/qwen litellm/luna"`). Either the directive loses that sentence or the exception becomes a code change in `reviewer_reject_reason` plus a rewritten test case — neither is a builder batch.
**Owner action (review-4 of run-20261106-062909, F4):** the uncommitted `ralph-pi.sh` `guard()` replaced `FATAL: pi write probe failed on every route / exit 3` with an unbounded `while :; … sleep 300`, and it rewrites `phase route-health` before every round, so the watchdog's `route-health|startup|verify) limit=900` arm compares against a refreshed epoch and can never fire: a loop with no live route parks silently, with `logs/ralph/disabled` already touched. The wait is the operator's decision (the file says so); bounding it is not builder-owned. Suggested: after N failed rounds write a phase no watchdog arm covers (e.g. `route-dead`) and stop refreshing the epoch on a failed round. **Relabelled per review-5 F7: this is an operator decision that `PROTECTED` does not enforce** — neither `ralph-pi.sh` nor `tools/ralph_pi_watchdog.sh` is in the `PROTECTED` set, so the boundary here is ownership, not a gate, and a reviewer should not re-derive it as a gate gap.
**Owner action (review-2 F3 / review-4 F3):** batch frames duplicate an older capture (`sha256 6646adf1…`) whenever a batch moves no pixels — batch 79's, 80's, 81's and 82's do, and each sidecar says so and names the measurement. Rejecting duplicates and requiring emulator verdicts needs owner-protected gate tooling; recorded, not changed.
**Owner action (review dispatch, second instance):** `logs/ralph-pi/run-20261106-062909/review-13.md` is a **failed dispatch** — its content is `REVIEWER FAILED model=litellm/deepseek-v4.1-flash-th rc=124`, its `.rc` is `124`, and it covers scope `96cfe849..953369da` (batch 93). A dispatched review that died is not an all-clear: that range stays OWED and the next mandatory review must be given the unreviewed range, not just the newest commit. Same root as the two items below (the reviewer spawn holds a qwen slot through `PI_CMD` until `REVIEW_TIMEOUT`), and the same reason it is owner-only: the lines are inside the uncommitted operator hunk. If the loop's own persistence already re-queued it, no action — but nothing in batch 94's tree can show that.
**Owner action (review dispatch):** `logs/ralph-pi/run-20261105-142911/review-6.md` is **0 bytes** — the reviewer was dispatched and returned nothing, and no `.rc` was written. That is a failed dispatch, not an all-clear; the range stays owed.
**Owner action (review-6 of run-20261106-062909, F2):** the reviewer spawn at `ralph-pi.sh:1081-1090` runs through `PI_CMD`, which the same uncommitted edit defaults to `/mnt/nas/Development/qwen-slot-pi.sh` — a gate whose only stated purpose is the two local qwen KV slots, bypassing prompts under 4000 chars. A review prompt is ~10.8 KB, so every hosted DeepSeek review holds a builder-class slot for its whole duration (measured 16 min 04 s, iter=5: invoked 06:29:17, collected 06:45:21), and with `QWEN_SLOTS=2` across three loops a review can starve a builder for up to `QWEN_SLOT_WAIT`. Fix: add `QWEN_SLOT_BYPASS=1` to that spawn's `env` list (or give the review arm its own `PI_CMD`). The builder cannot land it: the `PI_CMD` default line and the spawn block are both inside the operator's uncommitted hunk.
**Owner action (review-9 F4/F5, both confirmed against the files):** two statements in the operator's uncommitted harness edits are false as written, and both files are in the set this batch must not stage. (a) `tools/ralph_pi_watchdog.sh:15-21` claims every external limit sits ABOVE its in-process counterpart and that "14100/11400 (the previous values)" were equal to them; `git show HEAD:tools/ralph_pi_watchdog.sh` gives `worker) 7800` and `review-collect) 5700` — **tighter**, not equal — and `startup` is 1800 in-process against 900 out-of-process, so the stated invariant already fails for a phase in the same file (a legitimate 20-minute startup is restarted at 15). (b) `ralph-pi.sh:355-358` computes the `route-health` margin as "probe 300s + sleep 300s = 600s worst case", but the probe is `for model in $WORKER_MODELS` at `PROBE_TIMEOUT=300` each, so the worst case is `300n + 300` and `WORKER_MODELS` is env-overridable — at n ≥ 3 the round outlives the 900 s arm and the watchdog restarts the service mid-probe. Suggested: scope (a) to the phases it actually covers and state the real previous values; derive (b)'s sleep from the limit or rewrite the phase between probes rather than before the round.

**Owner action (review-10 F5, confirmed against the files):** the reviewer spawn at `ralph-pi.sh:1081-1090` is `timeout -k 30 "$REVIEW_TIMEOUT"` (10800 s, `:142`) wrapped around `"$PI_CMD"`, whose default at `:152` is the qwen slot wrapper with a wait cap LONGER than the review budget. A remote DeepSeek review that consumes no local slot can be SIGKILLed while queued behind two other loops' turns, and it lands as `review-<iter>.rc != 0` → `REVIEW_FAILS` → loop stop at 2, attributed to the model instead of to slot contention. Fix: `QWEN_SLOT_BYPASS=1` in that spawn's `env` list, or a reviewer-specific wait cap well under `REVIEW_TIMEOUT`. Both lines are inside the operator's uncommitted hunk, so it is not a builder batch (same root as review-6 F2 above).
**Owner action (review-10 F6, confirmed):** the same uncommitted `guard()` (`ralph-pi.sh:363-385`) replaced the FATAL `exit 3` with `while :; … sleep 300` — right for a busy route, wrong for a broken *invocation*: a missing/non-executable `PI_CMD`, a bad alias or a broken `sudo` returns 126/127 instantly and is now indistinguishable from "qwen is busy", while `phase route-health` is rewritten before every probe so the watchdog's 900 s arm can never fire. The loop parks silently, `logs/ralph/disabled` stays in place, and no BLOCKED record is written. Suggested: treat rc 126/127 and `[ ! -x "$PI_CMD" ]` as FATAL and cap consecutive failed probes (24 ≈ 2 h) as a backstop. Same unbounded wait as review-4 F4 with a second failure mode; the boundary is ownership, not a gate.
**Owner action (review-10 F7, confirmed):** `tools/runner/test_review_state.sh` lost its exec bit (100755 → 100644) in the same uncommitted set, and nothing runs it automatically — not `tools/check.sh`, not the `gates` phase, not a Makefile — so the loop's own review-state machine is manual-only and its mode drifted unnoticed. Restore 0755 (this uid cannot chmod the file) and either wire it into `gates` or record in `tools/check.sh` why it stays outside.
**Owner action (review-10 F8, confirmed):** `docs/OWNER_DIRECTIVE_2026-11-05.md`'s body was rewritten in place — the quoted `WORKER_MODELS`/`REVIEWER_FALLBACKS` block replaced and a "luna is retired" decision inserted into a file dated 2026-11-05 — with no amendment marker, and `ralph/builder.md`, which IS in `gates_batch_integrity.PROTECTED`, is modified in the worktree too. Until that set is committed with approval or reverted, any add-all commit reads as an unapproved protected edit; the directive wants an "amended <date>" section rather than an in-place rewrite.
**Owner action (review-10 F3, second half — not landable by a batch):** the recommended fix edits `docs/frames/2026-11-06-review9-remediation.txt`, but `gates_batch_integrity.py:71-76` fails any batch that modifies a historical frame or sidecar ("history is immutable") and that gate is itself `PROTECTED`. The false comparative in that sidecar is permanent by design; the corrected inequality lives in the shipped comments, in `PROGRESS_LOG.md` Iteration 119 and in every sidecar from here on. Same shape as review-9 F3.

**Owner action (session config, AGENTS P9):** neither `obsidian_*` nor `cognee_*` tools are exposed in this session, so the vault lookup is the degraded disk-only path and Cognee's graph search was not available at all. Reported, not worked around silently.

**Housekeeping (builder, deferred from batch 94):** three `@vfp-exempt` pins in `mdk_core/src/audio/sound_device.cpp` (lines 128, 147, 155) **do not parse** against `gates_vfp.EXEMPT_RE`: two are longer than the gate's 400-character window and one writes `measured n/a (corpus empty).` where the grammar wants a single token. `gate_exemptions` would call them `invalid @vfp-exempt grammar` and `MUST exemption missing measured cost` — but `scan_tree` never opens that file, because it is not in the (empty) hot-path corpus and carries no `@hotpath` marker, so today they are invisible. The moment a capture puts the mix loop in the corpus, three MUST findings fire on code that is correct and deliberately pinned. Fixing them is a reword of the R2/R3/R4 gameplay-fidelity pins, which AGENTS §5 says needs a capture and a fidelity verdict, so it is recorded rather than touched. Batch 94's own pin was written to parse (365 chars, `measured n/a.`) for exactly that reason, and `sound.h` is now grammar-clean if it is ever swept.

**Housekeeping (builder, deferred from batch 92):** `docs/CONTINUATION.md` is ~315 lines against `ralph/builder.md`'s "under ~150 lines: current state + priority order only". The overage is the per-review closure paragraphs (reviews 2, 5, 6, 8, 9, 10, 11, 12), each of which is already narrated in `docs/PROGRESS_LOG.md` Iterations 114–121. Deleting them is a history edit a reviewer should approve rather than one batch's cleanup, so it is recorded here with the proposal: replace those eight paragraphs with a table of the durable facts they carry (each already duplicated in the standing list below) and keep the owner-action block verbatim. Batches 88–93 each added one paragraph, so the file grows ~1 per review until someone consolidates it.

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
    3 = 12.34 MB, level 4 = 15.03 MB, level 8 = 19.08 MB of float against the 24 MiB heap the crt0 header
    asks for (review-1 F3: NOT 16 MB of FCRAM — see the memory fact below), so a full float decode was
    never safe) — which bounds nothing, see NEXT. Its `plays=1` emulator evidence was an over-claim
    corrected twice; the ledger line is the evidence now. Narratives "Iteration 108"–"Iteration 110".
6c. **DONE (94) — the sample store is the file's int16.** `Sound::samples` is `std::vector<int16_t>` and
   `Sound::sampleAt()` is the only conversion in the tree; the decode no longer touches a float
   (`(byte-128) << 8`, or the file's short). Measured on both builds with the new `tick1 mixsize <level>`:
   level 3's 3,234,976 samples go 12,939,904 → **6,469,952** bytes, level 4 15,757,072 → **7,878,536**,
   level 8 20,004,168 → **10,002,084**, with the sample counts identical across the two builds. No sample
   value moved: `golden/L3_audio_mix.txt` and `golden/L3_audio_meta.txt` **pass unchanged** (they were not
   regenerated) and `tick1 mixmeta 3` is byte-identical to the parent build's dump. Pinned by
   `audio_sink_tests.cpp`'s `testSampleStoreExact()` (both widths, all four extremes, expected floats
   written the oracle's way and compared as bits; mutation-checked — `kByteShift = 7` fails 7 checks).
   **The bank still has no bound, and halving did not remove the ceiling**: the SNI archives stay resident
   too (4.39 / 5.90 / 5.82 MB), so a fully decoded level 3 is 10.86 MB and level 8 is 15.82 MB — and the
   number they have to be measured against is the **24 MiB** `__heap_size` the crt0 header asks for, not the
   16 MB this file used to say (review-1 F3), so level 8's **bank** fits with 9.34 MB left **for** the
   level — which is not the same as the level fitting in it, because nothing measures the level's own
   cost (review-2 F3) — and the open item is
   unbounded growth, not a ceiling. Budget-and-evict stays open, and its two halves are coupled —
   freeing an archive's bytes requires preloading, which is the opposite of today's lazy decode.
   Narrative "Iteration 122".
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

## NEXT (batch 95)
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

**Audio, in priority order after the renderer items.** (a) **6d** — the console has no `ScriptRuntime`, so
ambient and object sounds cannot play; only Kurt's own set exists, and the emulator's ledger shows it
(`voices=0 plays=1`). It is the same missing piece as the object pass and the camera's script inputs, so
say which of the three it unblocks before starting it. (b) **the bank still has no eviction bound** — the
numbers are measured now (batch 94, `tick1 mixsize`): decoded 6.47 / 7.88 / 10.0 MB plus archives
4.39 / 5.90 / 5.82 MB for levels 3 / 4 / 8, against the 24 MiB application heap the crt0 header requests
(review-1 F3 — the "16 MB of FCRAM" this line used to quote was invented; and what fits is the bank's
15.82 MB, not a measured level cost, review-2 F3), and
the coupling is that freeing an
archive needs every sound in it decoded, i.e. a preload, which is the opposite of the lazy policy that
keeps today's peak low. A budget that evicts a voice's own sound mid-playback is the trap to avoid.
(c) the ndsp sink's `nsamples` unit is *measured* in the
emulator's HLE DSP, the frame counter's **kind** is *derived*, and its **rate is measured in the emulator
and still unmeasured on the console**: a slope between two ledger rows gives **204.544 /
204.562 / 204.566/s on three runs of one ELF and 204.549/s on a fourth ELF** (batch 94, whose mix code
moved), a 0.021/s spread that is exactly ±1 count over the
47.134 s window, which is within 0.013/s (0.006 %) of the header model `32728.5/160 =
204.553` and 0.234–0.256/s (0.12 %) from a power-of-two
`204.800`, against a ±0.05/s window-to-window scatter — so the quantum is **160.00 ±0.01** and the power-of-two
is out (review-10 F1, error bar from batch 92, and batch 94's fifth sample is the first taken from a
*different* binary, so the estimator does not depend on the mix's own code). A single row's `dpf/secs` cannot give this: it carries a constant ~20 ms start
offset, and `secs` prints at 3 dp now because the 2-dp rounding was the same size as the scatter. The
Old-3DS check stays open (AGENTS P6/P8). What to read on hardware is the ledger line's `submitted` against
`secs x 50` — the seconds the mixer was actually given, printed on the line since
batch 88 — and **not** `done` against `submitted`, which is an identity and proves
nothing (batch 82, review F1). The older `frame x 735/882` form of that division is
the same number only at exactly 60 fps (review-2 F3), and it **over**-reads below 60
fps, not under (review-8 F3). Reading `dspframes` off a hardware run is a liveness
check first: no installed header names the frame quantum, so a console rate needs a
slope against a clock the console itself owns, not one row's division.

**Deferred review findings:** review-4 of run-20261105-142911 F1/F2 (the geometric solid-arena pin stands in for the unported
DTI connection crossing; the fall probe hard-codes the start and omits the camera stage) need a
placed-fall oracle in full order plus a walk-out golden, and are deferred to a collision batch.
**New from batch 97: the MVE movie decoder has no player.** `formats/mve.{h,cpp}` is ported and
now covered (PCM both widths, DPCM, silence, the broken-size shapes) but nothing calls it, so it
is gc-sectioned out of the shipped ELF and `MISC/FLIC/MDKBZK.MVE` never plays. Wiring it needs a
video presenter (palette + indices → a canvas or a texture), which is the same canvas seam item 5b
is blocked on, so it is not a cheap follow-on. Original-3DS behaviour remains unmeasured.

## Standing facts a batch must not re-derive

* **`Mve` is ported, tested, and NOT LINKED into the shipped binary — so an unchanged `mdk.elf` is
  not evidence that a change to `formats/mve.cpp` did nothing.** `arm-none-eabi-nm mdk_3ds/mdk.elf
  | grep -ci Mve` reports **0** and `mdk_3ds/mdk.map` records `build/mve.o (__aeabi_idiv)`: the
  object is pulled in only to satisfy a shared helper and every Mve section is discarded by the
  `-ffunction-sections` + gc-sections link, because nothing in `mdk_core/src` or `mdk_3ds/src`
  outside `formats/mve.{h,cpp}` mentions `Mve` — there is no movie player on either target. Batch
  97 changed a shipped instruction and the ELF still came out md5-identical for exactly this
  reason. If you change `formats/mve.cpp` and reach for "the binary did not move, so the change was
  inert", check the map first. (review-3 open question 2 / batch 97.)
* **A bounds guard that cannot fire is worse than none, because it reads as coverage.**
  `if (count < 0)` in `decodeAudio` guarded a value that comes from `binU16` and so is never
  negative, while the write it existed to prevent went unchecked. The durable form of this check is
  `count < channels_` — the quantity the write actually depends on. And when a defect has two
  shapes, verify **two instruments** see it: batch 97's mono shape is a UBSan null reference and
  its stereo shape is invisible to UBSan and only AddressSanitizer catches it, which is why
  `ubsan_decode_test.sh` now runs both and why each half was proven to fire on its own shape.

* **Level 7's start is inside DANT_1 on a floor at z = 0 and the oracle drops him there too** — DTI start
  `(0,4,9)` at angle 90, `+ StartDrop` lifts it to 12, a BSP ray down hits `z = 0.01`, follow-camera eye
  `(0,-4,4.5)`. `golden/L7_spawn_fall.txt` is the oracle's own run of this. Do not re-ask "why does Kurt stop
  at z=0".
* **The sample store is int16 on purpose, and it is the oracle's float bit for bit (batch 94).** `Sound::samples` holds the file's int16 — the 16-bit path is the file's short, the 8-bit path is `(byte-128) * 256` — and `Sound::sampleAt()` is the **only** conversion in the tree. It is exact, not close, because both of the oracle's scales are powers of two: `((byte-128)*256) * (1/32768)` **is** `(byte-128) * (1/128)`. Do not "restore" the float store, and do not re-derive the argument: `golden/L3_audio_mix.txt` and `golden/L3_audio_meta.txt` pass **unregenerated** across the change, `testSampleStoreExact()` pins both widths and all four extremes against floats written the oracle's way and compared as bits (mutation-checked: `kByteShift = 7` fails 7 checks), and a `static_assert` in `sound.h` makes widening the store a build failure. **The 8-bit path is a multiply and not a shift, and that is not a style choice** (batch 95, review-1 F2): `((int32_t)byte - 128) << 8` is UB for `byte < 128` under C++17 [expr.shift], `mve.cpp:311` carried the same shift, and both are now `* 256` with the reason spelled out at each site. The measured footprint is `tick1 mixsize <level>`: level 3 3,234,976 samples = 6,469,952 bytes (was 12,939,904), level 4 7,878,536, level 8 10,002,084 — and **that is not the whole footprint**: the SNI archives stay resident too (4,390,060 / 5,904,992 / 5,821,432), so a fully decoded bank is 10.86 / 13.78 / 15.82 MB. **What that is against is the 24 MiB `__heap_size` in
the built ELF's crt0 header, not "16 MB of FCRAM", which batch 94 invented and review-1 F3 struck: libctru
`os.h` says `OS_OLD_FCRAM_SIZE = 0x8000000` = 128 MiB on OG, and the header also asks for a 32 MiB linear
heap, so a fully decoded level 8 **bank** fits with 9.34 MB left for the level itself — and nothing in
the tree measures what the level needs, so that remainder is a budget and not a verdict (review-2 F3) —
and the eviction item is about unbounded growth, not about a ceiling.** The cost is five ops per sample read in the mix loop — `lsl`/`ldrsh`/`vmov`/
`vcvt.f32.s32`/`vmul`, the `vmov` being an ARM→VFP transfer AGENTS §1 prices at 1 cycle throughput and 4
cycles latency (review-1 F7) —
two reads per output frame, 735 frames a frame, pinned `@vfp-exempt R8` at `sampleAt()`. If you widen it you
double the bank and re-open 6c.
* **The memory numbers this project quotes are the crt0 header and libctru's, and "16 MB of FCRAM" was
  invented (review-1 F3, batch 95):** the built `mdk.elf`'s crt0 header at vaddr `0x100010`/`0x100014`
  carries `__heap_size = 0x01800000` (**24 MiB**) and `__linear_heap_size = 0x02000000` (**32 MiB**), and
  libctru `os.h` gives `OS_OLD_FCRAM_SIZE = 0x8000000` (**128 MiB**) and `OS_FCRAM_SIZE =
  0x10000000` (256 MiB). `SPEC.md` says
  64 MB RAM / 32 MB system heap. `osGetMemRegionSize(MEMREGION_APPLICATION)` is the only figure that would
  settle what an untouched OG actually hands an app, and **nothing in this tree calls it** — that is a
  hardware measurement still owed. Quote the header, cite the source, and never re-invent a ceiling.
* **A guard-walk bound in a gate is a structural fact and never a line count (review-1 F1, batch 95):**
  `ndsp_sink_test.sh`'s header walk is bounded by the `{` of the block that owns the statement, because a
  braceless guard cannot live outside the block its statement is in. A `HEADER_WALK = 6` cap was the eighth
  could-not-see clause in a row and it reopened a hole one wrapped line further out for the third batch
  running; the fix deleted the cap. When a walk needs a bound, find the construct that bounds it — and when
  a guard test is written, its **green** half is the probes of correct spellings, which must be committed
  and not run by hand.
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
* **`ndspGetFrameCount()` counts ndsp MIXER frames, not samples (batch 88, review-2 F2; rate corrected by batch 89, review-8 F1; measured by batch 91, review-10 F1).**
  Derived from the linked `libctru.a`: `.bss.frameCount` is zeroed by `ndspInitialize`,
  gets exactly one `add` per `ndspThreadMain` iteration after the `svcSignalEvent`
  that kicks the DSP, and `ndspGetFrameCount` is its only reader — that is the
  counter's KIND, and it is proof. The **rate** is now measured too, and the
  estimator matters more than the number: `NDSP_SAMPLE_RATE =
  SYSCLOCK_SOC/512 = 16756991/512 = 32728.5 Hz` (`os.h:9`, `ndsp/ndsp.h:9`) has
  nothing to do with this channel's 44100, and the DSP frame quantum is named by
  nothing installed. **One row's `dpf/secs` is not a rate**: it carries a constant
  ~20 ms start offset (the counter starts with ndsp, `secs` with the first frame),
  and three batches mistook that offset for run noise until review-10 F1 proved the
  "run-to-run spread" had been one trajectory sampled twice — and batch 92's third run of
  a byte-identical ELF then showed the counter itself varies by one count on 25 of 48
  rows, so it is reproducible **to** a frame, not frame for frame. A **SLOPE
  between two rows** cancels the offset: the emulator reads 204.544/s twice, 204.562 and
  204.566/s over four runs of one ELF, within 0.013/s
  (0.006 %) of `32728.5/160 = 204.553` and 0.234–0.256/s (0.12 %) from a power-of-two
  `204.800`, so the quantum is **160.00 ±0.01 and the power-of-two is out**. The 0.021/s
  spread has **two** sources and batch 92 named only one (review-12 R11-2/R11-3): a ±1
  dspframe count, and a 4 ms movement of the measured window at the print's 3 dp, which
  alone is 0.017/s (`9641/47.130` against `9641/47.134`) — two of the four runs agree on
  all 48 counts and still differ by that much. The real
  resolution is the ±0.05/s scatter between 11 s sub-windows of one run and the ±0.02/s
  run-to-run spread of the slope, NOT the
  print's rounding band — quoting the rounding band instead would repeat batch 89's
  mistake pointing the other way. **The console's mixer rate is still unmeasured**
  (this is an HLE DSP), so on hardware the line is a liveness signal first. Batch 88
  printed 32768 Hz because it made the
  measurement fit; the claim it replaced ("240-sample blocks at 48 kHz") fitted the
  same magnitude at 200/s. A magnitude must never be accepted as a mechanism, and
  rounding a constant to reach one is the same error wearing a derivation.
* **A call site is not an unconditional statement, and a guard has three brace styles (review-11 R10-2, review-12 R11-1).** `ndsp_sink_test.sh` requires `audioClock += X;`, `mixer.update(X)` and `mixer.device().update(X)` to each own their line and to name the same identifier, but a line test alone sees only `if (c) call();`; a block guard `if (c) {` owns its line, and so do the **braceless** form and the **Allman** form, where the `{` sits alone and the keyword is one line higher. The gate therefore reads the header from **around** the statement — own-line prefix, a walk up from the statement, the enclosing block's opener, and a walk up from the brace when that opener is alone — with the walk stopping at `;`, `}` or `:` **before** it looks for a keyword (so a complete `if (paused) return;` above the statement is not mistaken for its guard) and a `case`/`default`/`public:` label never reading as a guard, and the frame loop whitelisted **by name** so correct code cannot be redded. The same shape applies to any "is this statement unconditional?" check in this repo: `main_3ds_flow_test.sh`'s `line_depths` is the sibling precedent. A count of call sites, of textual writes, or of lines is a proxy a mutation can leave alone; seven consecutive reviews have found one of these per batch.
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
  with the same `mdk.elf` `73bfe104…`; and two consecutive builds of the SAME tree
  differ too (batch 90: `48931bcd…` then `23c5354f…` from one `mdk.elf`
  `75b1edb0…`). Cite the ELF md5 in a sidecar; treat a
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

* **A frame sidecar must state, in its own text, why the pinned picture did not move, with the measurement that shows it** (`docs/frames/README.md:56-62`, review-9 F3). Batch 89's does not, and that gap is now permanent: `gates_batch_integrity.py` refuses any change to a frame `.txt` the range did not add ("history is immutable") and is itself `PROTECTED`, so a sidecar cannot be annotated after the fact by a batch. Write the paragraph at capture time. A static frame with no stated cause is exactly the byte-identical-record-proves-nothing failure `docs/evidence/README.md` was written about, and `validate_png_pair` will not catch it — it checks scene identity, `captured:` and one oracle field, nothing more.

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
