# Phase 2 continuation checkpoint

## ARENA STREAMING — PHASE 0 HARDWARE BASELINE, MEASURED (branch `experimental/arena-streaming`, base `36528a3`)

**This section is not a loop batch.** It is the Phase 0 measurement the arena
streaming work needs before any streaming change: the level-7 memory failure is
now REPRODUCED ON THE CONSOLE and MEASURED, not inferred.

**Harness correction first, because it cost hours.** The deploy was repeatedly
read as failing at ~50 % and re-uploaded; it had been succeeding every time. The
loop is `mdk_3ds/tools/hw_cycle.sh --once` and the full device runbook is
AGENTS.md §11 P10. `3dslink`'s piped progress line is stale when stdout is not a
terminal; the authority is the console's `/3ds/netload_stub/transfers.log`, whose
line for the build measured here is
`file=mdk.3dsx status=ok bytes_out=119371360 bytes_in=59419024 ms=210848`.
`bytes_in` is the *compressed* count, not a truncation.

**Method.** `mdk_3ds/src/main_3ds.cpp` carries an uncommitted `heapProbe()`
(temporary, Phase 0 only) that prints `mallinfo()` at each phase and installs
`std::set_new_handler` so an OOM names itself instead of exiting silently. The
guest log is `sdmc:/3ds/mdk/mdk.log`, `fflush`ed per line and served by the ATM
sysmodule at `http://192.168.4.167:8080/3ds/mdk/mdk.log`. Console
`old3ds-lan167`, level 7, backend PICA200 Citro3D, data dir `romfs:/`.

`cap` is `envGetHeapSize()` = **32,657,408 B (31.1 MiB)**. `arena` is what
newlib has sbrk'd from its 24 MiB reservation; `used` is `uordblks`.

| probe | arena | used | free | peak used |
|---|---:|---:|---:|---:|
| boot | 32,736 | 27,752 | 4,984 | 27,752 |
| renderer | 65,504 | 61,664 | 3,840 | 61,664 |
| level-data | 20,762,592 | 18,732,056 | 2,030,536 | 18,732,056 |
| cmi | 21,843,936 | 20,897,784 | 946,152 | 20,897,784 |
| arena-view | 21,843,936 | 20,971,792 | 872,144 | 20,971,792 |
| assets | 31,158,240 | 29,163,240 | 1,995,000 | 29,163,240 |
| pre-audio | 31,158,240 | 28,084,224 | 3,074,016 | 29,163,240 |
| **OOM** | 31,158,240 | 29,066,424 | 2,091,816 | 29,163,240 |

The last three lines of the run:

```
mem[pre-audio]: cap=32657408 arena=31158240 used=28084224 free=3074016 peak=29163240
mem[OOM]: cap=32657408 arena=31158240 used=29066424 free=2091816 peak=29163240
FATAL: operator new failed -- the 24 MiB heap is exhausted
```

**What the numbers establish**

1. **The failure is genuine newlib heap exhaustion, and it happens while loading
   the level's SNI archives** (`audio: loading SNI archives (level 7)` is the
   line immediately before the OOM). At that point the allocator had 2,091,816 B
   free inside a 31,158,240 B arena, plus 1,499,168 B of the 32,657,408 B
   reservation never sbrk'd — **3,590,984 B total against a 5,038,040 B SNI
   set**, so the request could not be served and no smaller allocation was left
   to give.
2. **`LevelData::load` is the largest single consumer: +18,670,392 B**, because
   it eagerly calls `Mto::getArena()` for all 19 arenas while the MTO byte
   buffer (8,050,956 B) is still resident.
3. **The arena *view* is nearly free (+74,008 B)**: 19 arenas become 135 batches,
   2,544 vertices, 870 triangles. The bytes are the cost, not the view — which
   is exactly what makes streaming the right lever.
4. **The sprite/HUD pass is the second consumer: +8,191,448 B** in one step
   (`mem[assets]`), which is why the arena reached 31.16 MB of its 32.66 MB cap
   before audio even started.
5. **CMI costs +2,165,728 B** for a 1,079,557 B file.
6. **`osGetMemRegionFree(MEMREGION_APPLICATION)` reads 0 at every point in the
   run** and is not a headroom signal: crt0 reserves `__heap_size = 0x01800000`
   and `__linear_heap_size = 0x02000000` out of the 64 MiB region at boot, so the
   region is fully spoken for before `main`. Read `mallinfo()`, never that.
7. **`mallinfo()` reclaims ~1.08 MB between `assets` and `pre-audio`**, so the
   peak (`29,163,240`) is above the `pre-audio` `used` — budget against peak,
   not against the line before the crash.

**Do not re-derive any of this.** The probe is uncommitted scaffolding: keep it
until Phase 0 baselines exist for the other levels, then delete it in one commit
with the numbers already in this section.

### The audio load cannot fit, with or without fragmentation

Live heap before audio is **28,084,224 B**; the three SNI archives total
**5,038,040 B** and `SoundBank` retains every byte (`Sni::bytes_`), so the
requirement is `28,084,224 + 5,038,040 = 33,122,264 B` against a
**32,657,408 B** cap — **short by 464,856 B before fragmentation is considered**,
and the run's peak (`29,163,240`) makes it worse than that.

Fragmentation is what actually trips first. `readFileRomfs()` is
`resize(size)` + `fread` in place, i.e. **one exact-size allocation per file**
(`mdk_3ds/src/platform/filesystem_romfs.cpp:156`), so `LEVEL7O.SNI` needs a
**single contiguous 4,059,388 B** block. Free total at OOM was
`2,091,816 + 1,499,168 = 3,590,984 B`, below that block even if it were one piece.

The `used` delta across the audio phase proves the identity of the failing
request: `29,066,424 - 28,084,224 = 982,200 B` was successfully allocated, and
`TRAVERSE.SNI` (732,952) + `LEVEL7S.SNI` (245,700) = `978,652 B`, leaving
**3,548 B** for the two `Sni` objects' entry vectors. So the two small archives
loaded and the **very next allocation — `LEVEL7O.SNI`'s 4,059,388 B `resize` —
is the one that raised `FATAL`.** That is arithmetic, not inference.

So "free some bytes somewhere" is not a sufficient fix: the largest contiguous
block is the binding constraint, and it must exceed 4,059,388 B.

**The lever is therefore the 18,670,392 B of eagerly resident MTO arena data,
not the SNI parser.** Two changes are indicated and they are independent:
1. free an arena's MTO bytes once its view is built (or load on demand), which
   buys 18.6 MB; and
2. do not hold all three SNI archives resident to answer one name lookup.

Do not "solve" this by shrinking the SNI working set alone — a 465 KB deficit
plus a 4 MB contiguous block will not survive the next level's assets.

### Recovery that cost the most wall clock (2026-11-17)

The console was **wedged**, not the build: HTTP :8080 answered nothing on any
path while ping was green and :17491 still accepted a socket, so `3dslink`
pushed `119,371,360 B` into a receive buffer nobody drained and `hw_cycle.sh`
timed out at 300 s. **Reboot it** — one command, returns in seconds:

```bash
python3 /mnt/nas/Development/netload_stub/tools/atmctl.py \
    --ip 192.168.4.167 --tcp-port 17492 reboot
```

That took `boot_count` 635 → 636 with a new `session` id, `:8080` came back 200
on the first probe, and the next `hw_cycle.sh --once` finished in **194 s** and
printed `FRESH e2cb292e20c0747de92b04b883485dc1`. This is now AGENTS.md §11
trap 8 and is checked before blaming a push. Also `3dslink`'s piped progress
line is stale when stdout is not a terminal — it prints `49.78%` on a transfer
that already completed; judge a push by `transfers.log status=ok` and
`bytes_out` equal to the local file size, never by that line.

### A clean exit can still wedge HTTP — and the harness must not call that a failed run (2026-11-17)

The 90 s `AUTOQUIT` level-7 cycle reported `TIMEOUT waiting for the guest log`,
and the console was wedged again afterwards (`:8080` answered 0 bytes on every
path, ping green). The run itself was **fine**: `/3ds/mdk/mdk.log` survived on
SD and ended

```
frame 300 elapsed=0.0707 steps=1383 feet=(0.0,4.0,0.0) yaw=90.0 health=100 eye=(0.0,-4.0,4.5) voices=0 plays=1 objects=7 obj=570c/13d peak=570c/13d
exiting after 326 frames (1535 steps): harness autoquit (MDK_AUTOQUIT)
--- session end ---
```

with audio running (`dspframes=17084`, `dspdropped=0`), the object mesh streamed
into linear memory, and **no crash dump** in `/luma/dumps/arm11/` (`atmctl
artifacts` shows screenshots only). So the game exits cleanly and the
**sysmodule wedges on HTTP afterwards** — a post-exit wedge, not a build
failure.

Two harness defects turned that into a false negative:

1. **md5-equality is not a completion signal.** The log is unchanged between the
   push and the game's first line, so two consecutive pulls match immediately
   and a partial log looks settled. `hw_cycle.sh` now stops on a **new
   `--- session end ---` marker** relative to the previous log and keeps polling
   through failed pulls (a failed curl during the poll is the wedge). Because
   every line is `fflush`ed, the marker is complete as served.
2. **Recovery has to survive dead HTTP.** `atmctl.py reboot` does an HTTP
   `status` pre-check, so in the wedged state it refuses; `cmd reboot` goes out
   over the fire-and-forget UDP `:8082` channel and needs no reply. Verified:
   two sends plus polling brought `:8080` back in **~2 minutes**
   (`boot_count` 638 → 640, `session f24d3fd0`). AGENTS.md §11 trap 8 now names
   this path, and trap 9 names the post-exit wedge. **The operator is never
   asked to power-cycle the console**; if two `cmd reboot` sends plus five
   minutes do not restore `:8080`, record it and move to a non-hardware
   increment.

**A third defect was the budget, and it is the one that looked most like a bad
build.** The poll cap was 300 s measured from *before* the push. `transfers.log`
shows this card taking `158 s`, `215 s`, `350 s` and `647 s` for the same
`119,371,492 B` payload (`net_wait_ms` alone reached `118,848 ms`), so a 90 s
`AUTOQUIT` run could not fit: the first timeout caught the log mid-frame
(`draw 1:` with no newline). The budget now starts **after** the push
(`MDK_HW_WAIT`, default 600 s) and the harness prints `push returned after Ns`.
The same run also showed `3dslink` returning 0 for a transfer that landed
nowhere, so the harness now probes `:8080` before pushing and afterwards checks
the device's own `transfers.log` for `status=ok` with `bytes_out` equal to
`ls -l mdk_3ds/mdk.3dsx`.

**The console is currently ARP-dead** (`ping` fails, `ip neigh` shows
`FAILED` for `192.168.4.167`, `discover --broadcast` times out). That is the one
state no command channel can reach: HTTP :8080, :17491, :17492 are all closed
and the last `cmd reboot` did not bring it back within five minutes of polling.
Nothing in the build is implicated — the last complete session on SD ended
cleanly at 326 frames with no crash dump. The next hardware increment starts by
re-checking ARP; the documented ladder (AGENTS.md §11 trap 10) says to poll ARP
for five minutes, try `discover --broadcast` in case DHCP moved it, and
otherwise continue with non-hardware work rather than escalate.

### The harness verdict logic is now tested without a console

Because every one of those failures was in `hw_cycle.sh` and not in the build,
the verdict logic gets a fixture test instead of another hardware cycle:
`mdk_3ds/tools/hw_cycle_selftest.sh` runs the real harness against a fake
console on `127.0.0.1:8080` with a stub `3dslink` on `PATH`. **5 PASS / 0 FAIL.**

Two more defects were found by actually running that fixture, and both had
already produced a green verdict in this session's own terminal:

1. **The baseline came from the local output file.** On a first run `$OUT` does
   not exist, so `PREV_END` was 0 while the device log — append-only across
   boots — already held markers from earlier sessions, and the first pull
   matched: `cycle 0s, FRESH` on a run that had not happened yet. The baseline
   is now a snapshot of `$LOG_URL` taken after the preflight probe and before
   the push; if that snapshot cannot be read the harness refuses to push,
   because a completion that cannot be told from an older one is not evidence.
   Fixture: a log that already ends with `--- session end ---` and never writes
   another now gives `TIMEOUT waiting for a new session end marker`, where the
   parent gave rc 0.
2. **A failed pull left the previous pull's bytes in the output file,** so the
   post-loop check read a pull the harness had not made. Measured: a fixture
   whose HTTP closes mid-run printed `FRESH` at the parent. The pull file is now
   unlinked before every attempt, a `GOT` flag records whether any attempt
   succeeded, and "no successful pull" is its own verdict naming the wedge —
   plus the recovery command and the note that the SD log survives it.

The unverified-push warning also became fatal (after five 2 s retries, because
the ledger line lands just after `3dslink` returns): the whole point of the
ledger is that a run of an unknown binary is not a measurement, and a warning
in the middle of a wall of output is how a stale run gets reported as fresh.

**Non-vacuity, measured:** against the parent `hw_cycle.sh` the self-test gives
**1 PASS / 4 FAIL** — the legitimate clean-exit case still passes there, and
exactly the four defect cases fail, which is the row that says the fixtures bite
on the real bugs and not on the rewrite. Against the working harness it is
**5 PASS / 0 FAIL.** This is a harness test: it says nothing about the console,
and no console was reachable to run.




## ARENA STREAMING — PHASE 1S: THE HEAP SPLIT, AND LEVEL 7 NOW RUNS ON THE CONSOLE (branch `experimental/arena-streaming`)

**This is not a loop batch either.** It is the first intervention against the
Phase 0 baseline above, and it is measured on the same console
(`old3ds-lan167`, firmware `9f3632fe047c+dirty`, level 7, PICA200 Citro3D,
`romfs:/`, mono).

**The change.** libctru's `__system_allocateHeaps` splits the application region
in half when neither size is pre-set: `linear = avail/2`, `heap = avail -
linear`. On this console that is `main cap = 32,657,408` and
`linear = 32.7 MiB`, and it is the wrong way round for MDK — textures live as
8-bit palette indices in the main heap and are expanded on demand under an LRU
(SPEC §"chosen 3DS texture strategy"), so the GPU side holds one arena's working
set, not a level's worth of RGBA8. `mdk_3ds/src/platform/heap_split.cpp` now
strong-defines the two weak libctru globals:

```c
u32 __ctru_heap_size        = 0;                 // main = avail - linear
u32 __ctru_linear_heap_size = 24 * 1024 * 1024;  // GPU working set + margin
```

The default allocator is still the one that runs — it reads both globals before
it splits, so `heap == 0` + `linear != 0` means "linear gets this much, main
gets the rest". That keeps `mappableInit()`, the `svcControlMemory` mapping and
the `fake_heap_start/end` wiring in libctru's own path; hand-replacing
`__system_allocateHeaps` is the trap recorded in
`30-architecture/patterns/3ds-adaptive-linear-heap-and-mappableinit-trap.md`
(omitting `mappableInit()` data-aborts in `gspInit` at boot). Verified in the
linked ELF: both symbols are `V` (weak) in `allocateHeaps.o`, and
`__ctru_linear_heap_size`'s initializer in `.data` is `0x01800000`.

The main size stays CONSOLE-ADAPTIVE (it is `avail - linear`), so a New 3DS or a
different region gets the larger main heap without a second constant.

**Measured on hardware** (`mdk_3ds/tools/hw_cycle.sh --once --ip 192.168.4.167`,
`FRESH 88b343f7d8dfb149360a82026d989714`, 303 s, guest log
`/tmp/kilo/hw-split1.log`):

```
main: heap split main=41046016 linear=25165824 (avail=66211840)
mem[assets]:      cap=41046016 arena=31682528 used=29615920 free=2066608  peak=29615920 lin=20699904
mem[pre-audio]:   cap=41046016 arena=31682528 used=28536904 free=3145624  peak=29615920 lin=20699904
audio: bank ready, 3 archives, decoded on demand
mem[post-audio]:  cap=41046016 arena=35745760 used=33577808 free=2167952  peak=33577808 lin=20699904
mem[runtime]:     cap=41046016 arena=35745760 used=33736528 free=2009232  peak=33736528 lin=20685696
loop: entering main loop
frame 5 elapsed=0.0939 steps=28 feet=(0.0,4.0,4.8) yaw=90.0 health=100
```

1. **The OOM is gone.** Main cap **32,657,408 → 41,046,016 (+8,388,608 B,
   +8.0 MiB)**; the audio phase now peaks at **33,577,808** instead of dying at
   `29,066,424`, so the 4,059,388 B `LEVEL7O.SNI` `resize` that raised `FATAL`
   has ~7.3 MB of room above it.
2. **The GPU side did not need the half.** Linear heap free after renderer +
   level-7 GPU allocation is **20,699,904 of 25,165,824**, i.e. the whole level-7
   working set costs **~4.45 MiB**. The old split was wasting ~16 MiB of linear
   on a workload that never touched it.
3. **Level 7 now reaches the main loop and moves.** Frames 1–5 with
   `steps` advancing, `feet` z 12.0 → 4.8, `arenas=19`, `canvas: quads=4
   draws=4 eyes=2`, objects streaming `570 corners / 13 draws` into a
   `4096-corner = 80 KB linear` buffer, `audio: sink=1 bank=1`, FZ/DN reasserted
   on the audio thread (`fpscr=83000090`).
4. **But the main heap is still tight, so this unblocks Phase 1, it does not
   replace it.** At the loop, `used=33,736,544` against `cap=41,046,016` leaves
   **2,009,216 B free and 7,309,472 B of slack to the cap**. That is enough for
   level 7 to run and not enough to absorb another level's residency — the
   18,670,392 B of eagerly parsed MTO arena data (Phase 0 finding 2) is still the
   lever, and the SNI archives are still fully retained by `SoundBank`.
5. **What is NOT yet evidenced, and must not be claimed:** arena transitions
   past the spawn (portal/level exits), level loads 1–8 other than 7, any
   stereo run, and the section-6 gameplay acceptance. This cycle proves boot,
   level-7 load, SNI load, and 5 frames of live gameplay only.

**Gates on this change:** `python3 tools/runner/audit_c_with_classes.py` →
`VERDICT: CONTRACT CLEAN -- 0 violations`; `python3 -m tools.runner.gates_vfp`
→ `PASS blockers=0 reviews=0`. The new translation unit contains no float work,
no allocation and no construct §10 gates.

**Instrumentation still uncommitted-by-design:** `heapProbe()` in
`main_3ds.cpp` now also logs `linearSpaceFree()` and the boot split, and the
`set_new_handler` fatal path stays until the Phase 0 baselines for the other
levels exist (see the Phase 0 note above).

Risk: medium — this changes the memory layout of every 3DS run (one new
translation unit plus probes), it is validated on hardware for level 7 only, and
a larger linear addition could still exhaust the now-smaller-than-half GPU heap
on a future level (measured headroom 20.7 MB, so not soon).

Risk: medium — this batch changes what the ledger row's acceptance command ACCEPTS (a reach budget outside its calibration band now refuses to judge at all), edits the shipped 3DS emulator harness, and ships no compiled code; the diff is two files, every new arm is mutation-measured, and the acceptance was re-run on :99.

**BATCH 175 (ledger row `3ds.hud-canvas`, acceptance `mdk_3ds/tools/emurun.sh --expect hud`, attempt 3 of the current cycle) — REVIEW-2 REMEDIATION: THE REACH BUDGET WAS A KNOB NOTHING COULD SEE, SO THE CALIBRATION NOW HAS A BAND AND THE SHIPPED INSTRUMENT REFUSES A BUDGET THAT HAS LEFT IT. Review-2 (`logs/ralph-pi/run-20261114-003546/review-2.md`, on batch 174) had **no Critical and no High**, one must-fix Medium (F1) and three Lows (F2, F3, F4); all four were reproduced against the tree before anything was changed and all four are fixed — nothing is rebutted. **F1 (Medium), RE-MEASURED AND NOT ACCEPTED ON THE REVIEWER'S WORD**: every ink fixture is anchored to `HUD_INK_REACH`, so raising it moves the fixtures with it — the parent test on the parent harness with only that constant at 88 printed `PASS EmurunHarness (296 checks)` at rc 0, a 7.3× loosening of the one tolerance that makes the ink rect a placement test, and the record's own line (a budget that reddens its boundary gets deleted, not tuned) makes tuning upward the expected move. **THE POLICY review-2's open question 1 asked for: a budget is a calibration — a measurement times a margin — so the measurement is now a constant and the band is derived from it**: `HUD_INK_SHORTFALL=2` (measured: bbox far 388,232 against the derived 390,234), `HUD_INK_REACH=12` unchanged at six times it, `HUD_INK_REACH_MAX=20` at ten times it and EXCLUSIVE, and `hud_ink_verdict` refuses a budget outside `[SHORTFALL, REACH_MAX)` as `HUD MALFORMED` **in the shipped instrument and not only in a test file someone can choose not to run**, naming the remedy (re-measure and restate the measurement, saying which capture). The reviewer's alternative ceiling (`HUD_INK_X1/4`) was measured and does not work: 390/4 = 97 admits the 88 the finding is about — a rect fraction is not a calibration. **FIVE NEW FIXTURES**: `inkshort` (shortfall == the measurement → passes, replacing the accidental lower pin at REACH=0), `inkreachshort` (REACH+1 → refuses by name: the failing side of the edge nothing sat on), `inkband` (**shortfall == the ceiling, ABSOLUTE, the one fixture not anchored to the budget, and the row a loosening reddens**), and `inkreachx`/`inkreachy`, one axis each; the test file also refuses to BUILD fixtures against an out-of-band budget rather than letting a block above `HUD_INK_Y0` red as `HUD INK NOT UNIQUE` (the reviewer's fixture-reason observation at 89). **NINE MUTANTS, ALL KILLED BY THEIR OWN ROWS**: arm deleted 12 FAIL, widened on x 2, on y 2, x term dropped 2, edge made exclusive 1, band guard deleted 4, ceiling made inclusive 1, floor made inclusive 1, F3 message reverted 4. **THE MUTANT TABLE CAUGHT THIS BATCH'S OWN FIRST CUT**: `bx1 + HUD_INK_REACH * 2` survived at rc 0 over 316 checks, because the arm is a DISJUNCTION and every fixture displaced both axes together so the other axis refused anyway — F1's class arriving through the axis, and the reason the two axis-isolated fixtures exist. **F3: THE `HUD INK NOT UNIQUE` MESSAGE WAS BACKWARDS ABOUT THE ONE PRODUCER THAT CAN HAPPEN** — `main_3ds.cpp` fills only health/dead/hurtFlash/whiteFlash, so `slots` is empty and `barMax` is 0, `drawInventory`'s loop body never runs and `drawBar` returns at `max <= 0`: the icons and bar it blamed draw nothing today, and at `kMaxSlots=5` the last icon anchor is canvas 224 → crop 149, left of the near bound anyway. The reachable HUD-side producer is the DEATH SKULL, centred at canvas (300,180) → crop (200,120) = exactly `HUD_INK_X0,HUD_INK_Y0`, through `drawImage` with `q.palette = palette_`, so it **straddles** the near corner and is partly outside by construction — the opposite of the message's claim. The message now names the level and the skull and says why; four rows pin it, one greping `drawInventory` and requiring 0. **F2**: the displaced HUD was recorded in canvas px and the numbers were crop px — Δ(130,71) crop = (195,106) canvas — corrected in the harness comment and the evidence file; e249567's subject stays as history (batch 170's F2 precedent). **F4**: "nothing greps `HUD VISIBLE`" was false — batch 174's own absence row greps it to require its absence; restated. **AND THE EMULATOR RUN CAUGHT WHAT THE SUITE COULD NOT**: the first acceptance printed `emurun.sh: line 1268: The: command not found` while returning rc 0 — a comment line had lost its `#`, `bash -n` passed it (a command name plus arguments is valid shell) and 316 checks passed over it; fixed, and the lifted block must now be SILENT when evaluated, pinned by a mutant that reproduces the bug. **`EmurunHarness` is 296 → 321 checks. THE FOUR-WAY**: new/working rc 0 (321); new/PARENT harness rc 1, one red (its fixture guard refuses a harness with no `HUD_INK_SHORTFALL`, so per-arm non-vacuity is the mutant table, as in batch 174); PARENT test/working rc 1 with **exactly one red, the parent's own row pinning the sentence F3 required changing** — not collateral; PARENT/PARENT rc 0 (296). **GATES**: `make -C tests/scripts -j8` rc 0, `All cases match the oracle.`, **830 PASS / 0 FAIL** (was 805; +25 is exactly these rows), reading `PASS EmurunHarness (321 checks…)`. `DISPLAY=:99 bash tools/check.sh` rc 0, `CHECK: ALL GATES PASSED`, `VERDICT: CONTRACT CLEAN -- 0 violations.`, `PASS L3_f1.bmp sha=58615fc85d02e009`, `BATCH INTEGRITY: PASS` (range-less, per review-1 F1), `COMPLETION SELFTEST: PASS (13 arms)`, `COMPLETION[d1]: 11 done / 13 total / 0 owner-blocked / 2 open`, `COMPLETION: LEDGER VERIFIED`, `SKIP gates_smdh`. `make -C mdk_3ds -j8` rc 0 with `mdk.elf` md5 `72328e9d5fd439d7f75703f5c28ccb80` **UNCHANGED** — correct, nothing was compiled; no `mdk_linux/**` file changed so `cmake --build build-linux` was not re-run, stated and not skipped. **P8, THE ACCEPTANCE ITSELF**: `DISPLAY=:99 mdk_3ds/tools/emurun.sh --expect hud` → **rc 0**, display `:99`, `mandarine.AppImage md5 0d2adab6` (r1.2.1, checked first), llvmpipe forced, `graphics_api=1`, `render_3d=0` (mono), `backend: PICA200 Citro3D`, `flow: screen=4 level=7`, frames **2460** by the deadline / **2580** after the reap, `canvas: quads=4 draws=2 mesh=2 corners=24 eyes=1 (canvas frame 2460)`, `HUD MARKER VISIBLE -- 121 canary pixels in the panel, 121 inside`, `HUD INK -- 134 of 134 HUD-palette pixels inside crop 200,120-390,234 (bbox 333,206-388,232, 0 outside, its far corner within 12 px of the derived 390,234)`. Emulator-green is not hardware-pass; no console was used. Frame `docs/frames/2026-11-14-the-budget-has-a-band.png` is byte-identical to its predecessors (census re-derived: 193 PNGs, 192 at `6769a137…`) — nothing on a render path changed. Prior-solution lookup: Pi knowledge (`mdk-sdl`) found nothing on a calibration band or a tolerance loosened; **Obsidian and Cognee MCP are ABSENT from this session's tool list** (AGENTS §9 P9: a session fault, recorded), vault read read-only from disk, bounded grep found nothing. Environment: `/tmp/kilo` still not writable by this lane (captures to `/tmp/mdk-b175` on `:99`), `Xvfb :99` already listening and left alone, ledger still unwritable with `ledger_close.py` PROTECTED. Full record: `docs/evidence/2026-11-14-175-the-budget-has-a-band.txt`.**

**NEXT, in order (batch 175; the list below this one is batch 174's and is superseded).**
0. **Two owner acts, and neither is a batch.** (a) `chgrp development docs/COMPLETION_LEDGER.tsv` plus `python3 tools/runner/ledger_close.py --row 3ds.hud-canvas` (and `--row linux.render-frame`) from a lane in the `development` group on a clean tree — the ledger is `coleshores:coleshores 0664`, unwritable here, `ledger_close.py` is PROTECTED, and both rows' acceptances are green (this row's was re-run this batch: rc 0). (b) **review-1 F1**: decide the door for loop-infrastructure commits that must edit `tools/runner/ledger_close.py` (owner-approved re-land, an infra exemption in `gates_batch_integrity.py`'s PROTECTED check, or a revert). review-2's F4/F5/F6 ride along: all three are in `ledger_close.py` or `.gitignore`, both PROTECTED.
1. **THE PANEL SPRITE IS SAMPLED WRONG BY THE GPU, AND THAT IS THIS ROW'S REAL REMAINING SLICE** (unchanged from batch 174's item 1). The ink's 2 px shortfall and the 134-pixel count are both measurements of a mis-sampled panel; the texture-addressing fix is the reason `HUD_INK_REACH` is 12 and not 2.
2. **WHEN THAT FIX LANDS, RE-MEASURE THE SHORTFALL AND RESTATE `HUD_INK_SHORTFALL`; DO NOT WIDEN `HUD_INK_REACH` ALONE** (new, and it is the policy review-2 F1 asked for). The harness now refuses any budget outside `[SHORTFALL, 10×SHORTFALL)`, so a shortfall that grows past 19 crop px cannot be absorbed by editing the budget — it has to be measured and the measurement restated with the capture it came from. That friction is the point: the cheap edit was the hole.
3. **DOES THE DEATH SKULL CARRY (0,255,0)?** (review-2 open question 2, promoted from a footnote because it is now the named reachable producer.) `hud_view.cpp` centres it on the canvas centre, which IS the ink rect's near corner, so if its sprite carries the canary colour a dead-Kurt capture prints `HUD INK NOT UNIQUE` for a HUD-side reason. Answering it needs `TRAVSPRT.BNI` plus the level palette, or a dead-Kurt emulator run; neither is available read-only here.
4. **THE DIGITS ARE FINE — do not go "fixing" them.** `drawNumber` builds its own `CanvasQuad`s and the shipped capture proves 58 of the 134 ink pixels are the health number itself (batch 174 §10).
5. **The ink rect's near bound is a bound, not a rectangle** (unchanged): `panel_.w`/`panel_.h` come from the sprite asset, so the near corner stays `kCanvasHeight/2`-derived and the weakest thing the anchoring guarantees.
6. **`/tmp/kilo` is not writable by this lane** (environment, re-measured: `touch` → Permission denied, `coleshores:coleshores 0775`). AGENTS §11 P7 says capture to `/tmp/kilo`; either the operator makes it group-writable for `development` or P7's path changes.
7. **The frame record's `BATCH INTEGRITY: PASS` is range-less and cannot see PROTECTED** (unchanged since batch 170): `tools/check.sh:175` passes no `--base`, so the protected-file and per-commit-frame rules never run in the gate set; both fixes are owner-protected files.
8. **Unchanged from batch 169's list:** the extent arm's zero-pixel margin, the ungated HUD dial rect, the ±7 % canvas-width pin, the un-freshness-checked explicit `--shot`, the uncommitted emulator capture, the ROOT-window grab on a shared display, and the missing Khronos validation layer.


**PREVIOUS BATCH 174 RECORD** (its NEXT list is superseded by batch 175's above; its findings and disposition are in `docs/evidence/2026-11-14-174-a-rect-that-only-asks-inside.txt`). **BATCH 174 (ledger row `3ds.hud-canvas`, acceptance `mdk_3ds/tools/emurun.sh --expect hud`, attempt 2 of the current cycle) — REVIEW-1 REMEDIATION: THE HUD'S INK RECT ASKED "IS IT INSIDE THE QUADRANT" AND NEVER ASKED "DID IT REACH THE ANCHOR", SO A HUD DRAWN 130 CANVAS PX OFF ITS OWN ANCHOR PASSED AT rc 0.** Review-1 (`logs/ralph-pi/run-20261114-003546/review-1.md`, on batch 173) had **no Critical and no High**, two actionable Mediums and three Lows; **all five were reproduced against the tree before anything was changed and all five are fixed — nothing is rebutted.** **F1 (Medium), MEASURED ON THE PARENT** with the functions lifted and the shipped marker painted so the marker's arms pass: HUD-palette green at the rect's NEAR corner (the reviewer's fixture) and a HUD displaced **130 canvas px left / 71 px up** of its anchor each printed `HUD INK -- 1512 of 1512 ... 0 outside` at **rc 0**, because every positional arm was ONE-SIDED containment (`bx1 <= X1`, `by1 <= Y1`, `inside <= area`) and the rect's whole point — that its FAR corner is derived from `kPanelRight`/`kPanelBottom` — was never asserted. **THE FIX MIRRORS A PATTERN ALREADY IN THE FILE**: `CANARY_REACH_X/Y` asks `bx1 >= REACH` for the origin-anchored marker, so the far-corner-anchored ink gets `bx1 + HUD_INK_REACH < HUD_INK_X1 -> HUD NOT AT ITS ANCHOR`. `HUD_INK_REACH=12` is DERIVED, not picked: ceil() costs 1 px, the shipped capture's shortfall measures **2 px on each axis** (bbox far 388,232 vs derived 390,234), and 12 is six times that while still only 18 canvas px — a budget at the measured 2 would red the moment the open texture-addressing fix changes which texels carry the colour, and a gate that cannot survive the fix it waits for gets deleted, not tuned. The arm refuses on `<`, so the budget is INCLUSIVE and a fixture sits on the boundary. F1's second half: the `outside` refusal blamed only the LEVEL, but `drawInventory`'s icons (canvas 32..368,328 -> crop 21..245,219) and the bar/skull share the SAME palette and sit outside the rect, so the message now names both producers. **F2 (Medium): `ralph-pi.sh.next` — an obsolete, executable, untracked copy of the loop harness in the repo root** (11913 B, Oct-26 routes `glm-5.3-flash-nvidia`/`minimax-m3.1-flash-nvidia`, no `pipefail`, no `flock`, no `self_sha`/`harness_verdict`, no pre-turn closer, named by nothing in the tree) — deleted; it also made this file's own item-0 instruction ("run `ledger_close.py` **on a clean tree**") impossible to follow. Not gitignored: `.gitignore` is PROTECTED and a stale harness should not exist rather than be hidden. **F3 (Low): THE "ONE SPELLING, TWO BASES" RULE WAS APPLIED TO THE BBOX AND NOT TO THE COUNTS**, and both of its failure modes landed on the counts — measured on the parent, `133 0388 0 ...` aborted the whole harness at an unnamed rc 1 with `0388: value too great for base` (the hazard the block's own comment says `10#` prevents) and a legitimate `0133 0133 00 ...` was REFUSED as MALFORMED because `$((0133))` is 91 while `[ 0133 -le N ]` is 133. Fixed by normalising `total`/`inside`/`outside` and the four bbox fields with `10#` once, after they are known to be digits. **F4 (Low): BATCH 173'S DECOMPOSITION DID NOT ADD UP, AND IT IS NOW MEASURED** — §4 said the digits were both 60 px and 74 px, and 74 + 76 = 150 > 134 for two sprites that cannot overlap by 16 of their own pixels; re-measured on this batch's capture, 134 total, **58** inside the digits' bbox (359,208)-(369,215), **76** outside, and that 76 is exactly §4(b)'s "digits removed -> 76", so the panel's share is settled twice over and **74 is arithmetically impossible**. §4(a)'s 60 came from a different capture and is within 2 px of 58; that mutant was not rebuilt to chase 2 px and the record says so. Appended as §10 of batch 173's evidence file, not rewritten; `HUD_INK_MIN=25` is a floor on the TOTAL, which every measurement agrees on. **F5 (Low): A REJECTED RUN ANNOUNCED THE HUD WAS VISIBLE** — `HUD VISIBLE` printed immediately before the ink refusal (review-3 F3's class, one arm over); the line always judged the RENDERER's marker and now says `HUD MARKER VISIBLE`, and `USAGE` now states that `--expect hud` requires the HUD's OWN palette colour at the HUD's own anchoring. **THE FIXTURE TEST CAUGHT THIS BATCH'S OWN FIRST CUT** — the two new ink fixtures omitted the marker, so the marker's arm rejected them first and neither new message printed: "a row that passes for the wrong reason", caught by the file's own discipline. **`EmurunHarness` is 283 -> 296 checks.** **THE FOUR-WAY:** new on working rc 0 (296); new on the PARENT harness rc 1 — its fixture guard refuses to build from a harness declaring no `HUD_INK_REACH`, collapsing the row to one red, so per-arm non-vacuity is the reviewer's own method instead (the parent's lifted functions return rc 0 on both displaced fixtures and a bash abort on the padded counts; the new ones refuse by name); PARENT test on WORKING sources rc 0, 283 checks, **zero collateral**; parent on parent rc 0 — rows 3 and 4 must run from inside `tests/scripts/`, because `ROOT` comes from the script's own location. **GATES:** `make -C tests/scripts -j8` rc 0, `All cases match the oracle.`, **805 PASS / 0 FAIL** (was 792; +13 is exactly these rows), reading `PASS EmurunHarness (296 checks…)`. `DISPLAY=:99 bash tools/check.sh` rc 0, `CHECK: ALL GATES PASSED`, `VERDICT: CONTRACT CLEAN -- 0 violations.`, `PASS L3_f1.bmp sha=58615fc85d02e009`, `BATCH INTEGRITY: PASS`, `COMPLETION SELFTEST: PASS (13 arms)`, `COMPLETION[d1]: 11 done / 13 total / 0 owner-blocked / 2 open`, `COMPLETION: LEDGER VERIFIED`, `audit_c_with_classes 0 violations`, `SKIP gates_smdh`. `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds -j8` rc 0 with `mdk.elf` md5 **`72328e9d5fd439d7f75703f5c28ccb80` UNCHANGED** — the only `mdk_3ds/**` file in the diff is a host shell script, so nothing new reached the ARM binary; no C++ changed, so no §10/R-rule exposure and no golden moved. **P8 OWED AND RUN** on the final build: `DISPLAY=:99 mdk_3ds/tools/emurun.sh --expect hud --shot …` -> **rc 0**, display `:99`, Mandarine r1.2.1 md5 `0d2adab6`, `graphics_api=1`, `render_3d=0`, `flow: screen=4 level=7`, frames 2460 by the deadline / 2580 after the reap, `canvas: quads=4 draws=2 mesh=2 corners=24 eyes=1 (canvas frame 2520)`, `HUD INK -- 134 of 134 … its far corner within 12 px of the derived 390,234`; emulator-green is not hardware-pass and no console was used. Frame `docs/frames/2026-11-14-a-rect-that-only-asks-inside.png` is byte-identical to its predecessors (census re-derived: 192 PNGs, 191 at md5 `6769a137…`, the one outlier being the pre-existing `2026-10-30-depth-pull.png`) with **zero** pure (0,0,255) pixels over 219 distinct colours — nothing on any render path changed, so a changed frame would have been the surprise. `cmake --build build-linux -j8` was re-run BY HAND (item 11b) and **found a genuinely stale binary**: `build-linux/mdk` was 16:50 against `hud_view.cpp` at 18:31, and the rebuild recompiled it — the frame `gates_frames` certifies is now built from the tree it describes. Prior-solution lookup: pi-knowledge `mdk-sdl` surfaced SPEC §7.3 (no PICA palette format) and `renderer_c3d.cpp`'s own linear-vs-tiled claim as the lead for item 1; **Obsidian and Cognee MCP are ABSENT from this session's tool list** (AGENTS §9 P9: a session fault, recorded, not papered over; vault read read-only from disk — the forsaken `citro3d-renderer-architecture-and-vertex-pipeline` note's Morton-tiling section is recorded as a LEAD for item 1, not as an answer, because it is the 16-bit GPU_RGBA4 path and this tree ships RGBA8). `/tmp/kilo` still not writable by this lane (captures to `/tmp/mdk-b174-logs/`); `Xvfb :99` was already up and was left alone. Full record: `docs/evidence/2026-11-14-174-a-rect-that-only-asks-inside.txt`.

**NEXT, in order (batch 174; the list below this one is batch 173's and is superseded).**
0. **Two owner acts, and neither is a batch.** (a) `chgrp development docs/COMPLETION_LEDGER.tsv` plus `python3 tools/runner/ledger_close.py --row 3ds.hud-canvas` (and `--row linux.render-frame`) from a lane in the `development` group on a clean tree — re-measured this batch: the ledger is `coleshores:coleshores 0664`, unwritable here, and `ledger_close.py` is PROTECTED. **The tree is now genuinely clean** (batch 174 deleted the stray `ralph-pi.sh.next` that made this instruction impossible to follow). Both rows' acceptances are green: `--expect hud` rc 0 re-run on this batch's final build, `bash tools/check.sh` rc 0 here and in batch 172. (b) **review-1 F1 of run-20261113-190705**: decide the door for loop-infrastructure commits that must edit `tools/runner/ledger_close.py` — owner-approved re-land, an infra exemption in `gates_batch_integrity.py`'s PROTECTED check, or a revert.
1. **THE PANEL SPRITE IS SAMPLED WRONG BY THE GPU, AND THAT IS THIS ROW'S REAL REMAINING SLICE** (unchanged from batch 173, deliberately not started here). Measured: ~4x too many texels across the panel's width; the CPU texels, the quad's vertices and the uv transform are all verified correct; synthetic patterns written into the GPU's texture buffer do not come out as written (a half-left/half-right pattern arrives as top/bottom bands, which rules out "just a scale" and points at the axis/stride mapping itself). **What is needed is the GPU-side UV->texel rule, established and not guessed.** `C3D_Tex` carries no stride field (measured: `/opt/devkitpro/libctru/include/c3d/texture.h` is `data/fmt/size/dim/param/border/lodParam`), and **this host has no citro3d source at all** — only the header and `lib/libcitro3d.a` — so the rule comes from upstream citro3d or from disassembling `C3D_TexInitWithParams`/`C3D_TexBind`, and it must be pinned with a pattern fixture BEFORE any scale is touched. **A cheaper first step nobody has tried: the padding arithmetic is checkable on the host without the GPU** — `nextPowerOfTwo` gives the padded dims, the draw path's `scaleX = realWidth/paddedWidth` is plain arithmetic, and a factor of exactly 4 on an RGBA8 (4-bytes-per-texel) texture is the shape a bytes-vs-texels confusion makes; measure the panel's real/padded dims and the emitted `uvtransform` before going near libcitro3d. **The scene textures are probably affected too.** **Do not ship a /4**: measured, with the panel's uv divided by 4 the digits stop reading "100".
2. **`HUD_INK_MIN=25` AND `HUD_INK_REACH=12` ARE CALIBRATED AGAINST TODAY'S MIS-SAMPLED PANEL.** Both are floors/shortfalls measured on the 134-pixel shipped capture (58 digits + 76 panel). The texture-addressing fix in item 1 will change which texels carry (0,255,0), so re-measure BOTH on the fix build before trusting either — a drop past 25 would red the row's acceptance and be diagnosed as `HUD NOT PAINTED`, and a shortfall past 12 as `HUD NOT AT ITS ANCHOR`. Review-1's open question 1 says the same thing.
3. **DO ANY INVENTORY ICONS CARRY (0,255,0)?** (review-1 open question 2, unresolved and not resolvable from the tree.) `drawInventory`'s icons are drawn from the same palette and crop x 21..245 straddles the ink rect's near bound, so a green icon would produce `HUD INK NOT UNIQUE` for a HUD-side reason. The shipped capture has 0 outside, so nothing is wrong today; answering it needs a run with items in the slots or the sprite assets. The refusal now names both possible producers, so a false red at least points at the right trees.
4. **THE DIGITS ARE FINE — do not go "fixing" them.** `drawNumber` builds its own `CanvasQuad`s and the shipped capture shows a crisp "100" at crop (359,208)-(369,215), 58 px of it the HUD's palette colour. Every older note saying the digits are smeared was describing the panel sprite behind them.
5. **The ink rect's near bound is a bound, not a rectangle.** `panel_.w`/`panel_.h` come from the sprite asset, so no source derivation can produce the panel's near corner; the quadrant bound holds only while the panel fits inside half the canvas on each axis (shipped: 84x63 against 600x360). The far corner — the one the reach arm now judges — IS derived, which is what makes the reach arm sound.
6. **`/tmp/kilo` is not writable by this lane** (environment, re-measured: `touch` -> Permission denied, `coleshores:coleshores 0775`). AGENTS §11 P7 says capture to `/tmp/kilo`; it is impossible here, so this batch captured to `/tmp/mdk-b174-logs/`. Either the operator makes the path group-writable for `development` or P7's path changes.
7+. **Unchanged from batch 173's list below:** the ledger-closer door, the range-less `BATCH INTEGRITY: PASS`, `positiveCount`'s unobservable ERANGE branch, `mdk.3dsx` not being a reproducible fingerprint, review-4 F1-F5 (all `linux.render-frame`, deferred with cause), the extent arm's zero-pixel margin, the un-gated HUD dial rect, the ±7 % canvas-width pin, the un-freshness-checked explicit `--shot`, the uncommitted emulator capture, the ROOT-window grab on a shared display, and the missing Khronos validation layer.


**PREVIOUS BATCH 173 RECORD** (its NEXT list is superseded by batch 174's above; its findings and disposition are in `docs/evidence/2026-11-14-173-the-verdict-could-not-see-the-hud.txt`. **BATCH 173 (ledger row `3ds.hud-canvas`, acceptance `mdk_3ds/tools/emurun.sh --expect hud`, attempt 1 of the current cycle) — THE ACCEPTANCE WAS GREEN ON A BUILD THAT PAINTS NO HUD, BECAUSE EVERY PIXEL IT JUDGED WAS DRAWN BY THE RENDERER.** Not a review remediation: review-4 is the newest review, it has **no Critical and no High**, and all five of its findings are `linux.render-frame` work, so they are deferred with cause and not dragged into this row. The slice is this row's own item 2, open since batch 147. **THE MUTANT, RUN BEFORE ANYTHING WAS CHANGED:** `x += 600.0f` in `hud_view.cpp`'s `drawImage` AND `drawNumber`, so the HUD queues all four quads and lands every one off the right edge of the canvas. Parent harness: `canvas: quads=4 draws=2` and `HUD VISIBLE` at **rc 0**. This batch's harness, same binary and same log: the same three lines, then `HUD NOT PAINTED` at **rc 7**. The log arm could not see it (the quads really were queued); the marker arm could not see it, because the (0,0,255) marker is `renderer_c3d.cpp`'s `drawDiagnostic` and the HUD contributes nothing to it. **THE INSTRUMENT IS THE HUD'S OWN PALETTE COLOUR.** Both HUD sprites are drawn with `q.palette = palette_`, and level 7's palette contains exactly (0,255,0), which the level's geometry never produces in the panel: measured **134 such pixels in the whole 400x240 panel, 134 inside the HUD's rect, 0 outside**, bbox crop (333,206)-(388,232). `hud_ink_verdict` adjudicates total/inside/outside/bbox with the arm discipline of five reviews (constants before comparisons, `inside + outside == total`, `inside <= rect area`, a bbox that cannot contradict its own count, `10#` so a padded field is one measurement and not octal 219 or a script abort), and the `outside` arm refuses the verdict if the level ever makes that colour itself. **THE RECT IS DERIVED, AND THE DERIVATION IS AN IMPORT:** far corner from the imported `mdk_render_canvas_width` plus `hud_view.cpp`'s own `kPanelRight`/`kPanelBottom` — canvas (584,350) -> crop (389.33,233.33) -> ceil (390,234); **the near corner is NOT derivable from source** (`panel_.w`/`panel_.h` come from the sprite asset), so it is the anchoring's weakest guarantee — the bottom-right quadrant, crop (200,120) — and is written as a bound, not dressed as a rectangle. The host test re-derives all four, and the existing `mdk_render_canvas_width` axis-swap mutation must now move the ink rect's far corner too. **TWO CLAIMS THIS ROW HAD WRONG, CORRECTED IN PLACE:** (a) **the health number reads "100" and is legible** — `drawNumber` builds its own quads, so the first mutation (which shifted only `drawImage`) left a crisp "100" behind; the scrambled element is the **panel sprite**, and every earlier note saying "the digits are smeared" was describing the panel behind them; (b) **the ink is both sprites** (74 of 134 survive the panel going off-screen, 76 survive the digits being removed), so the instrument is the HUD's palette colour and not the digits' colour. **WHAT IS STILL BROKEN:** the panel sprite arrives as a fine vertical stripe at ~5.3 crop px where the geometry says 21.3 — a factor of 4 — with the CPU texels, the vertices (pos (500,287)-(584,350), uv 0..1) and the uv transform's arrival in the shader all verified correct, and synthetic patterns written straight into the GPU's texture buffer not coming out as written (a period-32 wave renders ~4x too many bands; a left/right half renders as top/bottom bands). A uv/4 "fix" was tried and **rejected on measurement**: with the panel's uv divided by 4 the digits stop reading "100". `EmurunHarness` is **257 -> 283 checks**, and every pre-existing fixture now paints the ink too — the new arm red four of them for exactly the hole they had. **GATES:** `make -C tests/scripts` rc 0, **792 PASS / 0 FAIL** (was 766; +26 = the new rows exactly); `DISPLAY=:99 bash tools/check.sh` rc 0, `CHECK: ALL GATES PASSED`, `PASS L3_f1.bmp sha=58615fc85d02e009`, `BATCH INTEGRITY: PASS`, `COMPLETION SELFTEST: PASS (13 arms)`, `COMPLETION[d1]: 11 done / 13 total / 0 owner-blocked / 2 open`, `COMPLETION: LEDGER VERIFIED`, `audit_c_with_classes 0 violations`, `SKIP gates_smdh`; `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds -j8` rc 0 with `mdk.elf` md5 `72328e9d5fd439d7f75703f5c28ccb80` **UNCHANGED** — correct, the only `mdk_3ds/**` file in the diff is a host shell script. **P8 owed and run on the final build:** `--expect hud` -> **rc 0**, Mandarine r1.2.1 md5 `0d2adab6`, `:99`, `graphics_api=1`, `render_3d=0`, `flow: screen=4 level=7`, frame 480/600, `HUD INK -- 134 of 134 HUD-palette pixels inside crop 200,120-390,234 (bbox 333,206-388,232, 0 outside)`; emulator-green is not hardware-pass and no console was used. Frame `docs/frames/2026-11-14-the-verdict-could-not-see-the-hud.png` is byte-identical to its predecessors (census re-derived: 191 PNGs, 190 at `6769a137…`) with zero pure (0,0,255) pixels over 219 colours. Prior-solution lookup: pi-knowledge `mdk-sdl` surfaced the batch-147 register-word-order closure and nothing on a HUD-pixel canary; **Obsidian and Cognee MCP are ABSENT from this session's tool list** (AGENTS §9 P9: a session fault, recorded, not papered over; vault read read-only from disk). **Environment:** `/tmp/kilo` still not writable by this lane (captures to `/tmp/mdk-b173-logs/`), and `Xvfb :99` was not listening and was started by this lane per P7. Full record: `docs/evidence/2026-11-14-173-the-verdict-could-not-see-the-hud.txt`.

**PREVIOUS BATCH 172 RECORD** (its NEXT list is superseded by batch 173's above; its findings and disposition are in `docs/evidence/2026-11-13-172-no-picture-and-no-stale-one.txt`). **BATCH 172 (ledger row `linux.render-frame`, acceptance `bash tools/check.sh`, attempt 8 of 8) — REVIEW-3 F1: AN EXIT CODE REPORTS A FAILED CAPTURE, IT DOES NOT UN-WRITE THE DESTINATION.** Remediation batch on review-3 (`logs/ralph-pi/run-20261113-190705/review-3.md`, on `94ce8d35..bfc7c3e5`), VERDICT COMPLETE with **no Critical and no High**, so nothing was mandatory; the slice is its F1 (Medium), and it was reproduced on the PARENT binary before anything was changed: seed a valid frame at the capture path, make the write fail, and the run exits 1 with the PREVIOUS frame still sitting at that path at exactly the sha `tests/frames/reference.sha256` expects — batch 171 closed the reporting half of its own charter's hazard and left the bytes half live. **THE EXISTING TEST COULD NOT SEE IT, AND FOR A STRUCTURAL REASON:** `expect_capture_refusal` runs `rm -f "$file"` before invoking the binary, so every capture-refusal arm starts from an EMPTY path; an arm that wipes the path cannot observe a stale file surviving it. The four new arms are the first in the file that seed a file first. **THE FIX IS IN TWO PLACES BECAUSE THERE ARE TWO WAYS TO LEAVE A LIE AT A PATH.** (a) `mdk_linux/src/main.cpp`'s `clearCaptureDestination` removes a regular file at the destination when the run COMMITS to the request — after every argument refusal, before the first thing that can fail — so a failure at renderer creation, level load, readback or write leaves no picture instead of a stale one; this is the rule `tests/frames/capture_oracle.sh:171` already applies to its own output (`rm -f "$OUT"`), and it is deliberately `stat` + `S_ISREG` + `unlink` and NOT `std::remove`, which would rmdir an empty directory and — with `--capture=/dev/full`, this host's way to force a write failure — could unlink a device node. (b) `writeBmp` publishes by `rename`: the bytes go to `<path>.part` and only a complete write is renamed onto the destination, so a short write or a failed close leaves the destination exactly as it found it and unlinks the partial file this function made — and a truncated BMP is precisely a file that exists and is non-empty, which is the test a capture harness reaches for. **A SIDE EFFECT WORTH STATING: THE REVIEWER'S OWN REPRO NOW SUCCEEDS** (`unlink` needs the directory's write permission, not the file's): rc 0 and the pinned sha where the parent printed `could not open` and left the old bytes. **THE FOUR ARMS (27 → 31) ARE ALL PERMISSION-FREE**, for the reason the file already gives: a chmod arm goes red on a root lane for a reason unrelated to the code. 26 seeds a stale frame and makes the renderer impossible to create (`VK_ICD_FILENAMES=/nonexistent`), asserting rc ≠ 0, that the run failed WHERE IT WAS MADE TO — a lane with no Vulkan would otherwise pass the arm for the wrong reason — and that the path holds nothing; 27 is the mirror that keeps the fix from being too eager (an invocation the parser REFUSES must leave the file alone, so the clearing has to come after validation); 28 proves the clearing did not cost the run its picture (starts `BM`, ≥ 1000 bytes, no `.part` left); 29 pins the S_ISREG guard and the scratch-file cleanup at once (`--capture=/dev/full` fails, `/dev/full` is still a character device, no `/dev/full.part`). **NON-VACUITY, RUN:** new test on working sources 31 arms rc 0; new test on PARENT sources (both files from `git show HEAD:`, rebuilt, the restore inside a `trap … EXIT`) **rc 1 with exactly 1 of 31 failing** — arm 26, in the finding's own words — and the other 30 green, which is the collateral row in the same measurement. **F2 (Low) CONFIRMED AND ADOPTED AS A RULE:** `mdk.3dsx` is not reproducible (this batch's build gives `2bd6d2d6…`, one of review-3's three different values for one tree), so the published 3DS fingerprint is `mdk.elf`'s md5 only; `mdk.elf` stayed `72328e9d5fd439d7f75703f5c28ccb80` across every build here. **F3 (Low) CONFIRMED AND DEFERRED WITH CAUSE, no code changed:** the "asked for frame 1 and 0 of 0 requested" line is a real self-contradiction (both backends drop the request when the frame never began), but that sentence is pinned by three canonical-text mutants of `LinuxRefusalReport`'s F4, so changing it is a test-integrity edit of its own and the harm today is nil; the zero-risk half was done instead — `render.h` now documents that `requested` is a BACKEND count and that "0 of 0" is not evidence nothing was asked. **GATES:** `make -C tests/scripts -j8` rc 0, `All cases match the oracle.`, **766 PASS / 0 FAIL** — unchanged and correctly, since this test contributes ONE `PASS LinuxArgParse` line, now reading `(31 arms…)`; `DISPLAY=:99 bash tools/check.sh` rc 0, `CHECK: ALL GATES PASSED`, `PASS L3_f1.bmp sha=58615fc85d02e009`, `BATCH INTEGRITY: PASS`, `COMPLETION SELFTEST: PASS (13 arms)`, `COMPLETION[d1]: 11 done / 13 total / 0 owner-blocked / 2 open`, `COMPLETION: LEDGER VERIFIED`, `SKIP gates_smdh`, `audit_c_with_classes` 0 violations; `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds -j8` rc 0 with `mdk.elf` md5 `72328e9d5fd439d7f75703f5c28ccb80` **UNCHANGED** — a measurement with content, because the 3DS target DOES compile `render.h`, so an unchanged elf proves this batch's seam edit was comment-only for codegen; no `mdk_3ds/**` path is in the diff, so no P8 emulator evidence is owed and none was faked. `cmake --build build-linux -j8` rc 0 by hand five times (item 11b), and the binary is NOT byte-reproducible on this host (three md5s for identical sources), so it is verified by its tests and the frame sha and not by its own hash. Frame `docs/frames/2026-11-13-the-destination-is-cleared.png` is byte-identical to its predecessors (census re-derived: 190 PNGs, 189 at `6769a137…`) with **zero** pure (0,0,255) pixels over 219 colours — the capture's bytes never changed, only the order in which they reach the disk. Prior-solution lookup: Pi knowledge (`mdk-sdl`) surfaced this loop's OWN prior solution to the class — `ralph-pi.sh`'s `state_write`/`state_apply`, temp file plus checked rename, "a write that did not happen is never announced as done" — and `capture_oracle.sh`'s `rm -f "$OUT"`; both were reused as the shape of this fix instead of being reinvented. **Obsidian and Cognee MCP are ABSENT from this session's tool list** (AGENTS §9 P9: a session fault, not a licence to claim the lookup), vault read read-only from disk with no prior solution found. Full record: `docs/evidence/2026-11-13-172-no-picture-and-no-stale-one.txt`.

**NEXT, in order (batch 172; the list below this one is batch 171's and is superseded).**
0. **The row is one `chgrp` from `done`, and this was attempt 8 of 8, so the loop stops for an operator.** `chgrp development docs/COMPLETION_LEDGER.tsv`, then `python3 tools/runner/ledger_close.py --row linux.render-frame` (and `--row 3ds.hud-canvas`) from a lane in the `development` group on a clean tree. Re-measured this batch: `coleshores:coleshores 664`, `os.access(..., W_OK)` is False, and `ledger_close.py` is PROTECTED. Both rows' acceptances are green (`bash tools/check.sh` rc 0 here; `mdk_3ds/tools/emurun.sh --expect hud` rc 0 in batch 162, re-run independently by review-3).
1. **review-1 F1** — decide the door for loop-infrastructure commits that must edit `tools/runner/ledger_close.py`: owner-approved re-land, an infra exemption in `gates_batch_integrity.py`'s PROTECTED check, or a revert (the worst of the three: it restores the closer hang and the arbitrary-tree banking). That review's F4/F5/F6 ride along: all three are in `ledger_close.py` or `.gitignore`, both PROTECTED.
2. **review-3 F3 (Low), deferred:** the "asked for frame 1 and 0 of 0 requested" line. Fixing it means re-basing three canonical-text mutants of `LinuxRefusalReport`'s F4 (`capture-verdict-not-read`, `capture-verdict-printed-and-dropped`, `capture-verdict-not-returned`) and re-proving them against the new text; the meaning is now documented in `render.h`, so the imprecision cannot be mistaken for the contract.
3. **`mdk.3dsx` is not a reproducible fingerprint** (review-3 F2, adopted as a rule): publish `mdk.elf`'s md5 only. Still unisolated: which staging input varies (review-3 open question 2).
4. **`/tmp/kilo` is not writable by this lane** (environment, re-measured: `touch` → Permission denied). AGENTS §11 P7 says capture to `/tmp/kilo`; it is `coleshores:coleshores 0775`. Either the operator makes it group-writable for `development` or P7's path changes.
5. **The frame record's `BATCH INTEGRITY: PASS` is range-less and cannot see PROTECTED** (batch 170's item, unchanged): `tools/check.sh:175` calls `gates_batch_integrity.py` with no `--base`, so the protected-file and per-commit-frame rules never run in the gate set; both fixes are owner-protected files.
6. **`positiveCount`'s ERANGE branch is unobservable on this host** (batch 170's item, unchanged): arms 17/18 pin the refusal and not the mechanism; proving the mechanism needs an ILP32 or `-m32` build of the Linux frontend, which this loop does not make.
7-10. **Unchanged from batch 169's list below:** the extent arm's zero-pixel margin, the ungated HUD dial rect, the ±7 % canvas-width pin, the un-freshness-checked explicit `--shot`, the uncommitted emulator capture, the ROOT-window grab on a shared display, and the missing Khronos validation layer.
**PREVIOUS BATCH 171 RECORD** (its NEXT list is superseded by batch 172's above; its findings and disposition are in `docs/evidence/2026-11-13-171-the-picture-it-could-not-write.txt`). **BATCH 171 (ledger row `linux.render-frame`, acceptance `bash tools/check.sh`, attempt 7) — REVIEW-2 F1: THE CAPTURE PARSED, WAS REACHABLE, AND STILL WROTE NOTHING — AND THE RUN SAID 0.** Remediation batch on review-2 (`logs/ralph-pi/run-20261113-190705/review-2.md`, on `7bbb193e..94ce8d35`), VERDICT COMPLETE with **no Critical and no High**, so nothing was mandatory: **F1 (Medium) is this row's own class and is the slice**, and F2/F3 (Low) rode along because they were cheap. **F1 WAS MEASURED ON THE COMMITTED BINARY FIRST**, exactly as written: `--capture=/nonexistent-dir/x.bmp --capture-frame=1 --frames 2` → **rc 0**, `[vk] screenshot: could not open …`, no file; same rc 0 when the path is a directory. One discarded bool is the whole cause: `captureSwapchain()` returns bool, `mdk_render_end_frame` called it as a statement, `main` returned 0 unconditionally. Batch 170 closed the reachability half; this is the writability half, and it is worse than a wrong exit code because the harness upstream checks that the file it asked for **exists**, finds the BMP the previous run left there, and commits it as this build's frame. **THE FIX IS AN ACCOUNTING AND NOT A PRINTF**: `RendererVk` keeps `capturesRequested`/`capturesWritten`, the write counter rising **only** where `captureSwapchain`'s own result decided it (a failed open, a short write, a failed readback, an unmappable image, an unsubmitted frame — all five), a new seam query `mdk_render_capture_stats` hands both over, and `main` fails the run when it asked for a picture and got none (`captureFrame > 0 && captureWritten < 1`), stated against what the RUN ASKED FOR because "the write failed" and "the frame was never reached" are one failure from a harness's side. **WHY BOTH BACKENDS, AND WHY `mdk_3ds/**` IS IN THIS DIFF**: measured first, every `mdk_render_*` name in `render.h` is defined by both backends, so a query declared in the shared header and defined in one is a link error waiting for the next batch and `gates_core_parity` compares source LISTS, not symbols — review discipline only. The PICA path has no readback of a rendered target, so it counts a request and can never report a write: the honest answer, following the `mdk_render_gpu_wait_ms` "the 3DS backend reports 0" precedent in the same header, and not a stub — a backend reporting 0/0 for a request it ignored would turn the frontend's rule into a pass generator. **TESTS**: `LinuxArgParse` 25 → **27 arms** with a new `expect_capture_refusal` requiring three legs — non-zero, the refusal naming THE CAPTURE, and no picture left behind — the third being what stops a renderer-creation failure from passing the arm vacuously on a lane with no Vulkan; two arms, missing parent (ENOENT) and path-is-a-directory (EISDIR), chosen because they fail in different syscalls and neither depends on permissions (a chmod-555 directory reproduces the lie here but root writes into a read-only directory, so that arm would go red on a root lane for a reason unrelated to the code). **THE FOUR-WAY, RUN**: new+new rc 0 (27); new test + **PARENT binary rebuilt from `git archive HEAD`** → **rc 1, EXACTLY 2 red, both named, 25 ok**; PARENT test + new binary rc 0 (zero collateral); parent+parent rc 0. `LinuxRefusalReport` 21 → **23 checks, 78 arms, 7 probes**: new **F4** (the branch that reports the failure must set the value `main` returns — ask deleted / printed-and-dropped / not-returned, plus a rename probe) and new **B15** (the write counter must sit under a condition naming the call whose result it is, and the query must read its fields — four mutants, plus a nested-`if` probe); **the two existing B11 arms were rewritten** because they mutated text this batch changed (their `replace` would have matched nothing) and because the pre-batch revert reddened two checks and isolated neither. Against the parent sources F4 and B15 fail with their own text. **F2 FIXED**: the PASS line now claims what the arms pin and states the `--size=320x240junk` carve-out out loud. **F3 FIXED**: `docs/OPEN_ITEMS.md`'s recipe now names `--capture=<out>.bmp --capture-frame=60`, and that spelling was RUN (rc 0, 307254-byte BMP). **F4 DECLINED WITH CAUSE, file untouched**: `docs/evidence/README.md:31` — "Never edit a published pair" — and the reviewer agrees the risk is prospective and `gates_completion.py:147-151` reads only the ledger's named field; the disposition is recorded instead. **GATES**: `make -C tests/scripts -j8` rc 0, `All cases match the oracle.`, **766 PASS / 0 FAIL** — unchanged and correctly, each changed test file contributing ONE `PASS` line — reading `PASS LinuxRefusalReport (23 checks, 78 arms adjudicated red, 7 probes green)` and `PASS LinuxArgParse (27 arms: …)`. `DISPLAY=:99 bash tools/check.sh` rc 0, `PASS L3_f1.bmp sha=58615fc85d02e009`, `VERDICT: CONTRACT CLEAN`, `BATCH INTEGRITY: PASS` (range-less: per review-1 F1 it evaluates neither PROTECTED nor the frame rule), `COMPLETION SELFTEST: PASS (13 arms)`, `COMPLETION[d1]: 11 done / 13 total / 0 owner-blocked / 2 open`, `COMPLETION: LEDGER VERIFIED`, `SKIP gates_smdh`, `audit_c_with_classes 0 violations`. `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds -j8` rc 0 with `mdk.elf` md5 **`72328e9d5fd439d7f75703f5c28ccb80` — CHANGED** from `728eb2350c3ef8a81acda15f75617c1e`, the correct outcome for the first batch in this row that compiled code into the 3DS binary. `cmake --build build-linux -j8` rc 0 by hand, three times (nothing in the gate set compiles `mdk_linux/**`). **P8 OWED AND RUN**: `DISPLAY=:99 mdk_3ds/tools/emurun.sh --expect hud` → **rc 0**, display `:99`, `mandarine.AppImage md5 0d2adab6` (r1.2.1), frame **2520** by the deadline / **2640** after the reap, llvmpipe forced, `graphics_api=1`, `render_3d=0` (mono), `backend: PICA200 Citro3D`, `flow: screen=4 level=7`, `canvas: quads=4 draws=2 mesh=2 corners=24 eyes=1 (canvas frame 2520)`, `HUD VISIBLE -- 121 canary pixels in the panel, 121 inside`; emulator-green is not hardware-pass and no console was used. Frame `docs/frames/2026-11-13-the-picture-it-could-not-write.png` is byte-identical to its 187 predecessors (`6769a137…`; 189 PNGs, 188 at that md5). Prior-solution lookup: pi-knowledge `mdk-sdl` top precedent is the sibling tetrisphere-godot `tools/pc/capture.sh` ("no root screenshot was written" → non-zero), found independently by this loop and by review-2; **Obsidian and Cognee MCP are ABSENT from this session's tool list** (AGENTS §9 P9: a session fault, recorded, not papered over; vault read read-only from disk). **Environment**: `/tmp/kilo` still not writable by this lane (captures to `/tmp/mdk-batch171-logs/`), and `Xvfb :99` was NOT listening and was started by this lane per P7. Full record: `docs/evidence/2026-11-13-171-the-picture-it-could-not-write.txt`.

**NEXT, in order (batch 171; the list below this one is batch 170's and is superseded).**
0. **Two owner acts, and neither is a batch.** (a) `chgrp development docs/COMPLETION_LEDGER.tsv` plus `python3 tools/runner/ledger_close.py --row linux.render-frame` (and `--row 3ds.hud-canvas`) from a lane in the `development` group on a clean tree — the ledger is `coleshores:coleshores 0664`, unwritable here, and `ledger_close.py` is PROTECTED; both rows' acceptances are green. (b) **review-1 F1**: decide the door for loop-infrastructure commits that must edit `tools/runner/ledger_close.py` (owner-approved re-land, an infra exemption in `gates_batch_integrity.py`'s PROTECTED check, or a revert). review-2's F4/F5/F6 ride along: all three are in `ledger_close.py` or `.gitignore`, both PROTECTED.
1. **Is a write-failure exit code a product decision?** (review-2's open question 1.) This batch answered it for the Linux frontend in the direction the sibling `capture.sh` precedent and this file's own charter point — a capture harness that reports success with no picture is the failure the row exists to refuse. If the owner wants a warning instead of a failure, the one line to change is the `status = 1` in `main`, and `LinuxArgParse` arms 24/25 plus `LinuxRefusalReport` F4 are the pins that would have to move with it, deliberately and not by accident.
2. **The 3DS capture accounting is unreachable from the 3DS frontend.** `renderer_c3d.cpp` counts a request and can never write one, and `main_3ds.cpp` never asks. That is honest today and it is the shape a future 3DS screenshot feature must not forget: the day a 3DS frontend passes `screenshot_path`, it gets "requested 1, written 0" and must fail — which is the correct behaviour, not a bug, and is worth knowing before someone "fixes" the 0.
3. **`/tmp/kilo` is not writable by this lane** (environment, unchanged). AGENTS §11 P7 says capture to `/tmp/kilo`; it is `coleshores:coleshores 0775`. Either the operator makes it group-writable for `development` or P7's path changes. Also new this batch: `Xvfb :99` was not listening and had to be started; if the box is meant to keep `:99` up, that is an operator act too.
4. **The frame record's `BATCH INTEGRITY: PASS` is range-less and cannot see PROTECTED** (batch 170's item 1, unchanged): `tools/check.sh:175` passes no `--base`, so the protected-file and per-commit-frame rules never run in the gate set. Both fixes are owner-protected files.
5. **`positiveCount`'s ERANGE branch is unobservable on this host** (batch 170's item 2, unchanged): arms 17/18 pin the refusal and not the mechanism; proving it needs an ILP32/`-m32` build this loop does not make.
6-9. **Unchanged from batch 169's list below:** the extent arm's zero-pixel margin, the ungated HUD dial rect, the ±7 % canvas-width pin, the un-freshness-checked explicit `--shot`, the uncommitted emulator capture, the ROOT-window grab on a shared display, and the missing Khronos validation layer.

**PREVIOUS BATCH 170 RECORD** (its NEXT list is superseded by batch 171's above; its findings and disposition are in `docs/evidence/2026-11-13-170-the-frame-it-could-not-take.txt`). **BATCH 170 (ledger row `linux.render-frame`, acceptance `bash tools/check.sh`, attempt 6) — REVIEW-1 F3: THE CAPTURE FRAME PARSED, AND NOTHING ASKED WHETHER THE RUN COULD REACH IT. PLUS A CRITICAL THAT IS REAL, OWNER-PROTECTED, AND NOW SURFACED INSTEAD OF FILED AWAY.** Remediation batch: review-1 (`logs/ralph-pi/run-20261113-190705/review-1.md`, on `7d46ba4c..7bbb193e`) had **F1 Critical, F2 High, F3/F4/F5/F6 Medium, F7/F8 Low** and a VERDICT of INCOMPLETE. **Every one of the eight was reproduced or rebutted against the tree before anything was changed; none was accepted on the reviewer's word and none was waved off.** **F3 (Medium) IS THIS ROW'S OWN CLASS AND IS THE SLICE, and it was measured on the parent binary first:** `--capture=/tmp/rv_a.bmp --capture-frame=99 --frames 2` exits **0 and writes no file** (the frame bound breaks before frame 99), and `--capture-frame=1` with no `--capture` also exits **0 and writes nothing** (the loop breaks on the capture frame whether or not a path exists). The value PARSED, so no batch-169 arm could see either half — a different defect, one step further along. **THE FIX IS TWO REFUSALS AFTER THE WHOLE OPTION LOOP**, not in the option branches, because `--capture-frame=99 --frames 2` and `--frames 2 --capture-frame=99` are one request and a branch-local check catches only one order; the reversed order is its own arm so the placement is pinned and not asserted. The bound is `>` and not `>=` — the loop increments then tests, so a capture frame equal to the bound is the last reachable frame — and **arm 23 runs that boundary as a real capture**, because a rule bought one value too wide passes every reject arm and kills a legal invocation. Side effect named in the code: a non-zero capture frame now always has a path behind it, so the loop's `captureThisFrame` break can no longer truncate a run for nothing; the loop was not edited, the parser made the shape unreachable. **F7 (Low) FIXED WITH ITS LIMIT STATED:** `positiveCount` proved `[1, INT_MAX]` with a `long`, and where `long` is 32 bits `LONG_MAX == INT_MAX`, so strtol clamps, sets ERANGE, and the bound test cannot fire — the fail-open this predicate exists to remove, arriving through the type instead of the parse. `errno = 0` + `errno == ERANGE` now refuses it, in `positiveCount` and in the sibling `--seed` path the reviewer named. **HONEST LIMIT, measured:** on x86-64 the old bound already refused both overflow values, so arms 17/18 PASS ON THE PARENT TOO; they pin the refusal and do NOT prove the ERANGE branch, which is unobservable on this target and is not claimed to be. **F8 (Low) FIXED:** the PASS line claimed "a honoured frame count and a real capture still work" on a lane with no game data where those four arms are SKIPPED and never ran — a coverage claim that is simply false — so the summary now branches on `$skipped` and says SKIPPED with its count, and the never-used `total` is gone. **F1 (Critical) CONFIRMED, AND A BUILDER BATCH CANNOT CLOSE IT.** `gates_batch_integrity.py --base 7d46ba4 --head 7bbb193 --each-commit` → **FAIL: protected contract changed without owner approval: tools/runner/ledger_close.py**, plus `50231c37 … must add exactly one frame PNG; found 0`, and `ATTENTION.log` already recorded it as "owed". The reviewer's second half holds too: `iter-1.log:18` publishes a PASS from `--base 50231c3`, which IS the offending commit, and the range-less `BATCH INTEGRITY: PASS` that `tools/check.sh` prints evaluates neither PROTECTED nor the frame rule (check.sh:175 passes no `--base`; both rules live under `if args.base:`). Neither is a lie; together they are a record that hid a failure the loop itself had logged — **and that part is fixed here, in this file and the evidence file, which now publish the failing window and the ATTENTION line.** The code half is unreachable from a batch: both remedies (revert, or re-land with `--owner-approved`) edit `tools/runner/ledger_close.py`, which is in `PROTECTED`, so the repair trips the very gate that protects it. **And the gate has no door at all:** its infra regex `^(loop|ledger|owner)\b` exempts an infrastructure subject from the FRAME rule only, never from PROTECTED — measured, `--base 222f4f3 --head 2a71883 --each-commit` FAILS the same line on a properly `loop:`-prefixed commit. So the honest framing is not "a builder edited its own closer" but "loop-infrastructure commits edit the closer and the gate that protects it has no legitimate door", which is an owner decision with three doors in §6 of the evidence file; and what `50231c3` actually changed in the closer is `timeout=120` and the banking allowlist — **not** the ledger, not the acceptance read, not the row rewrite — so the power PROTECTED guards against was not widened by one line. Mitigation, not rebuttal: F1 stays open and owner-blocked. **F2 (High) CONFIRMED AS A HISTORICAL FACT AND NOT REWRITTEN:** `git show 50231c3:tests/scripts/linux_arg_parse_test.sh` → rc 128, and that commit added 0 frames, so HEAD was red at it; the reviewer's fix is a history rewrite of two commits that the review, ATTENTION.log, the state file and this batch's parent all name by SHA, and the audit trail that made F1 findable is the thing it would destroy. HEAD is green and re-measured. **F4 (Medium) HALF REBUTTED WITH EVIDENCE:** `subprocess.TimeoutExpired` IS a subclass of `subprocess.SubprocessError` (measured `True`) and `ledger_close.py`'s module guard catches exactly that pair, so a bounded timeout already prints a named refusal and exits 2 — there is no traceback; the wait-after-kill half stands, and `rpc_wait_bit_killable` is TASK_KILLABLE, so the kill does release the stall that was actually observed. **F5 (Medium) CONFIRMED** (`safe_untracked` names `docs/evidence/` and not `docs/frames/`) with one mitigation: a banking commit carrying no frame is itself reddened by the per-commit frame rule — the second error in F1's own gate output — so the failure mode is a loud FAIL, not a silent certification. **F6 (Medium) CONFIRMED** (2888 `.progress-log/` files, no `.gitignore` entry). F4/F5/F6 all live in PROTECTED files (`ledger_close.py`, `.gitignore`) and are deferred with cause as owner acts. **`LinuxArgParse` is 19 arms → 25 arms.** **THE FOUR-WAY:** new test on working sources rc 0 (25 arms); new test on the PARENT binary **rc 1 with EXACTLY 3 red — arms 14/15/16, all three named, 22 ok** — the row that says the new arms bite on real code, and arms 17/18 green on the parent exactly as §3 predicts; PARENT test on working sources rc 0, **zero collateral**; parent test on parent rc 0. No mutant table was needed because the production shape moved, so the parent row does the work a mutant table does when it does not. **GATES:** `make -C tests/scripts -j8` rc 0, `All cases match the oracle.`, **766 PASS / 0 FAIL** — unchanged and correctly, since this test contributes ONE `PASS LinuxArgParse` line however many arms it adjudicates; this run reads `(25 arms: … two real captures -- the capture-frame boundary included -- still work)`. `DISPLAY=:99 bash tools/check.sh` rc 0, `CHECK: ALL GATES PASSED`, `PASS L3_f1.bmp sha=58615fc85d02e009`, `VERDICT: CONTRACT CLEAN`, `BATCH INTEGRITY: PASS` — read for what it is per F1, a range-less static-invariants call — `COMPLETION SELFTEST: PASS (13 arms)`, `COMPLETION[d1]: 11 done / 13 total / 0 owner-blocked / 2 open`, `COMPLETION: LEDGER VERIFIED`, `SKIP gates_smdh`, `audit_c_with_classes 0 violations`. `make -C mdk_3ds -j8` rc 0 with `mdk.elf` md5 `728eb2350c3ef8a81acda15f75617c1e` **UNCHANGED** — no `mdk_3ds/**` path in `git diff --name-only`, so no P8 emulator evidence is owed and none was faked; the first attempt at this gate failed on `Please set DEVKITARM` because the ad-hoc shell did not export it, and it was re-run with the values `ralph-pi.sh` exports rather than being read as a real failure. `cmake --build build-linux -j8` rc 0 BY HAND (item 11b), three times on purpose: after the edit, with the PARENT source to mint the row-2 binary, and again after restoring. Frame `docs/frames/2026-11-13-the-frame-it-could-not-take.png` is byte-identical to its predecessors (census re-derived: 188 PNGs, 187 at `6769a137…`) with **zero** pure (0,0,255) pixels over 219 colours — the pinned spelling passes `--capture-frame=1` with no `--frames`, so it is exactly the case the new bound must NOT refuse, and a changed frame would have been the surprise. Prior-solution lookup: Pi knowledge (`mdk-sdl`) top hit 0.622 was `tests/frames/capture_cpp.sh`, the pinned capture spelling this batch must not break, checked; nothing on an unreachable capture frame or on the protected-closer question. **Obsidian and Cognee MCP are ABSENT from this session's tool list** (AGENTS §9 P9: a session fault, not a licence to claim the lookup), vault read read-only from disk — `50-tooling/ralph-pi-unattended-loop-script.md:489` records the closer as owner-protected, which is the rule F1 says was crossed. **Two environment faults recorded:** `/tmp/kilo` is `coleshores:coleshores 0775` and NOT writable by this lane, so AGENTS P7's capture directory is impossible here and captures went to `/tmp/mdk-batch170-logs/` on `:99`; and the ledger is still unwritable, re-measured. Full record: `docs/evidence/2026-11-13-170-the-frame-it-could-not-take.txt`.

**NEXT, in order (batch 170; the list below this one is batch 169's and is superseded).**
0. **Two owner acts, and neither is a batch.** (a) `chgrp development docs/COMPLETION_LEDGER.tsv` plus `python3 tools/runner/ledger_close.py --row linux.render-frame` (and `--row 3ds.hud-canvas`) from a lane in the `development` group on a clean tree — re-measured this batch: still `coleshores:coleshores 0664`, still NOT writable, and `ledger_close.py` runs the acceptance and then dies on `[Errno 13] Permission denied` writing the ledger. Both rows' acceptances are green. (b) **review-1 F1**: decide the door for loop-infrastructure commits that must edit `tools/runner/ledger_close.py` — owner-approved re-land, an infra exemption in `gates_batch_integrity.py`'s PROTECTED check, or a revert (the worst of the three: it restores the closer hang and the arbitrary-tree banking). F4/F5/F6 ride along: all three are in `ledger_close.py` or `.gitignore`, both PROTECTED.
1. **The frame record's `BATCH INTEGRITY: PASS` is range-less and cannot see PROTECTED** (new, from review-1 F1). `tools/check.sh:175` calls `gates_batch_integrity.py` with no `--base`, so the protected-file and per-commit-frame rules never run in the gate set; only the loop's own out-of-process `--each-commit` call does, and its failure lands in `ATTENTION.log` where no batch record has to read it. Closing it means either a `--base` in check.sh or a rule that a batch record must publish the ATTENTION delta — both owner-protected files.
2. **`positiveCount`'s ERANGE branch is unobservable on this host** (new, from review-1 F7). The arms pin the refusal, not the mechanism; proving the mechanism needs an ILP32 or `-m32` build of the Linux frontend, which this loop does not make. Stated so nobody later reads arms 17/18 as coverage of the ERANGE fix.
3. **`/tmp/kilo` is not writable by this lane** (new environment fault). AGENTS §11 P7 says capture to `/tmp/kilo`; it is `coleshores:coleshores 0775`. Either the operator makes it group-writable for `development` or P7's path changes. Captures for this batch went to a lane-owned directory and the display is recorded.
4-7. **Unchanged from batch 169's list below:** the extent arm's zero-pixel margin, the ungated HUD dial rect, the ±7 % canvas-width pin, the un-freshness-checked explicit `--shot`, the uncommitted emulator capture, the ROOT-window grab on a shared display, and the missing Khronos validation layer.
it 1; the only caller in the tree, tests/frames/capture_cpp.sh, was checked and still passes.

**PREVIOUS BATCH 169 RECORD** (its NEXT list is superseded by batch 170's above; its findings and disposition are in `docs/evidence/2026-11-13-169-name-it-or-dont-run-it.txt`). **BATCH 169 (ledger row `linux.render-frame`, acceptance `bash tools/check.sh`, attempt 4) — BATCH 168'S LEVEL SWEEP WAS LEVEL 3 SEVEN TIMES, AND THE ARGUMENT BUG BEHIND IT HAD FOUR MORE FAIL-OPENS IN THE SAME CHAIN.** No review finding was open — review-10 was remediated in full by batch 165 — so nothing here is a remediation and nothing is rebutted. **ATTEMPT 4 WROTE THE TEST AND THE RECORD AND THEN LOST THE FIX**: its non-vacuity run restored `main.cpp` from `git show HEAD:` to build the parent binary, and the iteration was cut off before the file went back — so attempt 5 inherited a registered test whose production change did not exist (`grep -c positiveCount mdk_linux/src/main.cpp` = 0 on the inherited tree), re-made it, and re-ran everything this entry claims. The parent restore is now inside a `trap ... EXIT`, because the run that proves a test bites is also the run that can delete the fix. `optionValue()` matches `--name=value`; `--level` is not `--level=`, and an unrecognized argument fell through the parse chain silently and ran the default. So batch 168's "levels 1..7, 300 frames each" never left level 3, and its own table said so: **the same "190 draws, 1469 triangles" seven times**, sitting in the evidence unread. Its second open item — "`--level 99` runs with the same startup draw count as level 3, so an unknown level falls back rather than erroring" — describes no behaviour of the frontend at all: `--level=99` refuses with `must be 3-8`, rc 1. **THE CONCLUSION SURVIVES AND IS NOW RE-MEASURED RATHER THAN COPIED:** the real sweep (`--level=`, 300 frames, seed 1) gives six different scenes and still zero refusals — level 3 190 draws/1469 tri, 4 28/252, 5 107/1682, 6 87/926, 7 153/1042, 8 243/2584. **THE CLASS IS BIGGER THAN ONE ARGUMENT, AND EVERY MEMBER WAS MEASURED ON THE SHIPPED BINARY BEFORE ANYTHING WAS CHANGED.** `atoi` answers 0 for `abc`, `0` and `""` alike, and every reader here takes 0 to mean "no limit" or "never" (`maxFrames > 0` gates the frame bound, `captureFrame > 0` gates the capture), so an unparsed value silently became the OPPOSITE of what was asked: `--frames abc`, `--frames 0` and `--frames -5` each ran UNBOUNDED (a harness that asks for a bounded run and then never stops), and `--capture=p --capture-frame=abc` and `--capture=` each exited 0 having written NO FILE (a capture harness reporting a picture it never took). A trailing `--frames` was rejected while being called "unrecognized", and `--level=abc` was rejected with a message about the range for a value that was never a number. **ONE PREDICATE, NOT FOUR SPELLINGS** — the lesson batch 158 learned one subsystem over with `emurun.sh`'s `decimal_measurement`: `positiveCount(name, text, out)` wants a leading digit, `strtol`, nothing after the digits, and a value in [1, INT_MAX]; `--frames`, `--capture-frame` and `--level` all route through it, `--capture` refuses an empty path, and each refusal names the field and the value. The leading-digit test is not decoration — `strtol` skips leading space and takes a sign, so " 5", `+5` and `-5` all parse without it. **AND THE IN-FLIGHT HELP TEXT WAS ITSELF WRONG**: it named `--seed` as the only option taking a separate value, and `--frames` does too; a refusal message that mis-states the interface it is refusing is a second bug wearing the first one's clothes. **THE TEST IS 19 RUNTIME ARMS AND NOT ONE GREP**, because "this binary, given that argument, exits non-zero and says why" cannot be established by reading source: 15 refuse, 2 accept, 1 frame count honoured, 1 real capture. Three things in it are deliberate. The `timeout 60` watchdog is load-bearing — four refuse arms make the PARENT binary run unbounded, so without it the non-vacuity run would HANG instead of failing, and rc 124 is reported as its own failure mode. Refusal text is matched with `case`, never `printf | grep -q`, because a pipe into an early-exit consumer is the SIGPIPE race batches 161-162 were reviewed for. And `expect_capture` asserts the file EXISTS AND IS NON-EMPTY, not just rc 0, because "did not refuse" and "did the thing asked" are different claims and the refuse arms alone could be satisfied by a frontend that refuses everything. **NON-VACUITY, RUN (re-measured in attempt 5, not copied):** new test on working sources 19 arms / 0 FAIL / rc 0; new test on PARENT sources (main.cpp restored from `git show HEAD:`, rebuilt) **10 of 19 FAIL, rc 1** — 3 unknown-argument, 4 `--frames` (all four the 60 s watchdog firing, which is the watchdog's whole purpose), 2 `--capture-frame`, 1 empty `--capture` — and the 9 that stay green are the rows that were already correct, kept precisely so a regression in `--seed`, the one spelling that always got it right, is caught too. The parent had no version of this test, so the four-way collapses to those two runs plus the whole existing suite on the working tree (gate 1). **STATED, NOT HIDDEN:** `--size=320x240junk` and `--at=1,2,3junk` are still accepted, because `sscanf` returns the fields it read and they are not fail-opens; the CLI is still inconsistent about `=` versus a space, and the honest response was to refuse and say which style each option wants, not to accept both spellings of everything in a batch whose subject is refusing; and NOTHING IN THE GATE SET COMPILES `mdk_linux/**` (item 11b), so the binary was built by hand with `cmake --build build-linux -j8` rc 0 and the test SKIPS rather than passes when it is absent. Oracle corroboration: the C# frontend's own numeric options use `int.Parse`/`float.Parse` (`src/Mdk.App/Program.cs:136,155,160,168,187`, re-read this batch), which throw — the oracle never silently runs a different scene. **GATES (all three re-run in attempt 5):** `make -C tests/scripts -j8` rc 0, `All cases match the oracle.`, **766 PASS / 0 FAIL** — 765 before this row existed, and +1 is exactly the new `PASS LinuxArgParse (19 arms…)` line; `DISPLAY=:99 bash tools/check.sh` rc 0, `CHECK: ALL GATES PASSED`, `PASS L3_f1.bmp sha=58615fc85d02e009`, `BATCH INTEGRITY: PASS`, `COMPLETION SELFTEST: PASS (13 arms)`, `COMPLETION[d1]: 11 done / 13 total / 0 owner-blocked / 2 open`, `COMPLETION: LEDGER VERIFIED`, `SKIP gates_smdh`, `audit_c_with_classes` 0 violations; `make -C mdk_3ds -j8` rc 0 with `mdk.elf` md5 `728eb2350c3ef8a81acda15f75617c1e` **UNCHANGED** — no `mdk_3ds/**` path in the diff, so no P8 emulator evidence is owed and none was faked. `cmake --build build-linux -j8` rc 0 by hand, three times (item 11b). The frame was re-captured from the fixed binary, not inherited from the attempt that lost it: 187 PNGs in `docs/frames`, 186 at md5 `6769a137…` including this one, 219 colours, **zero** pure (0,0,255) pixels. The verbatim block is §7 of `docs/evidence/2026-11-13-169-name-it-or-dont-run-it.txt`. No `mdk_3ds/**` file changed, so no P8 emulator evidence is owed. Prior-solution lookup: Pi knowledge (`mdk-sdl`) found no prior note on the class; the in-repo prior solution is `emurun.sh`'s `decimal_measurement`, cited above; **Obsidian and Cognee MCP are ABSENT from this session's tool list** (AGENTS §9 P9: a session fault, not a licence to claim the lookup), vault read read-only from disk with no prior solution found. Batch 168's evidence file carries a CORRECTION block, not a silent rewrite.

**PREVIOUS BATCH 168 RECORD** (its method and the CORRECTION batch 169 appended to it are in `docs/evidence/2026-11-13-168-nobody-refuses-here.txt`). **BATCH 168 (`linux.render-frame`) — THE 256-ENTRY REFUSAL MEMORY IS NOT REACHABLE HERE, AND NOT BECAUSE REFUSALS ARE RARE: ON THIS HOST THE ALLOCATOR NEVER FAILS.** A 600-frame level-3 run, 4096x3072, 16x16 and a nonexistent level all emit ZERO `[vk] REFUSED` lines, and `refused(` has 38 call sites, so the path is live and simply never fires: lavapipe does not refuse an allocation on this host. No gate was added, because a gate green only while this ICD keeps never refusing would go permanently red the moment item 7 lands. **BATCH 169 CORRECTED TWO OF ITS CLAIMS:** the "levels 1..7" sweep was level 3 seven times, and the "unknown level falls back" open item never existed. See batch 169 above and the correction block.

**BATCH 167 (ledger row `linux.render-frame`, acceptance `bash tools/check.sh`) — ITEM 10 SAID "THE RETIRE PATH FREES MEMORY IT NEVER UNMAPPED"; THE SHAPE IS TRUE, THE HAZARD IS A DIFFERENT QUESTION, AND IT IS THE ONE NOW PINNED.** Not a review remediation: NEXT-list item 10, open since batch 152. **Check-only batch — `git diff --name-only` is one file, `tests/scripts/linux_refusal_report_test.sh`, and no production line changed.** **THE PREMISE WAS MEASURED BEFORE ANYTHING WAS WRITTEN:** `destroyBufferPair` is `vkDestroyBuffer` + `vkFreeMemory`, `vkUnmapMemory` appears in this file only on staging paths and never on the retire path, and both persistent-mapping owners (`MeshVk::mapped[]`, `RendererVk::canvasMapped[]`) hand their mapping to a retire and never unmap it. **BUT WHETHER AN EXPLICIT UNMAP IS REQUIRED BEFORE A FREE IS A QUESTION ABOUT SPEC VALID-USAGE TEXT THIS HOST DOES NOT HAVE** — batch 165 measured that the vendored `vulkan_core.h` carries no prose at all — and review-10 F3's entire lesson is that an implied spec sentence nobody here can look up is a claim wearing a citation's clothes. **So this batch does not settle that question, and the check says so in its own message** so a future reader cannot mistake the silence for a verdict. **WHAT IS DECIDABLE IN THE TREE IS NARROWER AND IS WHAT THE HAZARD ACTUALLY TURNS ON: a mapping pointer must not outlive the retire that took its buffer, because nothing downstream can tell a live mapping from a released one.** Measured on the parent: `retireBuffer` is called from exactly two places, `mdk_render_release_mesh` and `ensureCanvasBuffer`, and BOTH already null the slot's mapping field; and the only reader of a mapping local, `mdk_render_update_mesh`, already takes `void* mapped = m.mapped[slot]`, tests it for null, refuses by name, and returns before the `memcpy`. **The implied hazard — a stale mapping dereferenced after the free — does not exist on the parent either. Item 10 was a shape observation wearing a defect's clothes, and the honest closure is to pin the property that keeps it a shape observation, not to invent a fix for a bug that is not there.** **CHECK B14.** (a) DISCOVERED, as in B13(c): every `retireBuffer` call that names a slot must null that slot's mapping field later in the same function — the rule reads the slot out of the call's own argument and looks for the matching `...[Mm]apped[...]` nulling, so one pattern covers `m.mapped[slot]` and `r->canvasMapped[i]` and will cover the third drop site nobody has written yet; a named list of the two current sites is the mistake B4 made in review-5. (b) every local taken from a mapping must be tested for null behind a `refused(` before anything `memcpy`'s through it, because a null mapping copied through is a segfault in a release build and the refusal is what turns it into a mesh that keeps its previous contents and a line saying so. Four arms each isolating B14: `retire-buffer-leaves-the-mapping-set`, `canvas-retire-leaves-the-mapping-set`, `mapping-copied-without-a-null-test`, `mapping-guard-refuses-silently`. **`LinuxRefusalReport` is 20 checks / 67 arms / 5 probes -> 21 checks / 71 arms / 5 probes.** **THE FOUR-WAY.** New test on working sources rc 0 (21/71/5); new test on the parent `4656bac` **rc 0 — green, and that is the honest result**, because the parent already nulls both mappings and already refuses on null, which is the finding above restated as a table cell, and a batch that moves no production shape cannot red its own parent; parent test on working sources rc 0, **zero collateral**; parent on parent rc 0. The instrument that proves B14 bites is its four arms, each reddening B14 and nothing else. **GATES:** `make -C tests/scripts -j8` rc 0, `All cases match the oracle.`, **765 PASS / 0 FAIL**, this run reading `PASS LinuxRefusalReport (21 checks, 71 arms adjudicated red, 5 probes green)`. `DISPLAY=:99 bash tools/check.sh` rc 0, `CHECK: ALL GATES PASSED`, `PASS L3_f1.bmp sha=58615fc85d02e009`, `BATCH INTEGRITY: PASS`, `COMPLETION SELFTEST: PASS (13 arms)`, `COMPLETION[d1]: 11 done / 13 total / 0 owner-blocked / 2 open`, `COMPLETION: LEDGER VERIFIED`, `SKIP gates_smdh`. `make -C mdk_3ds -j8` rc 0 with `mdk.elf` md5 `728eb2350c3ef8a81acda15f75617c1e` **UNCHANGED** — the diff is one test file, so no P8 emulator evidence is owed and none was faked. `cmake --build build-linux -j8` was **not** re-run and that is stated rather than skipped silently: no file under `mdk_linux/**` changed, so there is nothing new for it to compile, and the binary `gates_frames` certifies is the one batch 166 built and re-dated. Frame `docs/frames/2026-11-13-the-mapping-does-not-outlive-the-retire.png` is byte-identical to its predecessors (census re-derived: 185 PNGs, 184 at `6769a137…`) with **zero** pure (0,0,255) pixels over 219 colours — nothing on any render path changed, so a changed frame would have been the surprise. Prior-solution lookup: Pi knowledge (`mdk-sdl`) surfaced the retire/settle pattern and the review-25 descriptor closure already cited by batches 163-166, nothing on the mapping question. **Obsidian and Cognee MCP are ABSENT from this session's tool list** (AGENTS §9 P9: a session fault, not a licence to claim the lookup), vault read read-only from disk. Full record: `docs/evidence/2026-11-13-167-the-mapping-does-not-outlive-the-retire.txt`.

**BATCH 166 (ledger row `linux.render-frame`, acceptance `bash tools/check.sh`) — THE DESCRIPTOR SET WAITED TWO FRAMES AND THE IMAGE VIEW IT NAMED WAITED NONE: HALF A LIFETIME IS NOT A COMPLETION BOUNDARY.** Not a review remediation. This closes item **11e** below: review-9 F2 named the defect when the set half landed, batch 164 fixed the PROSE and recorded the code as "its own slice ... needs its own mutation arms", and review-10 re-confirmed the reachability analysis without closing it. **THE DEFECT:** `forgetDescriptorSets` retires the cached sets naming a view that is going away, and `mdk_render_update_texture`, `mdk_render_release_texture` and `mdk_render_release_palette` each then called `destroyImage` — view, image, memory — IN THE SAME CALL. The retired set waited `frameIndex + kFramesInFlight`, the boundary `vk::beginFrame`'s fence proves, while the `VkImageView` that set names was destroyed under a frame that might still be sampling it. AGENTS.md P3 says do not free an in-flight resource, and it says that about the RESOURCE, not the handle that refers to it. **REACHABILITY STATED, NOT INFLATED:** review-10 tried to break the latent claim and could not — `mdk_render_update_texture` has no caller, `mdk_render_release_texture`'s only caller is `kurt_sprite.cpp:515` reached only from the one-shot startup block before any frame is submitted, and `sprite_residency_bytes = 0` on Linux means the eviction never fires. Real in the API, unreachable on this frontend today, which is what batch 163 also said about the leak. **THE FIX IS THE OTHER HALF OF A PATTERN ALREADY IN THE FILE:** `RetiredImage {image, memory, view, safeFrame}` beside `RetiredSet`, `retiringImages` beside `retiring`, `retireImage()` pushing on the SAME boundary, and `settleRetired` destroying after the deadline test through `destroyImage` so the view/image/memory order lives in one place. All three drop sites retire instead of destroying and **null the slot's fields where they retired**, because leaving them set would have teardown destroy the same image twice; `mdk_render_destroy` drains the list, the device being idle there and nothing else able to see those images once their slot fields are nulled. `update_texture`'s keep-the-old-image failure path is untouched — the retire sits in the success branch, where the old handles really are unreferenced. **CHECK B13, EIGHT ARMS, AND TWO RESULTS THAT HURT AND ARE THEREFORE PUBLISHED.** B13 pins (a) the record `retireImage` pushes carries all THREE handles traced to its own parameters plus the exact boundary — B12(a)'s join rule again, for the same VK_NULL_HANDLE-initialiser reason; (b) the settle releases the handles of the entry whose `safeFrame` it compared, after that test, in view/image/memory order; (c) **DISCOVERED, not listed** — every function calling `forgetDescriptorSets` is a drop site and must retire rather than destroy, because naming the three current ones leaves the fourth added next month unguarded, the mistake B4 made in review-5; (d) a field retired must be nulled where it was retired and teardown must drain the list. **FIRST, THE PARENT TEST ON THE WORKING SOURCES IS rc 1 — one ARM red (`set-freed-without-a-deadline`), NO check red.** The parent's B12 read the settle's set loop with an unbounded tail, so the new image loop's `safeFrame` test satisfied the set loop's deadline rule and the mutant that deletes the set deadline was accepted: the old instrument was scope-blind in exactly the way review-9 F1 describes, and it could only be found by putting a second deadline in the same function. The new test bounds B12's tail at `retiringImages` and the arm bites again. **SECOND, A PROBE CAUGHT B13 REDDING A CORRECT PROGRAM:** the first version required the release to go through `destroyImage`, and writing the three calls out in the correct order is the same program — measured `inl: RED`, which is review-10 F5 repeated inside the batch whose predecessor's whole lesson is that a gate reddening an honest edit gets worked around, not obeyed. The rule now accepts either spelling and pins the ORDER, and both rewordings are permanent probes. **`LinuxRefusalReport` is 19 checks / 59 arms / 3 probes -> 20 checks / 67 arms / 5 probes.** **THE FOUR-WAY.** New test on working sources rc 0 (20/67/5); new test on the parent `47bc819` **rc 1 with exactly ONE check red, B13** — the row that says the new check bites on real code and not only on mutants; parent test on working sources rc 1, one arm and no check, explained above; parent on parent rc 0. A batch that moves a shape gets a red parent row, and this one moved one. **GATES:** `make -C tests/scripts -j8` rc 0, `All cases match the oracle.`, **765 PASS / 0 FAIL** — unchanged and correctly, since this test contributes ONE `PASS LinuxRefusalReport` line; this run reads `(20 checks, 67 arms adjudicated red, 5 probes green)`. `DISPLAY=:99 bash tools/check.sh` rc 0, `CHECK: ALL GATES PASSED`, `PASS L3_f1.bmp sha=58615fc85d02e009`, `BATCH INTEGRITY: PASS`, `COMPLETION SELFTEST: PASS (13 arms)`, `COMPLETION[d1]: 11 done / 13 total / 0 owner-blocked / 2 open`, `COMPLETION: LEDGER VERIFIED`, `SKIP gates_smdh`, `audit_c_with_classes` 0 violations — the new code is a struct, a vector and ordinary functions, no virtual, no template, no exception. `make -C mdk_3ds -j8` rc 0 with `mdk.elf` md5 `728eb2350c3ef8a81acda15f75617c1e` **UNCHANGED** — no `mdk_3ds/**` path in `git diff --name-only`, so no P8 emulator evidence is owed and none was faked. `cmake --build build-linux -j8` rc 0, BY HAND per item 11b, with the stale-binary hazard measured rather than assumed (binary 09:38:42 against source 09:38:31). Frame `docs/frames/2026-11-13-the-view-waits-too.png` is byte-identical to its predecessors (census re-derived: 184 PNGs, 183 at `6769a137…`) with **zero** pure (0,0,255) pixels over 219 colours — and the honest reading of that is that the path this batch alters is unreachable on this frontend, so the fix is proven by shape and mutation and not by a picture. Prior-solution lookup: Pi knowledge (`mdk-sdl`) surfaced the review-25 descriptor-CACHE closure this sits on and the 3DS backend's `settleDeferredMeshes`, the same defer-to-the-frame-boundary house shape, nothing on deferring the image half. **Obsidian and Cognee MCP are ABSENT from this session's tool list** (AGENTS §9 P9: a session fault, not a licence to claim the lookup), vault read read-only from disk. Full record: `docs/evidence/2026-11-13-166-the-view-waits-too.txt`.

**BATCH 165 (ledger row `linux.render-frame`, acceptance `bash tools/check.sh`, attempt 3) — THE JOIN WAS PINNED AT THREE OF ITS FIVE SEAMS, AND THE ONE THAT PRINTED PASS HAD THE LEAK RESTORED.** Review-10 (on batch 164, `bc5ef65`) had no Critical and no High and blocked its verdict on its two Mediums. **All five findings were re-run as mutants on a copy of the tree BEFORE touching anything and all five reproduced:** `retire-records-a-blank rc=0 PASS`, `settle-frees-somebody-elses-set rc=0 PASS`, `lost-set-counter-deleted rc=0 PASS`, `lost-set-print-deleted rc=0 PASS`, and review-10 F5's two CORRECT refactors both **red on correct code**. None rebutted; all five fixed. **F1 IS THE ONE THAT MATTERS AND IT IS EXACTLY THE CLASS BATCH 164 SAIT IT HAD CLOSED:** B12 asked for the token `retiringSets.push_back(` and never read the record, and `RetiredSet`'s in-class initialisers quietly value `set` to VK_NULL_HANDLE — so deleting `retired.set = set;` leaves a push with a plausible deadline, the pool is handed a null handle, the real set is never returned, and the leak batch 163 exists to close runs at full size behind `PASS LinuxRefusalReport (19 checks, 51 arms)`. **THE RULE IS NOW ONE JOIN READ OFF THE RECORD THAT IS PUSHED, PLUS ITS MIRROR ON THE FREE SIDE — A JOIN, NOT A TOKEN AND NOT A SPELLING:** (a) the helper's own `VkDescriptorSet` parameter is read from its signature and BOTH fields of the pushed record must be filled from it before the push, in either spelling — a local filled field by field or a record built at the call — with the deadline EXACTLY `frameIndex + kFramesInFlight` with nothing after it, which is the exactness MOVED INTO the join so it still kills review-9's `- 1` mutant; (b) the entry whose `.safeFrame` is compared is read from the comparison and the handle handed to `vkFreeDescriptorSets` must be that entry's `.set`, named in place or copied out — `retiringSets.back().set` frees a set whose frame was never proven finished (AGENTS P3) and leaks the entry erased instead; (c) the lost-set counter must sit inside a `!= VK_SUCCESS` block in `settleRetired` — not above the test, which counts frees that succeeded, and not inside a `refused(` call, which dedups and would count sentences instead of sets — and the total must print from `mdk_render_destroy` under a non-zero guard so a clean run reports no loss; (d) the drop-site argument is traced to a cache slot directly or through a named local, and that slot must be the one the loop removes, by either pop. **`LinuxRefusalReport` is 19 checks / 51 arms / 0 probes -> 19 checks / 59 arms / 3 probes**, the eight new arms each isolating B12 and one of them (`retire-a-named-local-from-the-wrong-slot`) being the non-vacuity of F5's tolerance: the trace works and what it traces to is not the entry being dropped. **THE PROBES ARE NEW TO THIS FILE AND ONE OF THEM CAUGHT THIS BATCH'S OWN FIX BEING WRONG.** An arm proves a rule is sensitive; a probe proves it is not merely sensitive to a SPELLING, which is what F5 is about — the mechanism is lifted from `panel_geom_test.sh`, not reinvented. The aggregate-at-the-push refactor measured RED on the first version of the new join rule, because a separate older sub-rule still demanded a literal `safeFrame = ...;` line: **a correct program was reddened by the fix that existed to stop reddening correct programs.** The exactness moved into the join, all three probes went green, and a rule that reddens an honest edit gets worked around rather than obeyed. **F3 (Low): THE SECOND UNVERIFIABLE QUOTE, REMOVED RATHER THAN RE-CITED.** Review-9 F4 was 'batch 163 quoted a spec sentence that does not exist'; batch 164 replaced it with another quoted spec sentence. Measured here rather than argued: `grep -c "Valid Usage"` over the vendored `vulkan_core.h` is **0** and the quoted phrase is not in it — the header carries no spec prose, so a quotation mark there is a citation claim nothing on this host can check. The comment now states the two rules AS rules and says why they are not quoted (the out-of-memory result codes do not say whether the sets were taken; a set handed to the call must be a valid handle this pool issued and not one already returned), which is the same conclusion and makes a RETRY the unsafe branch. Review-10's open question 1 is now moot for this tree. **THE FOUR-WAY, INCLUDING THE ROW THAT IS GREEN BECAUSE IT SHOULD BE:** new test on working sources rc 0 (19/59/3); new test on the parent `bc5ef65` **rc 0 — green, and that is the honest result**, because this batch moves NO production shape at all (the only `renderer_vk.cpp` change is a comment) and a batch that moves no shape cannot red its own parent; new test on `be5b673` and on `3aef28d` **rc 1 with exactly ONE check red, B12**, so the strengthening did not blunt the original pin; parent test on working sources rc 0, **zero collateral**; parent test on parent sources rc 0. The instrument that proves the new arms bite is the mutant table, which is the instrument review-10 used against batch 164. **GATES:** `make -C tests/scripts -j8` rc 0, `All cases match the oracle.`, **765 PASS / 0 FAIL** — unchanged and correctly, since this test contributes ONE `PASS LinuxRefusalReport` line and not one per check or arm; this run reads `(19 checks, 59 arms adjudicated red, 3 probes green)`, and `PASS EmurunHarness (257 checks)`. `DISPLAY=:99 bash tools/check.sh` rc 0, `CHECK: ALL GATES PASSED`, `PASS L3_f1.bmp sha=58615fc85d02e009`, `BATCH INTEGRITY: PASS`, `COMPLETION SELFTEST: PASS (13 arms)`, `COMPLETION[d1]: 11 done / 13 total / 0 owner-blocked / 2 open`, `COMPLETION: LEDGER VERIFIED`, `SKIP gates_smdh`. `make -C mdk_3ds -j8` rc 0 with `mdk.elf` md5 `728eb2350c3ef8a81acda15f75617c1e` **UNCHANGED** — no `mdk_3ds/**` path in `git diff --name-only`, so no P8 emulator evidence is owed and none was faked. `cmake --build build-linux -j8` rc 0, run BY HAND per item 11b: nothing in the gate set compiles `mdk_linux/**`, so a stale binary would be certified as happily as a fresh one. Frame `docs/frames/2026-11-13-the-join-pinned-at-both-ends.png` is byte-identical to its predecessors (census re-derived, not remembered: 183 PNGs, 182 at md5 `6769a137…`) with **zero** pure (0,0,255) pixels over 219 distinct colours. Prior-solution lookup: Pi knowledge (`mdk-sdl`) surfaced the 3DS backend's `settleDeferredMeshes` — the same defer-to-the-frame-boundary pattern this row's settle is made of — and the review-25 closure that fixed the descriptor CACHE and left the pool undone; nothing on pinning a retire/settle join and nothing on green-refactoring probes. **Obsidian and Cognee MCP are ABSENT from this session's tool list** (AGENTS §9 P9: a session fault, not a licence to claim the lookup), vault read read-only from disk with no prior solution found. Full record: `docs/evidence/2026-11-13-165-the-join-pinned-at-both-ends.txt`.

**BATCH 164 (ledger row `linux.render-frame`, acceptance `bash tools/check.sh`, attempt 2) — THE CHECK PINNED THE PARTS OF THE RETIRE AND NEVER PINNED THE JOIN; FOUR MUTANTS PROVED IT AND ALL FOUR ARE RED NOW.** Review-9 (on batch 163) had no Critical and no High, but blocked its own verdict on its two Mediums and both were real. **I re-ran the reviewer's four mutants on a copy of the tree BEFORE touching anything and all four reproduced verbatim:** `M1 retire-records-nothing rc=0 PASS`, `M2 settle-does-not-erase rc=0 PASS`, `M3 deadline-one-frame-early rc=0 PASS`, `M4/M4b retire-the-wrong-set rc=1` — that last red for the wrong reason (the registered arms mutate by exact string match and could no longer apply), which is sensitivity to a rewording, not a pin. **M1 is `push_back` -> `(void)retired;` inside `retireDescriptorSet`: the leak batch 163 exists to close, fully restored, printing `PASS LinuxRefusalReport (19 checks, 47 arms adjudicated red)`.** M3 is `+ kFramesInFlight - 1`, which frees a set on a frame whose fence has not been waited. **B12 NOW PINS THE JOIN, ONE SHAPE PER MUTANT, AN ARM AP PIECE:** (a) `retireDescriptorSet`'s body must contain `retiringSets.push_back(`; (b) the settle's set loop must contain `retiringSets.erase(` and AFTER the `vkFreeDescriptorSets` call (never erasing frees the same handle every frame, and once the pool reissues it that is a free of a LIVE set; erasing first loses a handle whose free failed); (c) the deadline must be EXACTLY `frameIndex + kFramesInFlight` with nothing after it — the old regex `safeFrame[^;]*frameIndex[^;]*kFramesInFlight` matched `- 1` because `[^;]*` runs to the semicolon, which is exactly how M3 hid; (d) the drop site's argument must be `<container>[<index>].set` for the same slot the swap-pop overwrites, which is the identity "the set handed to the pool is the set the cache dropped" and the one thing `r->descs.back().set` and `VK_NULL_HANDLE` both fail. **`LinuxRefusalReport` is 19 checks / 47 arms -> 19 checks / 51 arms**, and the five mutants now rc 1 with B12 red as a CHECK, not as an arm that could not apply. **THE FOUR-WAY, INCLUDING THE ROW THAT IS GREEN AND SHOULD BE:** new test on working sources rc 0 (19/51); new test on HEAD rc 0 — **green, and that is the honest result**, because batch 163's fix is committed at HEAD so HEAD already satisfies every new shape and a batch that moves no production shape cannot red that row; the instrument that proves the new arms bite is the mutant table, which is the instrument the reviewer used against batch 163. New test on the PRE-FIX commit `3aef28d`: **rc 1, exactly ONE check red — B12**, all four sub-findings named, so the strengthening did not blunt the original pin. Parent test on working sources rc 0, **zero collateral**; parent test on HEAD rc 0. **F2 (Medium) CONFIRMED AND FIXED BY THE REVIEWER'S OWN OPTION (b):** `mdk_render_update_texture`, `mdk_render_release_texture` and `mdk_render_release_palette` call `forgetDescriptorSets` and then `destroyImage` in the same call, and `destroyImage` destroys view -> image -> memory synchronously with no deadline — so the set waits two frames while the resource it names is freed on the spot, against AGENTS.md P3. Pre-existing (no `destroyImage` call site changed) and the reviewer said so; what made it a finding was batch 163's sentence that the set "waits for the same boundary the buffers do", true of the set and readable as covering the pair. Both comments now say THE SET, only the set, `retireDescriptorSet`'s doc comment carries the disclosure in full, and the real fix is item **11e** below. Option (a) — a `RetiredImage` — was NOT taken on purpose: it changes the lifetime of both resource families, interacts with `update_texture`'s keep-the-old-image failure path and with teardown, and needs its own arms; two subsystem slices in one commit is forbidden. **F3 (Low) FIXED, optional half DECLINED with a reason in the code:** the dry-pool sentence now says live PLUS pending-retired (a retired set holds its slot until the boundary frees it); reporting the two counts inside the refusal is declined because `refused()` dedups on the RENDERED SENTENCE and the table is capped at `kRefusalMemory = 256` with "past the cap everything prints", so a volatile number is a new key per frame and then a line per frame — the 22500-lines-for-48-requests flood B7 exists to prevent. **F4 (Low): THE SPEC SENTENCE BATCH 163 QUOTED DOES NOT EXIST, SO THE JUSTIFICATION WAS REPLACED, NOT RE-CITED** — no such text is in the spec and the vendored header carries no prose, so nothing was invented; the position now written rests on two undisputed facts (the result codes this call returns, and its valid-usage rule that a set "must not have been freed already"), which together make a RETRY the unsafe branch. The handle is dropped and the loss is COUNTED (`RendererVk::lostDescriptorSets`) and printed once at teardown, where a running total cannot repeat. **GATES:** `make -C tests/scripts -j8` rc 0, `All cases match the oracle.`, **765 PASS / 0 FAIL** — unchanged and correctly: the refusal test contributes ONE `PASS LinuxRefusalReport` line, this run's reading `(19 checks, 51 arms adjudicated red)`; (`grep -c FAIL` over the log returns 3, all three prose inside test labels, no check failed). `DISPLAY=:99 bash tools/check.sh` rc 0, `CHECK: ALL GATES PASSED`, `PASS L3_f1.bmp sha=58615fc85d02e009`, `BATCH INTEGRITY: PASS`, `COMPLETION SELFTEST: PASS (13 arms)`, `COMPLETION[d1]: 11 done / 13 total / 0 owner-blocked / 2 open`, `COMPLETION: LEDGER VERIFIED`, `SKIP gates_smdh`. `make -C mdk_3ds -j8` rc 0, `mdk.elf` md5 `728eb2350c3ef8a81acda15f75617c1e` **UNCHANGED** — no `mdk_3ds/**` file changed, so no P8 emulator evidence is owed. **The stale-binary hazard was checked, not assumed** (item 11b): nothing in the gate set compiles `mdk_linux/**`, so `build-linux/mdk` was built by hand twice — after the code edits and again after the final comment rewrap — so the binary (06:07:19) is NEWER than the source (06:07:04) `gates_frames` certifies; a standalone `-fsyntax-only` with the project's own flags is rc 0, zero warnings. Frame `docs/frames/2026-11-13-the-join-not-the-parts.png` is byte-identical to its predecessors (re-derived: 182 PNGs, 181 at md5 `6769a137…`) with **zero** pure (0,0,255) pixels, re-decoded rather than remembered. Prior-solution lookup: Pi knowledge (`mdk-sdl`) top hit 0.938 was the review-25 closure batch 163 built on, nothing on a retire/settle join; **Obsidian and Cognee MCP are ABSENT from this session's tool list** (AGENTS §9 P9: a session fault, not a licence to claim the lookup), vault read read-only from disk with no prior solution found. Full record: `docs/evidence/2026-11-13-164-the-join-not-the-parts.txt`.

**BATCH 163 (ledger row `linux.render-frame`, acceptance `bash tools/check.sh`, attempt 1) — A CACHE ENTRY AND AN ALLOCATED SET ARE TWO THINGS, AND ONLY ONE OF THEM WAS EVER RELEASED.** `getDescriptorSet` caches one `VkDescriptorSet` per (texture, palette, repeat) triple; `forgetDescriptorSets` dropped the entries naming a view about to be destroyed — review-25's fix, marked CLOSED at `docs/OPEN_ITEMS.md:497`, and correct — but on the parent `vkFreeDescriptorSets` appeared **0** times, `vkResetDescriptorPool` appeared **0** times, `poolInfo.flags` was never assigned, and `maxSets` is a fixed 4096 on a pool created once. Every set the cache forgot was spent for the life of the process; when the pool emptied, `vkAllocateDescriptorSets` failed, `getDescriptorSet` refused, and draws were skipped once per distinct request and then silently. Call sites: `mdk_render_update_texture` (every same-size update), `mdk_render_release_texture`, `mdk_render_release_palette` — the first two also being what `mdk_render_release(scope)` and `kurt_sprite.cpp:515`'s residency eviction are made of. **REACHABILITY STATED, NOT INFLATED:** on this frontend the churn is small today (`sprite_residency_bytes = 0` on Linux, so `bounded()` is false and `trimResidency` never evicts; nothing in `mdk_core` calls `mdk_render_update_texture` or `mdk_render_release` yet), while the 3DS backend returns 1 MiB for that field so the eviction path is live there. The leak is real in the API and latent on this target; what changed is that it stopped being the behaviour. **THE FIX HAS THREE PARTS BECAUSE THERE ARE THREE WAYS TO GET IT WRONG:** (a) the pool must be created with `VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT` — `vkFreeDescriptorSets` is invalid use without it, so the obvious free-at-the-drop-site is a second bug, and `vkResetDescriptorPool` takes every set in the pool with it, including ones a queued frame has bound, so it has no safe point in a frame loop; (b) the drop site **retires** rather than frees, reusing the buffers' own `frameIndex + kFramesInFlight` deadline made real by `vk::beginFrame`'s fence (AGENTS P3 puts the completion boundary before the CPU release); (c) `settleRetired` actually frees and **reads the result**, reporting once through the dedup-ing `refused()`. Check **B12** and six arms ship with it (`forget-stops-retiring-the-set`, `set-freed-at-the-drop-site`, `pool-not-created-freeable`, `retired-sets-never-freed`, `set-freed-without-a-deadline`, `set-free-result-discarded`), each isolating B12. **THE FOUR-WAY, AND THE FOURTH ROW IS THE ONE A NEW CHECK USUALLY SKIPS:** new test (19 checks/47 arms) on working sources **rc 0, 0 red**; new test on `git show HEAD:` **rc 1, exactly 1 of 19 red — B12**, all four sub-findings named; parent test (18/41) on parent sources **rc 0, 0 red**; parent test (18/41) on **working** sources **rc 0, 0 red — zero pre-existing collateral**, i.e. the new code breaks nothing already green. `LinuxRefusalReport` is 18 → **19 checks**, 41 → **47 arms**. **AND A MEASUREMENT THAT SAYS MORE ABOUT THE ROW THAN THE FIX: NOTHING IN THE GATE SET COMPILES `mdk_linux/**`.** `bash tools/check.sh` runs the eight contract gates, `gates_core_parity` compares source LISTS, and `gates_frames` uses `build-linux/mdk` if one happens to exist and SKIPS if not — so the target this row is about is built by hand or not at all, and a stale binary would be certified as happily as a fresh one. `cmake --build build-linux -j8` was run by hand: **rc 0**, `Built target mdk`. The fix is a `gate` line in `tools/check.sh`, which is in `gates_batch_integrity.PROTECTED` and is an owner act, not a batch; `core.formats` sat broken for 25 batches for exactly this reason, as the ledger header says. **REVIEW-8 (run-20261113-021356, on batch 162): no Critical and no High, so nothing was mandatory. F1 (Medium) and F3 (Low) are FIXED, re-measured and not copied; F2 (Low) is DEFERRED with cause** because it edits `tests/scripts/emurun_harness_test.sh`, which is another row's acceptance test. F1 confirmed: batch 162's published "working 34 443 B" is its own commit's PARENT (50e8276); re-measured with the test's own method, `3aef28d` is **8 801 + 26 351 = 35 153 B**, `50e8276` is 8 091 + 26 351 = 34 443 B, and the published parent 32 816 B is correct — the batch that existed to close a one-generation label slip made one itself, one row over. F3 confirmed: nine code lines end in `| head -1`, not twelve. Corrections are appended to batch 162's evidence file, not silently rewritten. Gates: `make -C tests/scripts -j8` rc 0, `All cases match the oracle.`, **765 PASS / 0 FAIL** — unchanged, and correctly: the refusal test contributes ONE `PASS LinuxRefusalReport` line to that count, not one per check. `DISPLAY=:99 bash tools/check.sh` rc 0, `CHECK: ALL GATES PASSED`, `PASS L3_f1.bmp sha=58615fc85d02e009`, `BATCH INTEGRITY: PASS`, `COMPLETION SELFTEST: PASS (13 arms)`, `COMPLETION[d1]: 11 done / 13 total / 0 owner-blocked / 2 open`, `COMPLETION: LEDGER VERIFIED`. `make -C mdk_3ds -j8` rc 0 with `mdk.elf` md5 `728eb2350c3ef8a81acda15f75617c1e` **UNCHANGED** — correct, no `mdk_3ds/**` file changed, so no P8 emulator evidence is owed. Frame `docs/frames/2026-11-13-sets-go-back-to-the-pool.png` is byte-identical to its predecessors (re-derived: 181 PNGs, 180 at md5 `6769a137…`) with **zero** pure (0,0,255) pixels. Prior-solution lookup: Pi knowledge surfaced `docs/OPEN_ITEMS.md:497`, the review-25 closure that fixed the cache and left the pool undone; **Obsidian and Cognee MCP are ABSENT from this session's tool list** (AGENTS §9 P9: a session fault, not a licence to claim the lookup), vault read read-only from disk with no prior solution found. Full record: `docs/evidence/2026-11-13-163-sets-go-back-to-the-pool.txt`.

**BATCH 162 (ledger row `3ds.hud-canvas`, acceptance `mdk_3ds/tools/emurun.sh --expect hud`, attempt 8) — review-7 F1 (Medium), F2 (Medium), F3 (Low): ALL THREE CONFIRMED AGAINST THE SHIPPED ARTIFACTS, ALL THREE FIXED, NONE REBUTTED. A MEASUREMENT YOU CANNOT RE-RUN IS NOT EVIDENCE, IT IS AN ANECDOTE WITH A NUMBER IN IT.** Batches 156-161 built this row's case on published rates and counts; review-7 re-ran two of them on its own host and neither survived. This batch re-ran every number it could reach, recorded the load beside each, and corrected the three that did not hold — two of which this project published about itself. **F1: THE FOUR-WAY TABLE NAMED THE WRONG GENERATION, AND THE ERROR REPRODUCES.** Batch 161's `253 checks, 19 FAIL`, `214 checks` and `the parent's 30 235-byte block` are the GRANDparent's (f9409a9), not the parent's (6345bf5b). Re-run with artifacts from the right commits: new test on the parent harness **257 checks / 12 FAIL** (all twelve are batch 161's `--secs` rows; batch 160's 7 pass on its own commit), parent test on the parent harness **227 / 0**, parent test on the working harness **227 / 0** (the collateral run, still the only one of the four that can see collateral), new test on the working harness **257 / 0**. The mislabelling was reproduced, not just corrected: this test on the grandparent harness reds exactly **19** and the grandparent's test is exactly **214** checks — one mistake, a comparison run against `HEAD~2` artifacts, surfacing in three places. Block sizes re-derived from the awk markers: parent **32 816 B**, grandparent 30 235 B, working 34 443 B. **[CORRECTED BY BATCH 163: the "working" figure is 50e8276's, not this batch's — `3aef28d` is 8 801 + 26 351 = 35 153 B; and NINE code lines end in `| head -1`, not twelve. Re-measured, not copied; see the CORRECTION block appended to `docs/evidence/2026-11-13-162-the-numbers-i-ran.txt`.]** **F2: TWO PUBLISHED RATES DID NOT REPRODUCE AND THE CAUSAL CLAIM BUILT ON ONE WAS BACKWARDS.** (a) "0 red in 20 full runs of the parent test" reproduced here (0/20) and not on review-7's host (2-3/20): a sample written as a constant; the honest range across two hosts is 0-3 of 20. (b) "0-1 of 200 on the parent's block" is wrong: the piped canary row reds **24 of 200** on the grandparent's block here (five samples on the parent's: 21, 23, 14, 22, 29 of 200) and 33 of 200 on review-7's host — the class was already live on the parent at about one run in eight. (c) "this batch's growth made it fifteen times flakier" is unsupported: **at equal load the parent's 32 816 B and this batch's 34 443 B blocks measure the same rate** (21-29 vs 20-24 of 200), and the gradient that produced the 15x claim (24/54/94) was measured while another measurement loop of the same batch was still running on the box — load is a driver of this race, so that run measured its own co-tenancy. **WHAT DOES DRIVE IT, measured on one payload at five match positions** (32 816 B, 200 runs each, status 141 = SIGPIPE): match at byte 4 463 → 26/200, 5 296 → 10, 9 052 → 20, 17 450 → 5, 30 218 → 0. The driver is the **bytes still to write after the consumer stops reading**, not the payload's size — and the model reproduces all three rows of batch 161's own table (canary_stats 23 764 remaining → 27/200, expect_check 2 598 → 1/200, decimal_measurement 2 001 → 0/200), which also names where the wrong number came from: `expect_check()` really does measure 0-1 of 200 because its match sits near the end, and its rate was written down as the block's. Measured through `head` too: `printf '%s\n' "$BLOCK" | head -1` reds **168-179 of 200** at 32 816 B, 2/200 at 16 000 B, 0/200 at 4 000 B. The fix is untouched by all this: the `case` form is 0/200 isolated, the rewritten file 0/20 full runs on both hosts, and F2 rested on the mechanism (15-27 of 200 with `pipefail`, 0 without), not on a percentage. **F3: THE SHAPE ROW MATCHED ONE SPELLING OF THE CLASS ITS LABEL CLAIMED.** `grep -c 'grep -[q]'` matched the literal `grep -q` while `grep -Fq`, `grep -qF`, `grep -qE`, `grep -e x -q` and `grep --quiet` are the same race in another coat. Now one pattern over CODE lines only (this file documents the rule in prose, and a scan that reads its own prose as code can never be satisfied — the reason `audit_c_with_classes.py` strips comments), plus a row for the other early-exit consumer, `head`, whose pattern requires the payload to be a variable the shell already holds so the twelve `| head -1` uses already in the file are not hits. Two controls ship with it (five early-exit spellings must match; `grep -c`, `grep -cE`, `wc -l`, `sort` must not), and non-vacuity is proven by mutation: injecting `printf x | grep -Fq pat` reds the file row, injecting `printf y "$SOME_VAR" | head -1` reds the head row, narrowing the pattern back to one spelling reds the positive control (expected 5, got 4). `EmurunHarness` is 253 → **257 checks**. **REVIEW-7 OPEN QUESTION 1** (is `MAX_SECS=3600` an owner-approved narrowing?) is answered in the file it asks about: a comment above `MAX_SECS` names the caller class — not the caller that wants a long run (every `--secs` in this repository's history is a plain decimal ≤ 45 and the acceptance passes none) but the wrapper whose variable went empty or wrong, and a soak past an hour is a different instrument that should ask for a frame count. **No owner decision is claimed.** Gates: `make -C tests/scripts -j8` rc 0, `All cases match the oracle.`, **765 PASS / 0 FAIL** (was 761, +4 = the net new rows exactly); `DISPLAY=:99 bash tools/check.sh` rc 0, `CHECK: ALL GATES PASSED`, `PASS L3_f1.bmp sha=58615fc85d02e009`, `BATCH INTEGRITY: PASS`, `COMPLETION: LEDGER VERIFIED`, `PASS gates_core_parity`, `SKIP gates_smdh`; `make -C mdk_3ds -j8` rc 0 with `mdk.elf` md5 `728eb2350c3ef8a81acda15f75617c1e` **UNCHANGED** — correct, the diff is a comment block in a shell harness, one shell test, and docs. Acceptance `DISPLAY=:99 mdk_3ds/tools/emurun.sh --expect hud` → **rc 0**: Mandarine r1.2.1 md5 `0d2adab6…`, `:99`, llvmpipe, `graphics_api=1`, `render_3d=0`, dspfirm seeded, level 7, frame 2460 by the deadline (46 lines) and 2520 after the reap (47), `HUD VISIBLE -- 121 canary pixels in the panel, 121 inside the marker's rect 0,0-11,11 (bbox 0,0-10,10, reaching the derived (10,10))` — the verdict batches 160-161 and review-7 all printed, which is the point: this batch changed no behaviour in the harness. Frame `docs/frames/2026-11-13-the-numbers-i-ran.png` is byte-identical to 178 predecessors (re-derived: 180 PNGs, 179 at md5 `6769a137…`) and the census was re-run, not remembered: **zero** pure (0,0,255) pixels, so the canary evidence is the live run and never the committed frame. Prior-solution lookup: Pi knowledge searched (`mdk-sdl`), no prior note on the class; **Obsidian and Cognee MCP servers are ABSENT from this session's tool list** (AGENTS §9 P9: a session fault, not a licence to claim the lookup), so no Cognee graph lookup is claimed and the vault was read read-only from disk. Full record: `docs/evidence/2026-11-13-162-the-numbers-i-ran.txt`, plus the CORRECTION block appended to batch 161's evidence file.

**PREVIOUS BATCH 161 RECORD** (its findings and their disposition are in `docs/evidence/2026-11-13-161-secs-and-the-race.txt`, including the CORRECTION block batch 162 appended to it, and in full as `## Iteration 161` in `docs/PROGRESS_LOG.md`). **BATCH 161 (attempt 7) — review-6 F1/F2/F3, all confirmed, all fixed: THE BASE DEFECT WAS ONE FLAG OVER, AND THE TEST HAD A RACE IN IT.** `--secs` was the other caller-spelled field and the only one reaching `$(( ))`, where a leading zero is a base: `--secs 010` settled 8 s with a confident verdict and `--secs ""` settled for nothing, so `MAX_SECS=3600`, a parse-time `decimal_measurement` refusal by name, and `10#$SECS` at the arithmetic went in; and a presence check that piped a 32 KB lifted block into `grep -q` was a SIGPIPE race under the test's own `pipefail`, so one `lift_contains` helper replaced all four. **WHAT IT LEFT FOR REVIEW-7 TO FIND was a four-way table that named the grandparent's artifacts the parent's, two flake rates that did not reproduce, and a shape row that matched one spelling of the class it claimed — all closed by batch 162.**

**PREVIOUS BATCH 160 RECORD** (its findings and their disposition are in `docs/evidence/2026-11-13-160-one-base.txt`, including the CORRECTION block batch 161 appended to it, and in full as `## Iteration 160` in `docs/PROGRESS_LOG.md`). **BATCH 160 (attempt 6) — review-5 F1/F2/F3, all confirmed, all fixed: THE COUNTS WERE COMPARED IN TWO DIFFERENT BASES.** Batch 159's new bbox arm used bash arithmetic, which reads `010` as OCTAL 8, while every `[ ]` in the same function read it as DECIMAL 10: `30 30 010 0 11 11` — a 24-pixel box holding 30 pixels — printed `HUD VISIBLE` at rc 0, and `121 121 08 0 11 11` aborted the script at the undocumented rc 1; `10#`-qualified, and the octal reading could also FALSE-RED a padded-but-consistent box, which is the half that matters for a row. Its F2 replaced a quadratic zero-strip (779 ms at 4 002 chars, 22 s at 20 002 — a hang, not a perf note) with one substring test on the prefix, proven equivalent to the shipped predicate over 196 field × bound pairs. Its F3 corrected a gate verdict credited to the wrong gate. **WHAT IT LEFT FOR REVIEW-6 TO FIND was `--secs` (batch 161's F1), the piped presence check (batch 161's F2), and a collateral claim that could not have failed (batch 161's F3) — all closed by batch 161.**

**PREVIOUS BATCH 159 RECORD** (its findings and their disposition are in `docs/evidence/2026-11-13-159-counts-and-bbox.txt`, including the CORRECTION block batch 160 appended to it, and in full as `## Iteration 159` in `docs/PROGRESS_LOG.md`). **BATCH 159 (attempt 5) — review-4 F1/F2/F3, all Low, all confirmed, all fixed: THE COUNTS AND THE BOX THEY HAPPENED IN WERE NEVER COMPARED TO EACH OTHER.** `121 121 10 10 10 10` — a ONE-pixel box holding 121 pixels — printed `HUD VISIBLE` at rc 0, and its mirror `121 121 0 0 12 12` did too, because containment had already proven every counted pixel inside 0,0-11,11 yet nothing compared the bbox to the rect or to its own area. One arm now refuses both (`area >= total`, `bbox <= rect`), placed after the bbox's own validation so no difference is negative. Its F2 made the length arm measure the NUMBER instead of the string (a padded `--min-frames 0000060` is the decimal 60), and its `need_value` guard stopped a flag that lost its value from aborting at an undocumented rc 1. Its F3 corrected a remembered predecessor count. **WHAT IT LEFT FOR REVIEW-5 TO FIND was the base the new arm read its fields in (batch 160's F1), the quadratic cost of the strip its own F2 introduced (batch 160's F2), and a gate verdict credited to the wrong gate (batch 160's F3) — all closed by batch 160.**

**THE GATE PROVED ITSELF AGAINST THE COMMIT IT REPLACED, AND THE COLLATERAL CLAIM IS NOW A SET DIFFERENCE, NOT A COUNT.** New test on the parent harness: **214 checks, 20 FAIL**; parent test on the parent harness: **182 checks, 0 FAIL** (green, as it must be); new test on the working harness: **214 checks, 0 FAIL**. The red labels were diffed against the label set of the parent test run on the parent harness and **the intersection is EMPTY — 0 pre-existing collateral**. The 20: 7 F1 bbox/count refusals, 6 missing-value rows, 6 padded-decimal accepts (only 2 of the 6 CAN fail on the parent — it already accepted `0`, `00`, `099`, `0060`, because the length arm only bites past the bound's own digit count), 1 named-not-unbound, 1 fault-string-gone, 1 parse-level padded floor, 1 frame-floor padded floor, 1 octal derivation. **A METHOD TRAP WORTH RECORDING, BECAUSE IT NEARLY PRODUCED A FALSE RESULT:** the test derives `ROOT` from its OWN path, so a parent copy of the TEST placed in `/tmp` reds 37 rows for path reasons that have nothing to do with the harness and look exactly like real collateral; the harness path is a parameter, the test path is not. `EmurunHarness` is 182 → **214 checks**.

**THE MEASUREMENT THAT MATTERS IS THAT THE STRICTER RULE DID NOT FALSE-RED.** `DISPLAY=:99 mdk_3ds/tools/emurun.sh --expect hud` → **rc 0**: Mandarine r1.2.1 md5 `0d2adab6`, `:99`, level 7, frame 2460 by the deadline and 2520 after the reap, `HUD VISIBLE -- 121 canary pixels in the panel, 121 inside … (bbox 0,0-10,10, reaching the derived (10,10))`. New-rule margins: area 121 ≥ total 121 (**margin 0**), bbox 10 ≤ rect 11 (margin 1), inside 121 ≤ 144 (margin 23). The area arm's zero margin is not a fragility — for a SOLID marker the bbox area and the count are the same number by definition, so the arm is an identity for what `drawDiagnostic` paints; the extent arm's zero margin (item 1) is the genuinely tight case, forced by geometry rather than identity. **The co-tenant hazard review-4 declined to run for was live here** — two `perfect_dark` Mandarine processes on the shared `:99`, and `emurun.sh` captures `import -window root`, the whole screen — but the verdict self-validates against it: a foreign capture has no (0,0,255) pixels in the panel, which is `HUD NOT VISIBLE` at rc 7, never a pass. Gates: `make -C tests/scripts -j8` rc 0, `All cases match the oracle.`, **722 PASS / 0 FAIL** (was 690); `DISPLAY=:99 bash tools/check.sh` rc 0, `CHECK: ALL GATES PASSED`, `PASS L3_f1.bmp sha=58615fc85d02e009`, `BATCH INTEGRITY: PASS`, `COMPLETION: LEDGER VERIFIED`; `make -C mdk_3ds -j8` rc 0 with `mdk.elf` md5 `728eb2350c3ef8a81acda15f75617c1e` **UNCHANGED** — correct, the diff is one shell harness and one shell test, no compiled source. Prior-solution lookup: Pi knowledge searched (`mdk-sdl`, hybrid and semantic), surfacing the sibling project's changed-pixel census — "changed: N … in band: N, outside band: 0", a count and a region checked against each other, which is F1's shape one subsystem over; **Obsidian and Cognee MCP servers are absent from this session's tool list** — a session configuration fault per AGENTS §9 P9, not a licence to claim the lookup happened — so the vault was read read-only from disk and no Cognee graph lookup is claimed.

**PREVIOUS BATCH 158 RECORD** (its findings and their disposition are in `docs/evidence/2026-11-13-158-one-measurement-rule.txt`, including the CORRECTION block batch 159 appended to it, and in full as `## Iteration 158` in `docs/PROGRESS_LOG.md`). **BATCH 158 (attempt 4) — review-3 F1/F2/F3/F4, all four confirmed and fixed: FOUR GUARDS ASKED "IS THIS A NUMBER?" AND FOUR ANSWERED DIFFERENTLY.** One `decimal_measurement <field> <max>` predicate replaced four spellings, and the canary's private copy was deleted — a second spelling is exactly how F1 happened (`*[!0-9]` shipped where the prose said `*[!0-9]*`, so `+121` and `-0` reached a ledger decision). F2 was the same fail-open 190 lines below the guard written to stop it: `--min-frames ""` disabled the short-run gate and printed a confident verdict over a 3-frame run; it is now validated in the argument loop and routed through `frame_floor`, which checks both sides and refuses under its own name. F3: an impossible near corner on a green line. F4: `inside > total` and `inside > 144` were both certified green; both bounds are derived from the rect constants. It reds exactly 35 of 182 on its parent with 0 collateral, and its own vacuous half is stated in its record (11 `dm` reject rows pass on the parent vacuously, which is why the 5 accept and 2 lift rows exist). **What it left for review-4 to find was the count/bbox relation (F1 above), the length arm measuring characters instead of value, and a remembered predecessor count (F3 above) — all closed by batch 159.**

**NEXT, in order.**
0. **Close `3ds.hud-canvas` and `linux.render-frame` from a lane that can write the ledger.** Unchanged and still the only blocker on both rows: `docs/COMPLETION_LEDGER.tsv` is `coleshores:coleshores 0664`, this lane is `piagent` (groups `piagent, development`), and `test -w` says NOT WRITABLE (re-checked this batch). The file is in `gates_batch_integrity.PROTECTED`, so a rename is a hand edit wearing a hat. The sanctioned closer is `tools/runner/ledger_close.py` — it re-runs the row's registered acceptance command and only then rewrites the row, committing `--no-verify` because the project hook (not the loop's gate) is what forbids a *model* from editing the ledger. So the operator fix is `chgrp development docs/COMPLETION_LEDGER.tsv` **and** a run of `python3 tools/runner/ledger_close.py --row 3ds.hud-canvas` from a lane in that group, on a clean tree. `3ds.hud-canvas`'s acceptance has been green since batch 147 and is green again here under a stricter rule than batch 160's; `linux.render-frame`'s since batch 149. **ATTEMPT-COUNT CORRECTION (batch 169):** the `attempt 8 of 8` sentence below was written for `3ds.hud-canvas` at batch 162; `linux.render-frame` is at attempt 5 of 8, and its registered acceptance `bash tools/check.sh` was green again in attempt 5 with all three gates re-run. Re-measured in attempt 5 rather than copied: the ledger is still `coleshores:coleshores 0664`, still NOT WRITABLE from this lane, and `linux.render-frame` is still `todo`. **This is attempt 8 of 8 on the row, and at 8 the loop stops for an operator. Eight consecutive attempts have now made the acceptance command stricter, kept it green, and — in batch 162 — re-verified the evidence that claims so; the row's blocker is a `chgrp` plus a `ledger_close.py` run from a lane in the `development` group, not a behaviour, and no attempt count changes that. THE OPERATOR ACTION IS UNCHANGED AND IS THE ONLY THING STANDING BETWEEN THIS ROW AND `done`.** Batch 163 re-checked it: `test -w` still says NOT WRITABLE, and `ledger_close.py --row linux.render-frame` now prints `working tree is dirty; refusing to certify a row against uncommitted work` — it will not certify against a dirty tree at all, and on a clean one it reaches the same `Permission denied`. So the closer needs a clean tree AND a writable ledger; both are operator-side.**
1. **The extent arm has a zero-pixel margin and inherits the 1:1 window assumption** (unchanged since batch 156). `bx1 == 10` is exactly the derived floor of 10.667, so any rasterisation difference of one pixel reddens the row. It is forced by the geometry on THIS emulator at THIS window size, and `tools/panel_crop.py` documents and makes the same assumption — but if a second emulator or window size ever enters the loop, re-derive `CANARY_X1/Y1/REACH_X/Y` then and not before. (Batch 159's new area arm also has margin 0 on the real run, but that one is an identity for a solid marker, not a geometric coincidence.)
2. **Nothing pixel-pins the HUD's own dial position** (unchanged). The rect is the MARKER's, drawn by `drawDiagnostic`; the dial's (333,191)-(388,232) placement is quoted from batch 147 and is not gated. A HUD that drew in the right place through a wrong matrix of its own would still pass `--expect hud`. Closing it needs the C# oracle's own dial rect, which needs the dotnet host of item 18.
3. **The aspect check pins the canvas width only to about ±7 %** (review-2 open question 1, unchanged). A `canvasWidth` anywhere in ≈ [533, 640) passes both the containment and the extent arms; this is the declared `[10,11]` pin behaving as designed, and closing it needs a second marker at a NON-corner canvas coordinate rather than a bigger tolerance.
4. **An explicitly supplied `--shot` is never freshness-checked** (review-2 open question 2, unchanged). Only the auto-`mktemp` path mints a fresh file, so `--shot /tmp/old.png` can be judged on a stale picture. Not reachable from the acceptance command, which always mints its own.
5. **The emulator's own capture is not committed** (review-3 open question 2, unchanged). Every file in `docs/frames/` is a 320×240 Linux capture containing **zero** (0,0,255) pixels, so the 1280×1024 emulator capture the canary verdict reads exists only as a log transcription; the acceptance re-runs it live, so the claim is reproducible but not independently auditable from the tree. Closing it means retaining the lane's `/tmp/mdk-emurun.*/` screenshot as a sidecar.
6. **The capture is a ROOT-window grab on a display other loops share** (review-4 open question 1, new but assessed here). `emurun.sh` uses `import -window root`, so a co-tenant Mandarine window on top of this lane's lands in the screenshot. Two `perfect_dark` runs were live on `:99` during batch 159 and the verdict still self-validated (a foreign capture has no blue pixels in the panel → `HUD NOT VISIBLE`, rc 7, never a pass), so the hazard is a false-red risk and not a false-green one. Closing it means capturing by window id, which needs the window to be found by the lane's own PID.
7. **Install a Khronos validation layer, or a second ICD, on this host.** Still the root of the `linux.render-frame` open items: `/usr/share/vulkan/explicit_layer.d` holds only nullhw, MESA overlay and MESA screenshot, so every VUID cited in batches 153-154 was read off the spec's naming. A fault-injection harness that makes one call fail and shows the refusal reaching the log is the missing instrument.
8. **A failed `vkEndCommandBuffer` leaves the buffer RECORDING, so the next frame's `vkResetCommandBuffer` is invalid use too** (`vk_context.cpp:1090`'s comment). Nothing un-records a buffer and the documented recovery is forbidden at that moment, so it is printed rather than solved.
9. **Unify B5's rule and C1's, and prove the unification is not a relaxation** (unchanged). C1's rule strictly generalises B5's; B5's arms must be re-run after the merge and must still red the same set on `git show HEAD:renderer_vk.cpp`. Its own slice.
10. **CLOSED by batch 167, by measurement and not by a fix.** The shape was true — `mdk_render_release_mesh` and `ensureCanvasBuffer` retire persistently mapped buffers and `destroyBufferPair` never calls `vkUnmapMemory` — but the implied hazard is not present: both retire sites already null the slot's mapping field and the only reader, `mdk_render_update_mesh`, already tests it for null behind a refusal before the `memcpy`. Whether an explicit unmap is REQUIRED before a free is a spec question this host cannot check and batch 167 refuses to answer on prose; what it pins instead is the property the hazard actually turns on — no mapping pointer outlives the retire that took its buffer, and no mapping is copied through untested — as check **B14** with four arms. The spec question itself moves to item 7, where it belongs.
11e. **CLOSED by batch 166.** The descriptor set waited two frames and the image view it names was freed on the spot: `mdk_render_update_texture`, `mdk_render_release_texture` and `mdk_render_release_palette` called `forgetDescriptorSets` and then `destroyImage` in the same call, so a frame still on the queue could be sampling a destroyed `VkImageView` while only the handle had been deferred — against AGENTS.md P3, which puts the boundary on the RESOURCE and not on the handle that names it. Batch 164 fixed the prose and named this as its own slice; batch 166 shipped it: `RetiredImage` beside `RetiredSet`, settled on the same `frameIndex + kFramesInFlight` boundary, with check **B13** and eight arms, the drop sites discovered rather than listed, and the retired slot field nulled where it was retired so teardown cannot destroy the same image twice. Latent on this frontend for the reason review-10 confirmed and restated in batch 166 §1.
11. **Descriptor-pool exhaustion: the leak is CLOSED (batch 163); the reachability question is not.** Nothing used to free a descriptor set, so every released-and-re-made texture or palette spent one of `maxSets = 4096` for the life of the process. `forgetDescriptorSets` now retires and `settleRetired` frees, pinned by B12 with ten arms — and since batch 164 the JOIN is pinned too, not just the parts: a retire that records nothing, a settle that never erases, a deadline one frame early, and a drop site that retires somebody else's set each redden B12 on their own (review-9 F1 had four mutants that all printed PASS). What is NOT closed: on this frontend the churn is still small (`sprite_residency_bytes = 0` on Linux means the sprite eviction never runs, and nothing in `mdk_core` calls `mdk_render_update_texture` or `mdk_render_release` yet), so the fix is proven by shape and by mutation, not by an executed exhaustion. When the sprite budget or the scope API gets wired on Linux, re-measure the live set count.
11b. **Nothing in the gate set compiles `mdk_linux/**`** (batch 163, new, and it is the row's own blind spot). `bash tools/check.sh` — the registered acceptance of `linux.render-frame` — runs eight contract gates; `gates_core_parity` compares source LISTS against `mdk_linux/CMakeLists.txt`; `gates_frames` uses `build-linux/mdk` if one exists and SKIPS if not. So a batch can change the Vulkan renderer and every gate stays green without compiling it, and a stale `build-linux/mdk` is certified by the frame gate as happily as a fresh one. `core.formats` sat broken for 25 batches for exactly this reason (the ledger header says so). The fix is a `gate` line in `tools/check.sh`, which is in `gates_batch_integrity.PROTECTED` — an owner act, not a batch. Until then: run `cmake --build build-linux -j8` by hand on any batch touching `mdk_linux/**` and say so in the record (batch 163 did, rc 0).
11c. **B5's `RESULT_VERBS` has no `Free`, and `vkFree*` is a mixed family** (batch 163, stated rather than hidden). `vkFreeMemory` and `vkFreeCommandBuffers` are void; `vkFreeDescriptorSets` returns a `VkResult`. B5 therefore cannot see a discarded `vkFreeDescriptorSets` result anywhere in the tree. Closing it properly means adding `Free` to the verbs and the two void entry points to `VOID_ENTRY_POINTS`, which MOVES B5 — and the file's own `CONTEXT_VOID_ENTRY_POINTS` precedent forbids moving B5 while a new check is being introduced. Batch 163 pinned the obligation for the one call it added (inside B12) and left the general rule for its own slice.
11d. **Review-8 F2 (Low), deferred with cause.** `EE_PAT` in `tests/scripts/emurun_harness_test.sh` matches only flag clusters ending in `q`, so `grep --silent` and `-m1/--max-count` — the same early exit — slip; `HD_PAT` requires `printf` and a brace-less `"$VAR"`, so `echo "$BLK" | head` slips. Confirmed real by the reviewer's own counts (`--silent` 32/100 red, `-m1` 23/100, both pattern counts 0). Deferred because the file is the acceptance test of ledger row `3ds.hud-canvas` and this iteration's ledger item is `linux.render-frame`; one row per batch. No live occurrence exists in the file today, which is why it is Low.
11f. **The Linux CLI now refuses bad VALUES and is still inconsistent about SPELLING** (batch 169, stated rather than hidden). `--frames` and `--seed` take their value as a separate word; every other option wants an `=`, and the refusal message now says so correctly (the in-flight draft named only `--seed`, and `--frames` does it too). Unifying the two styles is a caller-visible interface change of its own — `tests/frames/capture_cpp.sh` is the only caller in the tree, but the loop's own ad-hoc harnesses are not — and batch 169's subject was refusing, not unifying. Also still accepted, deliberately: `--size=320x240junk` and `--at=1,2,3junk`, because `sscanf` returns the fields it read and neither is a fail-open (both already refuse non-numeric and non-positive). And `linux_arg_parse_test.sh` SKIPS when `build-linux/mdk` is absent, which is item 11b wearing a different hat: on a lane that has not built the Linux target the 15 refuse arms never run, and a skip is not a pass.
12. **CLOSED by batch 168, by measurement: no, and the distance is not 256 vs 200 but 256 vs ZERO.** Level 3 at 600 frames and levels 1-7 at 300 frames each emit **zero** `[vk] REFUSED` lines, and so do 4096x3072, 16x16 and a nonexistent level; the whole-log census of the 600-frame run is `[main]` x10, `[audio]` x1, `[vk]` x0. `refused(` has 38 call sites, so the path is not unreachable code — it never fires because **lavapipe does not refuse an allocation on this host**, which is why the 2026-11-12 evidence had to inject faults to see a line at all. The cap exists to stop review-5 F2's 22500-line flood; a flood needs recurring refusals, which need an allocator that fails. **No gate was added for this**, because a gate green only while this ICD keeps never refusing would go permanently red the moment item 7 lands, and a permanently red gate is worse than none. **CLOSED by batch 169, which found the claim was never a measurement:** `--level 99` with a SPACE is not `--level=99`. The frontend never received the level and ran its default, level 3 — which is why the draw count matched. `--level=99` refuses with `must be 3-8`, rc 1, and there is no silent fallback. The whole "levels 1-7" sweep was level 3 seven times; the real sweep (six different draw counts, still zero refusals) is in batch 169's record.
12b. **Three questions review-8 asked that only the loop owner can answer** (run-20261113-021356, unchanged by batch 163). (i) Whether "attempt 8 of 8 stops the loop for an operator" is an owner-set budget at all — the reviewer could not find a per-row attempt cap in `ralph/builder.md`, `ralph/PROMPT.md`, `SPEC.md` or `SPEC-COMPLETION.md`, and neither can batch 163. (ii) Which risk classification governs a test-only change that can redden an acceptance: batch 162 declared `medium` and the reviewer was invoked as "Declared risk: high". Batch 163 declares `high` on its own reading (resource lifetime, target-specific) and notes the same gap. (iii) Rate variance on the reviewer's host (26/65/127 red at load 8.01 for the three block generations) neither confirms nor refutes batch 162's "same rate at equal load"; batch 163 did not re-measure it and claims nothing about it.
13. **Review-7 (run-20261113-021356, on batch 161): F1 (Medium, the four-way table named the grandparent's artifacts the parent's), F2 (Medium, two published flake rates did not reproduce and the "growth made it flakier" claim was backwards) and F3 (Low, the shape row matched one spelling of a class) are ALL FIXED by batch 162, none rebutted; its open question 1 is answered by a comment naming the caller class `MAX_SECS` protects against (no owner decision is claimed), its open question 2 is answered by the mechanism measurement — the driver is bytes still to write after the consumer stops reading, plus host load, not payload size — and its open question 3 is closed by re-running the collateral row on the true parent (227 checks, 0 FAIL). Its open question 4 (hardware) is item 18. **Review-6 (run-20261113-021356, on batch 160): F1 (Medium, `--secs` unvalidated and read in octal), F2 (Medium, the piped presence check was a SIGPIPE race) and F3 (Low, a collateral claim that could not have failed) are ALL FIXED by batch 161, none rebutted; its open questions 1 and 2 are closed by batch 161** (the shape chosen was the predicate AND `10#`, with `MAX_SECS` derived as a relation to `MAX_FRAMES`; and the race is gone because there is no pipe). Its open question 3 — the reviewer declined to re-run the live acceptance because of co-tenants on `:99` — is answered by batch 161's own run, rc 0, and its root-window concern stays item 6. **Review-5 (run-20261113-021356, on batch 159): F1 (Low), F2 (Medium) and F3 (Low) were ALL FIXED by batch 160, none rebutted. Its open questions 2 and 3 are CLOSED by batch 160** (the padded-field class is closed in the arithmetic, not by a second parser spelling; and a >6 KB padded floor is defended, at linear cost). **Its open question 1 — the reviewer declined to re-run the live acceptance because of co-tenants on `:99` — is answered by batch 160's own run, rc 0**, and its open question 1's second half (the shot is not retained) is item 5 here, unchanged, as is item 6 for its root-window concern.
14. **A padded bbox field that is CONSISTENT in decimal still prints its padding on the success line** (batch 160, stated rather than hidden). `121 121 000 0 010 010` passes and prints `bbox 000,0-010,010`. The numbers are the same numbers, the relation holds in the base every other reader uses, and `canary_stats` prints Python ints so it cannot emit the spelling; refusing it would be a spelling check wearing a relation's clothes. If a future capture source ever emits padded fields and the printing bothers a reader, normalise at the print, not at the guard.
15. **Review-4 (run-20261113-021356, on batch 158): F1, F2 and F3 (all Low) are FIXED by batch 159, none rebutted; its open question 2 is closed by batch 159 and its open questions 1 and 3 are items 6 and 5 here.** Its "what I attacked and could not break" list reproduced batch 158's claims exactly, including the 35-of-182 parent comparison and the vacuity disclosure, so no re-verification is owed on those.
16. **Review-4 (run-20261112-035144): F1 and F3 fixed by batches 155-156; F2/F4/F6/F7 stay open, deferred with cause, none rebutted** — record corrections in `docs/evidence/2026-11-12-14{6,7}-*.txt`, `docs/OPEN_ITEMS.md:294`, `docs/PROGRESS_LOG.md`, and the non-reproducible `.3dsx` hash.
17. **The level's own geometry on the console stays open** (batch 147 item 1) and **the panel instruments need re-baselining** (batch 147 item 2): every committed before-crop is a sunburst, so every panel A/B floor is measured under the old defect.
18. **The Linux frame's fidelity half is dark on this host and cannot be relit here.** `gates_frames` check 3 needs `/tmp/mdk-frame-reference/L3_f1.bmp`, and the oracle needs a dotnet 10 host; `/tmp/kilo` is `coleshores`-owned and not writable by this `piagent` lane, so captures go to `/tmp/mdk-lane`. Blocked, not unknown, and owner-gated behind `tests/frames/regenerate_reference.sh`. **Old-3DS hardware evidence stays open** (AGENTS.md P6/P8): everything here is Mandarine r1.2.1 on llvmpipe, mono, `:99`. Emulator-green is not hardware-pass.
19. **`docs/CONTINUATION.md` is ~3000 lines, not the ~150 the builder contract asks for.** The tail is batch records already in `docs/PROGRESS_LOG.md` in substance, so trimming it is a documentation task of its own and was not smuggled into a harness batch. Batch 159 condensed batch 158's inline record to a pointer-plus-summary and stopped there; batch 160 did the same to batch 159's; batch 161 did the same to batch 160's (five paragraphs → one pointer-plus-summary, its own record is six), batch 162 did the same to batch 161's (six paragraphs → one pointer-plus-summary, its own record is one long block plus the NEXT list) and stopped there; the tail is untouched.

**PREVIOUS BATCH 157 RECORD** (its own NEXT list is superseded by the one above; its findings and their disposition are in `docs/evidence/2026-11-13-157-measurement-bound.txt` and, in full, as `## Iteration 157` in `docs/PROGRESS_LOG.md`). **BATCH 157 (ledger row `3ds.hud-canvas`, attempt 3) — review-2 F1 and F2 (both Low, both confirmed, both fixed): THE GUARD COULD READ DIGITS BUT NOT NUMBERS.** A `case` character class plus a silent reset-to-safe could not express "this field is a measurement": `[` exits 2, not 1, when it cannot parse an operand and an `if` cannot tell that apart from false, so a digit string past `LONG_MAX` walked past every rejection and printed `HUD VISIBLE -- 9223372036854775808 canary pixels` at rc 0 — seven out-of-range stats lines all certified green at the parent. Batch 157 replaced the resets with named `HUD MALFORMED` refusals, bounded each field by the panel's own geometry (`CANARY_MAX_PIXELS=96000`, `CANARY_MAX_COORD=399`, both re-derived from `tools/panel_crop.py` by the test), checked the harness's own five canary constants, and pinned the premise (`[ 9223372036854775808 -lt 25 ]` must exit 2). It reds exactly 10 of 130 on the parent, 0 collateral. **What it left for review-3 to find was the arm itself (`*[!0-9]` where the prose said `*[!0-9]*`), the unvalidated `--min-frames`, the unchecked near corners, and the unchecked cross-field relations — all closed by batch 158.**
**PREVIOUS BATCH 156 RECORD** (its own NEXT list is superseded by the one above; its findings and their disposition are in `docs/evidence/2026-11-13-156-canary-extent.txt`, including the CORRECTION block batch 157 appended to it). **BATCH 156 (ledger row `3ds.hud-canvas`, acceptance `mdk_3ds/tools/emurun.sh --expect hud`, attempt 2) — review-1 F1 (Medium) and F2/F4/F5 (Low), all against the row's own acceptance command, all confirmed, all fixed. THE CANARY COULD SEE THE CORNER BUT NOT THE SIZE.** Batch 155 gave the canary a rectangle, but tested it from one side only: `inside` counted pixels with `cx <= CANARY_X1 and cy <= CANARY_Y1` and passed at ≥ 25, while the far corner `bx1/by1` was computed, printed, and **never compared to anything**. Built the reviewer's scenario before touching anything: `block(0,0,27,11)` — what `mdk_render_canvas_width` returning 240 instead of 600 paints — got `HUD VISIBLE -- 297 canary pixels in the panel, 132 inside … (bbox 0,0-26,10)`, **rc 0**, from `git show HEAD` of the harness, with the contradicting bbox on the same success line. The rect is now enforced from BOTH sides: `inside != total` → `HUD OVERSIZED` (containment) and `bx1 < CANARY_REACH_X || by1 < CANARY_REACH_Y` → `HUD UNDERSIZED` (extent), with `CANARY_REACH_X/Y = 10 = floor(10.667)` the derived far corner without the `ceil` slack `CANARY_X1/Y1 = 11` already carries. Containment already implies `bx1 <= 11`, so the pair pins the marker's bbox to [10,11] per axis — the extent the canvas's own width produces. **THE REVIEWER FOUND HALF THE HOLE; THE OTHER HALF IS THE MIRROR IMAGE.** A `canvasWidth` too LARGE shrinks the marker so it sits wholly inside the rect and still clears the floor (`block(0,0,8,8)` = 64 px, all inside): an inside-count passes it and containment passes it, and only an extent test sees it. That row is `HUD UNDERSIZED`. **F1's SECOND HALF: THE DERIVATION WAS A SECOND SPELLING, NOT AN IMPORT.** The test restated `far = size*sw/ch` by hand — a second source of truth for the very thing it was written to pin, and the mistake `tools/panel_geom.py` names as the reason `panel_crop.py` exists ("IMPORTED and not re-spelled"). It now reads the return EXPRESSION out of `mdk_render_canvas_width`'s body, substitutes the constants it names, and hard-fails on an expression it cannot resolve; and it derives the two axes SEPARATELY (`far_x = kSize·kScreenHeight/canvasWidth`, `far_y = kSize·kScreenWidth/kCanvasHeight`) because a canvasWidth error moves one and not the other. **AND THE IMPORT IS PROVEN, NOT ASSERTED:** the test mutates a COPY of `renderer_c3d.cpp` swapping the two screen axes in `mdk_render_canvas_width` — the exact mistake that function's own comment warns turns 600 into 216 — and requires the derivation to move, and to move only the axis that must: real tree `11 11 10 10`, mutant `30 11 29 10`. Had the ratio still been spelled by hand, the mutant would have derived the same rect and the row would have passed while certifying nothing. Full record: `docs/evidence/2026-11-13-156-canary-extent.txt`.

**F5 (Low, CONFIRMED): MALFORMED EVIDENCE CERTIFIED GREEN.** `case "${total}${inside}"` concatenated the two fields, so a stats line whose first field was blank read as "all digits", both comparisons then died on "integer expression expected", no branch was taken, and the function **fell through to `HUD VISIBLE`** — a fail-open on the line that decides a ledger row, where the guard it replaced failed closed. Each field is now validated on its own (bad count → 0 → fails the floor; bad bbox → −1 → fails the reach), and four rows pin it by stubbing `canary_stats`, because the point is the guard and not the Python that today always prints six fields. A 5-field partial print, which the parent certified `HUD VISIBLE`, now reds. **F2 (Low, CONFIRMED): THE FIXTURE WAS NOT THE TRANSFORM IT NAMED.** The `mirror` fixture built `block(W-rx1, H-ry1, …)` = (389,229)-(399,239); a reflection about the vertical axis maps (x,y)→(W-1-x,y) and lands at (389,0)-(399,10), so the fixture was the 180°/opposite-sense disposition, determinant **+1**, not the reflection the prose and batch 155's record both claimed. The fixtures are now BUILT FROM THE SIGN PAIR — `disposition(a,b)` computes the corner from `canvasMatrix`'s `out col0 = a·m col1`, `out col1 = b·m col0` — and all four dispositions are rows: `here` (1,−1) rc 0, `turn` (1,+1) rc 1, `flip` (−1,−1) rc 1, `rot180` (−1,+1) rc 1. **F4 (Low, CONFIRMED): THE FIXTURE NOW REFUSES TO RUN** when the harness declares no numeric constant, instead of carrying an empty value into python, getting ValueError, getting an empty capture, and reding four PRE-EXISTING rows for a fixture reason while one non-vacuity row passed vacuously on an awk string comparison. **F3 (Low, CONFIRMED, RECORD ONLY):** batch 155's record claimed its sidecar carried the standing disclosure; it carried four generated lines. The published pair cannot be edited (the integrity gate forbids it), so the correction is here and in the progress log, and this batch's own sidecar carries its disclosure — including that the pinned frame contains **zero** (0,0,255) pixels and cannot show the canary at all.

**THE GATE PROVED ITSELF AGAINST THE COMMIT IT REPLACED, AND THE COMPOSITION IS STATED (F4's second half).** The hard fail on missing constants makes the raw parent file unusable for a comparison, so the evidence isolates the RULE change from the CONSTANT addition — the parent's verdict with this batch's constants added — and it reds **exactly three of 112 and nothing else**: the containment row, the extent row, and the malformed-stats row. **3 new rows red, 0 pre-existing collateral, 0 vacuous.** The three disposition rows and their non-vacuity rows pass on the parent because the parent's inside-count already caught the corner family, which is the correct answer: those rows pin review-4 F1, which batch 155 closed. `EmurunHarness` is 95 → **112 checks**.

**THE MEASUREMENT THAT MATTERS IS THAT THE STRICTER RULE DID NOT FALSE-RED.** `DISPLAY=:99 mdk_3ds/tools/emurun.sh --expect hud` → **rc 0** on the real emulator: Mandarine r1.2.1 md5 `0d2adab6`, llvmpipe forced, `graphics_api=1` (OpenGL), `render_3d=0` (mono), dspfirm seeded, level 7, frame 2520 by the deadline and 2580 after the reap, `HUD VISIBLE -- 121 canary pixels in the panel, 121 inside … (bbox 0,0-10,10, reaching the derived (10,10))`. Containment 121 == 121, extent 10 ≥ 10. **The extent arm's margin is zero pixels**, which is uncomfortable for an acceptance command and is written down rather than hidden: the marker's far edge is at crop 10.667, pixel centres below it are columns 0..10, so `bx1 == 10` is forced by the geometry and not by the driver's mood. Gates: `make -C tests/scripts -j8` rc 0, `All cases match the oracle.`, **620 PASS / 0 FAIL** (was 603); `DISPLAY=:99 bash tools/check.sh` rc 0, `CHECK: ALL GATES PASSED`, `PASS L3_f1.bmp sha=58615fc85d02e009`, `BATCH INTEGRITY: PASS`, `COMPLETION: LEDGER VERIFIED`; `make -C mdk_3ds -j8` rc 0 with `mdk.elf` md5 `728eb2350c3ef8a81acda15f75617c1e` **UNCHANGED** — correct, the diff is one shell harness and one shell test, no compiled source. Prior-solution lookup: Pi knowledge searched (`mdk-sdl`), surfacing the imported-not-re-spelled doctrine; **Obsidian and Cognee MCP servers are absent from this session's tool list** — a session configuration fault per AGENTS §9 P9, not a licence to claim the lookup happened — so the vault was read read-only from disk and no graph lookup is claimed.

**NEXT, in order.**
0. **Close `3ds.hud-canvas` and `linux.render-frame` from a lane that can write the ledger.** Unchanged and still the only blocker on both rows: `docs/COMPLETION_LEDGER.tsv` is `coleshores:coleshores 0664`, this lane is `piagent` (groups `piagent, development`), and the file is in `gates_batch_integrity.PROTECTED` so a rename is a hand edit wearing a hat. **The operator fix is one command: `chgrp development docs/COMPLETION_LEDGER.tsv`.** `3ds.hud-canvas`'s acceptance has been green since batch 147 and is green again here under a rule stricter than batch 155's; `linux.render-frame`'s since batch 149.
1. **The extent arm has a zero-pixel margin and inherits the 1:1 window assumption** (this batch, sharpening the old item 1). `bx1 == 10` is exactly the derived floor of 10.667, so any rasterisation difference of one pixel reddens the row. It is forced by the geometry on THIS emulator at THIS window size, and `tools/panel_crop.py` already documents and makes the same 1:1 assumption — but if a second emulator or a different window size ever enters the loop, re-derive `CANARY_X1/Y1/REACH_X/Y` then and not before.
2. **Nothing pixel-pins the HUD's own dial position** (unchanged from batch 155). The rect is the MARKER's, drawn by `drawDiagnostic`; the dial's (333,191)-(388,232) placement is quoted from batch 147's measurement and is not gated. A HUD that drew in the right place through a wrong matrix of its own would still pass `--expect hud`. Closing it needs the C# oracle's own dial rect as the expectation, which needs the dotnet host of item 11.
3. **Install a Khronos validation layer, or a second ICD, on this host.** Still the root of the `linux.render-frame` open items: every VUID cited in batches 153-154 was read off the spec's naming because `/usr/share/vulkan/explicit_layer.d` holds only nullhw, MESA overlay and MESA screenshot. A fault-injection harness that makes one call fail and shows the refusal reaching the log is the missing instrument.
4. **A failed `vkEndCommandBuffer` leaves the buffer RECORDING, so the next frame's `vkResetCommandBuffer` is invalid use too** (`vk_context.cpp:1090`'s comment). Nothing un-records a buffer and the documented recovery is forbidden at that moment, so it is printed rather than solved.
5. **Unify B5's rule and C1's, and prove the unification is not a relaxation** (unchanged). C1's rule strictly generalises B5's; B5's arms must be re-run after the merge and must still red the same set on `git show HEAD:renderer_vk.cpp`. Its own slice.
6. **The retire path frees memory it never unmapped** (batch 152 item 1, unchanged). `mdk_render_release_mesh:2158` and `ensureCanvasBuffer:2381` retire persistently mapped buffers and `settleRetired → destroyBufferPair:282` never calls `vkUnmapMemory`. A shape decision about `RetiredBuffer`, not an argument.
7. **Descriptor-pool exhaustion is still not proven reachable** (review-5 F1 and every review since — unchanged). Nothing frees a descriptor set; every released-and-re-made texture or palette consumes one of `maxSets = 4096` for the life of the process.
8. **Is the 256-entry refusal memory reachable in one session?** (review-7 open question 1, unchanged.)
9. **Review-1 (run-20261113-021356, on batch 155) — no Critical/High, so nothing was mandatory; F1 (Medium) and F2/F4/F5 (Low) are FIXED by this batch, F3 is a record correction on a published pair that cannot be edited; none rebutted.** Its own open questions stay open and were not smuggled into this batch: it did not re-run the emulator (this batch did, rc 0), and **which display rotation is real is pinned by no committed artifact** — the correct marker's corner is invariant under both candidate rotations, so the measurement cannot distinguish them, and pinning it needs Mandarine's GX-present path or a second marker at a non-corner canvas coordinate.
10. **Review-4 (run-20261112-035144): F1 and F3 fixed by batch 155; F2/F4/F6/F7 stay open, deferred with cause, none rebutted** — record corrections in `docs/evidence/2026-11-12-14{6,7}-*.txt`, `docs/OPEN_ITEMS.md:294`, `docs/PROGRESS_LOG.md`, and the non-reproducible `.3dsx` hash.
11. **The level's own geometry on the console stays open** (batch 147 item 1) and **the panel instruments need re-baselining** (batch 147 item 2): every committed before-crop is a sunburst, so every panel A/B floor is measured under the old defect.
12. **The Linux frame's fidelity half is dark on this host and cannot be relit here.** `gates_frames` check 3 needs `/tmp/mdk-frame-reference/L3_f1.bmp`, and the oracle needs a dotnet 10 host; `/tmp/kilo` is `coleshores`-owned and not writable by this `piagent` lane, so captures go to `/tmp/mdk-lane`. Blocked, not unknown, and owner-gated behind `tests/frames/regenerate_reference.sh`. **Old-3DS hardware evidence stays open** (AGENTS.md P6/P8): everything here is Mandarine r1.2.1 on llvmpipe, mono, `:99`. Emulator-green is not hardware-pass.
13. **`docs/CONTINUATION.md` is ~2900 lines, not the ~150 the builder contract asks for.** The tail is batch records already in `docs/PROGRESS_LOG.md` in substance, so trimming it is a documentation task of its own and was not smuggled into a harness batch.

**PREVIOUS BATCH 155 RECORD** (its own NEXT list is superseded by the one above; its findings and their disposition are in `docs/evidence/2026-11-13-155-canary-rect.txt`). Risk then: medium — it shipped no compiled code and made a ledger row's acceptance command stricter.

**BATCH 155 (ledger row `3ds.hud-canvas`, acceptance `mdk_3ds/tools/emurun.sh --expect hud`, attempt 1) — review-4 F1 and F3, both against the row's own acceptance command, and both held. THE CANARY COULD COUNT BUT NOT SEE (F1, Medium, CONFIRMED and reproduced).** `emurun.sh`'s pixel verdict counted pure (0,0,255) pixels in the panel and asked nothing about where they were. `pica::canvasMatrix` (`pica_matrix.cpp:177-182`) does the panel quarter turn as `out.x = m.y`, `out.y = -m.x`; drop the minus, the determinant goes +1 → −1, and the marker paints in the panel's far corner at the same size and the same pixel count. Built the reviewer's scenario rather than arguing with it: an 11×11 block at crop (389,229) gets `HUD VISIBLE -- 121 canary pixels`, rc 0, from `git show HEAD` of the harness and `HUD MISPLACED … bbox (389,229)-(399,239)`, rc 1, from this batch. That is the same blind spot that hid batch 147's word-order bug for eleven runs — an orientation defect behind a quantity that cannot see orientation — wearing a different hat. **THE RECT WAS DERIVED BEFORE IT WAS MEASURED.** From shipped constants only: `kCanvasHeight 360`, `kScreenWidth 240`/`kScreenHeight 400`, `drawDiagnostic`'s `kSize 16`, `canvasWidth = 360·400/240 = 600` → the marker's canvas [0,16]² lands at crop `16·400/600 = 16·240/360 = 10.667`, i.e. columns/rows 0..10, an 11×11 block of **121 pixels**; `ceil` adds one column and one row of rasterisation slack → the declared rect 0,0-11,11. The console then printed `121 canary pixels … 121 inside … bbox 0,0-10,10`. Batch 147 had quoted 121 and (0,0)-(10,10) without deriving them; the two now meet from opposite directions. **F3 (the harness contradicted itself) is fixed in the same file:** the block declaring a green `--expect hud` "is not a visibility verdict and must not be recorded as one" outlived the code it described by two batches while `canary_verdict` two screens below printed `HUD VISIBLE`. The replacement states what the flag proves and names what it cannot: no HUD-off build exists, so this is a known-colour probe and not a reference diff, and it says nothing about the HUD's own quads being correct, coloured or complete. A distinct refusal — `HUD MISPLACED` — keeps a wrong-corner canvas from being misread as the invisible-HUD case, and the success line now prints the bbox so a green run records WHERE. Full record: `docs/evidence/2026-11-13-155-canary-rect.txt`.

**THE GATE PROVED ITSELF AGAINST THE COMMIT IT REPLACED, FOR FREE.** `tests/scripts/emurun_harness_test.sh` already took the harness path as `$1`, so the parent comparison is one command: `bash tests/scripts/emurun_harness_test.sh /tmp/emurun-parent.sh` reds **11** of the new **95** checks (was 84) and none of the 84 existing ones was touched. Two new rows are the non-vacuity and cannot be waved at: the mirrored capture DOES carry a full marker (the old count rule would have passed it) and carries NONE of it inside the rect. And because a hand-copied expectation is a second source of truth, the test re-derives the rect from the four shipped constants and fails on disagreement — a constant it cannot find is a hard failure, never a skipped check.

**GATES, AS RUN.** `make -C tests/scripts -j8` rc 0, `All cases match the oracle.`, **603 PASS / 0 FAIL** (was 592), `PASS EmurunHarness (95 checks: config placement + mount ownership + the HUD canary)`. `DISPLAY=:99 bash tools/check.sh` rc 0, `CHECK: ALL GATES PASSED`, `PASS L3_f1.bmp sha=58615fc85d02e009`, `BATCH INTEGRITY: PASS`, `COMPLETION: LEDGER VERIFIED`. `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds -j8` rc 0, `mdk.elf` md5 `728eb2350c3ef8a81acda15f75617c1e` UNCHANGED — correct, no compiled code changed. Emulator evidence (owed anyway: the acceptance IS an emulator run): `DISPLAY=:99 mdk_3ds/tools/emurun.sh --expect hud` → rc **0**, Mandarine r1.2.1 md5 `0d2adab6`, llvmpipe forced, `graphics_api=1` (OpenGL), `render_3d=0` (mono), dspfirm seeded, level 7, reached frame 2520 and 2580 after the reap. The frame is `cmp`-identical to its 171 identical predecessors (`6769a137…`, of 172 committed at HEAD) because nothing on the Linux render path changed; the sidecar carries the standing disclosure. Prior-solution lookup: Pi knowledge searched (surfaced the lift-don't-retype doctrine and `panel_geom.py`'s "a comparison that cannot fail enforces nothing"); **Obsidian and Cognee MCP servers are absent from this session's tool list** — a session configuration fault per AGENTS §9 P9, not a licence to claim the lookup happened — and the vault was read read-only from disk instead.

**PREVIOUS BATCH 154 RECORD** (its own NEXT list is superseded by the one above; its findings and their disposition are in `docs/evidence/2026-11-13-154-frame-stream-closed.txt`). Risk then: high — a call is added to the per-frame submit sequence (the frame's command buffer is now ended before it is queued), a refused frame now posts an empty submit to the queue, and an abandoned one-shot pair changes what a driver-failure path frees.

**BATCH 154 (ledger row `linux.render-frame`, acceptance `bash tools/check.sh`, attempt 8) — review-1 was right four times and wrong once, and the wrong one was its High finding. THE FRAME'S COMMAND STREAM WAS NEVER CLOSED (F1, Critical, pre-existing, confirmed).** `vk_context.cpp`'s `endFrame` called `vkCmdEndRenderPass` and submitted `frame.commandBuffer` while it was still RECORDING: `vkEndCommandBuffer` was called on it nowhere in the tree (`grep -rn EndCommandBuffer mdk_linux/src/` → two hits, both in the one-shot path of the other file). `VkSubmitInfo::pCommandBuffers` may only name a pending or executable buffer (VUID-vkSubmitInfo-pCommandBuffers-00075), so every frame this backend ever presented was submitted invalid, and the 139 committed screenshots are evidence about lavapipe's leniency and none about the submission. The end is called now (`:1090`), after the render pass and BEFORE the fence reset — a refused frame has to leave its fence signalled, and the reset is the call that would take that away — with its result read, because the one-shot path in the other file already had that rule and the frame path never applied it to itself. **THE UNFENCED ONE-SHOT FREED ITS OWN COMMAND STREAM (F3, High, confirmed).** Batch 152 stopped the callers releasing what an unfenced submission names and left the callee doing it: `endOneShot`'s tail ran `vkFreeCommandBuffers` + `vkDestroyCommandPool` on every outcome, and both entry points require the submitted commands to have completed — the one fact `unfenced` says is unknown. The guard at `renderer_vk.cpp:403` now returns before the releases and the pair is abandoned on the callers' terms. **THE LEFTOVER ACQUIRE WAS NOT A "STALE +1" (F4, Medium, confirmed).** `presentWait` is a BINARY semaphore: no count, two states, and a posted one makes the next `vkAcquireNextImageKHR` invalid use (VUID-vkAcquireNextImageKHR-semaphore-01779). Vulkan cannot withdraw a signal but it can consume one, so `drainPresentWait` (`:70`) posts an empty submit — no command buffers, TOP_OF_PIPE mask, just the wait — from the five exits that hold a posted signal (`:1012 :1020 :1092 :1114 :1122`) and deliberately not from the acquire-failure exits, where the state is unknown and waiting on a signal that may never arrive is a queue stall. **F2 (High) IS REBUTTED: `vkWaitForFences` DOES NOT RESET A FENCE.** The finding's first clause — "the wait succeeds and therefore resets the slot's fence to unsignalled" — is false, and the hang rests on it: waiting reports a fence and leaves it signalled; only `vkResetFences` returns it, and the only one in the file is at `:1112` in `endFrame`, immediately before the submit that signals it again. Every `return false` in `beginFrame` after the wait therefore leaves the fence SIGNALED, which is the invariant for a free slot, and the next wait returns at once. The `endFrame` submit path genuinely was a hang because THERE the fence had been reset — which is exactly why that one has a replacement and `beginFrame` must not grow one. No code changed; the premise is pinned by C3 instead of argued. **F5 (Low) confirmed:** the count that contradicted itself is 24, re-measured (calls=39 unchecked=24 at `f98a7cb`), and the strings-kept variant re-measured at 42/27 rather than the never-re-measured "60/23"; corrected in three files. Full record: `docs/evidence/2026-11-13-154-frame-stream-closed.txt`.

**THE GATE PROVED ITSELF AGAINST THE COMMIT IT REPLACED, AND THE ONE CHECK WITH NOTHING TO RED IS THE HONEST ONE.** `linux_refusal_report_test.sh` is 15 checks / 36 arms → **18 checks / 41 arms**: **C2** (the frame's buffer is ended, on the near side of the submit), **C3** (a refused frame never leaves its fence unsignalled — no reset in `beginFrame`, and the signalled replacement stays in `endFrame`), **C4** (every refusal that holds an acquired image consumes the acquire's signal), and **B10 extended to the callee** (the guard must precede the releases, and `endOneShot` must still be straight-line, because "before" only means "unreachable" without a loop). Against `git show HEAD:` of all three files it reds **exactly three of the eighteen and nothing else** — B10, C2, and C4 naming four real `return false` lines — while **C3 stays green on the parent, which is the correct answer for a check that pins a rebuttal rather than a defect**. One bug in C3's first draft was caught by running it and is the mistake this file has now made three times: the straight-line scan read text with string literals in it and matched the word "for" inside a refusal message, so the gate reported its own file as no longer straight-line; it scans `strip_code()` now.

**GATES, AS RUN.** `make -C tests/scripts -j8` rc 0, `All cases match the oracle.`, **592 PASS / 0 FAIL**, `PASS LinuxRefusalReport (18 checks, 41 arms adjudicated red)`. `DISPLAY=:99 bash tools/check.sh` rc 0, `CHECK: ALL GATES PASSED`, `PASS L3_f1.bmp sha=58615fc85d02e009`, `BATCH INTEGRITY: PASS`, `COMPLETION: LEDGER VERIFIED`. `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds -j8` rc 0 with `mdk.elf` md5 `728eb2350c3ef8a81acda15f75617c1e` UNCHANGED — no `mdk_3ds/` path in `git diff --name-only`, so no emulator run was owed and none was faked. `cmake --build build-linux -j8` rc 0. Healthy run on this tree: zero `REFUSED`, zero `[vk] … failed`, and the capture still printed `[vk] screenshot: wrote … (320x240)`. The frame is `cmp`-identical to its 139 predecessors (`6769a137…`) and its sidecar carries the disclosure — including the one honest weakness in this batch: F1 is the first change in this row that alters what the driver is asked to do on EVERY frame, and the evidence that it is neutral is an unchanged picture and a green suite, which is weaker than the evidence for the other four.

**NEXT, in order.**
0. **Close `linux.render-frame` and `3ds.hud-canvas` from a lane that can write the ledger.** `python3 tools/runner/ledger_close.py --row linux.render-frame` runs the acceptance, it passes, and it cannot write the row: `docs/COMPLETION_LEDGER.tsv` is `coleshores:coleshores 0664` and this lane is `piagent` (groups `piagent, development` — not `coleshores`). **The operator fix is one command: `chgrp development docs/COMPLETION_LEDGER.tsv`.** The acceptance has been green since batch 149. **This was attempt 8 of 8: the loop stops for an operator here, and the blocker is filesystem permissions, not code.**
1. **Install a Khronos validation layer, or a second ICD, on this host.** It is now the root of four open items: F1's VUID, F3's two, F4's and the reviewer's own open question 1 were all read off the spec's naming because `/usr/share/vulkan/explicit_layer.d` holds only nullhw, MESA overlay and MESA screenshot. Every fix in this batch is shape-pinned and none was executed — vault `30-architecture/patterns/a-defence-against-an-unreachable-condition-passes-as-proof-of-itself.md` is the reason that is written down instead of waved away. A fault-injection harness that makes one call fail and shows the refusal reaching the log is the missing instrument (supersedes the old item 2).
2. **A failed `vkEndCommandBuffer` leaves the buffer RECORDING, so the next frame's `vkResetCommandBuffer` is invalid use too** (new, `vk_context.cpp:1090`'s comment). Nothing un-records a buffer and the documented recovery is forbidden at that moment, so it is printed rather than solved. Same family: a failed `vkResetCommandBuffer`/`vkBeginCommandBuffer` after a successful acquire now drains the semaphore, so the old "stale +1" item is closed and replaced by the narrower one — the ACQUIRE-failure exits still leave its state unknown, and that is left alone on purpose.
3. **Unify B5's rule and C1's, and prove the unification is not a relaxation** (unchanged). C1's rule strictly generalises B5's; B5's arms must be re-run after the merge and must still red the same set on `git show HEAD:renderer_vk.cpp`. Its own slice, not a line in a batch.
4. **The retire path frees memory it never unmapped** (batch 152 item 1, unchanged, NOT touched here). `mdk_render_release_mesh:2158` and `ensureCanvasBuffer:2381` retire persistently mapped buffers and `settleRetired → destroyBufferPair:282` never calls `vkUnmapMemory`. A shape decision about `RetiredBuffer`, not an argument.
5. **Descriptor-pool exhaustion is still not proven reachable** (review-5 F1 and every review since — unchanged). Nothing frees a descriptor set; every released-and-re-made texture or palette consumes one of `maxSets = 4096` for the life of the process.
6. **Is the 256-entry refusal memory reachable in one session?** (review-7 open question 1, unchanged.) This batch adds one more distinct sentence to that budget.
7. **R8-F7: the row's acceptance command does not run the row's gate** — `bash tools/check.sh` never runs `linux_refusal_report_test.sh`; only `make -C tests/scripts` does. The fix is a `gate` line in `tools/check.sh`, which is owner-protected, so it is an owner task.
8. **Review-4 (run-20261112-035144) F1-F7 stay open, deferred with cause, none rebutted** — all seven are 3DS-side and belong to the `3ds.hud-canvas` row. **F1 (Medium)**: `emurun.sh`'s canary counts pure-blue pixels with no positional test, so a reflected canvas (determinant −1) passes `--expect hud` unchanged. **F3**: `emurun.sh:812-822` still calls `--expect hud` a non-visibility verdict.
9. **The level's own geometry on the console stays open** (batch 147 item 1) and **the panel instruments need re-baselining** (batch 147 item 2): every committed before-crop is a sunburst, so every panel A/B floor is measured under the old defect.
10. **The Linux frame's fidelity half is dark on this host and cannot be relit here.** `gates_frames` check 3 needs `/tmp/mdk-frame-reference/L3_f1.bmp`, and the oracle needs a dotnet 10 host; `/tmp/kilo` is `coleshores`-owned and not writable by this `piagent` lane, so captures go to `/tmp/mdk-lane`. Blocked, not unknown, and owner-gated behind `tests/frames/regenerate_reference.sh`. **Old-3DS hardware evidence stays open** (AGENTS.md P6/P8): everything here is Mandarine r1.2.1 on llvmpipe, mono, `:99`. Emulator-green is not hardware-pass.
11. **`docs/CONTINUATION.md` is ~2900 lines, not the ~150 the builder contract asks for.** The tail is batch records already in `docs/PROGRESS_LOG.md` in substance, so trimming it is a documentation task of its own and was not smuggled into a Linux-render batch.

**PREVIOUS BATCH 153 RECORD** (its own NEXT list is superseded by the one above; its findings and their disposition are in `docs/evidence/2026-11-12-153-frame-seam-results.txt`). Risk then: high — the frame seam's two discarded results are now read, and 24 discarded VkResults on the Linux device/swapchain init path now refuse instead of proceeding on a zero-initialised out-parameter.

**BATCH 153 (ledger row `linux.render-frame`, acceptance `bash tools/check.sh`, attempt 6) — the frame seam was throwing away both of its results, and 24 more VkResults in `vk_context.cpp` went the same way.** **The seam.** `mdk_render_end_frame` called `mdk::vk::endFrame(&r->ctx);` and discarded the bool; `mdk_render_begin_frame` returned from a failed `beginFrame` in silence. A frame whose fence wait, image acquire, command-buffer reset, begin, fence reset, submit or present failed was therefore reported as rendered, and its screenshot was read back from a swapchain image it never wrote and published as the pinned frame record — the invisible missing picture, in the two functions that bracket every frame. Both results are read now, both refuse through the dedup-ing `refused()` helper, and the capture runs only on a frame that was submitted. The seam prints and `vk_context.cpp` stays quiet on the frame path deliberately: the context file has no per-request memory and a frame failure repeats every frame. **The 24.** review-8 F3 named `vk_context.cpp` as where B5's defect class still lived and recommended a gate over it; measured at the parent, 39 result-bearing calls and 24 discarded. The worst was `:119`, `vkGetPhysicalDeviceSurfaceCapabilitiesKHR`: `capabilities` is zero-initialised, a zero `currentExtent` is indistinguishable from a surface with no preferred size, so `{0,0}` reached `vkCreateSwapchainKHR`, which failed, and the only line printed blamed the create — zero-initialised is not "undefined", it is a defined value that happens to be a lie. Then both `vkGetSwapchainImagesKHR` calls, the depth `vkBindImageMemory`, two `vkCreateSemaphore` and a `vkCreateFence` (a null semaphore is not "no semaphore", it is a wait that is silently absent), four enumerations (a failed count is not "zero of them"), the format/present-mode queries (their fallbacks are deliberate and stay; the silence was the bug), both `vkDeviceWaitIdle` calls — the one before a swapchain rebuild now refuses BEFORE destroying the framebuffers the wait existed to make safe (P3) — and the frame path's `vkWaitForFences`, `vkResetCommandBuffer`, `vkBeginCommandBuffer`, `vkResetFences`. **A hazard found while reading:** a refused submit leaves the frame fence reset and never signalled, so the next `beginFrame` waits `UINT64_MAX` on it — one refused submit becomes a hang two frames later. `vkSignalFence` is the direct fix and is NOT declared by the vendored header (`grep -c` 0, while `vkGetFenceStatus` is 2), so the fence is replaced instead, created before the old one is destroyed so a failed recovery changes nothing. Full record: `docs/evidence/2026-11-12-153-frame-seam-results.txt`.

**THE GATE PROVED ITSELF AGAINST THE COMMIT IT REPLACED.** `linux_refusal_report_test.sh` is 13 checks / 30 arms → **15 checks / 36 arms**: new **B11** (the seam reads both results, refuses on each, and the capture sits behind the frame's own result) and **C1** (every result-bearing Vulkan call in `vk_context.cpp` has its result read). Against `git show HEAD:` of all three files the new gate reds **exactly two of the fifteen and nothing else** — B11 and C1, naming all 24 sites at their real line numbers — so no existing check was weakened to admit the new ones. Two bugs in the first version of C1 were caught by running it and would have made it a pass generator: it read **string literals** (this file's refusal messages name the entry point that failed, so the raw scan found phantom sites where there are 24 — a figure batch 154 re-measured as 42 calls / 27 unchecked with strings kept, see `2026-11-13-154-frame-stream-closed.txt` §5), and it counted a **mention** of a `VK_` constant as a read, so the very arm written to prove the check — a bare `vkWaitForFences(..., VK_TRUE, UINT64_MAX);` — was accepted because VK_TRUE is an argument. C1's rule is B5's plus this file's name-then-test idiom and the two are deliberately not unified yet; see NEXT 1.

**THE REVIEW THIS BATCH WAS DISPATCHED ON PRODUCED NOTHING.** `logs/ralph-pi/run-20261112-153051/review-3.md` is **0 bytes** with an identity file (scope `ec3b13bc..f98a7cb0`, independence PROVED) and **no `.rc`**, where review-1 and review-2 each have one. Nothing to fix, nothing to rebut; per the loop's fail-closed rule that is a FAILED review and batch 152's commit stays unreviewed and owed. The last review with content is review-8, dispositioned 7/7 by batch 152. Nothing was skipped on the strength of an empty file: this batch came from the row's own next-list item 2, which is review-8 F3's deferred recommendation.

**GATES, AS RUN.** `make -C tests/scripts -j8` rc 0, `All cases match the oracle.`, **592 PASS / 0 FAIL**, `PASS LinuxRefusalReport (15 checks, 36 arms adjudicated red)`. `DISPLAY=:99 bash tools/check.sh` rc 0, `CHECK: ALL GATES PASSED`, `PASS L3_f1.bmp sha=58615fc85d02e009`, `BATCH INTEGRITY: PASS`, `COMPLETION: LEDGER VERIFIED`. `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds -j8` rc 0 with `mdk.elf` md5 `728eb2350c3ef8a81acda15f75617c1e` UNCHANGED — no `mdk_3ds/` path in `git diff --name-only`, so no emulator run was owed and none was faked. `cmake --build build-linux -j8` rc 0, no new diagnostics. Healthy-run measurement on this tree: zero `REFUSED` lines, zero `[vk] … failed` lines, and the capture still printed `[vk] screenshot: wrote … (320x240)`, which is also the proof the new capture guard is not accidentally always-false. The frame is `cmp`-identical to its 138 predecessors (`6769a137…`) and its sidecar carries the disclosure.

**NEXT, in order.**
0. **Close `linux.render-frame` and `3ds.hud-canvas` from a lane that can write the ledger.** Re-run verbatim at batch 153's tree: `ledger_close: running acceptance for linux.render-frame: bash tools/check.sh` then `ledger_close: could not verify -- [Errno 13] Permission denied: 'docs/COMPLETION_LEDGER.tsv'`. The acceptance RUNS and passes; the row cannot be written because the ledger is `coleshores:coleshores 0664` and this lane is `piagent`. **The operator fix is one command: `chgrp development docs/COMPLETION_LEDGER.tsv`** (`docs/` itself is already `development`-writable, so the lane could delete-and-replace the ledger; it does not, because the file is in `gates_batch_integrity.PROTECTED` and a rename is a hand edit wearing a hat). The acceptance has been green since batch 149; same tool and same blocker for `3ds.hud-canvas` (green since batch 147). **Attempt 6 of 8; the blocker is filesystem permissions, not code.**
1. **Unify B5's rule and C1's, and prove the unification is not a relaxation.** Two rules for one property is two rules to drift. C1's (statement-local comparison, or a named result whose next touch is a comparison) strictly generalises B5's (`VK_SUCCESS` anywhere in the statement) — but B5's arms must be re-run after the merge and the gate must still red the same set on `git show HEAD:renderer_vk.cpp`. Do it as its own slice with that evidence, not as a line in a batch.
2. **`vk_context.cpp`'s init paths are read but not exercised.** C1 proves the shape; nothing on this host can make a driver refuse a semaphore create, and no Khronos validation layer is installed (`/usr/share/vulkan/explicit_layer.d` holds nullhw, MESA overlay and MESA screenshot only). A fault-injection harness that makes one call fail and shows the refusal reaching the log is the missing instrument — the same gap review-6 open question 2 records for the other file.
3. **A failed `vkResetCommandBuffer` or `vkBeginCommandBuffer` after a successful acquire leaves `presentWait` with one pending signal nobody consumes** (new, made reachable by this batch, evidence §8). A stale +1 on a counting semaphore, not a leak and not a hang, and only on a driver that fails a reset or a begin on an otherwise healthy device. Nothing can undo it — Vulkan can post a CPU semaphore signal (`vkSignalSemaphore` IS declared by the vendored header, unlike `vkSignalFence`, which is not) but cannot withdraw one — and the reordering that would avoid it (reset and begin before the acquire) is worse, because an acquire failure would then leave the buffer recording and `vkResetCommandBuffer` on a recording buffer is invalid use. Commented at the site.
4. **The retire path frees memory it never unmapped** (batch 152 item 1, unchanged, NOT touched here). `mdk_render_release_mesh:2158` and `ensureCanvasBuffer:2381` retire persistently mapped buffers and `settleRetired → destroyBufferPair:282` does `vkDestroyBuffer` + `vkFreeMemory` with no `vkUnmapMemory` on any path; `destroyImage:981` asserts in a comment that freeing unmaps implicitly. Whether the spec requires the unmap cannot be settled on this host (item 2's reason), and unmapping before freeing is legal under either reading, so the fix is a shape decision about `RetiredBuffer`, not an argument.
5. **Descriptor-pool exhaustion is still not proven reachable** (review-5 F1, review-6/7/8 open questions — unchanged). Nothing frees a descriptor set: `vkFreeDescriptorSets`/`vkResetDescriptorPool` appear nowhere, and `forgetDescriptorSets()` drops only the CPU-side cache entry, so every released-and-re-made texture or palette consumes one of `maxSets = 4096` for the life of the process. Needs a counter or a free, not an argument.
6. **Is the 256-entry refusal memory reachable in one session?** (review-7 open question 1, unchanged.) The dedup set stops deduplicating at 256 entries, after which every refusal prints again; the 48-distinct measurement is level 3 only. This batch adds two more distinct sentences to that budget.
7. **R8-F7: the row's acceptance command does not run the row's gate** — `bash tools/check.sh` never runs `linux_refusal_report_test.sh`; only `make -C tests/scripts` does. The fix is a `gate` line in `tools/check.sh`, which is owner-protected, so it is an owner task. A silent `return false` is also still ungated (B4 discovers null handles, not `false`).
8. **Review-4 (run-20261112-035144) F1-F7 stay open, deferred with cause, none rebutted** — all seven are 3DS-side and belong to the `3ds.hud-canvas` row. **F1 (Medium)**: `emurun.sh`'s canary counts pure-blue pixels with no positional test, so a reflected canvas (determinant −1) passes `--expect hud` unchanged. **F3**: `emurun.sh:812-822` still calls `--expect hud` a non-visibility verdict. **F2/F4/F6/F7**: record corrections in `docs/evidence/2026-11-12-14{6,7}-*.txt`, `docs/OPEN_ITEMS.md:294`, `docs/PROGRESS_LOG.md`, and the non-reproducible `.3dsx` hash.
9. **The level's own geometry on the console stays open** (batch 147 item 1) and **the panel instruments need re-baselining** (batch 147 item 2): every committed before-crop is a sunburst, so every panel A/B floor is measured under the old defect. Review-4's open question 1 adds a cheap discriminator: dump the two masses' triangle count with `C3D_CullFace(GPU_CULL_NONE)`.
10. **The Linux frame's fidelity half is dark on this host and cannot be relit here.** `gates_frames` check 3 needs `/tmp/mdk-frame-reference/L3_f1.bmp`, and the oracle needs a dotnet 10 host; `/tmp/kilo` is `coleshores`-owned and not writable by this `piagent` lane, so captures go to `/tmp/mdk-lane`. Blocked, not unknown, and owner-gated behind `tests/frames/regenerate_reference.sh`. **Old-3DS hardware evidence stays open** (AGENTS.md P6/P8): everything here is Mandarine r1.2.1 on llvmpipe, mono, `:99`. Emulator-green is not hardware-pass.
11. **`docs/CONTINUATION.md` is ~2900 lines, not the ~150 the builder contract asks for.** The tail is batch records already in `docs/PROGRESS_LOG.md` in substance, so trimming it is a documentation task of its own and was not smuggled into a Linux-render batch.

**PREVIOUS BATCH 152 RECORD** (its own NEXT list is superseded by the one above; its findings and their disposition are in `docs/evidence/2026-11-12-152-unfenced-one-shot.txt`). Risk then: high — a one-shot upload's outcome now decides whether a resource is freed or abandoned, which is a lifetime change on the Linux render target, and the refusal gate gained a check, four arms and a line-numbering fix.

**BATCH 152 (ledger row `linux.render-frame`, acceptance `bash tools/check.sh`, attempt 5) — review-8's verdict was INCOMPLETE and all seven of its findings were real. None was rebutted; one word of one is corrected.** **R8-F1 (Medium, memory safety)** held and was the worst of them: `endOneShot` returned `bool`, and a bool has two values, so "the queue accepted the work and the wait could not prove it finished" had to share a value with "the queue refused it" — and both callers treated the shared value as the refusal and freed. The branch written to honour AGENTS.md P3 was the branch that broke it: it printed "the staging memory it read is not yet safe to release" and destroyed that memory four lines later, in `createHostImage`, and unmapped-and-freed the readback buffer in `captureSwapchain`. `endOneShot` now returns `enum class OneShot { done, neverSubmitted, unfenced }`; `unfenced` releases nothing in either caller — the handles are nulled because the function stopped owning them, not because they were destroyed, and the comment says so — and the wait is retried at device scope first, so a queue-wait failure a device wait survives becomes a *proven* completion and frees normally. **R8-F2 (Medium)** held: B6 pinned a block and not its condition, so the reviewer's one-token revert of `if (!uploaded)` to `if (!cb)` was accepted; B6 now requires the test that is true for EVERY bad outcome (`<var> != OneShot::done`, or `!<var>` for the bool spelling) to be the condition of the refusing, `return false` block before the single `return true` — pinning merely "the condition names the variable" would still let `if (upload == OneShot::neverSubmitted)` through. **R8-F3 (Medium)** held: the gate's comment declared `vkGet*` a void family and it is not — six of its entry points return a `VkResult` — so the exclusion is now a NAMED list of the three void ones this file calls, and any other `vkGet*` is result-bearing; the `vk_context.cpp` half is DEFERRED with a measured count, not a quote (§3 of the evidence: 34 of 34 result-bearing calls there are unread, and the worst is `:119`, where a failed `vkGetPhysicalDeviceSurfaceCapabilitiesKHR` leaves `capabilities` — zero-initialised, not undefined, which is the reviewer's one wrong word and is not safer — and a zero extent reaches `vkCreateSwapchainKHR`). **R8-F4 (Low)** held: B8 counted call sites, so a `return` inserted between the begin and the end stranded the pool with the gate green; every return in that range must now be guarded on the command buffer it did not get, which is the one exit the callee already cleaned up after. **R8-F5 (Low)** held: `strip_comments` deleted comment lines, so every line number the gate printed was a line of a 1905-line ghost of a 2355-line file — the eight sites batch 151's record lists are at real lines 319/324/325/495/555/1438/1509/1952, not 208/213/214/313/368/1134/1195/1581; comment lines are blanked now and the gate's numbers match `grep -n`. **R8-F6 (Low)** is fixed conservatively and NOT proven: the explicit `vkFreeCommandBuffers` now runs only when the end succeeded, and the pool destroy reclaims a recording buffer either way, so nothing leaks under either reading; `vkResetCommandBuffer` was not added because resetting an individual buffer needs a pool flag this pool does not ask for. **R8-F7 (Low)** is deferred with cause: the fix is a `gate` line in `tools/check.sh`, which is in `gates_batch_integrity.PROTECTED`. Full record: `docs/evidence/2026-11-12-152-unfenced-one-shot.txt`.

**THE GATE PROVED ITSELF AGAINST THE COMMIT IT REPLACED, AND FOUND A NEW DEFECT ON THE WAY.** `linux_refusal_report_test.sh` is 12 checks / 26 arms → **13 checks / 30 arms**, every arm still naming the one check it turns red. Against `git show HEAD:…/renderer_vk.cpp` the new gate goes red on **exactly one of the thirteen and nothing else**: B10, "a one-shot submission whose completion was never proven releases nothing", naming `createHostImage has no block that refuses an unfenced submission`. B5, B6 and B8 stay green on the parent, which is right — batch 151 read the results and paired the pool; what it did not do was distinguish an unproven wait from a refused one. Deciding whether the unfenced pair could go to the existing retire list meant reading that path, and it turned up a defect of its own, recorded rather than smuggled in: `mdk_render_release_mesh` (:2158) and `ensureCanvasBuffer` (:2381) retire persistently MAPPED buffers and `settleRetired` → `destroyBufferPair` (:282) frees them without ever calling `vkUnmapMemory` — nulling the CPU-side pointer is not an unmap. That runs on every mesh release and every canvas resize, not on a driver failure, and it needs a shape decision about `RetiredBuffer`, so it is next-list item 1.

**GATES, AS RUN.** `make -C tests/scripts -j8` rc 0, `All cases match the oracle.`, **592 PASS / 0 FAIL**, `PASS LinuxRefusalReport (13 checks, 30 arms adjudicated red)`. `DISPLAY=:99 bash tools/check.sh` rc 0, `CHECK: ALL GATES PASSED`, `PASS L3_f1.bmp sha=58615fc85d02e009`, `BATCH INTEGRITY: PASS`, `COMPLETION: LEDGER VERIFIED`. `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds -j8` rc 0 with `mdk.elf` md5 `728eb2350c3ef8a81acda15f75617c1e` UNCHANGED — `git diff --name-only` contains no `mdk_3ds/` path, so no emulator run was owed and none was faked. `cmake --build build-linux -j8` rc 0, no new diagnostics. The frame is byte-identical to batch 151's (`6769a137…`) because every change is on a path the driver takes only when it refuses and lavapipe never says no; its sidecar carries the disclosure.

**NEXT, in order.**
0. **Close `linux.render-frame` and `3ds.hud-canvas` from a lane that can write the ledger.** `python3 tools/runner/ledger_close.py --row linux.render-frame` runs the acceptance, it passes, and then `could not verify -- [Errno 13] Permission denied: 'docs/COMPLETION_LEDGER.tsv'`: the ledger is `coleshores:coleshores 0664` and this lane is `piagent`, and the row is in `gates_batch_integrity.PROTECTED` so hand-editing is a finding. The acceptance has been green since batch 149; same tool and same blocker for `3ds.hud-canvas` (green since batch 147). **This is attempt 5 of 8 and the blocker is filesystem permissions, not code: until a lane can write the ledger the loop re-dispatches a row whose acceptance passes.**
1. **The retire path frees memory it never unmapped** (new, `docs/evidence/2026-11-12-152-unfenced-one-shot.txt` §10). `mdk_render_release_mesh:2158` and `ensureCanvasBuffer:2381` retire persistently mapped buffers; `settleRetired → destroyBufferPair:282` does `vkDestroyBuffer` + `vkFreeMemory` with no `vkUnmapMemory` on any path. Two candidate fixes with different consequences — carry the mapping in `RetiredBuffer` and unmap at settle time, or unmap at retire time (which asks whether the CPU may still touch the pointer) — and no validation layer on this host to say which VU it breaks (see item 5).
2. **`vk_context.cpp` has 34 result-bearing Vulkan calls and reads none of their results** (new, evidence §3). Same defect class B5 exists for, one file over, and too big for a line in this batch. The one that matters today is `:119`: a failed `vkGetPhysicalDeviceSurfaceCapabilitiesKHR` leaves a zero-initialised `capabilities`, so `extent` comes out {0,0} and `vkCreateSwapchainKHR` is asked for a zero-extent swapchain — and the file prints a line for the create failure and nothing for the query that caused it. `:171/:177` (`vkGetSwapchainImagesKHR`) leaving `imageCount` 0 is next. The other six `vkGet` sites were read and are benign because their callers have fallbacks.
3. **Descriptor-pool exhaustion is still not proven reachable** (review-5 F1, review-6/7/8 open questions — unchanged). Nothing frees a descriptor set: `vkFreeDescriptorSets`/`vkResetDescriptorPool` appear nowhere, and `forgetDescriptorSets()` drops only the CPU-side cache entry, so every released-and-re-made texture or palette consumes one of `maxSets = 4096` for the life of the process. Needs a counter or a free, not an argument.
4. **Is the 256-entry refusal memory reachable in one session?** (review-7 open question 1, unchanged.) The dedup set stops deduplicating at 256 entries, after which every refusal prints again; the 48-distinct measurement is level 3 only. And the new `unfenced` refusal deliberately carries no leak counter, because the dedup keys on the rendered sentence and a counter would make every leak a new sentence (evidence §1).
5. **Two Vulkan lifetime rules in this file are asserted without the spec in hand, and this host cannot settle either.** R8-F6 (freeing a recording command buffer — now avoided rather than proven) and item 1 (freeing mapped memory) both need a VU or a validation-layer run, and `/usr/share/vulkan/explicit_layer.d` holds only nullhw, MESA overlay and MESA screenshot: no Khronos validation layer is installed. The fault-injection measurements are also still the builder's, not the tree's (review-6 open question 2).
6. **R8-F7: the row's acceptance command does not run the row's gate** — `bash tools/check.sh` never runs `linux_refusal_report_test.sh`; only `make -C tests/scripts` does (4.3 s). The fix is a `gate` line in `tools/check.sh`, which is owner-protected, so it is an owner task and not a line in a batch. `gates_core_parity.py` was considered and rejected: a Linux-render gate inside the gate named "the two targets link the same game" is a category error.
7. **A silent `return false` is still ungated.** B4 discovers null handles, not `false`, so `createHostBuffer`'s remaining silent `return false`s are outside every gate in both directions. The fix is a B4-for-bools and it is its own slice.
8. **Review-4 (run-20261112-035144) F1-F7 stay open, deferred with cause, none rebutted** — all seven are 3DS-side and belong to the `3ds.hud-canvas` row. **F1 (Medium)**: `emurun.sh`'s canary counts pure-blue pixels with no positional test, so a reflected canvas (determinant −1) passes `--expect hud` unchanged; gate the marker's bbox origin. **F3**: `emurun.sh:812-822` still calls `--expect hud` a non-visibility verdict, which the harness itself contradicts 20 lines below. **F2/F4/F6/F7**: record corrections in `docs/evidence/2026-11-12-14{6,7}-*.txt`, `docs/OPEN_ITEMS.md:294`, `docs/PROGRESS_LOG.md` (the "registers() emits memory order (x,y,z,w)" sentence, which is the phrase that WAS the bug), and the non-reproducible `.3dsx` hash.
9. **The level's own geometry on the console stays open** (batch 147's item 1): the panel is 83419/96000 clear with two textured red-orange masses either side of the panel's vertical centre; the matrix path is derived correct and the canvas proves it end to end. Review-4's open question 1 adds a cheap discriminator: dump the two masses' triangle count with `C3D_CullFace(GPU_CULL_NONE)`; if it doubles, it is a winding/mirror fault, not geometry. **Re-baseline the panel instruments** with it: every committed before-crop is a sunburst, so every panel A/B floor is measured under the old defect.
10. **The Linux frame's fidelity half is dark on this host and cannot be relit here.** `gates_frames` check 3 needs `/tmp/mdk-frame-reference/L3_f1.bmp`, and the oracle needs a dotnet 10 host; `/tmp/kilo` is `coleshores`-owned and not writable by this `piagent` lane at all, so captures go to `/tmp/mdk-lane`. The three known gap components are in `docs/OPEN_ITEMS.md` ("What the 1.240% frame gap actually is") and all three are owner-gated behind `tests/frames/regenerate_reference.sh`. Blocked, not unknown. **Old-3DS hardware evidence stays open** (AGENTS.md P6/P8): everything here is Mandarine r1.2.1 on llvmpipe, mono, `:99`. Emulator-green is not hardware-pass.
11. **`docs/CONTINUATION.md` is ~2900 lines, not the ~150 the builder contract asks for.** The tail is batch records already present in `docs/PROGRESS_LOG.md` in substance but not always verbatim, so trimming it is a documentation task of its own and was not smuggled into a Linux-render batch.

**PREVIOUS BATCH 151 RECORD** (its own NEXT list is superseded by the one above; its findings and their disposition are in `docs/evidence/2026-11-12-151-submit-results-read.txt`). Risk then: high — eight discarded VkResults on the Linux render path are now read, which changes what a refusal does to resource lifetime and draw correctness, and four of the twelve refusal-gate checks were rewritten to be stricter.

**BATCH 151 (ledger row `linux.render-frame`, acceptance `bash tools/check.sh`, attempt 4) — review-7's verdict was INCOMPLETE and all five of its findings were real. None was rebutted.** Each was reproduced against batch 150's tree first, and each came back green exactly as the reviewer said. **R7-F1 (Medium):** `endOneShot` discarded the VkResults of `vkEndCommandBuffer`, `vkQueueSubmit` and `vkQueueWaitIdle` and returned void, so `createHostImage` returned `true` for an image whose layout transition and buffer→image copy were in a command stream that never ran — review-5 F1's "an image that exists but is not the texture", re-opened one level down, invisible because B5's regex was `vkAllocate*`. `endOneShot` returns bool now, the three are sequenced rather than merely checked (a failed end must not reach the submit; a refused submit skips the wait), `createHostImage` refuses and destroys when the upload did not execute, and `captureSwapchain` fails instead of publishing a BMP of a buffer nobody wrote. **Widening B5 to the spec's result-bearing verb families then reddened five more sites of the same class, all fixed:** `vkBindBufferMemory` (a buffer bound to nothing, then mapped and returned as a success), `vkBindImageMemory`, `vkCreatePipelineCache` (an undefined handle reaching `vkCreateGraphicsPipelines`), and both `vkDeviceWaitIdle` calls — the out-of-frame dynamic-mesh update, now skipped rather than memcpy'd under a frame that may still be reading the slot (P3), and teardown. **R7-F2 (Medium):** B8 read only the two callee bodies, so `endOneShot(ctx, VK_NULL_HANDLE, cb)` at both call sites passed 12/12; it now pairs each call's pool argument with the identifier that call's `beginOneShotTracked` filled and requires every begin to have an end. **R7-F3 (Medium):** B3 keyed figures on the two-word `unsigned long long` cast, so `(unsigned)mdk::vk::kFramesInFlight` — the third number in both size-bearing lines — was derived by nothing and `(unsigned)8` passed; figures are now keyed on any integral cast and classified (size / size×frames / frames / anything else is red) with two set rules, one of which is what keeps batch 150's own frames-only arm red. **R7-F4 (Medium):** B4 matched `return VK_NULL_HANDLE;` exactly, so respelling the return deleted the descriptor refusal in silence; the parenthesised form and null-initialised named locals are discovered now. **R7-F5 (Low):** the disclosure is in this batch's sidecar, including the tree state at capture that review-6 asked for twice; batch 150's pair was not touched. **B6 was rewritten, not weakened** — it pinned the `else` sibling of `if (cb)`, a shape that legitimately moved once `endOneShot` could fail; it now pins that the one-shot result is read and that a refusing, `return false` block dominates the single `return true`. Full record: `docs/evidence/2026-11-12-151-submit-results-read.txt`.

**THE GATE PROVED ITSELF AGAINST THE COMMIT IT REPLACED.** `linux_refusal_report_test.sh` is 12 checks / 20 arms → **12 checks / 26 arms**, every arm still naming the one check it turns red. Against `git show HEAD:…/renderer_vk.cpp` the new gate goes red on **exactly two of the twelve and nothing else**: B5, naming all eight discarded VkResults (208 `vkEndCommandBuffer`, 213 `vkQueueSubmit`, 214 `vkQueueWaitIdle`, 313 `vkBindBufferMemory`, 368 `vkBindImageMemory`, 1134 `vkCreatePipelineCache`, 1195 and 1581 `vkDeviceWaitIdle`), and B6, the unfilled-image success. Where it is still weak is written down in the evidence file rather than left implied: B5 reads "the statement contains `VK_SUCCESS`", not data flow; B4's named-local scan is a file-wide name set (it can only add obligations); B8 pairs a call with the nearest preceding begin; B9 is still last-call-in-block.

**GATES, AS RUN.** `make -C tests/scripts -j8` rc 0, `All cases match the oracle.`, **592 PASS / 0 FAIL**, `PASS LinuxRefusalReport (12 checks, 26 arms adjudicated red)`. `DISPLAY=:99 bash tools/check.sh` rc 0, `CHECK: ALL GATES PASSED`, `PASS L3_f1.bmp sha=58615fc85d02e009`, `BATCH INTEGRITY: PASS`, `COMPLETION: LEDGER VERIFIED`. `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds -j8` rc 0 with `mdk.elf` md5 `728eb2350c3ef8a81acda15f75617c1e` UNCHANGED — no `mdk_3ds/` path in `git diff --name-only`, so no emulator run was owed and none was faked. `cmake --build build-linux -j8` rc 0, no new diagnostics. The frame is byte-identical to batch 150's (`6769a137…`) because every change is on a failure path and lavapipe never says no; its sidecar carries the disclosure.

**NEXT, in order.**
0. **Close `linux.render-frame` and `3ds.hud-canvas` from a lane that can write the ledger.** Re-run at batch 151's tree, verbatim: `ledger_close: running acceptance for linux.render-frame: bash tools/check.sh` then `ledger_close: could not verify -- [Errno 13] Permission denied: '/mnt/nas/Development/mdk-sdl/docs/COMPLETION_LEDGER.tsv'` — the acceptance RUNS and passes, and the row cannot be written, because the ledger is `coleshores:coleshores 0664` and this lane is `piagent`. The row is also in `gates_batch_integrity.PROTECTED`, so hand-editing is forbidden and would be a finding. The acceptance has been green since batch 149; same tool and same blocker for `3ds.hud-canvas` (green since batch 147). **Until a lane can write the ledger the loop will keep re-dispatching a row whose acceptance passes — this is attempt 4 of 8, and the blocker is filesystem permissions, not code.**
1. **A silent `return false` is still ungated, and now has a name for the missing check.** B4 discovers null handles, not `false`, so `createHostBuffer`'s remaining silent `return false`s (create, memory-type, allocate) are outside every gate in both directions. They are not silent *at the call boundary* — every caller is a B1 site or prints its own line — which is why this is deferred rather than fixed here; but a new one would be invisible. The fix is a B4-for-bools: discover every `return false` in a create helper and attribute a refusal to it, which is its own slice because it will find sites in `createHostImage` too.
2. **Descriptor-pool exhaustion is still not proven reachable** (review-5 F1, review-6 open question 1, review-7 open question 1 — unchanged). Nothing frees a descriptor set: `vkFreeDescriptorSets`/`vkResetDescriptorPool` appear nowhere, and `forgetDescriptorSets()` drops only the CPU-side cache entry, so every released-and-re-made texture or palette consumes one of `maxSets = 4096` for the life of the process. The refusal is audible when it happens; whether it can happen needs a counter or a free.
3. **Is the 256-entry refusal memory reachable in one session?** (review-7 open question 1.) The dedup set stops deduplicating at 256 entries, after which every refusal prints again; the 48-distinct measurement is level 3 only. Same answer as item 2: a counter, not an argument.
4. **The fault-injection measurements are the builder's, not the tree's** (review-6 open question 2). The 48-distinct / 22500-line / 100.000%-pixel-loss figures came from env-gated injections that were reverted, so no later reader can re-run them. Making them reproducible is a separate slice with its own gate.
5. **Review-4 (run-20261112-035144) F1-F7 stay open, deferred with cause, none rebutted** — all seven are 3DS-side and belong to the `3ds.hud-canvas` row. **F1 (Medium)**: `emurun.sh`'s canary counts pure-blue pixels with no positional test, so a reflected canvas (determinant −1) passes `--expect hud` unchanged; gate the marker's bbox origin. **F3**: `emurun.sh:812-822` still calls `--expect hud` a non-visibility verdict, which the harness itself contradicts 20 lines below. **F2/F4/F6/F7**: record corrections in `docs/evidence/2026-11-12-14{6,7}-*.txt`, `docs/OPEN_ITEMS.md:294`, `docs/PROGRESS_LOG.md` (the "registers() emits memory order (x,y,z,w)" sentence, which is the phrase that WAS the bug), and the non-reproducible `.3dsx` hash.
6. **The level's own geometry on the console stays open** (batch 147's item 1): the panel is 83419/96000 clear with two textured red-orange masses either side of the panel's vertical centre; the matrix path is derived correct and the canvas proves it end to end. Review-4's open question 1 adds a cheap discriminator: dump the two masses' triangle count with `C3D_CullFace(GPU_CULL_NONE)`; if it doubles, it is a winding/mirror fault, not geometry.
7. **Re-baseline the panel instruments** (batch 147's item 2): every committed before-crop is a sunburst, so every panel A/B floor is measured under the old defect.
8. **The Linux frame's fidelity half is dark on this host and cannot be relit here.** `gates_frames` check 3 needs `/tmp/mdk-frame-reference/L3_f1.bmp`, and the oracle needs a dotnet 10 host; `/tmp/kilo` is `coleshores`-owned and not writable by this `piagent` lane at all, so captures go to `/tmp/mdk-lane` here. The three known gap components are in `docs/OPEN_ITEMS.md` ("What the 1.240% frame gap actually is") and all three are owner-gated behind `tests/frames/regenerate_reference.sh`. Blocked, not unknown.
9. **`docs/CONTINUATION.md` is ~2890 lines, not the ~150 the builder contract asks for.** The tail is batch records already present in `docs/PROGRESS_LOG.md` in substance but not always verbatim, so trimming it is a documentation task of its own and was not smuggled into a Linux-render batch.

**PREVIOUS BATCH 150 RECORD** (its own NEXT list is superseded by the one above; its findings and their disposition are in `docs/evidence/2026-11-12-150-refusal-gate-redescribed.txt`). Risk then: high — the one-shot command-buffer/pool pairing changes what a refusal does to resource lifetime on the Linux target, and two gate checks were rewritten to be stricter.

**BATCH 150 (ledger row `linux.render-frame`, acceptance `bash tools/check.sh`) — review-6 said INCOMPLETE and was right about all three blocking findings.** Each was reproduced against batch 149's tree before anything was changed, and each came back green exactly as the reviewer said. **F1 (High):** B3's rule was `tokens(figure) - want - EXTRA`, which rejects a figure mentioning something *foreign* and accepts one mentioning *less* than the size — so `cap / 8` for a request of `cap × frames-in-flight` (review-5 F3's own defect class) and `kFramesInFlight` alone both passed 10/10. B3 is two-sided now: the figure's identifiers must be exactly the create call's size identifiers or those plus the frames-in-flight count, and it may not introduce an operator or a numeric literal the size expression did not have. **F3 (Medium):** B4 asked whether `refused(` sat anywhere between the innermost `{` and the return, and an unbraced `if (cond) return VK_NULL_HANDLE;` has the whole function body as its innermost block — a new silent guard in `getDescriptorSet` passed. Attribution is per return now; the unbraced form is judged by its condition, which never carries a refusal. **F2 (Medium):** `beginOneShotTracked` pushed its pool onto a file-scope vector that `endOneShot` popped from the front, and two post-push exits returned without removing it; one stranded entry made every later `endOneShot` free a foreign command buffer into the wrong pool and destroy that pool a step early. Fixed by deleting the ledger — the pool is handed to the caller and every failure exit undoes what it made. **F6 (Low)** was confirmed too (memory freed while bound to a live image, in `createHostImage`, and the same inverted order in `createHostBuffer` found while fixing it), and **F4 (Low)** by removing the `site` field that `-fmerge-constants` made meaningless. **F5 (Low)** is accepted, not rebutted: the asymmetry is now written down instead of implicit. Full record: `docs/evidence/2026-11-12-150-refusal-gate-redescribed.txt`.

**THE GATE GREW, AND SO DID THE FILE IT WAS WRONG ABOUT.** `linux_refusal_report_test.sh` is 10 checks / 13 arms → **12 checks / 20 arms**, every arm still naming the one check it turns red. B8 pins the pool handover (3 arms), B9 pins teardown order (1 arm), and both red against `git show HEAD:…/renderer_vk.cpp` on exactly the two defects and nothing else. Writing B8 also exposed a harness bug that had been silently doctoring the text every check in the file reads: `strip_comments` dropped any line starting with `*`, which is how this file writes `*outImage = VK_NULL_HANDLE;` and the new `*outPool = pool;`. It now tracks whether a block comment is actually open, and every pre-existing arm still reds on the corrected text — the only way that could be fixed without weakening anything.

**GATES, AS RUN.** `make -C tests/scripts -j8` rc 0, `All cases match the oracle.`, **592 PASS / 0 FAIL**, `PASS LinuxRefusalReport (12 checks, 20 arms adjudicated red)`. `DISPLAY=:99 bash tools/check.sh` rc 0, `CHECK: ALL GATES PASSED`, `PASS L3_f1.bmp sha=58615fc85d02e009`. `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds -j8` rc 0 with `mdk.elf` md5 `728eb2350c3ef8a81acda15f75617c1e` UNCHANGED — nothing here reaches the console, so no emulator run was owed and none was faked: `git diff --name-only` has no `mdk_3ds/` path. `cmake --build build-linux -j8` rc 0. The frame is byte-identical to batch 149's (`6769a137…`) because every change is on a failure path and lavapipe never says no.

**NEXT, in order.**
0. **Close `linux.render-frame` and `3ds.hud-canvas` from a lane that can write the ledger.** `python3 tools/runner/ledger_close.py --row linux.render-frame` exits 2 here because `docs/COMPLETION_LEDGER.tsv` is `coleshores:coleshores 0664` and this lane is `piagent`; the row is in `gates_batch_integrity.PROTECTED` so hand-editing is forbidden. The acceptance is green at this commit. Same tool for `3ds.hud-canvas` (green since batch 147).
1. **`vkCreateImageView`'s failure leak and `createHostBuffer`'s four silent `return false`s are outside every gate** (review-6 open question 4). B4 discovers null *handles*, not `false`, and `createHostBuffer` is the callee whose failures the callers print — so a refusal inside it is currently ungated in both directions. This is the same defect class this batch closed twice and it has a name now.
2. **Descriptor-pool exhaustion is still not proven reachable** (review-5 F1, review-6 open question 1, unchanged). Nothing frees a descriptor set: `vkFreeDescriptorSets`/`vkResetDescriptorPool` appear nowhere, and `forgetDescriptorSets()` drops only the CPU-side cache entry, so every released-and-re-made texture or palette consumes one of `maxSets = 4096` for the life of the process. The refusal is audible when it happens; whether it can happen needs a counter or a free, which is its own slice.
3. **The fault-injection measurements are the builder's, not the tree's** (review-6 open question 2). The 48-distinct / 22500-line / 100.000%-pixel-loss figures came from env-gated injections that were reverted, so no later reader can re-run them. Making them reproducible is a separate slice with its own gate.
4. **Review-4 (run-20261112-035144) F1-F7 stay open, deferred with cause, none rebutted** — all seven are 3DS-side and belong to the `3ds.hud-canvas` row. **F1 (Medium)**: `emurun.sh`'s canary counts pure-blue pixels with no positional test, so a reflected canvas (determinant −1) passes `--expect hud` unchanged; gate the marker's bbox origin. **F3**: `emurun.sh:812-822` still calls `--expect hud` a non-visibility verdict, which the harness itself contradicts 20 lines below. **F2/F4/F6/F7**: record corrections in `docs/evidence/2026-11-12-14{6,7}-*.txt`, `docs/OPEN_ITEMS.md:294`, `docs/PROGRESS_LOG.md` (the "registers() emits memory order (x,y,z,w)" sentence, which is the phrase that WAS the bug), and the non-reproducible `.3dsx` hash.
5. **The level's own geometry on the console stays open** (batch 147's item 1): the panel is 83419/96000 clear with two textured red-orange masses either side of the panel's vertical centre; the matrix path is derived correct and the canvas proves it end to end. Review-4's open question 1 adds a cheap discriminator: dump the two masses' triangle count with `C3D_CullFace(GPU_CULL_NONE)`; if it doubles, it is a winding/mirror fault, not geometry.
6. **Re-baseline the panel instruments** (batch 147's item 2): every committed before-crop is a sunburst, so every panel A/B floor is measured under the old defect.
7. **The Linux frame's fidelity half is dark on this host and cannot be relit here.** `gates_frames` check 3 needs `/tmp/mdk-frame-reference/L3_f1.bmp`, and the oracle needs a dotnet 10 host; `/tmp/kilo/dotnet10` belongs to another user and no `dotnet` exists for this lane, so every recent sidecar reads "live measurement: unavailable". The three known gap components are in `docs/OPEN_ITEMS.md` ("What the 1.240% frame gap actually is") and all three are owner-gated behind `tests/frames/regenerate_reference.sh`. Blocked, not unknown.
8. **Old-3DS hardware evidence stays open** (AGENTS.md P6/P8): everything here is Mandarine r1.2.1 on llvmpipe, mono, `:99`. Emulator-green is not hardware-pass.
9. **`docs/CONTINUATION.md` is ~2850 lines, not the ~150 the builder contract asks for.** The tail is batch records already present in `docs/PROGRESS_LOG.md` in substance but not always verbatim, so trimming it is a documentation task of its own and was not smuggled into a Linux-render batch.

Risk: high — four lines of shipped 3DS code change what EVERY draw on the console sends to the GPU.
`pica::registers()` wrote each vertex-uniform register's four words in (x,y,z,w) order; the PICA
reads them as (w,z,y,x). `mdk.elf` e70fcb73→728eb235, 95756 of 96000 panel pixels move, and the
ledger row `3ds.hud-canvas` closes on its own acceptance command. Full record:
`docs/evidence/2026-11-12-147-register-word-order.txt`.

**THE DEFECT, AND WHY ELEVEN RUNS MISSED IT.** `C3D_FVec` (c3d/types.h:37-64) declares its members
`w, z, y, x` — "Matches PICA layout" — so a register's FIRST word is its W and its LAST is its X.
`C3D_FVUnifMtxNx4` is a struct copy and `C3D_UpdateUniforms` hands `C3D_FVUnif + start*16` to
`GPUCMD_Add` verbatim (uniforms.o +0xd0..+0xe8), so nothing reorders between the staging buffer and
the shader. `Mtx_Scale` multiplies offset 12 by its x argument, `Mtx_Translate`'s fast path puts the
translation in each row's offset 0, and `Mtx_OrthoTilt` only projects at all read as (w,z,y,x): three
readings of the shipped library, one answer. MDK wrote `registers()` into `C3D_Mtx::m[]`, the RAW
view, in the order its comment calls "column i, top-down" — which in raw memory is the reverse.
Forsaken never hit it because its `matrix_to_c3d` (render_c3d.c:1121) writes the NAMED fields
`.x .y .z .w`, and the names are already reversed in memory.

**THREE MEASURED SYMPTOMS, ONE CAUSE.** (i) an identity MVP painted nothing: every register became
(0,0,0,1), so every vertex mapped to the single clip point (1,1,1,1); (ii) the canvas painted
nothing: its registers came out (1,0,-2/360,0) etc., so clip x = clip w and the whole HUD collapsed
onto one panel corner — which is exactly why batch 146's probe, reading the staging buffer, found
"exactly the four register rows that paint": the VALUES were right and the ORDER was wrong, and no
probe that reads the buffer through `C3D_FVec` members can see the difference; (iii) the sunburst
the panel has shown since the crops were committed, the open item that called itself "the one-sided
view". Batch 145's `.constf` run painted because picasso writes a `.constf` payload in declaration
order, which is the order the hardware wants — the two paths were never equivalent, and review-3 F3
was right that the record's claim that they were is the contradiction that mattered.

**THE FIX AND ITS PIN.** `registers()` now writes `out[i*4+j] = m[(3-j)*4+i]`. The host suite's
`shaderRow` was the second wrong half — it modelled the register as (x,y,z,w) in memory, and the two
wrongs cancelled — so it now models the hardware, and `testTheShaderReadsTheRegistersTheWayTheMatrix
MeansThem` keeps its assertion unchanged while going red against the old `registers()` (4 checks:
`pica_matrix:311` and `:327` ×3). No golden changed; `PASS PicaMatrix`, 95 PASS / 0 FAIL.

**THE ROW'S ACCEPTANCE IS GREEN.** `DISPLAY=:99 mdk_3ds/tools/emurun.sh --expect hud` → exit **0**,
`hud emitted -- canvas: quads=4 draws=2 mesh=2 corners=24 eyes=1` AND `HUD VISIBLE -- 121 canary
pixels in the panel`. The canary's bbox is (0,0)-(10,10) of the panel crop, which is where the
canvas's top-left 16x16 lands under the panel's quarter turn, and the HUD's dial measures
(333,191)-(388,232) — the bottom-right, where the C# oracle draws it. The canvas is not merely
visible, it is in the oracle's orientation. (`/tmp/kilo` is another user's on this host and is not
writable by this lane; captures are under `/tmp/kilo-mdk`. The harness is unchanged.)

**NEXT, in order.**
0. **Close the ledger row from a lane that can write the file.** `mdk_3ds/tools/emurun.sh --expect
   hud` is green at this batch's commit, but `docs/COMPLETION_LEDGER.tsv` is owned by another user on this
   host (`coleshores:coleshores`, mode 0664; this lane is `piagent`), so this batch could NOT set
   the row. It still reads `todo`. The row to write is:
   `3ds.hud-canvas<TAB>d1<TAB>slow<TAB>done<TAB>mdk_3ds/tools/emurun.sh --expect hud<TAB>mdk_3ds/src/render/pica_matrix.cpp,docs/evidence/2026-11-12-147-register-word-order.txt<TAB><this batch's short hash>`.
   `gates_completion` needs the verified hash to be an ancestor of HEAD, which is why the row is a
   follow-up commit and not part of the batch (and a follow-up commit has to be titled `loop:` or
   `ledger:`, because `gates_batch_integrity` requires every other commit to add exactly one frame
   PNG). This is an environment fact, not a result: the row's
   acceptance is measured green, the bookkeeping is what is blocked.
1. **The level's own geometry, which is now a different question and still open.** The sunburst is
   gone; the panel is 83419/96000 clear with the start arena's floor and light pool in the lower
   middle and two textured red-orange masses either side of the panel's vertical centre line. The
   matrix path is derived correct and the canvas proves it end to end, so the next theory has to be
   the level's vertices, its per-draw world composition, or its projection — not the upload. The
   symmetry about the panel's vertical centre is the first fact to use: under the quarter turn that
   axis is the scene's vertical, so the pair is a floor/ceiling or a left/right pair drawn where a
   plane should be. `docs/OPEN_ITEMS.md`'s "the one-sided view" is renamed to what it now is.
2. **Re-baseline the panel instruments.** Every committed before-crop in `docs/evidence/` is a
   sunburst, so every A/B floor measured against them is a floor under the old defect. Coverage
   moved from 96000 painted to 12581; a noise floor has to be re-measured on the new picture before
   any panel A/B is quoted again (batch 103's lesson: a floor is only valid for the scene it was
   measured on).
3. **Old-3DS hardware evidence stays open** (AGENTS.md P6/P8): everything here is Mandarine r1.2.1
   on llvmpipe, mono, `:99`. Emulator-green is not hardware-pass.

**DO NOT RE-RUN**: the whole batch-145/146 uniform-delivery hunt — the uniform location, the
canary's position, rebinding after a per-draw write, per-draw write ordering, the vertex layout, the
`.constf`-versus-uniform equivalence, or any probe that reads `C3D_FVUnif` and concludes the CPU side
is correct. All of it was chasing a non-defect; the defect was the word order, and it is fixed.
**Batch 142 (the ledger row `3ds.hud-canvas`) — the canvas lands as queue records, the HUD is wired, and the
picture does not appear; the batch ships the seam, the gate and eight closed doors.** The seam is the oracle's
shape: quads accumulate between `canvas_begin` and `canvas_end`, `canvas_end` uploads one mesh and queues one
record per same-material run (`Renderer.cs:340-358`, `:727-745`), and the `Pass::Overlay` marker is what tells
`drawRecorded` to install the record's `world` verbatim instead of composing it with the eye matrix
(`Renderer.cs:1073`). The tint is a vertex-shader uniform, not a vertex attribute, because the seam already
treats the tint as per-draw and the Vulkan backend carries it as a push constant. `emurun.sh --expect hud`
(exit 7) is the row's acceptance command and it passes — and it is a **queued-and-emitted** verdict, not a
visibility verdict, which is said in the harness comment, the sidecar and the open item because the gate
cannot say it itself. **Eight hypotheses were closed by running, not reading** (the matrix and the per-draw
write, the tint uniform and `GPU_PRIMARY_COLOR`, the HUD textures and the palette expansion, the command
buffer at 0.113, the far clip plane, the clipper's fixed-point range, a buffer allocated inside a frame — the
full list with its numbers is `docs/OPEN_ITEMS.md` and `docs/PROGRESS_LOG.md` "Iteration 142"). **A ninth was
attempted by this attempt and was inconclusive by its own design**: the head-of-frame probe (a red triangle
drawn immediately after `installEye`, before the replay) cannot be seen, because the level-7 scene covers
every pixel of the panel, so "the head draw landed" and "it did not" produce the same capture. It is recorded
as a negative about the probe, not about the hypothesis; the queue-order test OPEN_ITEMS names is still the
next one to run, and it has to be made observable (a known-colour quad diffed against a HUD-off run), not just
moved. Review-8 is closed on what this batch could act on: **no Critical and no High**; **F4** and **F5** (the
two comments that still said `TeleportKurt` was unported) and **F6** (the orphan Batch-140 fragment at
`CONTINUATION.md:59`) are fixed here; **F1** (two `ArenaSpace`s), **F2** (no executed pin that the unnamed
teleport arm leaves `_previousKurtPosition`/`_teleported` alone) and **F3** (opcode 173's unconditional yield
unpinned) are deferred with cause in `docs/PROGRESS_LOG.md` — they are the teleport row's, not this row's.
Gates, verbatim: `make -C tests/scripts` → `All cases match the oracle.` rc 0, **587** `^PASS` / 0 FAIL /
0 SKIP (was 575; the +12 are this batch's harness cases), no golden changed; `DISPLAY=:99 bash tools/check.sh`
→ `CHECK: ALL GATES PASSED` rc 0 (`audit_c_with_classes` 0, `gates_vfp` 0 blockers, `audit_arm11_math` clean,
`audit_r11_idiv` no candidates, `gates_core_parity` both targets, `gates_smdh` SKIP no built .cia,
`gates_frames` `L3_f1.bmp sha=58615fc85d02e009`, batch integrity PASS, ledger VERIFIED); `make -C mdk_3ds`
rc 0; `cmake --build build-linux -j8` rc 0; and the row's own acceptance command
`mdk_3ds/tools/emurun.sh --expect hud` → rc **0** on `:99`, mandarine md5 `0d2adab6`, llvmpipe forced, mono,
level 7 frame 2520/2580, `expect : hud OK -- canvas: quads=4 draws=2 mesh=2 corners=24 eyes=1 (canvas frame
2520)`. Frame `docs/frames/2026-11-11-hud-canvas.png` is byte-identical to its predecessors (sha256
`6646adf1…49cf8`), which is what a batch that cannot reach the Linux capture should produce — and its sidecar
says plainly that the frame proves nothing about the HUD.

**Batch 141 (iteration 8 of the ledger row `3ds.collision-doors`) — the teleport lands, and with it the
oracle's own teleport assertions stop being re-stated. Review-7 is closed 5/5, nothing rebutted.** Review-7
(`review-7.md`, DeepSeek V4.1 Flash TH, no Critical, no High, verdict INCOMPLETE on F1+F2): **F1** and **F2**
(both Medium) and **F3**, **F4**, **F5** (Low) are all confirmed against the C# and fixed; none is a
strawman. **F1** was real and self-contradictory: both `DANT_10` boxes are `type=1`, so both take the
`TriggerShow` arm (`:727-730`) and the `id = -1` box clears by naming no arena to THAT arm, with
`ShowArena`'s own empty test (`:574-578`) doing the clearing — the `else if (arena.empty())` LOAD arm at
`:731-734` is reached only by a type-3 record and batch 140's own M4 had already proved nothing reddens it;
the record now says `:726` + `:727-730` + `:574-578` in all three places and keeps `:731-734` in the
compile-verified list. **F2** was real: `preloadArena`'s comment claimed moving the clear above the load is
observable, and it is not — once `secondArena_` is the arena being loaded the second exemption would need
a connection whose far end is that same arena, and `findConnections()`'s only writer pairs two DIFFERENT
names, so no arena is ever its own neighbour (the hoist half of the claim stands and is kept). **F3**: the
corridor's objects appear because `ShowArena` calls `SpawnDtiAliens` (`:597`, its only caller) and a
preload does not, not because an active second is live; the pin's discriminating power rests on the spawn
being show-only, and the comment now says that. **F4**: the two boxes' z ranges are 0..7 and 0..6, not both
0..7. **F5**: batch 140's commit message counted the `(x3)` expansions where its own evidence file counted
checks; reconciled in that file's correction block rather than by editing a published commit. **The shipped
work**: `teleportKurt` / `teleportSecond` / `isCorridorLoaded` / `enterArena` / `teleported_` /
`Player::teleport`, opcodes 112 and 173, and the `LoadArena(neighbour)` batch 139 recorded as owed at
`:516` — which also closes review-6 F3 and makes `residentArena_` READ for the first time (its only oracle
reader is `IsCorridorLoaded`). Three executed cases: the oracle's `ArenaSwitchTests.FirstTickKeepsTeleport`
and `TeleportMakesUnreachableArenaSolid`, its `DoorArenaTests.TeleportLoadsCorridorArena`,
`TeleportKeepsLoadedCorridor` and — the one batch 139 could only reach with built doors —
`LoadedArenaPullsItsDoors` on the SHIPPED level-7 door, and a bytecode case pinning both dispatches'
operand order and yields. Eight mutations, all run: M1 (first-tick flag gone) 4 red, M2 (`showArena` before
`teleportSecond`) 4 red, M3 (`isCorridorLoaded` arm gone) 3 red, M4 (load-ahead gone) 1 red, M5
(`enterArena` gone) 1 red, M6 (white flash gone) 2 red, M7 (opcode 112's yaw read from operand 3) 1 red, M8
(opcode 112 yields unconditionally) 1 red. **Two negatives found by running, not reading**: M1 reddened
NOTHING in the first draft because the case reused a runtime whose tick 0 had already passed — the C#'s
fixture is fresh and now the case is too; and the first draft's `DoorAt` captured the wrong runtime in its
lambda, which a green run would have hidden had the door not also existed in that other runtime. Gates:
`make -C tests/scripts test_crossing` (the row's acceptance) → `All crossing cases match the oracle.` rc 0
(2 cases); `make -C tests/scripts` → `All cases match the oracle.` rc 0, **575** PASS / 0 FAIL / 0 SKIP (was
572), no golden changed; `DISPLAY=:99 bash tools/check.sh` → `CHECK: ALL GATES PASSED` rc 0
(`audit_c_with_classes` 0, `gates_vfp` 0 blockers, `audit_arm11_math` clean, `audit_r11_idiv` no
candidates, `gates_core_parity` both targets, `gates_frames` `L3_f1.bmp sha=58615fc85d02e009`, batch
integrity PASS, ledger VERIFIED); `make -C mdk_3ds` rc 0 with `mdk.elf` → `5549857c421bca2926635ec6a3e0f23e`
(shipped core moved, as it must); `cmake --build build-linux` rc 0; `emurun.sh --min-frames 60` on `:99`,
mandarine `0d2adab6`, rc 0, level 7 frame 2520/2580, `health=100 objects=7 plays=1` — the same numbers as
batch 140, i.e. **neither opcode has ever been seen on the 3DS target**. Frame
`docs/frames/2026-11-11-teleport-kurt.png` is byte-identical to its four predecessors (md5
`6769a13731983d65cee9df02da6cff37`), which is what a batch whose change cannot reach level 3 frame 1 should
produce. Measurements, the mutations, the two negative results and the not-evidenced list — above all the
two-`ArenaSpace` divergence: `docs/evidence/2026-11-11-teleport-kurt.txt`; narrative: `docs/PROGRESS_LOG.md`
"Iteration 141".

**Batch 140 (iteration 7 of the ledger row `3ds.collision-doors`) — the two trigger arms batch 139 left
compile-verified are now executed, and review-6 is closed 6/6.** Review-6 (`review-6.md`, DeepSeek V4.1
Flash TH, no Critical, no High, verdict INCOMPLETE on F1 alone): **F1** (Medium — opcode 223 was still
listed as unimplemented in `docs/OPCODE_COVERAGE.md`) and **F2, F3, F4, F5, F6** (all Low) are fixed;
nothing is rebutted. F1 was a recount the file had to carry: C# 238 cases, C++ 214 labels / 213 unique,
opcode 249's nested `case 0:` being the one non-dispatch, so **212 true dispatches and 26 remaining**
(212 + 26 = 238), recounted from the same brace-matched method spans the file documents. **F5** was the
real work: `CheckTriggers`' `TriggerShow → ShowArena(arena)` arm (`ScriptRuntime.cs:727-730`) and its
names-no-arena arm (`:731-734`, the only route from inside the trigger loop to `ShowArena("")`, which is
the only thing that ever empties the second slot) had never run, because `CDANT_1`'s pair are both type 3
— and a port that swapped the two arms, or dropped the clear, passed every gate. Level 7's `DANT_10`
ships both: a `type=1 id=18` box at x[509,511] y[4968,5077] and a `type=1 id=-1` box at x[511,514]
y[4964,5082] (`golden/L7_dti.txt:45-46`), and `DoorArenaTests.FarSpot = (495, 5005, 24)` is the oracle's
own spot immediately left of them; the first tick's geometric lookup answers `DANT_10` where the C#
would have used the unported `TeleportKurt`. `testTriggerShowBoxDrawsAndTheMinusOneBoxClears` walks
there and pins the SHOW arm alone (the corridor `arenas[18] = CDANT_9` takes the second slot ACTIVE and
DRAWN, and an object of the corridor's appears because an active second is a LIVE arena (`:703`) so its
own controller runs — which a preload never does — while the resident arena does NOT move, because a
corridor is not in the MTO, `:617-620`), then the CLEAR arm alone — the two boxes' y ranges
differ, so at y = 4965 only the `id = -1` box can answer and the clear is not a side effect of the show.
Mutations: **M1** (arms swapped) reddens the new case and batch 139's, **M2** (the show arm ignores an
empty name) reddens the clear lines, **M3** (the id read as a connection-list index) reddens the show
lines — and M3 is the reading batch 139 recorded as inseparable in `CDANT_1`, which `DANT_10` separates
because it writes exactly one connection record, so id 18 is out of range and the box would clear instead
of showing. **M4** (the load-with-no-arena arm removed) reddens NOTHING, recorded as the negative result
that proves the claim it stands for: every type-3 id in all six DTI goldens is 0..9 against 19 arenas per
level, so that arm is unreachable from shipped data and stays compile-verified. Gates: `make -C
tests/scripts` → `All cases match the oracle.` rc 0, **572** PASS / 0 FAIL / 0 SKIP (was 571) and no
golden changed; `test_crossing` (the row's acceptance command) 2 cases rc 0; `DISPLAY=:99 bash
tools/check.sh` → `CHECK: ALL GATES PASSED` rc 0; `make -C mdk_3ds` → rc 0 with `mdk.elf`
`6586bee947a2ca7ccce988ddb858114a` UNCHANGED; `cmake --build build-linux` rc 0; `emurun.sh
--min-frames 60` on `:99`, mandarine `0d2adab6`, rc 0, level 7 frame 2520/2580, `health=100 objects=7
plays=1` — the same numbers as batch 139 because the binary is identical. Frame
`docs/frames/2026-11-11-trigger-show-and-clear.png` byte-identical to its four predecessors.
Measurements, four mutations including one negative, and the not-evidenced list:
`docs/evidence/2026-11-11-trigger-show-and-clear.txt`; narrative: `docs/PROGRESS_LOG.md` "Iteration 140".

**Batch 139 (iteration 6 of the ledger row `3ds.collision-doors`) — the DTI trigger boxes land, and
with them the arena-load path they drive.** Review-5 (`review-5.md`, DeepSeek V4.1 Flash TH, no
Critical, no High, verdict **COMPLETE**, 3 Low): **F1** (the cutscene early-return comment still
calling the drawn list unported) and **F2** (no `drawnArenas()` accessor, while the C# has two readers
of that list — Viewer.cs:702 draws `DrawnArenas`, Viewer.cs:533 `SetSolid` reads `SolidArenas`) are
fixed; **F3** (the `gates_batch_integrity` revision check) is owner-only. This is the pair of writers
batch 138 left on either side of its snapshot point, and they are one chain: `CheckTriggers`
(`ScriptRuntime.cs:707-740`, 0x41bf1c) is the only per-tick caller of `PreloadArena` (`:602-612`),
which is the only caller of `LoadArena` besides `ShowArena`'s own (`:582-589`) — the oracle's third
`LoadArena` caller is `TeleportSecond` (`:516`), inside the unported `TeleportKurt`, and it owes the
same call when that lands (review-6 F3) — which is the only
caller of `PullDoors` (`:628-643`) — the door move whose absence is the "the doorway showed the sky"
artefact `DoorArenaTests` opens with. Ported: `checkTriggers()` at the oracle's position immediately
ABOVE `updateDrawnArenas()`, `segmentsCross`/`crossesBox` as `:756-770`/`:743-755` line for line (the
parametric form, inclusive at both ends, false for parallel and collinear), `preloadArena()` public
because VM opcode 223 `arena_set_neighbour` (`script_vm.cpp:829`) is its other caller, `loadArena()`
with the C#'s `_residentArena` (`:312`, written and not yet read — its only reader is
`IsCorridorLoaded` `:522-531` inside the unported `TeleportKurt`), and `pullDoors()` reusing the
existing `moveDoor`. The test is `testTriggerBoxesPreloadAndPullDoors` on shipped geometry: level 7's
`CDANT_1` pair of type-3 boxes (`golden/L7_dti.txt:49-50`, id=1 x[103,169] y[582,592] and id=0
x[103,169] y[569,580], both at z = -42..-36 under a tunnel floored at -3, and `:718-720` reads X and Y
only), walked one unit per tick. It pins that the step 593 → 592 fires on the EDGE alone (`Crosses`,
not `inside`), that a preload takes the second slot INACTIVE and makes the arena resident without
drawing it, that the two boxes name `DANT_2` and `DANT_1` — arenas[1] and arenas[0], read through
`residentArena()` — and, with an unrelated active second in the slot, that the box's tick is where the
drawn list drops it, which is the position of `checkTriggers()` and nothing else (mutation M1 moves
the call below the snapshot and reddens precisely that line). `PullDoors` is pinned with two BUILT
doors — the oracle's own `LoadedArenaPullsItsDoors` needs `TeleportKurt`, and on foot `updateDoor`
has already turned the real door by the time Kurt reaches the third arena: loading Kurt's own arena's
neighbour must not take his door, and loading a corridor must pull the door into it and turn it to 270
(`TurnedYaw`) while leaving the room resident because a corridor is not in the MTO (`:617-620`).
Mutations: M1 (below the snapshot) 1 red, M2 (point-in-box only) 4 red, M4 (current-arena exemption
gone) 3 red, M5 (active-second exemption gone) **nothing red** — recorded as a negative result, that
arm is ported and correct but `updateDoor` moves the same door later in the tick so both paths end in
the same place — and M3 is not a probe but a bug the case caught in its own first draft: the `u` term
of `segmentsCross` written with `s.X` instead of `r.X`, which made a horizontal move cross every box
whose x-range it reached. Gates: `make -C tests/scripts` → `All cases match the oracle.` rc 0,
**571** PASS / 0 FAIL / 0 SKIP (was 570) and **no golden changed** — and none can be added, `dotnet`
is gone from this box, so every expected value is read off the C# and the shipped DTI; `test_crossing`
still 2 cases rc 0; `DISPLAY=:99 bash tools/check.sh` → `CHECK: ALL GATES PASSED` rc 0 (`audit_c_with_classes`
0 violations, `gates_vfp` 0 blockers, `audit_arm11_math` clean, `audit_r11_idiv` no candidates,
`gates_frames` `L3_f1.bmp sha=58615fc85d02e009`); `make -C mdk_3ds` → rc 0, `mdk.elf`
`dbaf850a…` → `6586bee947a2ca7ccce988ddb858114a` (shipped core moved, as expected); `cmake --build
build-linux` → rc 0; `emurun.sh --min-frames 60` on `:99`, mandarine `0d2adab6`, rc 0, level 7 frame
2460/2520, `health=100 objects=7 plays=1` — ~2500 ticks of the new `checkTriggers()` over real
shipped level-7 records with nothing diverging, but **no trigger box, crossing or door pull has ever
been seen on the 3DS target**. Frame `docs/frames/2026-11-11-trigger-boxes.png` byte-identical to its
three predecessors. Measurements, five mutations including one negative, and the not-evidenced list —
above all that **only type 3 is exercised** (the `TriggerShow` arm and the id = -1 arm are
compile-verified only) and that the shipped data cannot separate "index into the arena list" from
"index into the arena's connection list" in `CDANT_1`: `docs/evidence/2026-11-11-trigger-boxes.txt`;
narrative: `docs/PROGRESS_LOG.md` "Iteration 139".

**Batch 138 (iteration 5 of the ledger row `3ds.collision-doors`) — review-4 is closed 3/3: the drawn
list becomes the tick's snapshot.** This is still priority item **(1)** of batches 134-137.
**F1 (Medium)** held: batch 137 derived the list at read time and justified it by saying `UpdateArenas`
"runs at the end of that same tick (`ScriptRuntime.cs:935`)" — the line is right, the position is not.
`:935` sits right after the crossing and `CheckTriggers` and BEFORE the object loop (`:962-987`) and
`RunControllers` (`:991`), and `_drawnArenas` is assigned nowhere else (`:694-695`), so the oracle's
pin is a snapshot of the tick's START and every later writer reaches it a tick late — a door closing
(`ObjectBehaviors.cs:99`), a door opening (`:411`), opcode 100 `arena_show` (`ScriptVm.cs:474`), all
three ported. So the port dropped the active second a whole tick before the oracle does: over
`CDANT_1` the next `move()` sweeps only `DANT_2`, finds no floor, and `bottom()` answers with
`_solid[0]`'s minimum Z. Now `ScriptRuntime::updateDrawnArenas()` — the ported drawn-list half of
`UpdateArenas` (`:658-664`) — runs at the oracle's own position (`script_runtime.cpp:584`, where the
C# has `CheckTriggers(); UpdateArenas();`) and is the only writer of `drawnArenas_` (the C#'s
`_drawnArenas`), and `solidArenas()` returns that cache instead of reading the fields; the rest of
`UpdateArenas` (loaded arenas, loop sounds, `obj.Visible`) stays unported and the comment says where
it and `CheckTriggers` must go. Before the first tick the list is empty, which is the oracle's value.
Executed pin: after the level-7 crossing the case calls `showArena("")` — what a closing door calls —
and proves the fields moved while the pin did not, that a downward move inside the room Kurt left still
hits its floor, and that the NEXT tick is where the second drops; mutation M1 (deriving again) reddens
three checks including the fall-through. **F2 (Low)** held: the same case's pre-first-tick assertions
pinned the derivation's `[""]` instead of the oracle's empty list, so the faithful fix would have
reddened them; they now assert `empty()` and the `bottom()` half is untouched. **F3 (Low)** held:
`CROSSING_ARM_COUNT` was a hand-maintained third copy of the arm count; `crossing_arms()` now returns
the arms (the wrapper's no-`make` arm folded in) and both printouts take `len(arms)` — M2 adds an arm
and the verdict prints 9, and batch 137's M3 still reddens the "one case only" arm. Also corrected,
unprompted: four off-by-a-few oracle citations in the prose this batch touches (`:660-666`→`:658-664`,
`:265-278`→`:265-282`, `:271-283`→`:274-280`, `_solidFirst` `:284`→`:285`), each verified by printing
the cited lines. Gates: `make -C tests/scripts test_crossing` → `All crossing cases match the oracle.`
rc 0 (2 cases); `make -C tests/scripts` → `All cases match the oracle.` rc 0 (**570** PASS, 0 FAIL,
0 SKIP — same 570, this batch adds assertions to an existing case); `DISPLAY=:99 bash tools/check.sh`
→ `CHECK: ALL GATES PASSED` rc 0 with `note: crossing selftest: 8 seeded arms, all adjudicated as
claimed`; `make -C mdk_3ds` → rc 0, `mdk.elf` `dbaf850adeac8738bf6ce2d3b5f92e81` (moved: the snapshot
is shipped code); `cmake --build build-linux` → rc 0; both frontend gates re-ran green (12/12 arms and
20 checks/56 arms/24 probes). Emulator (`emurun.sh`, `:99`, mandarine md5 `0d2adab6`, rc 0): level 7
idle, frame 2520 by the deadline / 2580 in the log, `health=100 objects=7 plays=1` — no regression,
**no crossing exercised**. Frame `docs/frames/2026-11-11-drawn-list-snapshot.png` is byte-identical to
its two predecessors, as expected for a batch whose change cannot reach a spawn. Measurements, three
mutations and the not-evidenced list — above all that **no golden executes a mid-tick `showArena`**, so
the lag is pinned through the door's callee rather than the door: `docs/evidence/2026-11-11-drawn-list-snapshot.txt`;
narrative: `docs/PROGRESS_LOG.md` "Iteration 138".

**Batch 137 (iteration 4 of the ledger row `3ds.collision-doors`) — the frontend pin becomes the
oracle's drawn list, and review-3 is closed 2/2 with one finding owner-only.** This is priority item
**(1)** of batches 134-136. `ArenaSpace::setSolid(const std::string&)` is replaced by
`setSolid(const std::vector<std::string>&)` — clear, then add the arena each name resolves to, first
match wins, an unresolvable name (including the `""` the scripts use for "no arena") adds nothing
without clearing what came before it — and the one-name form is gone rather than kept as an overload,
because the arity mismatch was the defect. `ScriptRuntime::solidArenas()` derives the drawn list from
`currentArena_`/`secondArena_`/`secondActive_` into a member scratch (the C# keeps `_solidFirst` for
the same reason); `UpdateArenas` is unported, so the list is derived, not cached, and the comment
states why that is the same value wherever a frontend reads it. Both frontends now pin
`if (runtime) space.setSolid(runtime->solidArenas()); else <geometric lookup>` after the camera step;
the geometric stand-in survives only in the no-runtime (asset-gate) branch and the spawn pin is
untouched. The snowboard half is written and guarded on `ridesBoard_` (no writer yet) because it is
the ONE case where a one-name pin is the oracle's value — review-3 open question 4. New executed pin:
`vm_tests`' `SolidArenasAreTheDrawnListAndThePinTakesBoth` walks the oracle's level-7
`DoorArenaTests` geometry and proves that with both names pinned a downward move inside the room
Kurt left keeps its floor while pinning the current arena alone loses it — review-6 F1 executed, and
mutation M1 shows it reddening. `frontend_solid_pin_test.sh` went 10/10 → 12 checks/12 arms (the
geometric check split into drawn-list and fallback, each with its own arm) and
`main_3ds_flow_test.sh`'s check 6 now requires the list instead of forbidding a runtime pin; both
still adjudicate every arm red (56 there, 24 green probes) and mutation M2 reddens both on the real
file. **Review-3 (DeepSeek V4.1 Flash TH, verdict COMPLETE, 3 Low, no Critical/High): all three
reproduced, none rebutted.** **F1 fixed** — the crossing PASS line claimed the non-oracle lines were
"named in the golden's header" and a golden is byte-compared so it cannot carry prose; they are named
above `crossingDump` and the text now says that and why. **F2 fixed** — the required-case rule was
pinned by nothing in the tree, so the adjudication moved into `judge_crossing_output` with eight
seeded arms (`crossing_arm_failures`) that run inside the gate on every `tools/check.sh` (check.sh is
owner-protected, so no `--selftest` call could be wired in) and via `--selftest`; mutation M3 shrinks
`SCRIPTS_REQUIRED_CASES` to `{"4"}` and the selftest reddens exactly that arm. **F3 owner-only** —
protecting the constant means editing `PROTECTED` or the ledger. Gates: `make -C tests/scripts
test_crossing` → `All crossing cases match the oracle.` rc 0 (2 cases); `make -C tests/scripts` →
`All cases match the oracle.` rc 0 (**570** PASS, 0 FAIL, 0 SKIP — the 570th is this batch's);
`DISPLAY=:99 bash tools/check.sh` → `CHECK: ALL GATES PASSED` rc 0 with `note: crossing selftest: 8
seeded arms, all adjudicated as claimed`; `make -C mdk_3ds` → rc 0, `mdk.elf`
`e29531aecceedd197df85a497f876a31` (moved: the pin is shipped code); `cmake --build build-linux` →
rc 0. Emulator (`emurun.sh`, `:99`, mandarine md5 `0d2adab6`, rc 0): level 7 idle, frame 2460/2520,
`objects=7 plays=1` — no regression, **no crossing exercised**. Frame
`docs/frames/2026-11-11-drawn-list-pin.png` is byte-identical to its predecessor, as expected for a
batch whose pin cannot differ at a spawn. Measurements, three mutations, not-evidenced list:
`docs/evidence/2026-11-11-drawn-list-pin.txt`; narrative: `docs/PROGRESS_LOG.md` "Iteration 137".

**Batch 136 (iteration 3 of the ledger row `3ds.collision-doors`) — review-2 remediated 4/4: the record stops
saying the crossing is unported, and the crossing gate now requires both cases.**
The review (`logs/ralph-pi/run-20261110-230107/review-2.md`, DeepSeek V4.1 Flash TH, no Critical, no High,
verdict INCOMPLETE on F1 and F2) was reproduced finding by finding and **all four held**; none is rebutted.
**F1**: batch 135 claimed review-1 F7 closed, but `main_3ds.cpp:792-793` still read "Neither is ported, so
`currentArena_` is written exactly once", citing a line range that is now the constructor, and the same
false-present-tense claim survived in `main_3ds_flow_test.sh` (its header, its check description, its check-6
body and failure text, and two arm docstrings) and in weaker form in `frontend_solid_pin_test.sh:85`. All of it
now states the reason that is true: the crossing IS ported, so `currentArena()` moves, and the pin stays
geometric because the oracle pins the scripts' DRAWN **list** (current + active second, `ScriptRuntime.cs:265-278`)
while `ArenaSpace::setSolid` takes **one** name — so a runtime pin drops the room behind the door Kurt came
through. Both checks and all their mutation arms are unchanged in behaviour; only the stated reason moved.
**F2**: the new crossing gate refused zero `PASS L<n>_crossing` lines and accepted **one**, so deleting the L7
case from `crossingCheck` would orphan `golden/L7_crossing.txt` with every gate green. It now requires the case
set by name (`SCRIPTS_REQUIRED_CASES = {"4","7"}`), and that was shown red two ways: the reviewer's stub-`make`
probe, and the real mutation (`levels[] = {4}`) run through the gate and then reverted. **F3**: the gate's
docstring said "THE THREE CHECKS" with seven listed, its failure verdict blamed core-source parity whatever had
failed, and `--json` had no crossing field; all three fixed, and the verdict line now deliberately names no cause.
**F4**: the object loop's three effect gates tested `== 0` where `ObjectMotion.cs:54` returns on `EffectFrames > 0`;
they now test `<= 0`, matching each other and `motionPath`/`motionVelocity`. Gates: `make -C tests/scripts
test_crossing` → `All crossing cases match the oracle.` rc 0 (2 cases); `make -C tests/scripts` → see the gate
block below; `DISPLAY=:99 bash tools/check.sh`; `make -C mdk_3ds` → rc 0, `mdk.elf` `3b2a13a8…`;
`cmake --build build-linux` → rc 0. Measurements, both mutation probes, and the not-evidenced list:
`docs/evidence/2026-11-11-review2-remediation.txt`. Narrative: `docs/PROGRESS_LOG.md` "Iteration 136".

**Batch 135 (iteration 2 of the ledger row `3ds.collision-doors`) — review-1 remediated 7/7: the crossing
goldens' provenance is now true, level 7 discriminates, and the gate suite runs the row's acceptance command.**
The review (`logs/ralph-pi/run-20261110-230107/review-1.md`, DeepSeek V4.1 Flash TH, no Critical/High, verdict
INCOMPLETE) was reproduced finding by finding and **all seven held**; none is rebutted. **F1**: every provenance
citation in the crossing goldens pointed at a comment or a `StepTo` call instead of the assertion, and
`L4 s=1 current` had **no oracle assertion behind it at all** — the C# names that arena by teleporting into it
and asserts nothing after the first tick. Every citation was re-read with `grep -n` (table in
`docs/evidence/2026-11-11-crossing-remediation.txt` §1), the golden's header now names the two lines that are
NOT oracle assertions, and the PASS line stopped claiming otherwise. **F3**: the L7 case passed a geometric-arena
implementation (geometry flips at y = 676, the crossing happens at 674, the walk ended at 687 where both agree);
the golden now also prints `L7 s=675 current=DANT_2 geometry=CDANT_1`, and the reviewer's mutation — `runTick`
taking `arenaAt` every tick — now reddens **both** levels. **F2**: the claimed anti-rot protection did not exist
(`tools/check.sh` never ran `tests/scripts`), so `gates_core_parity.py` check 7 **PARITY-SCRIPTS-CROSSING** now
builds and runs the row's acceptance command — 0.7 s warm / 38 s cold, against the full suite's **7m27s**, which
is why the gate runs the crossing target and not all 569 cases — and it reddens on no-`make`, a broken golden,
and a verdict printed with zero `PASS` lines behind it. **F5/F6/F7** folded in: the `followArenas` effect gate,
the `showArena`/`LoadArena`/`PullDoors` gap stated instead of excused, and the four comments that still said the
crossing was unported (`main_3ds.cpp`, `mdk_linux/main.cpp`, `arena_space.h`, `frontend_solid_pin_test.sh`).
**F4 is owner-only and still open**: `ralph/builder.md` is `PROTECTED` and modified-unstaged; this batch committed
with explicit paths, not `git add -A`, so it could not be swept in. Gates: `make -C tests/scripts test_crossing`
→ `All crossing cases match the oracle.` rc 0; `make -C tests/scripts` → `All cases match the oracle.` rc 0
(569 PASS, 0 FAIL, 0 SKIP); `DISPLAY=:99 bash tools/check.sh` → `CHECK: ALL GATES PASSED` rc 0 with
`note: crossing target: make -C tests/scripts test_crossing exit 0 — 2 crossing case(s): L4, L7`;
`make -C mdk_3ds` → rc 0; `cmake --build build-linux` → rc 0. Emulator (`emurun.sh`, `:99`, mandarine md5
`0d2adab6`, rc 0): level 7 idle, frame 2460/2580, `objects=7 plays=1` — no regression, **no crossing exercised**,
so the 3DS rest-pitch change is still unmeasured on a real crossing. Frame
`docs/frames/2026-11-11-crossing-review-remediation.png` is byte-identical to its predecessor, as expected for a
batch that moves no pixels. Narrative: `docs/PROGRESS_LOG.md` "Iteration 135"; measurements and the not-evidenced
list: `docs/evidence/2026-11-11-crossing-remediation.txt`.

**Batch 134 (iteration 1 of the ledger row `3ds.collision-doors`) — the doorway crossing lands, and
`test_crossing` exists.** The row's acceptance command did not exist (review-4 F1: `No rule to make target
'test_crossing'`), and the substance behind it was the subsystem every frontend has worked around since batch 75:
`FindConnections`, `CrossedArena`, `CrossesDoorway`, `Spans`, the `Doorway` enum, the `_records` cache and
`FollowArenas`, transliterated from `ScriptRuntime.cs:775-881` and wired at the C#'s own call sites — the
constructor (`findConnections`, :364), the tick (:918-919) and the object loop (ObjectMotion.cs:104-107). The
port answers **every** assertion of the oracle's two crossing tests: level 4 stays in `MEAT_7` while Kurt is
inside the corridor's *box* but through no door, moves to `CMEAT_7` at y = 14825 with `MEAT_7` as the active
second, comes back, and level 7's tunnel walk reaches `DANT_2`. `make -C tests/scripts test_crossing` is a new
phony target for those two cases and they also run inside `tick1`'s case list, so the row cannot rot the way
`tests/formats` did. Shown red on two mutations (frozen arena → both levels; the on-the-plane boundary → level
7) and shown green-but-unpinned on two others (`DoorwayDrop`, the hatch arms) — recorded as gaps, not hidden.
The near-miss is in the record: the first draft dropped the `MinusZ`/`PlusZ` arms, and the shipped DTIs use dirs
6/7 in levels 3, 5, 6 and 8, so level 5 would have had every doorway sealed. Gates: `make -C tests/scripts` →
`All cases match the oracle.` rc 0 (569 PASS, 0 FAIL, 0 SKIP); `make -C tests/scripts test_crossing` → `All
crossing cases match the oracle.` rc 0; `DISPLAY=:99 bash tools/check.sh` → `CHECK: ALL GATES PASSED` rc 0;
`make -C mdk_3ds` → rc 0. Emulator (`emurun.sh`, `:99`, mandarine md5 `0d2adab6`, rc 0): level 7 idle, frame
1620/1680, `objects=7 plays=1` — no regression, and no crossing exercised. Full narrative:
`docs/PROGRESS_LOG.md` "Iteration 134"; measurements and the not-pinned list:
`docs/evidence/2026-11-10-crossing.txt`.

*(Two claims in the paragraph above are no longer true and batch 135 corrected them, per review-1 F1 and F2: the
golden lines do **not** each cite an oracle assertion — two of them cite none, and the citations pointed at the
wrong lines — and being inside `tick1`'s case list did **not** stop the row rotting, because `tools/check.sh`
never ran `tests/scripts`. The paragraph is kept as written for the record; the corrections are batch 135's.)*

**Batch 133 (iteration 5 of the ledger row `core.formats`) — the format target is now built by the gate suite.**
The row's own comment named two defects and batches 129-132 fixed only the first: the link was broken, and it
stayed broken for 25 batches *"unnoticed because check.sh never ran this target"*. That second half was still
true at dispatch — the row could rot back with every gate green. `tools/runner/gates_core_parity.py` check 6
(PARITY-FORMATS) now closes it: it parses `tests/formats/Makefile`'s own `SOURCES` block, reports
`PARITY-FORMATS-GHOST` (an entry that does not exist) and `PARITY-FORMATS-BASENAME` (two entries sharing a
basename, which the Makefile's `notdir` object keys silently collapse into one object), and then **builds and
runs** the target as `PARITY-FORMATS-LINK` — a nonzero exit, a timeout, a missing `make`, or an exit 0 that
printed no `N checks, M failures` line are all MUST, never a skip. Shown red on four probes (ghost entry;
stub `tests/formats/texture.cpp`, where the collapse really did break the link with `undefined reference to
mdk::formats::Texture::parse`; an `undefined reference to mdk_notARealSymbol()` in `formats/dti.cpp`, the
exact shape of the original rot; and `PATH=/nonexistent`), reverted, and green clean at 1.6 s warm /
16 s cold. `docs/evidence/2026-11-10-core-formats.txt` is the measured artefact batch 129 said the row needed,
and it states what the run does *not* evidence — including that the target compiles 16 of the 32 sources under
`mdk_core/src/formats`, now printed as a gate note instead of hidden. **The ledger closed under this iteration
by its owner**: `8c751cc` committed the ledger and flipped `core.formats` to `done` (evidence `tests/formats`,
verified `8ba4335`), so review-4 F1's ledger half is the owner's and is done; what this batch adds is what keeps
a `done` row with a *slow* acceptance command true between milestones. Gates: `make -C tests/formats` →
`156235 checks, 0 failures` rc 0 from a clean build; `make -C tests/scripts` → `All cases match the oracle.`
rc 0 (567 PASS, 0 FAIL, 0 SKIP); `DISPLAY=:99 bash tools/check.sh` → `CHECK: ALL GATES PASSED` rc 0 with
`PASS gates_core_parity` and the new `note: format target: make -C tests/formats exit 0 -- 156235 checks, 0
failures`; `make -C mdk_3ds` → rc 0. Review-4 is 8 findings: 1 closed here (F1's `core.formats` half), 1 closed
by the owner mid-iteration (F4), 6 owner-only and recorded (`F2` reproduced — deleting the two open rows turns
`--strict` from rc 1 to `COMPLETION: DONE[d1]` rc 0 — `F3`, `F5`, `F6`, `F7`, `F8`), 0 rebutted by argument
alone. Full narrative: `docs/PROGRESS_LOG.md` "Batch 133".

**Batch 132 (iteration 4 of the ledger row `core.formats`) — the ownership counter cannot be made to lie.**
Review-3 confirmed three defects in the contract batch 131 wrote, all of one family: it held only while
nobody wrote an ordinary line of C++. **`Arena` is now non-copyable and non-movable** — `liveCount()` is the
only instrument that can see an arena die and the implicit copy operations bypass the constructor and
destructor that maintain it, so one copy made the counter say one object while two existed and deleting the
original then said zero while the copy was still being drawn; the four `= delete` lines plus four
`static_assert`s in `format_tests.cpp` make the counter's invariant a compile-time property (shown: remove
the deletes and the target does not compile, all four assertions firing). **`LevelData::load`'s reset is
complete** — it released four fields and left `dti_`, `mtoBytes_` and `levelTextures_` behind, and a failed
`readFile` does not touch the buffer it was given, so a load that failed after the reset was a *mixture*:
level N wearing level 3's DTI and textures, which `archivesOf()` would have used for the new arenas'
surfaces. Every field is now emptied before the first read and `number_` is set only on success, so the rule
is one line — *a `LevelData` after `load` holds exactly the level that call loaded* — and
`AFailedLoadLeavesAnEmptyLevel` pins it (shown: remove the reset lines and it fails on exactly the three
fields). **`BetaDemo::loadMto` now releases the out-vector it replaces**, which leaked a caller's whole
first level on a second call; it has no caller yet, so that half is compile-verified by the Linux target
only, and this batch did not widen `tests/formats`' source list to force the point. **Review-3 is closed
7/7: F2/F5/F6 fixed, F1/F3/F4/F7 owner-only and recorded as owed** — F1 remains the sole blocker on the row
(`docs/COMPLETION_LEDGER.tsv` is PROTECTED and `coleshores`-owned, so no batch can fill its evidence column
and attempt 9 ends the loop with `exit 6`), F3 is `make -C tests/formats` in `tools/check.sh`'s gates, F4 is
a capture-binary identity check in `gates_frames.py` (this batch's own frame came from a binary built at
`14:21:42`, after every source edit, and reproduced the manifest hash), F7 is a `gates_completion.py`
selftest arm. Gates: `make -C tests/scripts` → `All cases match the oracle.` rc 0 (per-step rc 0); `bash
tools/check.sh` (`DISPLAY=:99`) → `CHECK: ALL GATES PASSED`; `make -C mdk_3ds` → rc 0. ASan+UBSan on the
format target: rc 0, zero leaks, `156235 checks, 0 failures`. Emulator (`emurun.sh`, `:99`, mandarine md5
`0d2adab6`, rc 0) reached guest frame 2580 on level 7, identical to batch 131. Full narrative:
`docs/PROGRESS_LOG.md` "Batch 132".

**Batch 131 (iteration 3 of the ledger row `core.formats`) — the arenas have an owner.** Batch 130 left a
measured lead: the format target under ASan+UBSan reported `47394202 byte(s) leaked in 15442 allocation(s)`,
every one of 193 leak stacks under `Mto::getArena`, because `Mto` had no destructor while `LevelData::~LevelData`
commented that "the arenas belong to the MTO" and `beta_demo.h` said the caller deletes the arenas `add()`
hands over. Both statements are now true. **`getArena()` parses, so the Mto frees**: a new `owned_` vector
records the arenas the Mto made and `~Mto()` deletes exactly those — which is what the oracle's
`Dictionary<string, Arena>` in `Mto.cs` means in a language with destructors. **`add()` borrows**, so
`owned_` never sees it, and the naive destructor that deletes the whole cache is a double free in
`BetaDemo::loadMto` (measured: the run aborts). `Mto` is movable and not copyable, and both move operations
are hand-written rather than defaulted — the assignment must release what the target owned, because
`LevelData::load`'s `out->mto_ = Mto::parse(...)` is exactly that path, and a defaulted move would leak the
previous level there instead. `beta_demo.cpp`'s `*outMto = mto` became a move. `LevelData::load` now resets
a reused out-param (palettes, corridors, arenas, MTO) instead of appending a second helping — which my own
change made a hazard, since a second load would otherwise have left `arenas_` dangling rather than leaking.
`Arena::liveCount()` was added so the contract is testable in the normal build, and **five cases** pin it
(`MtoFreesTheArenasItParsed`, `MtoKeepsTheArenasItWasGiven`, `LevelDataReleasesItsArenasAndCorridors`,
`MtoAssignmentReleasesWhatItOwned`, `LevelDataLoadIntoALoadedLevelStartsOver`), each shown to fail under a
sabotage of the code it pins. **ASan+UBSan after: rc 0, zero leaks, zero runtime errors, `156218 checks,
0 failures`.** Gates: `make -C tests/scripts` → `All cases match the oracle.`; `bash tools/check.sh`
(`DISPLAY=:99`) → `CHECK: ALL GATES PASSED`; `make -C mdk_3ds` → rc 0. Emulator (`emurun.sh`, `:99`,
mandarine md5 `0d2adab6`, rc 0) reached guest frame 2580 on level 7 with the arenas owned. **Review-15 is
closed 1/10 on remediation (R15-8, batch 130) and 10/10 on disposition**: R15-1/2/3 and R15-4/5/6/7/9 are
confirmed against the files and are owner-only, R15-10 is accepted as disclosed. **The owner has landed the
completion harness in the working tree** — the ledger, `OWNER_BLOCKS.md`, `gates_completion.py`, the
`check.sh` gate 9 and `ralph-pi.sh`'s per-iteration row selection all exist and run (`COMPLETION[d1]: 9 done
/ 13 total / 0 owner-blocked / 4 open`) — but they are **uncommitted and not builder work**: six of the nine
dirty paths are `PROTECTED`, four are `coleshores`-owned and unwritable by uid 994, and `git commit` runs
`gates_batch_integrity.py --staged`, which refuses them without `MDK_OWNER_APPROVED`. This batch did not
self-authorise that; it committed only its own paths and left the owner's files untouched. **R15-1 is
therefore still the one blocker**: `core.formats` has a green acceptance and an `evidence` column no batch
can fill, and attempt 9 ends the loop with `exit 6`. Full narrative: `docs/PROGRESS_LOG.md` "Batch 131".

**Batch 130 (iteration 2 of the ledger row `core.formats`) — the format target's link-graph guard stops
reporting clean when it cannot run, and the row's own blocker is measured rather than asserted.** The
ledger dispatched `core.formats` (acceptance `make -C tests/formats`) for the second time. The acceptance
is green and was re-measured, not inherited: `running 15 tests` … `156190 checks, 0 failures`, rc 0.
**The row still cannot be closed by any batch, and that is now the whole finding.** `docs/COMPLETION_LEDGER.tsv`,
`docs/OWNER_BLOCKS.md` and `tools/runner/gates_completion.py` are `coleshores:coleshores` with `test -w`
NO for this uid, and the ledger is additionally in `gates_batch_integrity.PROTECTED`; so a row whose
acceptance passes keeps an empty `evidence` column, `--next` returns it every iteration, and attempt 9
trips `ralph-pi.sh:798` → `exit 6`, which is in the unit's `RestartPreventExitStatus=2 6 7` and parks it.
That is review-15 **R15-1 (Critical, confirmed, owner-only)** and it is the one decision that unblocks the
loop's exit; the edit is one line in a file no batch can write. **What this batch fixed is R15-8**, the only
finding in review-15 that sits in a file this uid may write and that is not owner-protected — and it sits on
this row's own acceptance target. The link-graph guard was `nm -C '$@' | grep -qE '$(VM_PATTERN)'`, and a
pipeline's status is its last command's: with `nm` off PATH, `grep` read empty input, matched nothing, and
the guard printed the clean answer — measured on the shipped binary, the old recipe line reported clean on
the very artefact that does link `mdk::LevelData::load`. The dump now goes to `$(BUILD)/format_tests.symbols`,
`command -v nm` is a precondition, a nonzero `nm` or an empty dump fails and echoes `nm`'s own text, and every
refusal path removes the artefact. Three arms measured, not argued: `nm` missing → refuses; `nm` present and
exiting 1 → refuses with its stderr; a pattern that matches → refuses (positive control, so the guard is shown
to fail rather than assumed to). **Review-15 is closed 1/10 on remediation and 10/10 on disposition:** R15-8
fixed as above; R15-1/2/3 (two Critical, one High) and R15-4/5/6/7/9 are confirmed against the files and are
owner-only, each in `ralph-pi.sh`, `gates_completion.py`, the ledger or `check.sh` — files this uid cannot
write or may not stage, so they are escalated, not closed; R15-10 is accepted as disclosed (a frame published
before its commit necessarily names its parent, and `gates_frames`/`docs/frames/README.md` are owner-protected
frame tooling). **A measured lead for the next `core.formats` attempt, recorded rather than worked:** the
format target under ASan+UBSan reports `47394202 byte(s) leaked in 15442 allocation(s)` with stacks through
`Arena::parse` ← `Mto::getArena`; `Mto` has no destructor, `Mto::add` stores *borrowed* arenas that
`beta_demo.h:29-31` says the caller deletes, and `LevelData::~LevelData` comments "the arenas belong to the
MTO" while nothing implements that — so arenas a level parses on demand are owned by nobody. Fixing it is an
ownership change across `formats/mto.*`, `game/level_data.*` and `formats/beta_demo.*` (which does
`*outMto = mto`, a copy that a naive deleting destructor would double-free), so it is its own high-risk batch,
not a rider on a green row. Also measured: `/tmp/kilo` is `coleshores:coleshores` and not writable by the
loop's uid, so P7 captures went to `/tmp/mdk-b130`; the display is recorded (`:99`, Xvfb pid 2316266).
Full narrative: `docs/PROGRESS_LOG.md` "Batch 130".

**Batch 129 (iteration 15) — ledger row `core.formats`: the format tests link again, because the
pose-cache handle got the translation unit its header always described.** `make -C tests/formats` had
not linked since batch 102 (`727ade1`) and no gate ran it, so 25 batches passed over a dead target. The
failure was `level_data.cpp:37: undefined reference to mdk::scripts::forgetPoseCaches()`, and the cause
was a half-true comment: `scripts/pose_cache.h` says it exists so the game layer can drop the pose
caches without including the scripts layer, but the *definition* was in `scripts/object.cpp`, so the
header removed the include while the link still demanded the VM — measured by `nm -u` to reach `Bsp`,
`ArenaSpace`, `Kurt`, `Player`, `SoundMixer` and `Mto`, i.e. the game. The caches, the bake helper,
`pose_parts()`, `forgetPoseCaches()` and `forget_poses()` now live in `scripts/pose_cache.cpp`, which
needs two formats TUs the test already lists; the move diffs byte-for-byte except one comment, and the
three refused alternatives (link the VM, delete the call and leave the harness with no drop, stub it in
the test) are each recorded with the reason they are wrong. The pin is a link-graph assertion in
`tests/formats/Makefile` that greps the **linked binary** for script-VM symbols and refuses the
artefact, because the tempting wrong repair — keep adding game sources until the linker stops
complaining — *links*; positive control measured at 107 matching symbols in `tick1` against 0 in
`format_tests`. No behavioural golden was added and the reason is stated rather than skipped: the drop
is unobservable through the port's API (a rebuilt pose is bit-identical to a cached one) and the only
distinguishing probe is map-node address identity across a `clear()`/`emplace()`, which malloc is free
to satisfy with the same address. **Review-14 is closed 0/4 on remediation and 8/8 on disposition:**
F1–F4 (two Critical, two High) are all confirmed against the files and all sit in the operator's
uncommitted completion-ledger set, which this uid cannot even write (`ralph-pi.sh`,
`docs/COMPLETION_LEDGER.tsv`, `docs/OWNER_BLOCKS.md`, `tools/runner/gates_completion.py` are
`coleshores:coleshores` and `test -w` says NO) or may not stage (`tools/check.sh`,
`gates_batch_integrity.py` are `PROTECTED`, and `CONTINUATION.md:871` forbids staging them and forbids
setting `MDK_OWNER_APPROVED=1`); F5–F8 are confirmed and deferred or owner-only, each with its patch.
That answers review-14's open question 1 with facts: **no batch can close a ledger row today**, so
`core.formats` stays `todo` even though its acceptance now passes, and the loop can dispatch rows it
cannot finish. Full narrative: `docs/PROGRESS_LOG.md` "Batch 129".

**Batch 128 (iteration 14) — review-13 closed 3/3 — the bulk-fill rule reads every setter in the
statement.** Test-infrastructure only, in one file, and no shipped byte moves: it closes a Medium
hole in the rule that pins the shipped R = 255 defect — a C++ statement could carry a SECOND bulk
setter the scan never read — and re-opens four legal zero spellings that same rule wrongly refused. A
gate that cannot see the defect it claims to refuse is a pass generator, and a gate that reddens a shape
the port may write gets routed around (review-16 R2), so both halves are behaviour-adjacent. It closes
review-13 **3/3** (no Critical, no High: one Medium, two Low), all three reproduced against the
committed gate before anything changed and none rebutted. **F1**: batch 126 pinned the padding fill's
destination and length, batch 127 pinned where it SITS, and neither pinned HOW MANY — `bulk_op`
returned the first bulk setter in a statement, and a statement ends at a semicolon and not at a
newline, so `memset(pixels, 0, texels * 4), memset(pixels + 4, 0xFF, 4);` at the shipped fill's
position painted four opaque bytes at the base of the texture and printed 41/41, on the committed gate
AND on the pre-batch one (the reviewer's own note that this is pre-existing is confirmed and is why it
is Medium). `bulk_ops` now returns every match in the statement, deduplicated by open paren because a
plain `memcpy(` is matched by two rules and is one operation, and the statement is `padding` only when
EVERY setter in it is — the fail-closed doctrine the argument reader already states, applied to
multiplicity. Four arms, one of them labelled as the accident-of-order control it is (a `memcpy` second
was already refused because the copy rule ran first), plus a probe for the legal half: two whole-buffer
transparent fills comma-joined, which is green both before and after and is the only thing that tells
the fixed rule apart from an over-tightened one. **F2**: the zero class carried `[uU]?` only, so
`memset(pixels, 0L, texels * 4)` and `0x0UL` — byte for byte the shipped fill — returned "write" while
the comment above claimed "the zero LITERALS of the two languages"; the class is now every integer
literal of value 0 in the four bases (decimal `0`, octal `00`, hex `0x0`, binary `0b0`) with any
integer suffix, the prose states the scope and its edge instead of a slogan, four probes pin the zero
side and two arms pin the edge (`07` is octal 7, `0b1` is one: refused before, refused now). **F3**:
the previous sidecar was four generated lines with no disclosure, so the frame directory alone would
read a static re-capture as evidence about the fill position; this pair's sidecar carries the md5, the
`cmp`-identity, the count (136 of 137 PNGs share one md5) and the sentence that it is a PRESENCE check
and not a fidelity check — the review-12 sidecar itself is immutable by gate and stays owner-only.
Measured: 41 checks / 94 arms / 27 probes, up from 41/88/22 — no check added and none removed, the
`rep.check(` name lists diff identical, so all new coverage is in arms and probes; the three genuinely
new arms were green on the pre-batch gate and red now, the four new zero probes were red before and
green now, and all 122 mutants compile for armv6k with 0 diagnostics. No `mdk_3ds/**` or
`mdk_core/**` byte is in this batch, so no P8 emulator evidence is owed and none is claimed; `mdk.elf`
`67fadc4179c40ee3a4e5090a2218641a` and `mdk_c3d.shbin` `d46abb60de20b2aa0659043aeb97b9c9` are
byte-identical to batches 122–127. Full narrative: `docs/PROGRESS_LOG.md` "Batch 128".

**Batch 127 (iteration 13) — review-12 closed 8/8 — the padding fill is pinned by where it sits.**
Test-infrastructure only, in one file, and no shipped byte moves: it closed two High
holes in the rule that pins the shipped R = 255 defect — a padding fill could sit anywhere, and a
NONZERO character literal counted as "the transparent texel" — and re-opens two legal spellings that
same rule had wrongly refused. A gate that cannot see the defect it claims to refuse is a pass
generator, and a gate that reddens a shape the port may write gets routed around (review-16 R2), so
both halves are behaviour-adjacent. It closes review-12 **8/8** (no Critical: two High, two Medium,
four Low), all eight reproduced against the committed gate before anything changed and none rebutted.
**F1**: batch 126 pinned the padding fill's destination and its length and never looked at where the
fill SITS, so `memset(pixels, 0, texels * 4)` moved to the line before `return pixels;`, erased every
texel the helper had just written, and printed 41/41 — the case review-11 F1 had named ("at any length
and any position, including after the draw") and the one batch 126's own closing sentence asserted
impossible. A padding fill is now legal only BEFORE the first statement that writes a texel (the
helper call counts: the fill exists to be overwritten) and not INSIDE a loop whose body writes one,
which is the same wipe one brace deeper and invisible to source order — review-12 open question 2,
answered as a defect and pinned by an arm, not left as a limit. **F2**: `ZERO_FILL`'s literal
alternative `'(?:\0|x0+|0)'` accepted `'0'` — the character 0x30, so a padding row of alpha 48,
opaque garbage — and `'x0'`, a multi-character constant; batch 125 refused both and batch 126's
literal fix let them through, so this was a regression. The class is now the zero LITERALS only
(`0x…0`, `0[uU]?`, `'\0'`/`'\00'`/`'\000'`, `'\x0'`/`'\x00'`), which also closes **F5**, since the
`x0+` alternative could never reach a real escape. **F3**: review-11 F4's fix landed in
`DEREF_WRITE_TAIL` only, and the two subscript rules kept an operator slot that cannot reach `<<=` or
`>>=`, so `out[(size_t)x * 4 + 3] >>= 1` and `((uint8_t*)pixel)[3] >>= 1` halved a texel alpha green
while the docstring presented that slot as proof there was no hole; the slot now matches the
shift-assigns and the prose says so. **F4**: the fill's length was compared as the author's TEXT, so
`memset(pixels, 0, texels*4)` — the shipped statement after a column wrap — reddened a correct
filler; both sides now normalise whitespace out of the arithmetic. **F6**: the algorithms required a
literal `std::`, so `fill(pixel, pixel + 4, 0xFFu)` after `using std::fill;` was invisible; the bare
names are in scope exactly when the file carries the using-declaration, the shape `helper_call_re`
already uses (review-8 F5), with one arm and one probe for the two directions. **F7** and **F8** are
the state of record lying: a `4/4` for a review with five findings, and a `.3dsx` md5 that does not
reproduce (re-measured here: a third value from the same tree) beside a claim that `mdk.elf` is
absent when it is on disk and byte-stable. Measured: 41 checks / 88 arms / 22 probes, up from
41/81/19 — no check added and none removed, so the new coverage is all in arms and probes; every new
arm was green on the pre-batch gate and red now, both new fill-spelling probes were red before and
green now, and all 111 mutants compile for armv6k with 0 diagnostics. No `mdk_3ds/**` or
`mdk_core/**` byte is in this batch, so no P8 emulator evidence is owed and none is claimed;
`mdk_c3d.shbin` `d46abb60de20b2aa0659043aeb97b9c9` is byte-identical to batches 122–126 and
`mdk.elf` is `67fadc4179c40ee3a4e5090a2218641a`; the re-published frame is the duplicate review-11
F5 describes, which stays owner-only. Full narrative: `docs/PROGRESS_LOG.md` "Batch 127".

**Batch 126 (iteration 12) — review-11 closed 4/5 (F5 deferred, owner-only) — the bulk-fill rule
reads the operation, not the punctuation.** Test-infrastructure only, in one file: it widened what
`c3d_frame_test.sh` could refuse and narrowed rules that reddened legal spellings, so it both opened
and closed coverage on the rule that pins the shipped R = 255 defect. A gate that cannot see the
defect it claims to refuse is a pass generator, and a gate that reddens a shape the port may write
gets routed around (review-16 R2), so both halves were behaviour-adjacent even though no shipped
byte moved. It closed review-11 4/5 (no Critical, no High: three Medium, two Low; F5 deferred, and it
is owner-only), all five reproduced against the committed gate before anything changed, and every one
of them sat INSIDE the rule batch 125 had just added. **F1**:
batch 125 adjudicated a memset destination by `return "+" in args[0]`, which is neither "is this an
offset" nor "is this the padding" — `memset(&pixel[3], 0, 1)`, `memset(&out[(size_t)x * 4], 0, 4)`
and `memset(pixel, 0, 4)` each erased texel bytes a correct helper call had just written and each
passed 41/41; the destination is now adjudicated against the body's OWN allocation (a zero fill is
the padding fill only when it names the buffer the body `malloc`'d and its length is that
allocation's own size expression), and a per-row padding fill is a stated refusal, not a workaround.
**F3**: the intrinsic list was a spelling list — `\bmemset` cannot match `__builtin_memset` and the
`std::` algorithms were in no list — so the match is now the OPERATION (`BULK_FILL` sets a constant,
`BULK_COPY` moves source bytes, which is a texel write whatever it copies), with three probes holding
the legal C++ spellings of the padding fill open. **F2**: the whitelisted `'\0'` was unreachable
because `call_args_at` dropped literal contents; it now takes `keep_literals`, defaulted off so every
other reader is untouched. **F4**: batch 125's read fix read `>>=` as a comparison, so the tail is
now `[^=;]*(?:<<|>>|[^=!<>])=(?!=)` and shift-assigns are writes again. **F5** (the frame pair being
the 134th byte-identical PNG) is owner-only: `gates_batch_integrity.py` is PROTECTED. Measured: 41
checks / 81 arms / 19 probes, up from 41/74/16 — no check added or removed, so the new coverage is
all in arms and probes; every new arm was green on the pre-batch gate and red now, and all 101
mutants compile for armv6k with 0 diagnostics. No `mdk_3ds/**` or `mdk_core/**` byte is in this
batch, so no P8 emulator evidence is owed and none is claimed; `mdk_c3d.shbin`
`d46abb60de20b2aa0659043aeb97b9c9` is byte-identical to batches 122–125 and the re-published frame
is the duplicate F5 describes. Full narrative: `docs/PROGRESS_LOG.md` "Batch 126".

**Batch 125 (iteration 11) — review-10 closed 4/4 — the path resolution reaches the parameter, and
the write rules stop reading reads.** Review-11 found the defects inside this batch's own rule (see
Batch 126 above), but every claim this paragraph makes still holds. **F1**: the palette-path
resolution dead-ended at a PARAMETER, and the branch an unresolved call falls into — source-fed,
"the alpha must be the fourth byte" — is exactly what the shipped defect does with the palette's
alpha, so a filler whose palette parameter was renamed passed all 41 checks; a parameter now resolves
at the function's call sites in this file (two arms, one green-half probe). **F2**: the deref-write
tail `[^=;]*=` swallowed the operator of `!=`, `<=` and `>=`, so reads through a pointer reddened a
correct filler; the tail now matches only an assignment, `binding()`'s same-class `==` hole is fixed,
and the read spellings are committed as a probe — which is also F4, whose "26-spelling table" now
exists as suite artifacts. **F3**: memset was adjudicated at all — a zero fill at a base pointer is
the padding fill (an all-zero texel is alpha 0 under ANY byte order, the index-0 invariant), a
nonzero fill is a texel write, and an unparseable argument list fails closed. Measured: 41 checks /
74 arms / 16 probes, up from 41/71/13. Full narrative: `docs/PROGRESS_LOG.md` "Batch 125".

**Batch 124 (iteration 10) — review-9 closed 8/8 — the texel path is resolved, not recognised.**
It closes review-9 8/8 (no Critical, **two High**), and both Highs falsified batch 123's closure
claim. Both were reproduced against the committed gate before anything changed. **F1**: the bare-dereference write rule was anchored at the
start of the FOLDED statement, and folding makes a guard part of the statement, so
`if (index != 0) *(pixel + 3) = ...`, an `else` and a one-line `for` body each passed all 41 checks
with memory holding the palette's alpha in the red slot; the rule is now `deref_write`, which reads
the token before the `*` (statement start, punctuation, operator, or `else`/`return`/`do`/`case`)
instead of asking whether the `*` is first, and it cannot be un-anchored without reddening the legal
declaration `const uint8_t* colours = paletteRgba;`. **F2**: the two texel paths were told apart by
the literal word `palette` in the colour arguments, so a one-line alias made a palette-fed call look
source-fed, where a forced alpha was allowed - the shipped defect with the word `palette` gone; the
path is now resolved through a one-line `binding()`, and `stride_of` says whether a source has an
alpha byte at all, which is what closed **F3** (Medium) too: a 4-stride source must hand over its own
fourth byte, only a 3-stride source may force opaque. **F4** (Medium) made `byte_index` follow a
hoisted byte local, because the batch that widened the byte reader reddened a legal refactor in four
checks. **F8** (Low) resolves the `using`-declaration in the blanked copy, so a commented-out `using`
no longer lets a local function named `texelBytes` pass for the helper. **F5, F6, F7** (Low) are
record corrections carried in this pair, since `gates_batch_integrity` forbids editing a historical
sidecar. Measured: 41 checks / 71 arms / 13 probes, up from 41/65/10; every new arm was green on the
pre-batch gate and is red now, and all 85 mutants compile for armv6k with 0 diagnostics. No
`mdk_3ds/**` or `mdk_core/**` byte is in this batch, so no P8 emulator evidence is owed and none is
claimed; `mdk.elf` `67fadc4179c40ee3a4e5090a2218641a`, `mdk_c3d.shbin` `d46abb60de20b2aa0659043aeb97b9c9`
and the re-published frame are byte-identical to batch 123's. Full narrative:
`docs/PROGRESS_LOG.md` "Batch 124".

**Batch 123 (iteration 9) - review-8 closed 5/5 (no Critical, two High), and review-9 falsified two
of its claims.** The bypass scan moved from LINES to folded statements over comment-blanked text,
and the channel-order and alpha rules moved from the two named expansions to every 5-argument call
in the file - both still true, both still load-bearing. What review-9 found was that the write rule
recognised only a dereference that began its statement, and that the two paths were told apart by a
literal substring rather than by data flow; both are closed above. Two claims this file made that
batch 124 corrected rather than quietly repaired: the texel family's breakdown summed to eleven
(it is ten, and the sink check was counted twice) and the pair was called the 132nd (it was the
131st). Full narrative: `docs/PROGRESS_LOG.md` "Batch 123".

**Batch 122 (iteration 8) - review-7 closed 6/6.** F1, F2 and F3 (Medium) fixed; F4, F5 and F6
(Low) fixed, none rebutted. F1's recommended remediation was taken in a stronger form than
proposed: pinning the sink to the two NAMES would have refused a legal third expansion, so the
sink is pinned to the HELPER (the producer must fill texels through `pica::texelBytes`) and a
probe holds the legal third-expansion spelling open, which is the half that keeps the gate from
being routed around (review-16 R2). F5 also fixed a blind spot the reviewer did not name:
`code_lines` drops any line starting with `*`, so a bare `*dst = x;` write was invisible to the
bypass detector; the scan now runs over `blank_comments`. F3's measurement was false as written
and is corrected in the Makefile, in `docs/CONTINUATION.md` and in batch 121's log entry (marked
as a correction, not rewritten); what actually holds is that 0 basenames map to two distinct
source paths. F6 cannot be fixed in place - `gates_batch_integrity` refuses any edit to a
historical sidecar - so the corrected count rides forward in this pair. Two things this batch
claimed that review-8 later falsified, left visible rather than quietly repaired: that the bypass
detector saw "every spelling of a write" (it did not see a wrapped one) and that the texel family
was closed at six checks (it was eight, and is now ten). Full narrative:
`docs/PROGRESS_LOG.md` "Batch 122".

**Batch 121 (iteration 7) - review-6 closed 4/4.** F1 fixed with a pin at each site plus two
probes holding the legal spellings open; F2 fixed by correcting the record in place and moving the
vault and sibling-port citations into `pica::texelBytes`, with the `clearColour` analogy demoted
to a second witness; F3 confirmed but its recommended remediation is forbidden -
`gates_batch_integrity` refuses any batch that edits a historical sidecar (measured), so the
required "why this picture did not move" paragraph is carried forward in this batch's pair
instead; F4 confirmed and the comment now names only the mechanism that reproduces (a HEADER
edit), with the reviewer's residual VPATH-collision scenario closed by measurement - a claim
batch 122 corrected: the counts were 52 and 64, 51 basenames are shared between the lists, and
what holds is that no basename maps to two distinct paths. Full narrative:
`docs/PROGRESS_LOG.md` "Batch 121". Two
review-6 open questions are left stated and unfixed on purpose: the index-0 discard is argued
from construction rather than measured (it needs a keyed texture over a non-black clear, which
the pinned scene has no), and `expandRgba`'s per-texel loop versus the old row `memcpy` is cold
by §2 and unmeasured in both states.

**Batch 120 (iteration 6) - the R = 255 colour defect, closed.** The standing lead in this file
for many batches, now resolved. It was candidate (a) of the two named here, the texel byte order;
candidate (b), an unconfigured second Tev stage, was never involved. `pica::texelBytes` is the
new single definition of the PICA's `[A, B, G, R]` texel layout, used by `expandIndices` and by
`expandRgba` - the latter is no longer a row-wise `memcpy`, which is what painted every
flat-coloured surface with its own alpha as red. The seam's own convention is unchanged and is
now written down in `render/render.h`: `rgba` is RGBA in memory on both targets and each backend
converts for its own GPU. The index-0 discard is unaffected and had to stay that way, so it is
pinned: an all-zero texel has alpha 0 under ANY byte order, which is the whole reason the defect
hid behind transparency that kept working. See
`docs/evidence/2026-11-09-L7-texel-order-fixed.txt`. Downstream: colour is no longer a confound
for the one-sided-view geometry question, which stays open and is NOT claimed here. The R = 255
candidate set in this file is now empty.

**Batch 119 (iteration 5) — review-4 closed 4/4 (no Critical, no High): F1 and F2 fixed and
pinned by mutations, F3 closed as a record correction, F4 stated as owner-only.** **F1 (Medium,
confirmed)**: reproduced verbatim — with the honest probe's heredoc removed the script printed
`0/0 honest call(s) accepted` and then `PASS ArmCompile`, exit 0, while the same file already
refuses exactly that shape for the mutant sweep. Fixed with a manifest checked from three
directions, because each catches a mutation the others survive: a name with no file fails by
name (a deleted or renamed heredoc), a file no name lists fails by count (an unlisted heredoc
would never be compiled), and a loop measuring zero fails before the pass line (a glob renamed
inside the loop, every file present). The reviewer's `!= 0` guard alone was not taken as the
whole fix, because their own scenario includes deleting **one** of the four mutants, which
leaves three refused and a nonzero guard satisfied while a class silently disappears. **F2
(Low, confirmed)**: `1/2 honest call(s) accepted` was immediately contradicted by "No honest
call was accepted"; the sentence is now conditional, and the case was latent rather than
harmless — with one shipped probe a failing probe always leaves zero accepted, so it goes live
the moment anyone adds a second. **F3 (Low, confirmed)**: batch 118's P8 line omitted the
invocation and the guest-log path; the recoverable half was verified from the surviving lane
(`/tmp/mdk-emurun.319894`, 48 frame lines ending at frame 2580) and the invocation is not
reconstructed because it is recorded nowhere. The standing rule is now stated in the progress
log and in the standing facts below: a P8 block quotes the invocation and the harness's own
`guest log :` line verbatim. **F4 (Low, confirmed, not actionable by a batch)**: the dirty
`ralph/builder.md` is `PROTECTED`, invisible to a gate that reads committed ranges only, and
both remediations are owner-only — so this batch staged its paths explicitly instead of
`git add -A` and left it uncommitted. Review-4's open question 1 (should `mdk_3ds/Makefile` join
`PROTECTED`) is the same class and also owner-only. Full narrative: `docs/PROGRESS_LOG.md`
"Batch 119".

**Batch 118 (iteration 4) — review-3 closed 4/4 (no Critical, no High): F1–F4 all fixed, none
rebutted, and the fix to F1 made the claim true instead of shrinking it.** **F1 (Medium, confirmed)**:
batch 117 said the printf diagnostic "is a gate and not a notice" while nothing in the loop's gate
chain ran the script it had put the enforcement in — `run_gates` invokes exactly three commands and
`arm_compile_check.sh` is not one of them, and that script's own header explained why it is not wired
into `make -C tests/scripts`, so the contradiction was structural. The reviewer offered wiring it
into owner-protected `tools/check.sh` or rewording the claim; this batch took neither and instead put
`-Werror=format` in the shipped `CXXFLAGS`, so the gate the loop already runs is the thing that
refuses a mismatch, and the header now names the Makefile as the enforcing half and says outright
that the script is "a runner a batch invokes and quotes, NOT part of the loop's gate chain". The
first `-n` check reported 0 compile commands because the objects were cached — an incremental build
would have made this claim unmeasured, so it was re-verified only after `make clean`. **F2 (Low,
confirmed)**: the header justified the 23 casts by saying `%d` with an `int32_t` "wrote stack garbage
— or a wrong unit", which is false on this target and contradicted this log's own line 21487;
`-dM -E` gives `__INT32_TYPE__ long int` with `__SIZEOF_INT__ == __SIZEOF_LONG__ == 4`, so the
printed bytes were always right and the 23 were standard-level UB with no ABI-observable effect.
**F3 (Low, confirmed)**: "29 of the 132" was wrong on both numbers — re-measured **26 of 133** (flow
26, c3d 0) — and the re-measurement nearly produced a wrong 0, because GCC prints the tag as
`[-Werror=format=]` and a grep for `-Wformat` matches none of it. **F4 (Low, confirmed)**: a failing
honest probe incremented the SOURCE counter while the summary printed `1 honest call accepted`
unconditionally and then blamed a clean sweep; the probe half is now a counted glob reporting
`ok/total` with its own FAIL line, shown by mutation (`0/1`, exit 1, "the source sweep above was
clean and is NOT what failed here"). Recorded not fixed: a clean build carries **3 pre-existing
`-Wreorder` warnings** in `sound_mixer`, so batch 117's "zero warnings" was a cached-objects
artifact; and deleting the format *attribute* is still caught only by the un-wired runner, which
needs an owner edit to `tools/check.sh` to close. Measured: `PASS Main3dsFlow (20 checks, 56 arms
adjudicated red, 24 probes green)`, `PASS C3dFrame (31 checks, 49 arms adjudicated red, 2 probes
green)`, `PASS ArmCompile (133 mutant(s), 0 diagnostics, 73 shipped source(s) format-clean, 4 format
mutant(s) refused, 1/1 honest call(s) accepted, …)`, `make -C tests/scripts` -> `All cases match the
oracle.`, `bash tools/check.sh` -> `CHECK: ALL GATES PASSED` (`gates_smdh SKIP`, no built `.cia`),
`make -C mdk_3ds` builds `mdk.elf` `b1997a5dca83639acf603d518abe8e62` with 0 errors. `pi-knowledge`
`mdk-sdl` searched — its `log_3ds.h` chunk is stale (still the pre-batch-117 declaration), so the
repository was the authority; no `obsidian_*`/`cognee_*` tools are exposed in this session, recorded
as a session gap and not as a lookup that happened. `ralph/builder.md` was dirty at dispatch, is
`PROTECTED`, and is left uncommitted on purpose.

**Batch 117 (iteration 3) — review-22's open question 2, taken up: the compiler owns printf
arguments now.** No review finding was open (review-22 had no Critical, no High, and batch 116 closed
F1–F7), so this is not a remediation batch: it is the escalation the review named and batch 116
scheduled. `mdk::log3dsPrintf` had **72 call sites in three files** and the only checker in the tree
read **one** of them; the attribute made the compiler read the other seventy-one and the first build
reported **23 `-Wformat=` diagnostics at 8 call sites** — `%d` handed an `int32_t`, which on
`arm-none-eabi` is `long int` (`view.batchCount()`, `arena->triangleCount()`,
`runtime->objectCount()`, the four `object*` counters, `streamCapacityLogged`, `attempted`, and the
ten `Census` counters in `logCensus`). All are 32-bit values behind a 32-bit conversion, so the fix
is the file's own convention — a `(int)` cast at the call, chosen over `%ld` because the format
strings are what the gate adjudicates — and it is **measured** codegen-neutral: `mdk.elf` byte-for-byte
unchanged and the emulator's log lines identical. A warning nobody fails on is not a gate, and
`tools/check.sh` is protected, so `tests/scripts/arm_compile_check.sh` is where it bites: **73
shipped sources** compiled with `-Wall -Wextra -Werror=format -fsyntax-only` (all clean today, so any
future mismatch anywhere stops a gate by file and line) plus **four mutants that must be refused**
(dropped argument = review-18 F4, `%*d` with no value = review-19 F2, a pointer where `%d` asks an
int, and the `%d`/`int32_t` class the tree itself carried) and **one honest call that must compile**.
Both halves were mutated: delete the attribute and all four mutants compile and fail by name; remove
one `(int)` cast and the sweep names `main_3ds.cpp:1028:48`. What the compiler cannot see — which
quantity a figure *claims to be* — stays check 17's, arity rule included (a warning fails nothing, and
without the guard a dropped argument is an `IndexError` that kills the suite instead of a named FAIL);
the comment claiming the rule existed "because `log3dsPrintf` carries no printf format attribute" is
corrected in place. The grammar gap review-22 F1 exposed is closed rather than refused: `FIGURE_FMT`'s
flag class is `[-+ #0]*`, what `vsnprintf` matches, so `% d` is adjudicated on its merits — the
existing arm refuses `("objects: refused % d bytes", objectCorners)` as a wrong figure, and a new
probe accepts the same slot carrying the capacity's own byte figure; reverting the flag class turns
that probe red by name (`23 of 24`). Four arms and one probe anchored on the refusal line's argument
text were repointed at `(int)attempted` in the same commit. Stated boundaries: the mutant compile pass
does not gain `-Werror=format` (measured: **26 of 133** arms and probes are format defects on
purpose — corrected by review-23 F3, which printed `29`; `probe20`'s dangling argument is
review-21 F1's shape), and the Makefile is untouched because
`-Werror=format` in the shipped build is an AGENTS R10 build-setting change with its own evidence
bar, which this batch has now made cheap by measuring the tree clean for it — and batch 118 paid
that bar and put the flag in the Makefile. Measured: `PASS
Main3dsFlow (20 checks, 56 arms adjudicated red, 24 probes green)`, `PASS C3dFrame (31 checks, 49
arms adjudicated red, 2 probes green)`, `PASS ArmCompile (133 mutant(s), 0 diagnostics, 73 shipped
source(s) format-clean, 4 format mutant(s) refused, /opt/devkitpro/devkitARM/bin/arm-none-eabi-g++)`,
`make -C tests/scripts` -> `All cases match the oracle.`, `bash tools/check.sh` -> `CHECK: ALL GATES
PASSED` (`gates_smdh SKIP`, no built `.cia`), `make -C mdk_3ds` builds with `mdk.elf`
`b1997a5dca83639acf603d518abe8e62` and zero warnings. `mdk_3ds/**` changed, so P8 evidence is owed
and is in the progress log: `emurun.sh --secs 45 --min-frames 60` **exit 0**, display `:99`,
emulator `mandarine.AppImage md5 0d2adab6`, frame 2580 / 48 frame lines, `health=100`,
`objects=7 obj=570c/13d`, mono, llvmpipe. `pi-knowledge` `mdk-sdl` searched, no prior finding
predating this one; no `obsidian_*`/`cognee_*` tools are exposed in this session, recorded as a
session gap and not as a lookup that happened, with one targeted vault `grep` as the disk fallback.
`ralph/builder.md` was dirty at dispatch, is `PROTECTED`, and is left uncommitted on purpose.

**Batch 116 (Iteration 23) — review-22 closed 7/7: F1–F6 fixed with a pin each, F7 stated as a
limit; nothing rebutted.** **F1 (Medium, confirmed)**: the skip batch 115 added was keyed on the
gate's regex, not on the property — `FIGURE_FMT` does not read the space flag (the file's own stated
limit), so `("objects: refused % d bytes", objectCorners)` had zero `convs`, was skipped by EVERY
figure rule including the byte-unit backstop, and printed ` 480 bytes` for an 80 KB request
(`vsnprintf` honours the space flag, `log_3ds.cpp:79`) with a full `PASS Main3dsFlow`; reproduced
verbatim, and batch 114's gate raised `ValueError` on it while batch 113's reddened it, so the crash
fix had turned a fail-closed into an exemption. It is now `if not convs: if "%" in
fmt.replace("%%", ""): fail; break` — a percent that is neither `%%` nor a readable conversion is a
format this check cannot adjudicate, so it is refused and attributed. The reviewer's
`re.search(r"%(?!%)", fmt)` was not taken: it matches the second percent of `%%` and would refuse
every honest `%%` in the tree. One arm; reverting the branch loses exactly it (`55 of 56`). **F2
(Medium, confirmed)**: the clause batch 115's record said carried the weight — "reading filled slots
so a dangling capacity cannot vouch for the line" — was pinned by nothing; the reviewer's
argument-list mutant passed `54/54 arms, 22/22 probes` while accepting
`"objects: refused %d at frame %lu", objectCorners, (unsigned long)frame, attempted`. One arm parks
the capacity past the last conversion, and that mutant now loses exactly it (`55 of 56`). This is
review-21 F1 one round later, and the second time in three rounds that a sentence in a record had no
mutant behind it. **F3 (Low, confirmed)**: the accept half of the same rule was unpinned — a mutant
refusing every unit-less line that prints a figure kept `54/54` and `22/22` while reddening
`"objects: linearAlloc FAILED, %d refused", attempted`; one probe, and that mutant now loses exactly
it (`22 of 23`). **F4 (Low, confirmed, form corrected)**: the byte-unit rule now states its reach in
code (`if convs and BYTE_UNIT...`) and at the rule — it has not adjudicated a figure-less format
since batch 115, the prose-unit probe is green by design, and what the rule still refuses is a format
asking for figures and deriving none from the capacity (`("%s bytes", name)`); the reviewer's
`if figs and BYTE_UNIT...` is vacuous (`figs` and `not figs`) and was not used. **F5 (Low,
confirmed)**: batch 115's `51 of 53` and `52 of 53` were stale quotes; re-measured on batch 115's
committed file they are `52 of 54` and `53 of 54`, and neither mutant prints a probe tally at all
because the suite exits at the arm stage — the `probes 22/22` and `probes 21/22` in the reviewer's
evidence block are not output the shipped tool can produce. Batch 115's other two quotes reproduce
exactly. **F6 (Low, confirmed)**: `real = adjudicate(SRC)` was bare, so the crash class over the real
file was still an unnamed traceback; one `adjudicate_named(label, text, quiet)` now wraps every call,
shown by a bounded-unwrap gate over a seven-paren source printing
`FAIL main_3ds flow the shipped source: adjudicating it raised TypeError: ...` with exit 1 and zero
`Traceback` lines. **F7 (Low, policy)**: a unit-less line printing only a frame number stays red,
and the limit is now stated where the rule is; the only widening that keeps review-21 F2's shape red
needs a "is this the frame counter" predicate with arms of its own, for a shape the port does not
write, and the honest edit (print the capacity with the frame) is green. **Review-22 open question 2**
is answered with a cost: a `format (printf, 1, 2)` attribute on `mdk::log3dsPrintf` would let the
compiler retire this family's arity and grammar rules, but it lives in `mdk_3ds/src/platform/log_3ds.h`
— a shipped-instruction change owing P8 emulator evidence — so it is a named next batch, not a
side-effect of a gate batch. Measured: `PASS Main3dsFlow (20 checks, 56 arms adjudicated red, 23
probes green)`, `PASS C3dFrame (31 checks, 49 arms adjudicated red, 2 probes green)`, `PASS
ArmCompile (132 mutant(s), 0 diagnostics)`, `make -C tests/scripts` -> `All cases match the
oracle.`, `bash tools/check.sh` -> `CHECK: ALL GATES PASSED` (`gates_smdh SKIP`, no built `.cia`),
`make -C mdk_3ds` builds with `mdk.elf` `b1997a5dca83639acf603d518abe8e62`. No `mdk_3ds/**` or
`mdk_core/**` path changed, so no P8 emulator evidence is owed and none is claimed. `pi-knowledge`
`mdk-sdl` searched, no prior finding predating this one; no `obsidian_*`/`cognee_*` tools are exposed
in this session, recorded as a session gap and not as a lookup that happened. `ralph/builder.md` was
dirty at dispatch, is `PROTECTED`, and is left uncommitted on purpose.

**Batch 115 (Iteration 22) — review-21 closed 3/3: F1, F2 and F3 all fixed, none rebutted.**
**F1 (Medium, confirmed)**: a refusal line whose format names a unit word and carries NO conversion —
`("objects: linearAlloc FAILED, bytes refused", attempted)`, a deleted conversion with its argument
left behind — killed check 17 with `ValueError: min() iterable argument is empty`, exit 1, checks
18–20 unscored, no arm and no probe scored; reproduced verbatim. The review-17 R3 skip asked about
the argument list (`not vals and not convs`) when what excludes a line is that the FORMAT prints no
figure, so it is now `if not convs: continue`, with the baked-digit rule above it still refusing
`"objects: refused 81920 bytes"`. One probe pins the skip, and `adjudicate_mutant()` now reports a
raising adjudication as a named FAIL: with the skip reverted the probe fails by name and the tally
says `21 of 22 probes`, where with the wrapper also removed the suite dies on a bare traceback —
review-15 F4's symptom, now closed as a class. **F2 (Medium, confirmed)**: batch 114's rationale for
deleting the whole-argument rule was false — the rule also refused a refusal line naming NO unit word
that prints a figure which is not the refused request, and batch 114 accepted
`"objects: refused %d at frame %lu", objectCorners, (unsigned long)frame` with a full PASS while
batch 113 reddened it. Of the two remediations the reviewer offered (restore the property in the form
that still matters, or state the loss and pin the accepted shape) this batch takes the FIRST: a
refusal line whose format names no unit anywhere must print the refused capacity in one of the slots
it FILLS — conditioned on it printing a figure at all, and reading filled slots so a dangling
capacity cannot vouch for the line. It was not the prose-plus-probe route because `PROBES` is
"correct code each check must accept" and that shape is review-11 F1's defect. Every honest line
review-20 F2 was about names its unit and stays green; the route-around of printing an unneeded
corner figure now has to print the capacity. Two arms; deleting the rule loses exactly those two
(`51 of 53`). Batch 114's answer to review-20's open question 1 is withdrawn. **F3 (Low,
confirmed)**: `unwrap` was documented as a fixpoint and bounded at six passes, so
`(((((((81920)))))))` walked through the literal rule green while `cap_name` failed to read a
capacity wrapped that deep and reddened an honest line; it is now `while True` with the equality
return, terminating because every pass only removes characters. Pinned from both sides — the arm is
red shipped and green on a gate mutant with the bound restored (`52 of 53`), and the probe is green
shipped and red on that mutant. Review-21 open question 4's other half is closed too: `8192e1` IS
81920 and the old pattern matched `8192` and stopped, so `LITERAL_RE` gained an exponent
alternative and one more arm (reverting the pattern loses exactly it, `53 of 54`). **Open questions
2 and 3 are deferred with cause**: `tools/runner/gates_batch_integrity.py` not seeing an unstaged
`PROTECTED` edit, and a re-published frame being indistinguishable from a new capture, both live in
owner-protected tooling. Measured: `PASS Main3dsFlow (20 checks, 54 arms adjudicated red, 22 probes
green)`, `PASS C3dFrame (31 checks, 49 arms adjudicated red, 2 probes green)`, `PASS ArmCompile (129
mutant(s), 0 diagnostics)`, `make -C tests/scripts` -> `All cases match the oracle.`, `bash
tools/check.sh` -> `CHECK: ALL GATES PASSED` (`gates_smdh SKIP`, no built `.cia`), `make -C mdk_3ds`
builds with `mdk.elf` `b1997a5dca83639acf603d518abe8e62`. No `mdk_3ds/**` or `mdk_core/**` path
changed, so no P8 emulator evidence is owed and none is claimed. `pi-knowledge` `mdk-sdl` searched,
no prior finding predating this one; no `obsidian_*`/`cognee_*` tools are exposed in this session,
recorded as a session gap and not as a lookup that happened. `ralph/builder.md` was dirty at
dispatch, is `PROTECTED`, and is left uncommitted on purpose.

**Batch 114 (Iteration 21) — review-20 closed 4/4: F1, F2 and F4 fixed, F3 rebutted with evidence.**
**F1 (Medium, confirmed)**: neither half of batch 113's F1 fix was pinned -- deleting the whole
`CORNER_UNIT` loop, and separately deleting the restored whole-argument rule, each left
`PASS Main3dsFlow (20 checks, 45 arms, 18 probes)` byte-identical to shipped, both reproduced here.
Three arms now isolate the corner rule and one probe isolates what replaced the other clause, and
the four gate mutants that were invisible each lose exactly the arms that name them (47/50, 49/50,
47/50, 19/20 probes). **F2 (Medium, confirmed)**: the whole-argument rule batch 113 restored reddened
an honest refusal line that prints ONLY the byte figure the capacity explains -- the shape
`renderer_c3d.cpp`'s own refusal writes -- and it is deleted rather than narrowed, because the
per-slot rules already require every byte figure to BE the backend's capacity arithmetic and every
corner figure to BE the capacity, so the line-level rule could only refuse a line that disagreed
with nothing while the cheap way past it was to invent a corner figure, which is review-11 F1 bought
with the gate's own PASS. That answers review-20's open question 1: the rule is "the capacity
explains every figure the line claims", not "the capacity is an argument". **F4 (Low, confirmed)**: a
unit word now belongs to the conversion NEAREST it, so `corners %d` is a corner slot like `%d corners`
is (the before-spelling was green while `say()` claimed it), the limit's inverted parenthetical is
gone, and one fixpoint `unwrap()` plus one pattern refuses the eleven spellings of a stale figure
(`(81920)`, `((int64_t)81920)`, `(int64_t)(81920)`, `-81920`, `81920.0f` were green). **F3 (Medium,
REBUTTED)**: the quoted `PASS C3dFrame (31 checks, 48 arms adjudicated red, 2 probes green)` over the
batch-112 gate IS reachable and was reproduced verbatim with the natural pre-arm placement; the
reviewer's placement between `r->inFrame = true;` and `r->frame++;` broke an unrelated arm's anchor,
and the suite's 47/48 refusal to score a mutant it cannot build is fail-closed, not a false green.
Measured: `PASS Main3dsFlow (20 checks, 50 arms adjudicated red, 20 probes green)`, `PASS C3dFrame
(31 checks, 49 arms adjudicated red, 2 probes green)`, `PASS ArmCompile (123 mutant(s), 0
diagnostics)`, `make -C tests/scripts` -> `All cases match the oracle.`, `bash tools/check.sh` ->
`CHECK: ALL GATES PASSED` (`gates_smdh SKIP`, no built `.cia`), `make -C mdk_3ds` builds. No
`mdk_3ds/**` or `mdk_core/**` path changed, so no P8 emulator evidence is owed and none is claimed.
`ralph/builder.md` was dirty at dispatch, is `PROTECTED`, and is left uncommitted on purpose.

**Batch 113 (Iteration 20) -- review-19 closed 4/4 (1 Critical, 2 Medium, 1 Low): F1-F4 all fixed,
none rebutted.** Check 17's figure rules were re-derived from the format a third time, from two
things batch 112 stated as limits and was wrong about. (a) *Which argument*: a conversion consumes
`1 + (number of star fields)` arguments and prints the LAST of them, so a star width is neither
counted as a missing argument nor adjudicated as a byte figure (`conv_arity()`). (b) *Which slot*:
the byte rule kept its wide window and the corner rule could only read the text a format writes
AFTER its conversion -- a limit review-20 F4a later showed was wrong, and batch 114 replaced it.
(c) *Which literal*: `bare_literal()` refused `81920`, `81920u`, `0x14000`, `(int64_t)81920` and
`(unsigned long)81920`, where the old test caught the first only. On the backend side the arming is
position-checked like the frontend's has been since review-14 F1: every `= true` write to the latch
must be a depth-1 statement of a latch-guarded block. Three arms and one probe in the frontend gate,
one arm in the backend gate.

**Batch 112 (Iteration 19) — review-18 closed 6/6 (no Critical, no High): F1-F6 all fixed.**
The two latch nets are whitelists now (`LATCH_SHAPE` + `unpinned_uses()` in both gates): any
occurrence of the latch's name outside its declaration, the two reads the guard accepts and the
pinned writes reddens, so a direct-initialised reference, a brace-initialised one, a
by-reference helper call, `std::addressof` and a read-only `const bool&` are all refused -- the
last one on purpose, and both gates' failure text now says why (F6), because a name scan cannot
see a write through an alias and must not be trusted while one exists. Four new arms, two per
gate. Check 17's figure rules are re-derived from the format: conversions are matched by the
full conversion grammar (F4), a digit left in the format text after they are stripped is a
figure with no source (F2), and only a slot whose own text names a byte unit is adjudicated
against the backend's byte count, so `%d corners at frame %lu` is legal again (F3) while a stale
literal is still red wherever it sits. Record accuracy: the mutant count is quoted from the tool
and no split is quoted (F5). Measured: `PASS Main3dsFlow (20 checks, 42 arms adjudicated red, 17
probes green)`, `PASS C3dFrame (31 checks, 48 arms adjudicated red, 2 probes green)`, `PASS
ArmCompile (111 mutants, 0 diagnostics)`, `make -C tests/scripts` all cases match, `bash
tools/check.sh` ALL GATES PASSED, `make -C mdk_3ds` builds. `ralph/builder.md` was dirty at
dispatch, is `PROTECTED`, and is left uncommitted on purpose.

**Batch 111 (Iteration 18) — review-17 closed 7/7 (no Critical, no High): R1-R7 all fixed.**
Check 20's alias scan now reads the whole comment-blanked file, matching the c3d twin, and one
arm pins the scope itself (an alias declared outside the frame body) rather than only the
pattern; the c3d scan gained the reference-declaration spelling plus an arm. Check 17's figure
list is now every argument after the capacity, resolved through one level of local initialiser
(`local_inits`/`fold_locals`, beside the `const_ints`/`fold_consts` pair), so a hoisted byte
figure is compared as the arithmetic it hides; two arms, one per shape (hoisted local, stale
literal). A refusal line that names no figure at all is legal (review-10 F6's failure: a gate
that rejects a shape the port may write gets routed around) and the figure rules are conditional
on the format naming a figure. Record accuracy: two transcribed counts deleted rather than
corrected, one dangling arm name repointed at what actually pins the property, and the two
sentences describing `arm_compile_check.sh`'s prerequisites corrected in place. Open question 1
closed: the script now mirrors the Makefile's `ifeq ($(DEBUG),1)` for `DEFINES +=` instead of
dropping it, and names which flags are read versus restated. Measured: `PASS Main3dsFlow (20
checks, 38 arms adjudicated red, 15 probes green)`, `PASS C3dFrame (31 checks, 46 arms
adjudicated red, 2 probes green)`, `PASS ArmCompile (102 mutants, 0 diagnostics)`, `make -C
tests/scripts` all cases match, `bash tools/check.sh` ALL GATES PASSED, `make -C mdk_3ds` builds.
`ralph/builder.md` was dirty at dispatch, is `PROTECTED`, and is left uncommitted on purpose.

**Batch 110 (Iteration 137) — review-16 closed 6/6 (no Critical, no High).** The change that
mattered was a scope widening, not an addition: check 20's write-set scan stopped at the object
pass, so a per-frame latch clear written in the frame body that *contains* the pass compiled
clean and restored review-12 R1's ~60x/s spam with everything green — the reviewer's Medium,
reproduced from its own file before being fixed. Alongside it: the backend's `LATCH_NEG`
accepted a guard spelling it could never match, so a legal `== false` guard reddened the gate
(and the frontend's write scan had the mirror-image bug, reading a legal `== false` guard as a
write — found here, not by the reviewer); the c3d gate gained a probe mechanism, because a gate
that can only prove mutations are caught can never report that it rejects legal code; every arm
of that gate gained a binding to the check each claims, generated from the measured run; and
`arm_compile_check.sh` now names the generated header the c3d half needs instead of an include
directory, and exits 1 rather than PASSing over a skipped gate. One finding was rebutted in
part: review-16 R4 asked for `-I mdk_3ds/build` to be dropped, and `renderer_c3d.cpp:60`
includes the header generated into it — the include stayed, the *directory requirement* is what
batch 111 dropped. All of that gate's mutants compiled for the 3DS target.

**Batch 109 (Iteration 136) — review-15 closed 5/5 (no Critical, no High): F1–F5 all fixed, none
 rebutted on merit.** **F1**: checks 18 and 19 read the clear and the guard but not the *write set*,
 so a second `streamFailureLogged = false;` at the end of the refusing branch — or one written once
 per frame after the `if/else` — cleared the latch on the frame that refused and printed the refusal
 ~60×/s with `PASS Main3dsFlow (19 checks, 30 arms, 11 probes)`. Check 20 now requires every write
 to the latch inside the pass to be the arming inside a latch guard, the clear in the `else`, or the
 anchored assignment form, each at brace depth 1 of the block that makes it run when it should; two
 arms, one per mutant. Review-15's open question 1 is answered **yes, pinned**: a defensive
 per-frame reset is the defect, not a shape the port writes on purpose. **F2**: the backend check
 batch 108 added resolved ONE refusal printf and ONE clear in ONE function — review-14 F7's lesson
 applied in the frontend and not here — so a second refusal line after the guard printed every
 refusing frame and a per-frame clear in `mdk_render_begin_frame` re-armed the latch under a
 sustained refusal, both green on `PASS C3dFrame (30 checks, 41 arms)`. The net is now every
 refusal line in the function plus every line in its failing branch, and the clear is pinned twice:
 in place, and as the only clear of the latch in the file (new check
 `mesh-refusal-latch-cleared-nowhere-else`, three new arms). **F3**: batch 108 replaced batch 107's
 format-agnostic scan of the refusing branch with a format filter over the pass and recorded it as
 a *widening*; the filter is narrower on the branch, so a line inside the refusing branch that never
 spells FAILED — review-11 F1's wrong number on every refusing frame — was read by neither check.
 The net is now the **union** of both, `arm_failure_second_line_names_the_frame_count` was re-cut
 with a format that deliberately does not name the refusal so it tests the branch half instead of
 co-operating with the filter, and a probe keeps an honest non-FAILED line green. Open question 2:
 the narrowing was a defect, and the record no longer calls that change a widening. **F4**:
 `backend_mesh_atoms()` returning None was documented to redden check 17 and instead raised
 `TypeError`, killing the script before checks 18–20 and every arm; it now reports red, verified on
 the reviewer's skeleton tree. **F5**: the compile claim now has a committed generator,
 `tests/scripts/arm_compile_check.sh`, which dumps mutants from the gates' own lists and compiles
 them with flags read out of `mdk_3ds/Makefile` — and running it found **two backend arms that had
 never compiled** (`flushDraws`, a name that exists only in a comment, and a two-argument
 `shaderProgramConfigure` where libctru declares three), review-14 F3's class found by a machine
 instead of by a reviewer. `PASS Main3dsFlow (20 checks, 32 arms adjudicated red, 12 probes green)`,
 `PASS C3dFrame (31 checks, 44 arms adjudicated red)`, `PASS ArmCompile (90 mutant(s), 0
 diagnostics)`.

**Batch 108 (Iteration 135) — review-14 closed 7/7 (no Critical, no High): F1–F7 all fixed, none
 rebutted on merit.** F1/F2 (Medium) and F3–F7 (Low) each reproduced on the reviewer's own mutant
 shape before any edit. **F1**: nothing in the repository pinned `streamFailureLogged = true;` —
 deleting it left the guard always true, printing the refusal ~60x/s, with `PASS Main3dsFlow (19
 checks, 27 arms, 9 probes)`. Check 19 now requires the guarded block to arm the latch as a direct
 statement of itself; the order inside the block is deliberately NOT pinned (print-then-arm prints
 once too), and there is a probe for the other order. **F2**: check 18's accepted assignment form
 was anchored to a *position* where the property is *reachability*; a copy nested in
 `if (objectCorners == 0) { … }` satisfied it, never cleared, and silenced the second, larger
 refusal — F7 with checks 18 and 19 both green. Both accepted forms are now brace-depth-tested
 (depth 1 from the block's own `{`), which answers the reviewer's open question 1: "runs on every
 frame" IS decidable in text. **F3**: the header claimed every arm compiles; three did not, and two
 were worse than the claim — `arm_objects_ungated` spliced one character early and re-emitted the
 block's `{` (`if (true) {{`, unbalanced), and `arm_objects_before_arena` anchored on
 `text.find("if (arena) {")`, the first of three such blocks, which sits above the declaration of
 `runtime`. All three now build real mutants; all 30 arms and 11 probes compile with the Makefile's
 flags. **F6**: naming the corner stride — the refactor that makes drift impossible, which is what
 D4 was *for* — reddened check 17 and four arms; one level of local `const`/`constexpr` integer
 definitions now folds on both sides. **F7**: a refusal line in a sibling condition outside the
 first refusing branch was read by neither check; the scope is now the object pass with family
 membership decided by the line's own format string. **F4/F5**: the marker count is gone from both
 records and the directive's corrected quote is attributed to the batch that wrote it. **Open
 question 2 answered by pinning, not by deciding**: the backend's `meshAllocFailureLogged` — the
 line that actually printed 1710 times, pinned zero times while its frontend copy was pinned three
 — now has a check and four arms in `c3d_frame_test.sh`, the file whose arms can mutate
 `renderer_c3d.cpp`. `PASS Main3dsFlow (19 checks, 30 arms, 11 probes)`, `PASS C3dFrame (30
 checks, 41 arms)`.

**Batch 107 (Iteration 134) — review-13 closed 6/6 (no Critical, no High): D1–D4 fixed in the
 gate, D5/D6 fixed in the record; nothing rebutted on merit.** Review-13 found that batch 106's
 "closed 5/5" rested on gates that still could not fail at the thing they named. All four Medium
 findings reproduced here before any edit, on the reviewer's own mutant shapes, and all four
 mutants compile clean for the 3DS target with the Makefile's flags — so each is now a named arm.
 **D1**: check 17 resolved only the *first* `log3dsPrintf(` after the test and check 19 required
 only that *some* guarded block contained *a* printf, so a second, unguarded refusal line built
 from `objectCorners` passed all 19 checks, 22 arms and 6 probes — review-11 F1's 9 KB-for-an-80 KB
 disagreement plus the ~60/s spam, back on the console. Both checks now iterate over **every**
 printf in the refusing branch: 17 requires each to name the refused capacity, 19 requires each to
 sit inside a latch-guarded block. **D4**: check 17 compared the derived argument's *identifier
 set*, so `* 4` for the corner stride stayed green while the printed byte count disagreed with the
 backend's — the variable substitution was only one spelling of F1. It now reduces the argument to
 a factor set and requires it to equal the set parsed out of `renderer_c3d.cpp:1698`'s own
 `linearAlloc((size_t)cap * 5 * sizeof(float))`, with `/1024` required exactly when the line names
 KB; changing the renderer's stride to 6 reddens the check against an unmodified frontend, which is
 the drift coupling the reviewer asked for. **D2**: check 18's equivalent assignment form was
 searched for over the whole file, so the assignment placed *inside* the refusing branch — sets the
 latch, can never clear, F7 exactly — satisfied it; the form must now sit in the block containing the
 test, before the test. **D3**: check 19 substring-tested the condition, so
 `if (!streamFailureLogged || true)` stayed green while printing every frame; the condition must now
 `fullmatch` one of the two accepted negative spellings. `PASS Main3dsFlow (19 checks, 27 arms
 adjudicated red, 9 probes green)`. **D5**: the record's bold-marker count was an intermediate 652;
 the number is dropped entirely now and parity is stated as the invariant with the recount
 command, because any count published in a summary is falsified by that same summary. **D6**: the
 directive quoted `tetrisphere-godot/tools/ralph-pi.sh:120` in a simplified form under a heading
 that says the file was read directly; it is quoted verbatim now, which strengthens the point it
 was making.

**Batch 106 (Iteration 133) — review-12 closed 5/5 (no Critical, no High): R1 and R2 fixed in the
gate, R3 annotated in the directive, R4/R5 fixed; nothing rebutted on merit.** Review-12's two
Medium test findings said batch 105's "closed 8/8" rested on two gates that could not fail. Both
were right, and both were reproduced before being fixed. **R2**: check 17 asked whether the refused
capacity appeared *anywhere* in the refused-mesh printf's tail, so keeping `attempted` corners and
building the **KB** figure from `objectCorners` passed — the corner count honest, the byte count 8x
wrong, the exact disagreement review-11 F1 was raised about still on the console. It now parses the
call's arguments (string- and paren-aware: the format string contains a comma and the KB argument a
`sizeof`) and requires every derived figure to compute with the same quantity as the first. **R1**:
check 18 asked whether *some line equalled* `streamFailureLogged = false;`, which pins a line and
not the guard — widening `if (!streamFailureLogged)` to `if (true)` left the assignment in place and
the refusal printing 60×/s, and the gate printed PASS; a trailing comment on that line reddened the
suite from the other side. It is now a brace-scan (the clear must sit in the `else` **sibling** of
the `allocationFailed()` test, i.e. the branch every non-refusing frame takes) and the guard has its
own check 19 with its own isolating arm: `PASS Main3dsFlow (19 checks, 22 arms adjudicated red, 6
probes green)`. **R3**: the directive still claimed `tetrisphere-godot` and `perfect_dark` "carry the
same chains"; both siblings contradict it (`tetrisphere …/tools/ralph-pi.sh:120` builds on `luna`,
which this directive retires; `perfect_dark/tools/ralph-pi.sh:167,176-178` makes its builder and
reviewer the **same** DeepSeek family, the arrangement the file calls BLOCKED), so it is annotated
in place with those citations. **R4**: batch 105 left an unmatched bold opener in the owner-action block
(651 markers, odd → 652, even). **R5**: `object_draw.h` offered "either 0 or the OLD mesh's size" as what
`capacity()` holds after a refusal; the old mesh is released and `capacity_` zeroed *before* the
request that can be refused, so only 0 is reachable.

**Batch 105 (Iteration 132) — review-11 closed 8/8 (no Critical, no High): F1 and F7 fixed and
re-measured on the console, F2/F4/F6/F8 corrected in the record, F3 struck from the directive,
F5 deferred with cause; nothing rebutted on merit.** Review-11 F1 was that the refused-mesh log
line review-10 asked for printed **the frame's corner count** as the size of the allocation that
failed, while `ObjectDraw::ensure` rounds up to a power of two and hands THAT to the backend — so
level 3 refusing would have read `480 corners = 9 KB` against `renderer_c3d.cpp`'s own
`linearAlloc(83886080) FAILED`, one refusal and two committed numbers 8x apart, and the attempted
capacity was not even reachable by the caller. `ObjectDraw` now records the capacity before the
request that may be refused and the frontend prints it; re-running the same one-line mutant on the
console gives `linearAlloc(83886080) FAILED for a dynamic mesh of 4194304 corners` and
`objects: stream mesh of 4194304 corners = 81920 KB linear — linearAlloc FAILED` — the same request
in two units, each printed exactly once. F7: both edge triggers were latches that could never
re-arm, so a second, larger refusal after a recovery was silent; both clear now. Two new gated
checks (`PASS Main3dsFlow (18 checks, 20 arms adjudicated red, 4 probes green)`). **And the mutant
handed back a better version of batch 104's colour fact: its panel's clear colour is `(16,32,48)`,
so it is a control — 37536 painted pixels with R = 255, 58464 clear with none.** It is not "the
panel is red": every fragment the renderer paints arrives with R = 1.0 while G and B take 117–130
distinct values, and the clear path delivers R = 16 correctly. That localises the open colour
question to the fragment/vertex colour upload and away from the clear word. The one-sided view is
still open; the Linux binary's picture is unchanged.

**Batch 104 (Iteration 131) — review-10 closed 6/6 (F1 Critical and F2 High fixed, F3 fixed, F4/F5/F6
fixed; no rebuttal on merit, one half of F6 deferred with cause).** See "Iteration 131" in
`docs/PROGRESS_LOG.md` for the narrative and the verbatim gate output. Its F3 line — "the two
frontends that own a log print it" — was false and is corrected by batch 105: only the 3DS frontend
reads `allocationFailed()`.

**Batch 103 (Iteration 130) — review-9 closed 7/7 (no Critical, no High; F1-F6 fixed, F3/F7 deferred with
cause).** See "Iteration 130" in `docs/PROGRESS_LOG.md` for the narrative and the verbatim gate output.
Its instrument claim is corrected by batch 104 above: the 53515 px are real, the mechanism was not
established, and the `geom` comparison it canonised cannot discriminate a fully painted panel.

**Batch 101 (Iteration 129) — review-7 closed 5/5, none rebutted.** **F1 (High)**: batch 100
wrote "BOTH frontends here use the geometric arena under his feet" into `arena_space.h` and only
one of them did. `mdk_linux` pinned `runtime->currentArena()` when it had a runtime — a value the
unported crossing leaves at the spawn arena forever — and did **nothing** when it had none, so the
pin was the spawn arena in both branches and a door walk dropped Kurt through the room he entered,
in the frontend that produces the pinned frame. Now the same geometric re-probe the 3DS got, and the
header says what is true. Pinned by a new gate that reads **both** frontends for the shared claim:
`PASS FrontendSolidPin (10 checks, 10 arms adjudicated red)` — and its own arm discipline caught one
of this batch's arms on the first run (the unguarded-refresh arm destroyed the geometric shape, so a
different check caught it first), which is the proof the rule works. **F2 (Medium)**: the arm written
for check 6 was satisfied by check 5, so check 6 had no isolating mutation and could be stubbed to a
pass generator; every check now has one label in a `CHK` table, every arm names the check it must
redden, the comparison is exact equality, and a second arm (frozen pin added *after* the geometric
one) isolates check 6 — `PASS Main3dsFlow (11 checks, 13 arms adjudicated red)`, and the reviewer's
stub-to-`True` probe now prints `only 12 of 13 arms adjudicated red`. **F3 (Medium)**: the `option`
default was asserted on `main()`'s runtime, which `main()` had just assigned, so reverting the header
to `= 0` reddened nothing; the test now builds its own runtime, and the revert now prints
`FAIL line 538: runtime.option == 1` (the first probe of that was invalid — no rebuild — and is
recorded as such). **F4/F5 (Low)**: `Viewer.cs:874` is the `TOOSCARYFORME` gore cheat, not the resume
path (`ScriptRuntime.Snapshot.cs:142`), and the sink test's claim is scoped to the invariant it
executes rather than the frontend sequence it cannot reach. Gates: `All cases match the oracle`,
`CHECK: ALL GATES PASSED`, `make -C mdk_3ds` links with `mdk.elf` **`b0878389…`** — byte-identical to
batch 100's, which is the proof the console half was comments only. Emulator (P8, `:99`, Mandarine
r1.2.1 `0d2adab6`, frame 2580, `health=100` on all 48 rows): `scripts: runtime ready (seed
-1336855983, option 1)`, ledger `frame=2580 secs=43.214 submitted=2156`. The frame is byte-identical
to batch 100's and the sidecar says why. Narrative "Iteration 129".

**Batch 100 (Iteration 128) — review-6 closed 6/6, none rebutted.** **F1 (High)** was that the
collision pin batch 99 moved to `runtime->currentArena()` is written exactly once, on the runtime's
first tick, because `CrossedArena` (the DTI type-6 doorway crossing, `ScriptRuntime.cs:791-803`) is
not ported — so the pin was the **spawn arena for the whole level**, `move()` uses only the pinned
arena, and walking through an open door dropped Kurt through the room it opened into into a
fall-out respawn loop. Reverted to the geometric refresh and **deleted the claim that the runtime's
pin "is the oracle's own"**, which was false in this tree. The real fix is the crossing plus a
two-name `setSolid` (the oracle passes current + active second; one name cannot express it), named
below as the collision batch. The same frozen input also pins the camera's rest pitch, which the
review did not name; that half is left as the oracle's formula on a frozen input rather than
swapped for an unmeasured stand-in, and is named in the shipped comment. **F2 (Medium)**: `Option`
was 0 under a comment calling it "the oracle's Option"; the oracle's field initialises to 1
(`ScriptRuntime.cs:214`) from a gore setting that also defaults on. Now 1 everywhere, and the
golden suite is green with it — the evidence that no golden distinguishes the two. Pinned by a new
opcode-232 test (equality, not truthiness, both ways). **F3 (Medium)**: nothing executes a respawn,
so the sink re-attach was pinned only by a source read; the invariant it stands on is now executed
on the host — a rebuilt runtime's mixer device is silent until the sink is re-attached. **F4/F5**:
the two headers that described the opposite of the tree are corrected, and the mixer-ownership
comment now says the port owns the runtime's mixer and the C# shares the Viewer's. **F6**: the
runtime constructor's `applyDisabledTriangles()` now runs on the console for the first time, so
level 8's `GUNT_2` invisible wall became passable — recorded, not measured. Gates: `All cases match
the oracle`, `CHECK: ALL GATES PASSED`, `make -C mdk_3ds` links (`mdk.elf` **`b0878389…`** — the
identity field, per review-5; the `.3dsx` is not reproducible). Emulator (P8, `:99`, Mandarine
r1.2.1 `0d2adab6`, exit 0, frame 2580): `scripts: runtime ready (seed 1601142235, option 1)`,
`objects=7`, `health=100` on all 48 rows, ledger `frame=2580 secs=43.213 submitted=2156`. Still
unobserved on the console: a script sound and a respawn. The frame is byte-identical to batch 99's
and the sidecar says why. Narrative "Iteration 128".

**Batch 99 (Iteration 127) — priority item 6d landed: the console runs the scripts.** The C#'s shape
is now the console's: one `ScriptRuntime` per level over the CMI, the sprites archive and the bank,
built in `spawnPlayer` so a respawn restarts its objects, tick count and RNG (`Viewer.cs:232`); the
step order `player.update` → `runtime->update(step)` → `follow.update` (`Viewer.cs:505-535`); the
pitch from `camera_track` then the scripts' arena (`Viewer.cs:746-747`); the solid-arena pin from
`currentArena()`, **guarded on non-emptiness** because `setSolid("")` clears the pin and the runtime
has no current arena before its first tick. The mixer is the C#'s one mixer: `ScriptRuntime` owns it,
opcode 89 writes into it, and the frontend mixes through a pointer that follows it, with the sink
re-attached on every rebuild. **What it does not do: draw the objects.** They are simulated and
unseen — level 7's seven of them — and the object pass is the next batch because it is a picture
change and a picture change carries its own frame. Emulator (P8, `:99`, Mandarine r1.2.1
`0d2adab6`, exit 0, frame 3480): `scripts: runtime ready (seed 2037550638, option 0)`,
`objects=7`, `health=100` on all 63 rows, ledger `frame=660 secs=11.123 submitted=551`. **No script
sound fired in the measured scene** (`plays=1`, level 7's idle scripts play nothing at the spawn), so
the audible half rests on the wiring being pinned, not on a heard sound. Two gates moved:
`ndsp_sink_test.sh` no longer pins the *spelling* of the audio feeds (33 arms still red, 9 probes
still green), and `main_3ds_flow_test.sh` gained six checks and seven arms — and its own new check
read the module's `SRC` instead of `adjudicate`'s argument, which its own arm caught on the first
run: a check that adjudicates the unmutated file is a pass generator, eleventh time. `mdk.elf`
`c8295e96…` → `4232fd94…`, `nm | grep -ci ScriptRuntime` 0 → 107. Narrative "Iteration 127".

**Review-4 (run-20261108-195620) closed by batch 98: four findings, no Critical, no High — three
fixed, one confirmed and gate-blocked, none rebutted on merit** (Iteration 126). **F1 (Medium)** was
that the delta loop's `if (p >= fileSize) return false;` — the only thing between a frame whose
opcode declares data past EOF and an out-of-bounds read — had no case that could reach it, and the
reason it could not be reached is the finding: `end` is the opcode's *declared* data end, so with an
honest tail the loop's own `p < end` stops it on the last real byte and the file bound is
unreachable. The pin therefore has to **truncate the file** while the headers keep declaring the
original lengths, and has to over-declare `size` so `written < count` cannot be what stops it
either; three shapes (mono cut on the seed, mono cut one delta short, stereo cut on its seeds) now
assert `Broken` and zero samples. **F2 (Low)** was the `p < end` half: `Mve.cs:228` is strictly
less-than, and widening it to `p <= end` consumed the **next opcode's length byte** as a delta and
passed the whole suite, because every other case stops on `written < count` instead. **F4 (Low)**
was the sanitizer gate's own PASS line still reading "probe ran both 8-bit decodes and both 16-bit
paths" while `main()` had also run the entire MVE suite since batch 97 — the one line that says
what was measured under-described it, which is the exact misconception that script's header exists
to prevent; it now names the sweeps, the MVE decodes and both instruments. **F3 (Low) is confirmed
and cannot be fixed by a batch**: the batch-97 sidecar cites
`2026-11-09-b95-ub-audio-clock.png` where the file is `2026-11-08-b95-ub-audio-clock.png`, and
`gates_batch_integrity.py` — itself `PROTECTED` — fails any batch that modifies a historical frame
`.txt` ("history is immutable"), so the typo is permanent by construction, exactly like batch 89's
sidecar gap (review-9 F3); the correct path is named in this batch's sidecar and the convention is
carried forward. **And the finding the review did not make, found by this batch's own battery:**
deleting the **seed loop's** `p + 2 > fileSize` or `decodePcm`'s `p + step > fileSize` was **NOT
CAUGHT** — no case reached either, and neither sanitizer sees them because the bytes are still
inside the movie vector's allocation. Both are pinned now (a file cut mid-seed in either codec and
a file cut mid-16-bit-pair), which is why the battery is recorded in the progress log rather than
asserted.

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

Last updated: 2026-11-09 (**batch 104 — review-10 closed 6/6, and the two findings that mattered were both
about the measuring instrument rather than the game. **F1 (Critical)**: `panel_geom.py geom` was bound as
the picture claim for every console A/B on a scene where it cannot move — the object pass paints all 96000
panel pixels, so the painted/clear map is a constant and `geom: 0 px` was arithmetic; it now refuses a
degenerate side with exit 3, prints the clear fraction, and `tests/scripts/panel_geom_test.sh` gates it
(10 checks, 7 arms adjudicated red, 2 green probes). **F2 (High)**: batch 103's "a control that reports 0 px
was comparing a capture with itself" is retracted — batch 102's control was two independent runs in two
lanes (`docs/evidence/2026-11-09-L7-object-pass-parent.txt:29` names the second capture) and its panels
really were identical; what the pair of numbers actually says is that a noise floor is only valid for the
scene it was measured on, and batch 103 quoted the arena-only floor at an arena+objects A/B. **F3**:
`ObjectDraw` now records a refused `linearAlloc` and `main_3ds.cpp` prints it once, so a level whose stream
mesh cannot be allocated says so instead of drawing nothing silently, and `renderer_c3d.cpp` logs its own
refusal like every other allocation it fails. **F4**: `10520 px` became `10464`, which is what the two
crops subtract to. **F5**: `panel_geom.py` imports `PANEL_SIZE` from `panel_crop.py` instead of re-spelling
it. **F6**: check 16 counts call sites in a comment-blanked copy of the source, with three green probes that
redden if the blanking is dropped or made string-blind (16 checks, 18 arms, 3 probes). Also in this batch:
the operator's harness set is committed (single qwen builder route, waited out instead of failed over,
timeouts raised for shared-KV contention, the watchdog taught to heal a failed unit, and
`tools/reclaim_qwen_slots.sh` for a leaked admission slot) — except `ralph/builder.md`, which the integrity
gate forbids without owner approval and which therefore stays unstaged with that reason recorded. New
measured fact for NEXT: **every painted pixel of the console panel has R = 255**, against 9758/76800 in the
Linux frame of the same core. Superseded tail of the batch-103 line, whose instrument claim batch 104
corrects above: batch 103 — the strongest standing candidate for the one-sided view is closed by measurement, and the panel instrument's real noise floor is measured. The vertex shader computed the MVP with `mov r0, inpos` on a THREE-component position attribute, while every devkitARM citro3d example forces `w` to 1.0; had that component been 0 the MVP's whole translation column would have been multiplied by zero. It is not: forcing it makes the console's panel crop BYTE-IDENTICAL (pixel sha `75b22ac5…`, `docs/evidence/2026-11-09-L7-shader-w-{parent,current}.txt`), so the attribute loader already supplies `w = 1` and the shader keeps the line as an explicit statement rather than a dependency. The same run measured the instrument: two runs of ONE ELF differ by **53515 px of 96000 in full colour and 0 px in the painted/clear map** (`docs/evidence/2026-11-09-panel-noise-{a,b}.txt`), so the "0 px noise floor" recent sidecars quote does not reproduce and a full-RGB panel diff is not a control — `tools/panel_geom.py` is the canonical comparison from here. Review-9 (no Critical, no High) closed: F2's duplicated-draw mutant stayed green on all 15 checks and now reddens check 16 with its own arm (16 checks / 18 arms); F4's `11.4%` was a KB figure written as a percentage (570/4096 = **13.9%**, and 570×5×4 B = 11400 B = 11.4 **KB** of the 80 KB mesh); F5's three oracle citations are corrected to `Viewer.cs:705-707` / `702-708`; F6's `stream mesh 0 corners = 0 KB` line no longer prints before there is a mesh. F1 is the NEXT section's restatement. F3 (no `Preload` caller, unbounded object-texture residency) and F7 (mono-only evidence) are deferred with cause. `mdk.elf` `4d27d6f8…` → `9714104e…`. Emulator on `:99` (r1.2.1 `0d2adab6`, exit 0) reaches frame 2880 with `health=100` and `objects=7 obj=570c/13d`. Superseded tail of the batch-102 line: batch 102 — priority item (a) landed: the console draws the scripts' objects. `main_3ds.cpp` gains the C#'s `DrawObjects` between `view.draw()` and `kurtSprite.draw()` (Viewer.cs:702-709), at `runtime->tick_blend(pending)`, skipping the invisible and the removed, over an `objectLook` cache keyed by arena name — and the console's steady-state stream is `570 corners / 13 draws`, which is `L7_t1000_draw.txt`'s `STREAM vertices=570` and 13 queued draws exactly. The linear answer CONTINUATION asked for: the stream mesh is made ONCE at 4096 corners = 80 KB linear and NEVER grows (level 7 uses 570 of 4096), against the C#'s 5.2 MiB `Renderer.StreamVertices` and the ~1 MB linear headroom. It also fixed a defect the pass would have shipped: `ScriptRuntime` owns `modelCache_` BY VALUE (script_runtime.h:580), so `runtime.reset()` on a respawn frees every `Model*` that `ObjectView::layouts_` and the pose caches key on, and a recycled address inherits the previous model's geometry — both frontends now call `forget_poses()` + `objectView.clear()` in `spawnPlayer`, which is why the object-pass declarations sit ABOVE it. `mdk.elf` `4232fd94…` → `4d27d6f8…`, `nm | grep -ci` ObjectView/ObjectDraw 0 → 2 each. Emulator on `:99` (r1.2.1, exit 0) reaches frame 2880, `health=100` on all 53 rows, and the panel crop moves 58520 px of 96000 against a 0 px noise floor — see `docs/evidence/2026-11-09-L7-object-pass-{parent,current}.txt`. What it did NOT produce: the predicted script sound. `plays` stays at Kurt's own 1 and `voices=0`, because level 7's seven objects never fire one in 2880 frames; that measurement is still open. Superseded tail of the batch-99 line: batch 99 — priority item 6d, the 3DS frontend runs the scripts (one `ScriptRuntime` per level, built in `spawnPlayer`, ticked between Kurt and the camera, the frame mixing the mixer the runtime owns), its objects simulated and NOT drawn until this batch** `mdk.elf` `c8295e96…` → `4232fd94…`, `nm | grep -ci ScriptRuntime` 0 → 107; the emulator on `:99` (r1.2.1, exit 0) reaches frame 3480 with `objects=7` and `health=100` on all 63 rows, and no script sound fired. Superseded tail of the batch-97 line: batch 97 — review-3 of run-20261108-195620 closed, 2 fixed and 3
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
**Owner action (review-4 of run-20261106-062909, F3) — STATUS AFTER BATCH 105:** batch 105 struck the exception sentence from `docs/OWNER_DIRECTIVE_2026-11-05.md`, because it contradicts `SPEC-COMPLETION.md` harness rule 4 (PROTECTED) and `ralph-pi.sh`'s own "THERE IS NO INDEPENDENCE EXCEPTION HERE", both of which the harness already enforces and `test_review_state.sh` already pins. **If the owner does intend a same-family exception, batch 105's edit is the thing to revert**, and `ralph-pi.sh`, `ralph/validator.md` and `tools/runner/test_review_state.sh` must change with it. Adding `docs/OWNER_DIRECTIVE_*.md` to `gates_batch_integrity.PROTECTED` is also an owner call (review-11 F3): a directive is currently editable by a batch with no gate noticing.
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

**Owner action the loop cannot take (batch 104).** The operator's harness set has been
committed at last (single qwen builder route that waits its route out instead of failing
over, raised timeouts for shared-KV contention, a watchdog that heals a failed unit,
`tools/reclaim_qwen_slots.sh`), because four batches of leaving it unstaged left the running
loop and the committed contract diverging — review-10 open question 2. Two things remain
owner-only. (1) `ralph/builder.md` is in `gates_batch_integrity.py`'s `PROTECTED` set, so the
working-tree edit to it **cannot** be committed without `MDK_OWNER_APPROVED`; it is still
unstaged, and the loop will keep reporting a dirty tree until an owner either commits it or
reverts it. (2) `tools/runner/test_review_state.sh` is reachable from no gate — wiring it into
`tools/check.sh` is the fix and `tools/check.sh` is owner-protected too. Its exec bit also
arrived dropped (755 → 644) from outside this uid and cannot be restored by `piagent`.

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
   `mdk_render_canvas_quad` is a no-op on this backend (`renderer_c3d.cpp:2345`), so a HUD call would draw
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
6d. **DONE (99) — the 3DS frontend runs the scripts.** `main_3ds.cpp` builds a `ScriptRuntime` per
   level (CMI + bank + sprites archive, all three required, or the runtime is not built and the log
   says why), rebuilds it on respawn, ticks it between Kurt and the camera, takes the pitch and the
   solid-arena pin from it, and mixes the mixer it owns — so opcode 89's `play_sound` now reaches the
   ndsp sink. **Its objects are simulated and NOT drawn** (see NEXT): level 7's seven of them tick
   unseen, and the object pass is the next batch. No script sound has been *observed* on the console
   yet (`plays=1` in the measured level-7 idle run). Narrative "Iteration 127".
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

## NEXT (batch 143)
Batch 142 closed the ledger row's acceptance command (`emurun.sh --expect hud` → rc 0) and **not the thing the
row is named for**: the HUD still paints nothing. In priority order:
**(1) find why a Citro3D canvas draw paints no pixels.** The seam is correct on every CPU-side measurement
(4 quads, 2 records, the matrix the census reads is the matrix the GPU is handed, the panel lands inside the
clip square) and eight state hypotheses are already closed by running — read `docs/OPEN_ITEMS.md` "The 3DS
canvas draws no pixels" before spending a probe, because six of those eight look like obvious first guesses.
The next probe has to be **observable**, which batch 142's head-of-frame probe was not: draw one known-colour
probe quad (pure magenta, alpha 1, the whole canvas) as the canvas's only quad, capture, and diff against a
run with `hudReady` false; then repeat with the Overlay bucket sorted FIRST in the queue. A diff that moves on
neither run says the fault is not positional and is in the draw itself, and that is worth two runs. The old
sibling symptom is the clue: the diagnostic triangle has never painted either, and it shares only two things
with the canvas — a per-draw matrix write and being last in the frame — both already tested and rejected.
**(2) review-8 F2 and F3, the two cheap pins the teleport row owes**: nothing executed pins that the *unnamed*
`teleport_player` arm leaves `previousKurtPosition_`/`teleported_` alone (hoisting them out of the `if` leaves
575 green), and nothing pins opcode 173's *unconditional* yield (a conditional one also leaves 575 green).
Both are test-writing in `tests/scripts/vm_tests.cpp`, both were confirmed by the reviewer's own mutants, and
three records currently claim F3 is pinned when it is not. **(3) the two `ArenaSpace`s** (review-8 F1): the C#
hands ONE space to Kurt and the runtime (Viewer.cs:194, :207); the port has two, so `enterArena` reaches the
arena RULE and not the collision, which is what `TeleportMakesUnreachableArenaSolid` is actually about. It
touches the solid pin, `move()`, the sight probes and both frontends — its own batch. **(4)** everything the
batch-142 plan below carried and batch 142 did not reach: the crossing capture, `ShutDoors`/`Behaviors.Shut`,
the hatch pin, `FollowArenas`'s golden, `obj.Visible`. **Owner-visible consequence of this batch, stated
plainly:** the harness closes a ledger row by running its acceptance command, so `3ds.hud-canvas` now closes
on a green `--expect hud` while the HUD draws no pixels. The gate measures what it can measure (queued and
emitted) and says so in its own comment, the sidecar and the open item; whether the row should be re-scoped to
a pixel test is an owner decision, not a batch's. **Owner-only and still owed:** review-5 **F3**, review-6 OQ1
/ review-7 OQ3 (`ralph/builder.md` modified-unstaged since 2026-11-05, invisible to `gates_batch_integrity`),
review-7 OQ1 (a 0-byte `review-N.md` reads as "no findings") and OQ2, review-1 **F4** / review-3 **F3**
(`SCRIPTS_REQUIRED_CASES` is not in a `PROTECTED` file), whether the gate suite should ever run all 587 golden
cases, and the missing Obsidian/Cognee MCP servers in the builder session (AGENTS P9: a disk-only lookup is a
degraded lookup; batch 142 had neither server). **Still open and not touched:** every `hw.*` row (no Old-3DS
hardware in hand), the Phase 2 RE sweep, and the two standing owner blocks (`spec.voice-pool`,
`re.phase-2-sweep`). Old-3DS performance readiness remains unmeasured and stays open.

## NEXT (batch 142) — SUPERSEDED
*Batch 142 was dispatched on the ledger row `3ds.hud-canvas`, not on this plan; the ledger picks the row and
the batch may not. Kept verbatim because the items below it are still open.*
**This was attempt 8 of 8 on `3ds.collision-doors`, so the loop stops for an operator after this batch.**
What is left on the row, in priority order: **(1) a crossing capture, and with it a mid-tick door and a
trigger box.** Unchanged from batch 140 and still the top item: the 3DS rest pitch, the drawn-list pin
across a doorway, the one-tick lag, the trigger boxes, the door pull and now the teleport all read a
`currentArena()` that moves, and no capture has ever shown any of them. An `emurun.sh` run that walks level 4
through connection 1012 (or level 7 through 1001 and down the `CDANT_1` tunnel, or across the `DANT_10`
pair at x = 509 and 511) on `:99` would close review-1 OQ3, review-2 OQ3, review-3 OQ3, review-4's mid-tick
gap, review-6 OQ2, batch 139's "never seen on the 3DS target" and this batch's "neither opcode 112 nor 173
has been seen" in one go. It needs a scripted walk in the harness, so it is not free. **(2) the two
`ArenaSpace`s.** The C# has ONE space shared by Kurt and the scripts (Viewer.cs:194, :207); the port has the
frontend's (main.cpp:413, main_3ds.cpp:535) and the runtime's own (its constructor). `enterArena` therefore
makes a teleport-only arena answer the arena RULE and not yet the collision, which is the one thing the
C#'s `TeleportMakesUnreachableArenaSolid` is actually about. Unifying them touches the solid pin, `move()`,
the sight probes and both frontends, so it is its own batch with its own blast radius, and it is the first
candidate here precisely because batch 141 found it rather than worked around it. **(3) `ShutDoors` and
`Behaviors.Shut`** (`ScriptRuntime.cs:486-492`, `ObjectBehaviors.cs:150-165`): the oracle's last
`DoorArenaTests` case, `ShutDoorsLeavesNoneOpen`, needs the teleport that only now exists, so it is
reachable for the first time and it is a small slice (the guard on `DoorClosed|DoorStaysOpen`, the shut
animation run to `MaxShutTicks`, the state bits). **(4) the hatch pin.** DTI dirs 6/7 are every doorway on
level 5 and half of 3/6/8; the arms are ported and demonstrated on shipped geometry but no oracle assertion
covers them, and the dotnet probe that would produce one is gone (`/tmp/kilo/dotnet10` is absent and
`/tmp/kilo` is not writable by this uid — an owner/environment item). **(5) `FollowArenas`'s golden**,
which needs nothing new. **(6)** the rest of `UpdateArenas` — the loaded-arena bookkeeping, the loop
sounds and `obj.Visible` — still lives behind `updateDrawnArenas()`, and until `obj.Visible` lands the
drawn list has no consumer and the "doorway showed the sky" artefact cannot manifest here at all (review-6
F4); the same is now true of `LevelView.Enter`, the `ArenaEntered` subscriber that would make a
teleported-to arena DRAWABLE. **(7) the snowboard branch of `solidArenas()`** is written and unreachable:
`ridesBoard_` has no writer until the ride slice. **(8)** the one arm no shipped data reaches: a type-3
(LOAD) record naming no arena (`:731-734`), unreachable because every type-3 id in all six DTI goldens is
0..9 against 19 arenas per level (batch 140's M4, re-affirmed by review-7's independent recount).
**Owner-only and still owed:** review-5 **F3** (the `gates_batch_integrity` revision check), review-6 OQ1 /
review-7 OQ3 (that gate cannot see a worktree-only protected edit, and `ralph/builder.md` has been
modified-unstaged since 2026-11-05, so the loop is running under instructions that exist in no commit),
review-7 OQ1 (a 0-byte `review-N.md` at dispatch is read as "no findings" — `ralph-pi.sh:864` selects
with `ls -t | head -1` and no `-s` test) and OQ2 (the risk line and the dispatch disagreed for batch 140),
review-1 **F4** / review-3 **F3** (`SCRIPTS_REQUIRED_CASES` lives in a non-`PROTECTED` file), the
`3ds.hud-canvas` acceptance cell (`emurun.sh --expect hud` has no such flag), a decision on whether the gate
suite should ever run all 575 golden cases (measured 7m27s warm, so today `gates_core_parity` runs only the
crossing target), and the missing Obsidian/Cognee MCP servers in the builder session (AGENTS P9: a
disk-only lookup is a degraded lookup, and batch 141 had neither server). **Still open and not touched:**
every `hw.*` row (no Old-3DS hardware in hand, no `mdk_3ds/tools/hw_cycle.sh`), the Phase 2 RE sweep, and
the two standing owner blocks (`spec.voice-pool`, `re.phase-2-sweep`). Old-3DS performance readiness remains
unmeasured and stays open.

## NEXT (batch 140)
**Batch 139's item (2) is closed — both remaining trigger arms are executed — so what is left on
`3ds.collision-doors` is evidence the harness cannot produce yet, not code.** In priority order:
**(1) a crossing capture, and with it a mid-tick door and a trigger box.** The 3DS rest pitch, the
drawn-list pin across a doorway, the one-tick lag and the trigger boxes and door pull all read a
`currentArena()` that moves, and no capture has ever shown any of them: an `emurun.sh` run that walks
level 4 through connection 1012 (or level 7 through 1001 and down the `CDANT_1` tunnel into the
y = 569..592 boxes, or across the `DANT_10` pair at x = 509 and 511) on `:99` would close review-1
OQ3, review-2 OQ3, review-3 OQ3, review-4's mid-tick gap, review-6's OQ2 and batch 139's "never seen on
the 3DS target" in one go. It needs a scripted walk in the harness, so it is not free, and it is the
only thing that retires the "the emulator idles at a spawn" caveat for good. **(2) `PullDoors` through
real geometry**: the pin uses built doors because on foot `updateDoor` turns the real door first; a
scripted walk or `TeleportKurt` would let the oracle's own `LoadedArenaPullsItsDoors` be reproduced
instead of re-stated. **(3) the hatch pin.** DTI dirs 6/7 are every doorway on level 5 and half of
3/6/8; the arms are ported and demonstrated on shipped geometry but no oracle assertion covers them,
and the dotnet probe that would produce one is gone (`/tmp/kilo/dotnet10` is absent and `/tmp/kilo` is
not writable by this uid — an owner/environment item). **(4) `FollowArenas`'s golden**, which needs
nothing new now that `PreloadArena` exists. **(5) `TeleportKurt`**, the only other way the oracle moves
`currentArena_` and the only way into an arena no doorway leads to; it is also the only reader of the
resident arena (`IsCorridorLoaded`, `:522-531`), so until it lands `residentArena_` stays
written-and-unread, and it is what would let the crossing goldens' first-tick lines become oracle
assertions. It brings `TeleportSecond` with it, and `TeleportSecond` owes `LoadArena(neighbour.Name)`
(`:516`) — naming the neighbour without the load leaves an inactive, unloaded second with its doors still
parked in the room it came from (review-6 F3). **(6)** the rest of `UpdateArenas` — the loaded-arena
bookkeeping, the loop sounds and `obj.Visible` — still lives behind `updateDrawnArenas()`, and until
`obj.Visible` lands the drawn list has no consumer and the "doorway showed the sky" artefact cannot
manifest here at all (review-6 F4). **(7) the snowboard branch of `solidArenas()`** is written and
unreachable: `ridesBoard_` has no writer until the ride slice, so that half is compile-verified only.
**(8)** the one arm no shipped data reaches: a type-3 (LOAD) record naming no arena (`:731-734`),
unreachable because every type-3 id in all six DTI goldens is 0..9 against 19 arenas per level (batch
140's M4). **Owner-only and still owed:** review-5 **F3** (the `gates_batch_integrity` revision check)
and review-6 OQ1 (that gate cannot see a worktree-only protected edit, and `ralph/builder.md` has been
modified-unstaged since 2026-11-05, so the loop is running under instructions that exist in no commit),
review-1 **F4** / review-3 **F3** (`SCRIPTS_REQUIRED_CASES` lives in a non-`PROTECTED` file), review-4
**OQ2** (a 0-byte `review-N.md` at dispatch is read as "no findings"), the `3ds.hud-canvas` acceptance
cell (`emurun.sh --expect hud` has no such flag), a decision on whether the gate suite should ever run
all 572 golden cases (measured 7m27s warm, so today `gates_core_parity` runs only the crossing target),
and the missing Obsidian/Cognee MCP servers in the builder session (AGENTS P9: a disk-only lookup is a
degraded lookup, and this iteration had neither). **Still open and not touched:** every `hw.*` row (no
Old-3DS hardware in hand, no `mdk_3ds/tools/hw_cycle.sh`), the Phase 2 RE sweep, and the two standing
owner blocks (`spec.voice-pool`, `re.phase-2-sweep`). Old-3DS performance readiness remains unmeasured
and stays open.

## NEXT (batch 139)
**The row's writers around the snapshot point are all ported now, so what is left on
`3ds.collision-doors` is evidence the harness cannot produce yet plus the two arms no shipped box on
level 7 exercises.** In priority order: **(1) a crossing capture, and with it a mid-tick door and a
trigger box.** The 3DS rest pitch, the drawn-list pin across a doorway, the one-tick lag and now the
trigger boxes and the door pull all read a `currentArena()` that moves, and no capture has ever shown
any of them: an `emurun.sh` run that walks level 4 through connection 1012 (or level 7 through 1001
and down the `CDANT_1` tunnel into the y = 569..592 boxes) on `:99` would close review-1 OQ3, review-2
OQ3, review-3 OQ3, review-4's mid-tick gap and this batch's "never seen on the 3DS target" in one go.
It needs a scripted walk in the harness, so it is not free. **(2) the two unexercised trigger arms.**
`CDANT_1`'s pair are both type 3, so `TriggerShow → ShowArena(arena)` (`:727-730`) and `Load` with no
arena → `ShowArena("")` (an id of -1 or past the arena list) are compile-verified only; the next level
whose reachable arena carries a type-1 or an id = -1 box, walked, closes them — and level 3's
`HMO_*` arenas ship type-3 boxes with ids 0..9 (`golden/L3_dti.txt:51-52,59-90`), so the search is for
a type-1, not for a level. **(3) `PullDoors` through real geometry**: the pin uses built doors because
on foot `updateDoor` turns the real door first; a scripted walk or `TeleportKurt` would let the
oracle's own `LoadedArenaPullsItsDoors` be reproduced instead of re-stated. **(4) the hatch pin.** DTI
dirs 6/7 are every doorway on level 5 and half of 3/6/8; the arms are ported and demonstrated on
shipped geometry but no oracle assertion covers them, and the dotnet probe that would produce one is
gone (`/tmp/kilo/dotnet10` is absent and `/tmp/kilo` is not writable by this uid — an
owner/environment item). **(5) `FollowArenas`'s golden**, which needs nothing new now that
`PreloadArena` exists. **(6) `TeleportKurt`**, the only other way the oracle moves `currentArena_` and
the only way into an arena no doorway leads to; it is also the only reader of the resident arena
(`IsCorridorLoaded`, `:522-531`), so until it lands `residentArena_` stays written-and-unread, and it
is what would let the crossing goldens' first-tick lines become oracle assertions. **(7)** the rest of
`UpdateArenas` — the loaded-arena bookkeeping, the loop sounds and `obj.Visible` — still lives behind
`updateDrawnArenas()`, and **(8) the snowboard branch of `solidArenas()`** is written and unreachable:
`ridesBoard_` has no writer until the ride slice, so that half is compile-verified only.
**Owner-only and still owed:** review-5 **F3** (the `gates_batch_integrity` revision check), review-1
**F4** / review-3 **F3** (`ralph/builder.md` is `PROTECTED` and modified-unstaged, and
`SCRIPTS_REQUIRED_CASES` lives in a non-`PROTECTED` file), review-4 **OQ2** (a 0-byte `review-N.md` at
dispatch is read as "no findings"), the `3ds.hud-canvas` acceptance cell (`emurun.sh --expect hud` has
no such flag), a decision on whether the gate suite should ever run all 571 golden cases (measured 7m27s
warm, so today `gates_core_parity` runs only the crossing target), and the missing Obsidian/Cognee MCP
servers in the builder session (AGENTS P9: a disk-only lookup is a degraded lookup, and this
iteration had neither). **Still open and not touched:** every `hw.*` row (no Old-3DS hardware in hand,
no `mdk_3ds/tools/hw_cycle.sh`), the Phase 2 RE sweep, and the two standing owner blocks
(`spec.voice-pool`, `re.phase-2-sweep`). Old-3DS performance readiness remains unmeasured and stays
open.

## NEXT (batch 138)
**The row's acceptance command passes and the pin is now the oracle's snapshot, so the only thing left
on `3ds.collision-doors` is evidence the harness cannot produce yet, plus the two writers that sit on
either side of the snapshot point.** In priority order: **(1) a crossing capture, and with it a
mid-tick door.** The 3DS rest pitch, the drawn-list pin across a doorway and this batch's one-tick lag
all read a `currentArena()` that moves, and no capture has ever shown any of them: an `emurun.sh` run
that walks level 4 through connection 1012 (or level 7 through 1001) on `:99` would close review-1
OQ3, review-2 OQ3, review-3 OQ3 and review-4's mid-tick gap at once, because the door that opens or
closes on the far side of a doorway is the only writer that can land after the snapshot. It needs a
scripted walk in the harness, so it is not free. Until then the lag is pinned by calling the door's own
callee between ticks (`vm_tests`), which is the property, not the path. **(2) `CheckTriggers`
(`ScriptRuntime.cs:707`)** — the last writer ABOVE the snapshot point, unported, so a trigger box that
shows or preloads an arena still does nothing. Its position is now named in the code (immediately above
`updateDrawnArenas()`, `script_runtime.cpp:584`); putting it anywhere else lands its `ShowArena` a tick
late, exactly the way the door's does today. **(3) `LoadArena` + `PullDoors`**
(`ScriptRuntime.cs:615-643`): an arena being loaded takes the doors leading into it out of its
neighbours and turns them around (`ObjectBehaviors.MoveDoor`, `_residentArena`), and until it lands a
door can stay behind in the neighbour after Kurt crosses. It is what `DoorArenaTests` actually asserts.
**(4) the hatch pin.** DTI dirs 6/7 are every doorway on level 5 and half of 3/6/8; the arms are
ported and demonstrated on shipped geometry but no oracle assertion covers them, and the dotnet probe
that would produce one is gone (`/tmp/kilo/dotnet10` is absent and `/tmp/kilo` is not writable by this
uid — an owner/environment item). **(5) `FollowArenas`'s golden**, which needs `PreloadArena`
(`ScriptRuntime.cs:602-614`). **(6) `TeleportKurt`**, the only other way the oracle moves
`currentArena_` and the only way into an arena no doorway leads to; it is also what would let the
crossing goldens' first-tick lines become oracle assertions instead of two rules that agree. **(7)**
the rest of `UpdateArenas` — the loaded-arena bookkeeping, the loop sounds and `obj.Visible` — now has
a home (`updateDrawnArenas()`), and **(8) the snowboard branch of `solidArenas()`** is written and
unreachable: `ridesBoard_` has no writer until the ride slice, so that half is compile-verified only.
**Owner-only and still owed:** review-1 **F4** / review-3 **F3** (`ralph/builder.md` is `PROTECTED` and
modified-unstaged, and `SCRIPTS_REQUIRED_CASES` lives in a non-`PROTECTED` file), review-4 **OQ2** (a
0-byte `review-N.md` at dispatch is read as "no findings"), the `3ds.hud-canvas` acceptance cell
(`emurun.sh --expect hud` has no such flag), and a decision on whether the gate suite should ever run
all 570 golden cases (measured 7m27s warm, so today `gates_core_parity` runs only the crossing
target). **Still open and not touched:** every `hw.*` row (no Old-3DS hardware in hand, no
`mdk_3ds/tools/hw_cycle.sh`), the Phase 2 RE sweep, and the two standing owner blocks
(`spec.voice-pool`, `re.phase-2-sweep`). Old-3DS performance readiness remains unmeasured and stays
open.

## NEXT (batch 137)
**The row's acceptance command passes, the gate suite runs it and refuses a one-case PASS, and the
frontends now collide against the oracle's drawn list instead of a geometric stand-in.** What is left
on `3ds.collision-doors`, in priority order: **(1) a crossing capture.** The 3DS rest pitch and the
drawn-list pin both read a `currentArena()` that moves, and no capture has ever shown it: an
`emurun.sh` run that walks level 4 through connection 1012 (or level 7 through 1001) on `:99` would
close review-1 OQ3, review-2 OQ3 and review-3 OQ3 at once, and it needs a scripted walk in the
harness, so it is not free. **(2) `LoadArena` + `PullDoors`** (`ScriptRuntime.cs:615-643`): an arena
being loaded takes the doors leading into it out of its neighbours and turns them around
(`ObjectBehaviors.MoveDoor`, `_residentArena`), and until it lands a door can stay behind in the
neighbour after Kurt crosses. It is what `DoorArenaTests` actually asserts, and the drawn-list
dependency it named is now ported, so this is the remaining collision gap. **(3) the hatch pin.** DTI
dirs 6/7 are every doorway on level 5 and half of 3/6/8; the arms are ported and demonstrated on
shipped geometry but no oracle assertion covers them, and the dotnet probe that would produce one is
gone (`/tmp/kilo/dotnet10` is absent and `/tmp/kilo` is not writable by this uid — an
owner/environment item). **(4) `FollowArenas`'s golden**, which needs `PreloadArena`
(`ScriptRuntime.cs:602-614`) — the drawn-list half of its dependency landed here. **(5)
`TeleportKurt`**, the only other way the oracle moves `currentArena_` and the only way into an arena
no doorway leads to; it is also what would let the crossing goldens' first-tick lines become oracle
assertions instead of two rules that agree. **(6) the snowboard branch of `solidArenas()`** is written
and unreachable: `ridesBoard_` has no writer until the ride slice, so that half is compile-verified
only. **Owner-only and still owed:** review-1 **F4** (`ralph/builder.md` is `PROTECTED` and
modified-unstaged — it now carries the TH reviewer identity and the search-scope rule; a batch can
neither commit nor revert it), review-3 **F3** (make `SCRIPTS_REQUIRED_CASES` owner-protected: it
lives in `gates_core_parity.py`, which is not in `PROTECTED`; the new seeded arms make a shrink loud
on the next gate run rather than silent, which is the most a batch can do), review-4's F2-F8, the
`3ds.hud-canvas` acceptance cell (`emurun.sh --expect hud` has no such flag), and a decision on
whether the gate suite should ever run all 570 golden cases (measured 7m27s warm, so today
`gates_core_parity` runs only the crossing target). **Still open and not touched:** every `hw.*` row
(no Old-3DS hardware in hand, no `mdk_3ds/tools/hw_cycle.sh`), the Phase 2 RE sweep, and the two
standing owner blocks (`spec.voice-pool`, `re.phase-2-sweep`). Old-3DS performance readiness remains
unmeasured and stays open.

## NEXT (batch 136)
**The row's acceptance command passes, is run by the gate suite, and can no longer be satisfied by one of its
two cases.** What is left on `3ds.collision-doors` is substance the row never claimed, in priority order:
**(1) the frontend pin.** `currentArena_` is the oracle's now, so both frontends could stop refreshing the
collision world from the geometric arena under Kurt's feet — but the oracle passes the LIST of current + active
second (`ScriptRuntime.cs:265`, `Viewer.cs:533`) and `ArenaSpace::setSolid` takes one name, so this needs a
two-name `setSolid` and the scripts' drawn-arena list first, and `tests/scripts/frontend_solid_pin_test.sh`
reads both frontends for the old rule and must move with it. **(2) a crossing capture.** The 3DS rest pitch
reads a `currentArena()` that moves and no capture shows it: an `emurun.sh` run that walks level 4 through
connection 1012 on `:99` would close review-1's open question 3 and review-2's open question 3, and the harness
would need a scripted walk, so it is not free. **(3) `LoadArena` + `PullDoors`** (`ScriptRuntime.cs:615-643`),
the gap review-1 F5 named: an arena being loaded takes the doors leading into it out of its neighbours and turns
them around; until it lands a door can stay behind in the neighbour after Kurt crosses. It is what
`DoorArenaTests` actually asserts and it needs the drawn list, so it and (1) are one slice. **(4) the hatch
pin.** DTI dirs 6/7 are every doorway on level 5 and half of 3/6/8; the arms are ported and demonstrated on
shipped geometry but no oracle assertion covers them, so the pin needs either a restored dotnet probe (the SDK
at `/tmp/kilo/dotnet10` is gone and `/tmp/kilo` is not writable by this uid — an owner/environment item) or a
new assertion in the oracle's own suite. **(5) `FollowArenas`'s golden**, which needs `PreloadArena` and the
drawn list. **(6) `TeleportKurt`**, the only other way the oracle moves `currentArena_` and the only way into an
arena no doorway leads to. **Owner-only and still owed:** review-1 **F4** (`ralph/builder.md` is
modified-unstaged and `PROTECTED`; commit it deliberately or revert it — a batch can neither), review-4's F2-F8,
the `3ds.hud-canvas` acceptance cell (`emurun.sh --expect hud` has no such flag), and a decision on whether the
gate suite should ever run the whole golden suite (measured 7m27s warm, so today `gates_core_parity` runs only
the crossing target and the other 567 cases still depend on the builder running them). **Still open and not
touched:** every `hw.*` row (no Old-3DS hardware in hand, no `mdk_3ds/tools/hw_cycle.sh`), the Phase 2 RE sweep,
and the two standing owner blocks (`spec.voice-pool`, `re.phase-2-sweep`). Old-3DS performance readiness remains
unmeasured and stays open.

## NEXT (batch 135)
**`3ds.collision-doors`'s acceptance command passes and is now run by the gate suite; the ledger cell is the
owner's to flip** (the ledger is `coleshores`-owned and in `PROTECTED`). In priority order for the next batch:
**(1) the frontend pin.** `currentArena_` is now the oracle's, so `mdk_3ds/src/main_3ds.cpp` and the Linux
frontend can stop refreshing the collision world from the geometric arena under Kurt's feet — but the oracle
passes the LIST of current + active second (`ScriptRuntime.cs:265`, `Viewer.cs:533`) and `ArenaSpace::setSolid`
takes one name, so this needs a two-name `setSolid` first, and `tests/scripts/frontend_solid_pin_test.sh` reads
both frontends for the old rule and must move with it. **(2) a crossing capture.** The rest pitch on the 3DS now
reads a `currentArena()` that moves, and no capture shows it: an `emurun.sh` run that walks level 4 through
connection 1012 on `:99` would close review-1's open question 3 (the harness would need a scripted walk, so this
is not free). **(3) `LoadArena` + `PullDoors`** (`ScriptRuntime.cs:615-643`), the gap review-1 F5 named: an arena
being loaded takes the doors leading into it out of its neighbours and turns them around, and until it lands a
door can stay behind in the neighbour after Kurt crosses. It is what `DoorArenaTests` actually asserts, and it
needs the drawn-arena list. **(4) the hatch pin.** DTI dirs 6/7 are every doorway on level 5 and half of 3/6/8;
the arms are ported and demonstrated on shipped geometry but no oracle assertion covers them, so the pin needs
either a restored dotnet probe (the SDK at `/tmp/kilo/dotnet10` is gone, and `/tmp/kilo` is not writable by this
uid — an owner/environment item) or a new assertion in the oracle's own suite. **(5) `FollowArenas`'s golden**,
which needs `PreloadArena` and the drawn-arena list. **(6) `TeleportKurt`**, the only other way the oracle moves
`currentArena_` and the only way into an arena no doorway leads to. **Owner-only and still owed:** review-1 **F4**
(`ralph/builder.md` is modified-unstaged and `PROTECTED`; commit it deliberately or revert it — a batch can
neither), review-4's F2, F3, F5, F6, F7, F8 (all in `PROTECTED` files unwritable by uid 994), the
`3ds.hud-canvas` acceptance cell (`emurun.sh --expect hud` has no such flag), and a decision on whether the gate
suite should ever run the whole golden suite (measured 7m27s warm, so today `gates_core_parity` runs only the
crossing target and the other 567 cases still depend on the builder running them). **Still open and not touched:**
every `hw.*` row (no Old-3DS hardware in hand, no `mdk_3ds/tools/hw_cycle.sh`), the Phase 2 RE sweep, and the two
standing owner blocks (`spec.voice-pool`, `re.phase-2-sweep`). Old-3DS performance readiness remains unmeasured
and stays open.

## NEXT (batch 134)
**`3ds.collision-doors`'s substance is landed and pinned; the row's ledger cell is the owner's to flip** (the
ledger is `coleshores`-owned and in `PROTECTED`). In priority order for the next batch: **(1) the frontend pin.**
`currentArena_` is now the oracle's, so `mdk_3ds/src/main_3ds.cpp` and the Linux frontend can stop refreshing the
collision world from the geometric arena under Kurt's feet — but the oracle passes the LIST of current + active
second (`ScriptRuntime.cs:265`, `Viewer.cs:533`) and `ArenaSpace::setSolid` takes one name, so this needs a
two-name `setSolid` first, and `tests/scripts/frontend_solid_pin_test.sh` reads both frontends for the old rule
and must move with it. **(2) the hatch pin.** DTI dirs 6/7 are every doorway on level 5 and half of 3/6/8; the
arms are ported and demonstrated on shipped geometry but no oracle assertion covers them, so the pin needs
either a restored dotnet probe (the SDK at `/tmp/kilo/dotnet10` is gone from this host — an owner/environment
item) or a new assertion in the oracle's own suite. **(3) `FollowArenas`'s golden**, which needs `PreloadArena`
and the drawn-arena list (the oracle's `DoorArenaTests` door assertions). **(4) `TeleportKurt`**, which is the
only other way the oracle moves `currentArena_` and the only way into an arena no doorway leads to. **Owner-only
and still owed:** review-4's F2, F3, F5, F6, F7, F8 (all in `docs/COMPLETION_LEDGER.tsv`, `docs/OWNER_BLOCKS.md`,
`tools/runner/gates_completion.py`, `ralph-pi.sh` — `PROTECTED`, unwritable by uid 994), the `3ds.hud-canvas`
acceptance cell (`emurun.sh --expect hud` has no such flag), and the still-uncommitted protected
`ralph/builder.md` (modified in the working tree at dispatch; this batch did not stage it). **Still open and not
touched:** every `hw.*` row (no Old-3DS hardware in hand, no `mdk_3ds/tools/hw_cycle.sh`), the Phase 2 RE sweep,
and the two standing owner blocks (`spec.voice-pool`, `re.phase-2-sweep`). Old-3DS performance readiness remains
unmeasured and stays open.

## NEXT (batch 133)
**`core.formats` is `done` and stays that way by construction.** The owner flipped the row in `8c751cc` and this
batch gated the cause its comment names, so the next dispatch is `3ds.collision-doors` (`--next` confirms).
**Review-4's remaining six findings are all in files a batch cannot write** — `tools/runner/gates_completion.py`,
`docs/COMPLETION_LEDGER.tsv`, `docs/OWNER_BLOCKS.md` and `ralph-pi.sh` are `coleshores`-owned, not writable by
uid 994, and in `PROTECTED`. In priority order: **(1) F2** — `gates_completion` compares *counts* of `done` rows,
so deleting the open rows (the only rows that can buy completeness) turns `--strict` green; reproduced, and it is
the one finding that makes the loop's real exit condition forgeable. Compare ids, not counts, and seed two arms.
**(2) F5** — `completion_next()` discards stderr and the caller reads empty stdout as "no open row", so a ledger
that cannot be read (exit 2) silently removes the item constraint for an iteration; only exit 1 means "nothing
open". **(3) F3** — `d1` is defined as the owner's goal but omits `p8.cia-build`, which is filed `d2`, and cites
`CONTINUATION.md:871` for text that is at `:1040`; either move the row or say that `d1` is a build and the CIA is
deferred. **(4) F6/F7** — a `done` row is verified by "some path exists", and `word_present` lets prose in
`OWNER_BLOCKS.md` legalise a block; `owner_blocked_ids()` is dead code. **(5) F8** — a frame sidecar cannot name
the binary it came from; the field belongs in `gates_frames.py`. **Also owed by the owner:** `ralph/builder.md` is
still modified and uncommitted (protected, so this batch would not stage it), and two acceptance cells name
commands that do not exist (`make -C tests/scripts test_crossing`, `emurun.sh --expect hud`). **Still open and
not touched:** every `hw.*` row (no Old-3DS hardware in hand, and `mdk_3ds/tools/hw_cycle.sh` does not exist),
the Phase 2 RE sweep, and the two standing owner blocks (`spec.voice-pool`, `re.phase-2-sweep`). Old-3DS
performance readiness remains unmeasured and stays open.

## NEXT (batch 129)
**The ledger now chooses the work, and the next open d1 row is `3ds.collision-doors` with an acceptance
that does not exist.** `make -C tests/scripts test_crossing` names a target `tests/scripts/Makefile`
does not define (review-14 F5, confirmed: its `.PHONY` list is `all bin test clean`). Two ways out and
only one of them is a batch's: the owner rewrites the acceptance to a command that exists and
discriminates, or a batch adds a `test_crossing` target that runs the crossing golden the row means.
Adding a *new* phony target is additive and is allowed — what a batch must not do is change what the
existing `make -C tests/scripts` means, because that command is the recorded acceptance of the already
`done` `core.goldens` row and silently redefining a closed row's evidence is editing a golden by
another route. The row's substance is also real work, not a typo fix: the crossing pin the row wants is
the collision/door behaviour the 3DS build has never been measured against, and `3ds.hud-canvas` behind
it has the same shape (`emurun.sh --expect hud` — no such flag; the harness's `*)` arm swallows it, so
the command exits 2 whatever the game does). Both rows need their acceptance made executable before
their subject is even reachable, and that is a ledger edit, which is owner-only.
**Owner-only, blocking, in priority order:** (1) F2 — take `docs/COMPLETION_LEDGER.tsv` out of
`PROTECTED` and gate it on monotonic transitions instead, or no batch can record progress and the loop
parks at exit 6 on the first row it finishes; (2) F1 — make a `done` row quote measured output, not a
path, or the ledger becomes a pass generator with a nicer name; (3) F3/F4 — gate the completion exit on
`LAST_GATES=PASS` and give it its own exit code; (4) commit or revert the uncommitted completion-ledger
set, which is what makes every one of these unlandable here; (5) F5's acceptance strings; (6) F6's
outside-the-file pin for `c3d_frame_test.sh`'s declared counts. **Closed by this batch, pending the
owner's one-line flip:** `core.formats` — `make -C tests/formats` exits 0 with `156190 checks, 0
failures`; the row needs `status=done`, an `evidence` artefact quoting that command and its output, and
`verified` = this batch's commit. **Still open and not touched:** every `hw.*` row (no Old-3DS hardware
in hand, and `mdk_3ds/tools/hw_cycle.sh` does not exist), the Phase 2 RE sweep, and the two standing
owner actions the ledger already names (`spec.voice-pool`, `re.phase-2-sweep`).

## NEXT (batch 128)
**The texel family is closed on the operation, the spelling, the position and the multiplicity, and the
loop must now leave it.** This is the fifth consecutive batch whose finding was not a missing check but
the rule the previous batch had just added reading *N* of *N+1* dimensions of the defect it names —
batch 124's path resolution, 125's parameter, 126's operation, 127's position, 128's multiplicity — and
that recurrence is the signal, not a compliment: a rule that takes five batches to converge is being
maintained rather than finished, and a sixth would say the same about this one. What the ten texel
checks now pin: the helper's byte layout (host); each expansion's body against every write spelling —
subscript on a name, on a cast, a dereference anywhere in the statement, a shift-assign or compound
assign through EITHER side of the `*`, and **every** bulk byte-setter in the statement that is not the
whole-buffer transparent zero fill, whichever intrinsic or algorithm spells it, qualified or reached
through a `using`-declaration; the fill's destination, its length as arithmetic rather than as text,
its POSITION against the first write and against the loop nesting, and now its COUNT; the argument order
of every 5-argument call in the file; and each call's alpha against the path its own arguments resolve
to, by alias, by hoisted byte, by PARAMETER at its call sites in this file, and by stride. An eleventh
texel check needs a finding these ten cannot reach, and the honest answer is that the next finding in
this area will be a dimension of a rule a batch wrote, which is an argument for a different subsystem,
not for an eleventh check. The stated limits are the known edges and all are written where the claim is
made: a sink fed through the PARAMETER of a factored-out function; a 3-byte source read through a
hoisted pointer; a palette arriving through an expression the scan cannot read; a palette needing a
FOURTH hop of call-site resolution (the bound is three hops, and it fails open for the path only); two
same-name function definitions, which `fn_def_site` resolves to the first; a per-row padding fill,
refused because proving that region is all padding is arithmetic this scan does not do; a loop this scan
cannot read as a loop (a macro that expands to one) is not searched for a write inside it; and — new,
from this batch — a bulk setter that reaches the statement through a macro this scan does not expand,
which is why the rule is on the statement and not on the program.

**The two named game items are the next real work and both are blocked on a named cause, not on
effort.** (1) **the canvas seam** — `mdk_render_canvas_quad` is a no-op on this backend, which is what
priority item 5b (the HUD) and the MVE movie presenter both wait on; the canvas draw has to become a
record in the frame's queue so the per-eye replay draws it once per eye, and it is the largest
unbuilt piece of "a CIA that plays a level". (2) **the collision batch** — `FindConnections` +
`CrossedArena`, which unfreezes `currentArena_` from the spawn arena and lets `ArenaSpace::setSolid`
take the oracle's current + active second, and which needs a doorway-crossing oracle rather than a
hand-written expectation. Either one changes `mdk_3ds/**` and therefore owes an `emurun.sh` verdict
block on `DISPLAY=:99`, recorded in the full shape review-4 F3 named (invocation, display, emulator
md5, `guest log :` path, frames, exit code). Note again that `/tmp/kilo` is owned by another uid on this
host and is not writable by the loop's uid (`piagent`), so a capture directory has to be chosen that the
loop can actually write — batch 128 used `/tmp/mdk-scratch-pi` — and the harness's own `TMPDIR` handling
checked before the run.

**Three things in this area stay stated rather than fixed, and none is a defect.** (1) The index-0
discard's survival is argued from construction plus the helper's host row and is NOT directly measured —
measuring it needs a keyed texture over a non-black clear colour, which the pinned scene does not
contain. (2) RGBA8 tiling on real hardware (review-7's open question 1) is UNRESOLVED and cannot be
resolved from this host: the vault block this file cites says Morton, MDK writes linear per-texel, and
the coherence measurement that clears it was taken in Mandarine, which is explicitly not hardware
evidence. It is a hardware capture or a Mandarine GPU-decode read, not a gate. (3) The frame pair is a
PRESENCE check, not a fidelity check — and from this batch that statement is in the sidecar as well as
here, which is review-13 F3's whole content.

**Owner-only items unchanged, plus one.** The dirty `ralph/builder.md` (PROTECTED, seventh batch
running, and invisible to a gate that reads committed ranges only), whether `mdk_3ds/Makefile` joins
`PROTECTED`, `tools/check.sh` being protected so `arm_compile_check.sh` stays a quoted runner rather
than a gate, that a historical `docs/frames/*.txt` sidecar cannot be amended by any batch — so
review-11 F5 and review-13 F3 can only be satisfied forward, never backward — that
`gates_batch_integrity.py:68-70` demands exactly one newly added `docs/frames/*.png` per batch while a
batch with no shipped byte can only supply a byte-identical re-capture (136 of the tree's 137 PNGs now
share one md5), and that `mdk.3dsx` is NOT bit-reproducible, so `mdk.elf` and `mdk_c3d.shbin` are the
build hashes to quote. Also unchanged: neither `obsidian_*` nor `cognee_*` is exposed in this session,
so the vault lookup stays the degraded disk-only path and Cognee's graph search is unavailable (P9).

## NEXT (batch 127)
**The texel family is closed on the operation, on the position and on the spelling, and the loop
should now leave it.** Review-12's two Highs were both inside the rule batch 126 had just added, which
is the fourth batch running where the finding was not a missing check but a check that read one
dimension of the defect and not the next one. What the ten texel checks now pin: the helper's byte
layout (host); each expansion's body against every write spelling — subscript on a name, on a cast, a
dereference anywhere in the statement, a shift-assign or compound assign through EITHER side of the
`*`, and any bulk byte-setter that is not the whole-buffer transparent zero fill, whichever intrinsic
or algorithm spells it, qualified or reached through a `using`-declaration; the fill's destination,
its length as arithmetic rather than as text, and its POSITION against the first write and against
the loop nesting; the argument order of every 5-argument call in the file; and each call's alpha
against the path its own arguments resolve to, by alias, by hoisted byte, by PARAMETER at its call
sites in this file, and by stride. An eleventh texel check needs a finding these ten cannot reach.
The stated limits are the known edges and all are written where the claim is made: a sink fed through
the PARAMETER of a factored-out function; a 3-byte source read through a hoisted pointer; a palette
arriving through an expression the scan cannot read; a palette needing a FOURTH hop of call-site
resolution (the bound is three hops, and it fails open for the path only); two same-name function
definitions, which `fn_def_site` resolves to the first; a per-row padding fill, refused because
proving that region is all padding is arithmetic this scan does not do; and — new, from this batch —
a loop this scan cannot read as a loop (a macro that expands to one) is not searched for a write
inside it.

**The two named game items are the next real work and both are blocked on a named cause, not on
effort.** (1) **the canvas seam** — `mdk_render_canvas_quad` is a no-op on this backend, which is what
priority item 5b (the HUD) and the MVE movie presenter both wait on; the canvas draw has to become a
record in the frame's queue so the per-eye replay draws it once per eye, and it is the largest
unbuilt piece of "a CIA that plays a level". (2) **the collision batch** — `FindConnections` +
`CrossedArena`, which unfreezes `currentArena_` from the spawn arena and lets `ArenaSpace::setSolid`
take the oracle's current + active second, and which needs a doorway-crossing oracle rather than a
hand-written expectation. Either one changes `mdk_3ds/**` and therefore owes an `emurun.sh` verdict
block on `DISPLAY=:99`, recorded in the full shape review-4 F3 named (invocation, display, emulator
md5, `guest log :` path, frames, exit code). Note that `/tmp/kilo` is owned by another uid on this
host and is not writable by the loop's uid, so a capture directory has to be chosen that the loop can
actually write, and the harness's own `TMPDIR` handling checked before the run.

**Three things in this area stay stated rather than fixed, and none is a defect.** (1) The index-0
discard's survival is argued from construction plus the helper's host row and is NOT directly
measured — measuring it needs a keyed texture over a non-black clear colour, which the pinned scene
does not contain. (2) RGBA8 tiling on real hardware (review-7's open question 1) is UNRESOLVED and
cannot be resolved from this host: the vault block this file cites says Morton, MDK writes linear
per-texel, and the coherence measurement that clears it was taken in Mandarine, which is explicitly
not hardware evidence. It is a hardware capture or a Mandarine GPU-decode read, not a gate. (3) The
frame pair is a PRESENCE check, not a fidelity check — see the owner-only item below.

**Owner-only items unchanged, plus one.** The dirty `ralph/builder.md` (PROTECTED, sixth batch
running, and invisible to a gate that reads committed ranges only), whether `mdk_3ds/Makefile` joins
`PROTECTED`, `tools/check.sh` being protected so `arm_compile_check.sh` stays a quoted runner rather
than a gate, the fact that a historical `docs/frames/*.txt` sidecar cannot be amended by any batch,
so a sidecar error can only be carried forward, that
`gates_batch_integrity.py:68-70` demands exactly one newly added `docs/frames/*.png` per batch while
a batch with no shipped byte can only supply a byte-identical re-capture (135 of the tree's PNGs now
share one md5), and — new, from review-12 F8 — that `mdk.3dsx` is NOT bit-reproducible: three builds
of one unmodified tree produced three md5s, because the RomFS staging embeds something that is not
the tree. Quote `mdk.elf` and `mdk_c3d.shbin` as the build-product hashes and say plainly that the
`.3dsx` is a packaging artefact, not a fidelity measurement.

## NEXT (batch 126)
**The texel family is closed on the OPERATION, not on a spelling — and it should stop here, with the
residual named instead of claimed away.** Review-11's three Mediums were all inside the rule batch
125 had just added, which is the third batch running where the finding was not the missing check but
a check that matched a spelling of the defect. What the ten texel checks now pin: the helper's byte
layout (host); each expansion's body against every write spelling — subscript on a name, on a cast,
a dereference anywhere in the statement, a shift-assign or compound assign through a dereference, and
any bulk byte-setter that is not the whole-buffer transparent fill, whichever intrinsic or algorithm
spells it; the argument order of every 5-argument call in the file; and each call's alpha against the
path its own arguments resolve to, by alias, by hoisted byte, by PARAMETER at its call sites in this
file, and by stride. An eleventh texel check needs a finding these ten cannot reach. The stated limits
are the known edges and all are written where the claim is made: a sink fed through the PARAMETER of
a factored-out function; a 3-byte source read through a hoisted pointer; a palette arriving through
an expression the scan cannot read (a member of a member, a call whose name says nothing, a filler
with no call site in this file); a palette needing a FOURTH hop of call-site resolution (the bound is
three hops, and it fails open for the path only — the alpha stays fail-closed); two same-name
function definitions, which `fn_def_site` resolves to the first (it would fail toward the stricter
branch, not toward a free pass); and — new, from review-11 F1 — **a per-row padding fill
(`memset(out + (size_t)width * 4, 0, (paddedWidth - width) * 4)`) is refused**, because proving that
region is all padding is arithmetic on `width` and `paddedWidth` this scan does not do. A batch that
wants that shape widens the rule with that proof; it does not route around it.

**Three things in this area stay stated rather than fixed, and none is a defect.** (1) The index-0
discard's survival is argued from construction plus the helper's host row and is NOT directly
measured — measuring it needs a keyed texture over a non-black clear colour, which the pinned scene
does not contain. (2) RGBA8 tiling on real hardware (review-7's open question 1) is UNRESOLVED and
cannot be resolved from this host: the vault block this file cites says Morton, MDK writes linear
per-texel, and the coherence measurement that clears it was taken in Mandarine, which is explicitly
not hardware evidence. It is a hardware capture or a Mandarine GPU-decode read, not a gate. (3) The
frame pair is a PRESENCE check, not a fidelity check — see the owner-only item below.

**Owner-only items unchanged, plus one.** The dirty `ralph/builder.md` (PROTECTED, fifth batch
running, and invisible to a gate that reads committed ranges only), whether `mdk_3ds/Makefile` joins
`PROTECTED`, `tools/check.sh` being protected so `arm_compile_check.sh` stays a quoted runner rather
than a gate, the fact that a historical `docs/frames/*.txt` sidecar cannot be amended by any batch,
so a sidecar error can only be carried forward, and — new, review-11 F5 — that
`gates_batch_integrity.py:68-70` demands exactly one newly added `docs/frames/*.png` per batch while
a batch with no shipped byte can only supply a byte-identical re-capture: 134 of the tree's 135 PNGs
share one md5. Either exempt batches whose diff carries no `mdk_3ds/**`/`mdk_core/**`/
`build-linux/**` path, or require the new PNG's md5 to differ from every historical one. Until then
the frame pair proves the picture did not regress only when a byte moved, and this batch says so.

**The two named game items are unchanged and both are blocked on a named cause, not on effort.**
(1) **the canvas seam** — `mdk_render_canvas_quad` is a no-op on this backend, which is what
priority item 5b (the HUD) and the MVE movie presenter both wait on; the canvas draw has to become
a record in the frame's queue so the per-eye replay draws it once per eye, and it is the largest
unbuilt piece of "a CIA that plays a level". (2) **the collision batch** — `FindConnections` +
`CrossedArena`, which unfreezes `currentArena_` from the spawn arena and lets `ArenaSpace::setSolid`
take the oracle's current + active second, and which needs a doorway-crossing oracle rather than a
hand-written expectation. Either one changes `mdk_3ds/**` and therefore owes an `emurun.sh` verdict
block on `DISPLAY=:99`, recorded in the full shape review-4 F3 named (invocation, display, emulator
md5, `guest log :` path, frames, exit code).

## NEXT (batch 125)
**The texel family is closed on resolved paths — parameters included — and it should stop here
again, this time with the residual named instead of claimed away.** Review-10's one High was the
same failure mode as review-8's and review-9's: a rule that matched a spelling of the defect
instead of the defect. What the ten checks now pin: the helper's byte layout (host), each
expansion's body against every write spelling — subscript on a name, on a cast, a dereference
anywhere in the statement, and a memset whose fill is not the transparent texel — the argument
order of every 5-argument call in the file, and each call's alpha against the path its own
arguments resolve to, by alias, by hoisted byte, by PARAMETER at its call sites in this file, and
by stride. An eleventh texel check needs a finding these ten cannot reach; the stated limits are
the known edges and all are written where the claim is made: a sink fed through the PARAMETER of a
factored-out function, a 3-byte source read through a hoisted pointer, and — new, from review-10
F1 — a palette arriving through an expression the scan cannot read (a member of a member, a call
whose name says nothing, a filler with no call site in this file).

**Two things in this area stay stated rather than fixed, and neither is a defect.** (1) The index-0
discard's survival is argued from construction plus the helper's host row and is NOT directly
measured - measuring it needs a keyed texture over a non-black clear colour, which the pinned scene
does not contain. (2) RGBA8 tiling on real hardware (review-7's open question 1) is UNRESOLVED and
cannot be resolved from this host: the vault block this file cites says Morton, MDK writes linear
per-texel, and the coherence measurement that clears it was taken in Mandarine, which is explicitly
not hardware evidence. It is a hardware capture or a Mandarine GPU-decode read, not a gate.

**Owner-only items unchanged:** the dirty `ralph/builder.md` (PROTECTED, fourth batch running, and
invisible to a gate that reads committed ranges only), whether `mdk_3ds/Makefile` joins `PROTECTED`,
`tools/check.sh` being protected so `arm_compile_check.sh` stays a quoted runner rather than a gate,
and the fact that a historical `docs/frames/*.txt` sidecar cannot be amended by any batch, so a
sidecar error can only be carried forward.

**The two named game items are unchanged and both are blocked on a named cause, not on effort.**
(1) **the canvas seam** - `mdk_render_canvas_quad` is a no-op on this backend, which is what
priority item 5b (the HUD) and the MVE movie presenter both wait on; the canvas draw has to become
a record in the frame's queue so the per-eye replay draws it once per eye, and it is the largest
unbuilt piece of "a CIA that plays a level". (2) **the collision batch** - `FindConnections` +
`CrossedArena`, which unfreezes `currentArena_` from the spawn arena and lets `ArenaSpace::setSolid`
take the oracle's current + active second, and which needs a doorway-crossing oracle rather than a
hand-written expectation. Either one changes `mdk_3ds/**` and therefore owes an `emurun.sh` verdict
block on `DISPLAY=:99`, recorded in the full shape review-4 F3 named (invocation, display, emulator
md5, `guest log :` path, frames, exit code).

## NEXT (batch 124)
**The texel family is closed on resolved paths, not on recognised names, and it should stop here.**
Review-9 had two Highs and both were the same failure mode as review-8's: a rule that matched a
*spelling* of the defect instead of the defect. What the ten checks now pin: the helper's byte layout
(host), each expansion's body against every write spelling - subscript on a name, on a cast, and a
dereference anywhere in the statement - the argument order of every 5-argument call in the file, and
each call's alpha against the path its own arguments resolve to, by alias and by stride. A eleventh
texel check needs a finding these ten cannot reach through a spelling they still recognise by
accident; the two stated limits in the gate (a sink fed through a PARAMETER, and a 3-byte source read
through a hoisted pointer) are the known edges, and both are written where the claim is made.

**Two things in this area stay stated rather than fixed, and neither is a defect.** (1) The index-0
discard's survival is argued from construction plus the helper's host row and is NOT directly
measured - measuring it needs a keyed texture over a non-black clear colour, which the pinned scene
does not contain. (2) RGBA8 tiling on real hardware (review-7's open question 1) is UNRESOLVED and
cannot be resolved from this host: the vault block this file cites says Morton, MDK writes linear
per-texel, and the coherence measurement that clears it was taken in Mandarine, which is explicitly
not hardware evidence. It is a hardware capture or a Mandarine GPU-decode read, not a gate.

**Owner-only items unchanged:** the dirty `ralph/builder.md` (PROTECTED, third batch running, and
invisible to a gate that reads committed ranges only), whether `mdk_3ds/Makefile` joins `PROTECTED`,
`tools/check.sh` being protected so `arm_compile_check.sh` stays a quoted runner rather than a gate,
and the fact that a historical `docs/frames/*.txt` sidecar cannot be amended by any batch, so a
sidecar error can only be carried forward.

**The two named game items are unchanged and both are blocked on a named cause, not on effort.**
(1) **the canvas seam** - `mdk_render_canvas_quad` is a no-op on this backend, which is what
priority item 5b (the HUD) and the MVE movie presenter both wait on; the canvas draw has to become
a record in the frame's queue so the per-eye replay draws it once per eye, and it is the largest
unbuilt piece of "a CIA that plays a level". (2) **the collision batch** - `FindConnections` +
`CrossedArena`, which unfreezes `currentArena_` from the spawn arena and lets `ArenaSpace::setSolid`
take the oracle's current + active second, and which needs a doorway-crossing oracle rather than a
hand-written expectation. Either one changes `mdk_3ds/**` and therefore owes an `emurun.sh` verdict
block on `DISPLAY=:99`, recorded in the full shape review-4 F3 named (invocation, display, emulator
md5, `guest log :` path, frames, exit code).

## NEXT (batch 122)
**The texel gate is closed on the sink and the whole file, not on two names, and the gate family
should stop growing here.** Review-7 had no Critical and no High and all six findings are closed.
What the family now pins: the helper's byte layout (host), the two expansions' bodies, the
argument provenance order, both alphas, every texel-filling function in the file, and the one
place the PICA receives the bytes. A seventh texel check needs a finding that these six cannot
reach, and review-7's five mutants are all inside them now.

**Two things in this area stay stated rather than fixed, and neither is a defect.** (1) The
index-0 discard's survival is argued from construction plus the helper's host row and is NOT
directly measured - measuring it needs a keyed texture over a non-black clear colour, which the
pinned scene does not contain. (2) RGBA8 tiling on real hardware (review-7's open question 1) is
UNRESOLVED and cannot be resolved from this host: the vault block this file cites says Morton,
MDK writes linear per-texel, and the coherence measurement that clears it was taken in Mandarine,
which is explicitly not hardware evidence. It is a hardware capture or a Mandarine GPU-decode
read, not a gate.

**Owner-only items unchanged:** the dirty `ralph/builder.md` (PROTECTED, invisible to a gate that
reads committed ranges), whether `mdk_3ds/Makefile` joins `PROTECTED`, `tools/check.sh` being
protected so `arm_compile_check.sh` stays a quoted runner rather than a gate, and the fact that a
historical `docs/frames/*.txt` sidecar cannot be amended by any batch, so a sidecar error can
only be carried forward.

**The two named game items are unchanged and both are blocked on a named cause, not on effort.**
(1) **the canvas seam** - `mdk_render_canvas_quad` is a no-op on this backend, which is what
priority item 5b (the HUD) and the MVE movie presenter both wait on; the canvas draw has to
become a record in the frame's queue so the per-eye replay draws it once per eye, and it is the
largest unbuilt piece of "a CIA that plays a level". (2) **the collision batch** -
`FindConnections` + `CrossedArena`, which unfreezes `currentArena_` from the spawn arena and lets
`ArenaSpace::setSolid` take the oracle's current + active second, and which needs a
doorway-crossing oracle rather than a hand-written expectation. Either one changes
`mdk_3ds/**` and therefore owes an `emurun.sh` verdict block on `DISPLAY=:99`, recorded in the
full shape review-4 F3 named (invocation, display, emulator md5, `guest log :` path, frames,
exit code).

## NEXT (batch 121)
**The texel path is now pinned where the defect was, and the gate family is closed again on the
terms review-6 named.** Review-6 had no Critical and no High; all four findings are closed (F1,
F2, F4 fixed; F3 confirmed with its remediation blocked by an owner-protected gate). Two things
in this area are left stated rather than fixed, and a later batch should not mistake either for
a defect: the index-0 discard's survival is argued from construction plus the helper's host row
and is NOT directly measured — measuring it needs a keyed texture over a non-black clear colour,
which the pinned scene does not contain; and `expandRgba` is now a per-texel loop where it was a
row `memcpy`, which is cold by §2's criteria and unmeasured in both states. Owner-only items
unchanged from batch 119: the dirty `ralph/builder.md` (PROTECTED, invisible to a gate that reads
committed ranges), whether `mdk_3ds/Makefile` joins `PROTECTED`, and `tools/check.sh` being
protected so `arm_compile_check.sh` stays a quoted runner rather than a gate. A fourth now
joins that list: a historical `docs/frames/*.txt` sidecar cannot be amended by any batch, so a
sidecar omission can only be carried forward, never corrected in place.

**The two named game items are unchanged and both are blocked on a named cause, not on effort.**
(1) **the canvas seam** — `mdk_render_canvas_quad` is a no-op on this backend, which is what
priority item 5b (the HUD) and the MVE movie presenter both wait on; the canvas draw has to
become a record in the frame's queue so the per-eye replay draws it once per eye, and it is the
largest unbuilt piece of "a CIA that plays a level". (2) **the collision batch** —
`FindConnections` + `CrossedArena`, which unfreezes `currentArena_` from the spawn arena and lets
`ArenaSpace::setSolid` take the oracle's current + active second, and which needs a
doorway-crossing oracle rather than a hand-written expectation. Either one changes
`mdk_3ds/**` and therefore owes an `emurun.sh` verdict block on `DISPLAY=:99`, recorded in the
full shape review-4 F3 named (invocation, display, emulator md5, `guest log :` path, frames,
exit code).

## NEXT (batch 176)
**THE CYCLE'S PAYLOAD COULD NOT END A RUN, AND NOTHING IN THE HARNESS ASKED WHETHER IT COULD.**
Every push since the harness landed had been built with the plain
`make -C mdk_3ds` this file's own P10 block printed — no `AUTOQUIT`, therefore no
`-DMDK_AUTOQUIT`. `harness_3ds.cpp` compiles its `#else` answers in that build:
`harnessAutoquitDue()` is `return false` and `harnessChainloadStub()` does nothing,
so `main_3ds.cpp:904` can never fire, no `--- session end ---` can ever be written,
and the only verdict `hw_cycle.sh` could reach was a timeout on a console still
playing the game. **The binary said so and nothing read it:** the pushed `.3dsx`
contains `harness autoquit (MDK_AUTOQUIT)` — that is `main_3ds.cpp`'s literal,
present in every build — and does **not** contain `/3ds/netload_stub.3dsx`, which
only exists under the macro. A string that survives both builds cannot be the test
for which build you have; the chainload path can.

Three fixes, all checkable without a console:
1. `main_3ds.cpp` logs `main: harness build, autoquit=90s chainload=netload_stub`
   at boot under `#ifdef MDK_AUTOQUIT`, so every pull states which payload ran.
2. `hw_cycle.sh` refuses any `.3dsx` without the chainload path, before the
   preflight and before the push, and exits 1 with the rebuild command.
3. The self-test grew the case (`MDK_HW_DSX` is its seam for a payload that is not
   the tree's build): `6 PASS / 0 FAIL` on this harness, and against `HEAD`'s
   `hw_cycle.sh` **`5 PASS / 1 FAIL`** — the failing case is the new one, and the
   parent's output is the defect in one line: it pushed the no-harness payload and
   printed `FRESH`.

**FOUND BY ITS OWN GATE, AND WORTH KEEPING.** The first draft of (2) was
`strings -a "$DSX" | grep -q '/3ds/netload_stub.3dsx'`; `hw_cycle.sh` is
`set -uo pipefail`, `grep -q` exits at the match and closes the pipe, `strings`
dies of SIGPIPE, `pipefail` reports 141, and a **good** payload was refused on all
five existing cases. It is now `grep -aqF -- '/3ds/netload_stub.3dsx' "$DSX"`:
no pipe, no SIGPIPE, and grep reads the archive directly.

**P8 verdict blocks (review-4 F3 shape).** Emulator `/mnt/nas/Development/mandarine.AppImage`
md5 `0d2adab6ad078c78e6d03e72086f2eb6` (r1.2.1, the pinned good build), X display `:99`
(`Xvfb :99 -screen 0 1280x1024x24`; the pre-existing server was dead with a stale
`/tmp/.X99-lock`, which was removed and the server restarted), payload
`mdk_3ds/mdk.3dsx` 119 371 828 bytes built with `AUTOQUIT=90`:
* `DISPLAY=:99 mdk_3ds/tools/emurun.sh --min-frames 60 --shot /tmp/kilo/payload-fix-plain.png`
  → **exit 0**, `guest log : /tmp/mdk-emurun.190970/xdg/mandarine-emu/sdmc/3ds/mdk/mdk.log`,
  ends at frame 2520, and line 5 of that log is the new boot line. No
  `--- session end ---`: `emurun.sh`'s reap ended the run before the 90 s guest
  deadline, so this block proves the payload boots and runs, not that its deadline
  fires. That one is the console's to show.
* `DISPLAY=:99 mdk_3ds/tools/emurun.sh --expect hud --min-frames 60 --shot /tmp/kilo/after-payload-fix.png`
  → **exit 1, `HUD MISPLACED`**: 25 ink pixels, 21 inside the derived crop
  `200,120-390,234`, bbox `(384,222)-(392,229)`; the HUD's own canvas line read
  `quads=4 draws=2 mesh=2 corners=24 eyes=1` at frame 2460 and the log ends at
  2580. **Open, and not this batch's acceptance:** the batch's change is a log
  write under `#ifdef` and cannot move a canvas, so this is a pixel-geometry
  finding for the HUD/canvas row (batch 155's refusal doing its job), to be
  triaged against a hardware capture — which is unavailable, because see below.

**The console is still ARP-dead, so no hardware pass and no ledger row closes.**
`hw.harness-cycle`, `hw.profile-og`, `hw.pixels-rgba8`, `hw.cia-boot` and
`hw.sleep-resume` remain `todo`. `192.168.4.167` fails ping, its neighbour entry is
`FAILED`, broadcast discovery times out, and `:8080`/`:17491`/`:17492` are closed;
`cmd reboot` over UDP `:8082` did not restore it within five minutes. The next
hardware increment is the same one as before: `hw_cycle.sh --once --ip 192.168.4.167`
on the now-verifiable payload, then the arena-transition and section-6 gameplay
acceptance that were always behind it.

## NEXT (batch 119)
**Nothing in the gate family is open, and nothing in it should be widened without a finding that
needs it.** Review-4 found no Critical and no High, its Medium was a coverage hole in a runner
rather than a claim about the shipped build, and the runner now fails closed on measuring
nothing. Two items from that review are **owner-only** and are named here so they are not lost:
(a) `ralph/builder.md` is dirty in the worktree, is `PROTECTED`, and cannot be committed or
stashed by a batch, and the gate that would notice a worktree edit is itself `PROTECTED`;
(b) whether `mdk_3ds/Makefile` joins `PROTECTED`, so a shipped build-flag change is gated by the
suite rather than by review and R10 discipline alone. A third standing owner item predates them:
`tools/check.sh` is owner-protected, so `arm_compile_check.sh` — the runner that proves the
printf attribute is alive — stays outside the loop's gate chain by owner decision.

**The two named game items are unchanged and both are blocked on a named cause, not on effort.**
(1) **the canvas seam** — `mdk_render_canvas_quad` is a no-op on this backend
(`renderer_c3d.cpp:2345`), which is what priority item 5b (the HUD) and the MVE movie presenter
both wait on; the canvas draw has to become a record in the frame's queue so the per-eye replay
draws it once per eye, and it is the largest unbuilt piece of "a CIA that plays a level". (2)
**the collision batch** — `FindConnections` + `CrossedArena`, which unfreezes `currentArena_`
from the spawn arena and lets `ArenaSpace::setSolid` take the oracle's current + active second,
and which needs a doorway-crossing oracle rather than a hand-written expectation. Either one
changes `mdk_3ds/**` and therefore owes an `emurun.sh` verdict block on `DISPLAY=:99`, recorded
in the full shape review-4 F3 named (invocation, display, emulator md5, `guest log :` path,
frames, exit code).

## NEXT (batch 117)
**The gate family is closed on the terms batch 116 named, and this batch paid for it.** Review-22's
open question 2 asked whether the conversion grammar should keep being patched or whether the compiler
should own it; batch 117 gave `mdk::log3dsPrintf` the `format (printf, 1, 2)` attribute, made the
diagnostic a gate in `arm_compile_check.sh`, and closed the one grammar gap the reviews kept finding
by putting the space flag in `FIGURE_FMT`. What the family now pins, and what it no longer needs to
grow: the compiler owns argument **count and type** at all 72 log call sites, the gate owns which
quantity each figure **claims to be**, and a conversion neither C nor `FIGURE_FMT` can read (`%'`,
`%1$`) is refused by name rather than skipped. A thirteenth patch to check 17 is not planned; if a
review finds a fourteenth thing that a reader with a profile could have found, the retirement argument
batch 113 wrote is due and this file should say so.

**Two cheap follow-ups this batch measured and deliberately did not spend.** (1) `-Werror=format` in
`mdk_3ds/Makefile`, so the shipped build itself refuses a mismatch instead of warning: the tree is now
measured clean for it (73/73 sources, zero diagnostics), and it is an AGENTS R10 build-setting change
owing the toolchain-version, clean-build-command, disassembly and hardware-check evidence R10 asks
for. (2) Teaching `tools/check.sh` to invoke `arm_compile_check.sh`, which today is a script the loop
runs and quotes rather than one the gate runs -- `tools/check.sh` is owner-protected, so that is an
owner edit, and it is named here so it is not lost.

**The two named game items are unchanged and both are blocked on a named cause, not on effort.** (1)
**the canvas seam** -- `mdk_render_canvas_quad` is a no-op on this backend
(`renderer_c3d.cpp:2345`), which is what priority item 5b (the HUD) and the MVE movie presenter both
wait on; the canvas draw has to become a record in the frame's queue so the per-eye replay draws it
once per eye, and it is the largest unbuilt piece of "a CIA that plays a level". (2) **the collision
batch** -- `FindConnections` + `CrossedArena`, which unfreezes `currentArena_` from the spawn arena
and lets `ArenaSpace::setSolid` take the oracle's current + active second, and which needs a
doorway-crossing oracle rather than a hand-written expectation. Either one changes `mdk_3ds/**` and
therefore owes an `emurun.sh` verdict block on `DISPLAY=:99`. The one-sided-view investigation below
stays open and is the sharper of the two picture questions, and the R = 255 candidate set is still
two, not five (below).

## NEXT (batch 116)
**What is actually next, restated by batch 116.** This is the twelfth round on the same three checks,
and the shape of the last three is the same: review-20, review-21 and review-22 each found a clause of
check 17 whose reach depended on what `FIGURE_FMT` can and cannot read — a unit word belonging to the
wrong conversion, a format with a conversion the pattern does not match, a space flag. Review-22
found no Critical and no High and every finding was settleable by a mutant against the shipped
source, so batch 113's retirement clause still does not trigger on its own terms (it turned on a
finding "a reader with a profile could have found", and none of these is that). But the review named
the way out and this batch agrees it is the right one: **give `mdk::log3dsPrintf` a
`__attribute__((format (printf, 1, 2)))` and let the compiler own argument count and type**, which
retires check 17's arity and conversion-grammar rules instead of patching them a thirteenth time. That
is a shipped-code change in `mdk_3ds/src/platform/log_3ds.h`, it can turn existing call sites into
warnings on both targets, and it owes P8 emulator evidence, so it is a batch of its own and not a
side-effect of a gate batch. If the loop would rather not spend that batch, the alternative is to
leave the family frozen at what it now pins — every figure sourced, every unit word nearest its
conversion, no unreadable conversion accepted — and stop widening it.

**The two named game items are unchanged and both are blocked on a named cause, not on effort.** (1)
**the canvas seam** — `mdk_render_canvas_quad` is a no-op on this backend
(`renderer_c3d.cpp:2345`), which is what priority item 5b (the HUD) and the MVE movie presenter both
wait on; (2) **the collision batch** — `FindConnections` + `CrossedArena`, which unfreezes
`currentArena_` from the spawn arena and lets `ArenaSpace::setSolid` take the oracle's current +
active second, and which needs a doorway-crossing oracle rather than a hand-written expectation.
Either one changes `mdk_3ds/**` and therefore owes an `emurun.sh` verdict block on `DISPLAY=:99`.
The one-sided-view investigation below stays open and is the sharper of the two picture questions,
and the R = 255 candidate set is still two, not five (below).

## NEXT (batch 114)
**What is actually next, restated by batch 114.** This was the tenth round on the same three checks,
and batch 113 said the family should be retired rather than patched a tenth time if a review found
another thing in it. Review-20 found no Critical and no High, and every one of its four findings was
settleable by text against the shipped source rather than by a profile -- which is the distinction
batch 113's retirement clause actually turned on. The family was therefore closed out, and closed out
by *removing* a rule rather than adding one: check 17 now has one attribution rule (a unit word
belongs to the conversion nearest it), one literal test (one unwrap, one pattern), and no line-level
rule that a legal line can trip. Two weaknesses stay stated in the code rather than argued away: check
17 reads a *format string* to decide family membership, so a refusal line whose format is a variable
is outside the net; and a slot the unit words assign no unit is not adjudicated against the backend
unless its argument is a literal, because the gate cannot know what an unlabelled number claims to
be. If a review finds a third thing in check 17 that a reader with a profile could have found, the
retirement argument is due and this file should say so rather than host an eleventh patch.

**Batch 115 is expected to be a game batch, and a batch that changes `mdk_3ds/**` owes emulator
evidence under P8.** The two named items are unchanged from batch 113 and both are blocked on a named
cause rather than on effort: (1) **the canvas seam** -- `mdk_render_canvas_quad` is a no-op on this
backend (`renderer_c3d.cpp:2345`), which is what priority item 5b (the HUD) and the MVE movie
presenter both wait on; (2) **the collision batch** -- `FindConnections` + `CrossedArena`, which
unfreezes `currentArena_` from the spawn arena and lets `ArenaSpace::setSolid` take the oracle's
current + active second, and which needs a doorway-crossing oracle rather than a hand-written
expectation. Neither is a gate batch. The one-sided-view investigation below stays open and is the
sharper of the two picture questions, and the R = 255 candidate set is still two, not five (below).

*(What batch 113 said, kept for the record.)*
**What is actually next, restated by batch 112.** Batch 111 said batch 112 should be a game
batch. It was not: review-18 arrived with three Mediums open and reachable today, and leaving a
confirmed green-on-a-defect net unremediated to prefer a more interesting batch is the trade this
loop must not make. That is now eight rounds on the same three checks, and the honest reading is
the one batch 110 already gave: a net that keeps being wrong about its own scope needs review
against a profile or a rewrite, not another patch. Batch 112 is the batch that argues the family
is finished rather than the eighth patch: F1's fix replaced the family's third blacklist with a
whitelist, and a whitelist is the shape that cannot be spelled around, so the alias question that
took four rounds (review-15 F1, review-16 oq2, review-17 R1, review-18 F1) is closed as a class
and not narrowed. The family's one remaining known weakness is stated in the code, not hidden:
check 17 reads a *format string* to decide whether a line belongs to the refusal family, so a
refusal line whose format is a variable is outside the net.

**What is actually next, restated by batch 113.** Batch 112 said it was the batch that argued the
gate family was finished rather than the eighth patch. Review-19 answered that in the only way
that settles it: it found a Critical in the same check 17, in a different property -- not another
alias spelling but WHICH slot of a line is adjudicated, and how many arguments a conversion
consumes. So the honest statement is now the one batch 110 gave and batch 112 restated: this family
has been patched nine rounds by the agent whose work it grades, and the next time a review finds a
thing in it that a reader with a profile could have found, the family is retired to a review
against a real capture, not patched a tenth time. Two weaknesses are stated in the code rather
than argued away: check 17 reads a *format string* to decide family membership, so a refusal line
whose format is a variable is outside the net; and a slot whose own text names no unit is not
adjudicated against the backend unless its argument is a literal, because the gate cannot know what
an unlabelled number claims to be.

**Batch 114 is expected to be a game batch, and a batch that changes `mdk_3ds/**` owes emulator
evidence under P8.** Two named items, both blocked on a named cause rather than on effort: (1)
**the canvas seam** -- `mdk_render_canvas_quad` is a no-op on this backend
(`renderer_c3d.cpp:2345`), which is what priority item 5b (the HUD) and the MVE movie presenter
both wait on; (2) **the collision batch** -- `FindConnections` + `CrossedArena`, which unfreezes
`currentArena_` from the spawn arena and lets `ArenaSpace::setSolid` take the oracle's current +
active second, and which needs a doorway-crossing oracle rather than a hand-written expectation.
Neither is a gate batch. If a review-19 arrives instead, the standing offer stands: the gate
family's remaining weakness is the one named above, and a finding that turns out to be a ninth
spelling of the alias question is an argument for retiring the family, not for a ninth patch.
The one-sided-view investigation below stays open and is the sharper of the two picture
questions, and the R = 255 candidate set is now two, not five (below).

**What batch 112 changed, in one paragraph, so the next gate reader does not re-derive it.**
Both latch nets are `LATCH_SHAPE` + `unpinned_uses()`: the latch's name is refused outside its
declaration, the two reads the guard accepts and the pinned writes, in both gates, and the
failure text says a pointer or reference to the latch is refused on purpose. Check 17's rule is
"no refusal line may disagree", stated as: every figure comes from an argument (a digit left in
the format after the conversions are stripped is a figure with no source), the format's own unit
words decide which arguments are byte figures, the refused capacity must appear somewhere, and
conversions are matched by the full conversion grammar with `conversions <= arguments`.
Measured at batch 112: `PASS Main3dsFlow (20 checks, 42 arms, 17 probes)`, `PASS C3dFrame (31
checks, 48 arms, 2 probes)`, `PASS ArmCompile (111 mutants, 0 diagnostics)`, `make -C
tests/scripts` all cases match, `bash tools/check.sh` ALL GATES PASSED.

**Batch 109 re-measured the R = 255 lead while scoping, and the candidate set is now two, not five.**
`docs/evidence/2026-11-09-L7-shader-w-current.png` again: 96000 of 96000 pixels have R = 255, 131
distinct colours, G takes 65 distinct values and B takes 91. Two things were then excluded by
reading, not guessing. (1) **The vertex colour path is excluded.** `mdk_3ds/shaders/mdk_c3d.v.pica`
writes `mov outclr, ones` — the vertex colour is literally white and the packed RGBA8 colour is
never uploaded (the shader's own header says so) — and the draw path's Tev is
`C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, …)` with `GPU_REPLACE`, so the fragment colour IS the
texture sample and no vertex colour reaches it. (2) **The palette and the expansion's byte order are
excluded as a pair.** `expandIndices` writes `pixel[0..2] = paletteRgba[0..2]`, `pixel[3] = 255`, and
the Vulkan backend uploads the SAME `palette.rgba()` bytes as `VK_FORMAT_R8G8B8A8_UNORM` and renders
correct colours, so the source bytes are R-first and correct. What is left is the one constant 255 in
the pipeline — the FORCED ALPHA — and the two ways it can arrive at the fragment's red channel.
**(a)** the texel byte order: something between the four bytes `expandIndices` writes and the four the
PICA reads is rotated by one, so alpha lands in red and blue lands in alpha. This is a HYPOTHESIS, not
a fact, and the note says why it is not obvious: this project already knows the PICA's CLEAR WORD is
`0xRRGGBBAA` while the seam passes `0xAARRGGBB` (`mdk_3ds/src/render/pica_matrix.h:302-319`, which
painted alpha as red once), but a texture format and a clear register are different conventions and
this file has NOT verified `GSP_RGBA8_OES`'s byte order against GBATEK — `gspgpu.h:29` only says
"RGBA8. (4 bytes)". **(b)** the second Tev stage: nothing in this file configures it — `grep -n
"GetTexEnv(1)" mdk_3ds/src/render/renderer_c3d.cpp` is empty — so stage 1 is whatever `C3D_Init` left
while stage 0 is set explicitly every draw, and AGENTS P2's rule is that GPU state left unset is not a
state but the previous frame's or garbage. Each half is ONE build and one `panel_geom.py` run: (a) by
writing the expansion's four bytes rotated, (b) by setting stage 1 to `previous * primary`. Neither
needs a geometry theory, and if (a) is right the one-sided view may be a colour bug wearing a geometry
costume. Do NOT re-derive the two exclusions above.

*(Batch 104 did not advance the renderer head either; it closed the instrument review-10 proved could not
fail and added a measured colour fact to the head. The item below is restated against the committed panel,
because review-9 F1 is right that batch 102 erased the picture this item was written against: the head item
used to say "geometry covers panel columns 200..399 and coverage stops at a straight vertical line at the
panel's middle", and the census equality "58464 exact clear pixels in BOTH builds" is the batch that
invalidated it. That sentence is now false for the current build and is struck.)*
**The one-sided view, restated against the post-object-pass panel.** What the committed crops measure
(`python3 tools/panel_geom.py census docs/evidence/2026-11-09-L7-{object-pass-parent,shader-w-current}.png`):

    arena only (batch 102 parent):  clear 58464, painted-left 0,     painted-right 37536
    arena + objects (now):          clear     0, painted-left 48000, painted-right 48000

So there are now TWO questions, not one, and the second is the sharper one.

1. **The arena still never reaches the left half.** 0 painted pixels of 48000 in the arena-only build, and
   the boundary is a straight vertical line at the panel's middle, not a silhouette. That is unchanged by
   batch 102 and is the original defect.
2. **Seven objects paint the entire left half and 10464 px more of the right — and that is now PROVEN,
   not inferred.** Forcing the stream mesh's `linearAlloc` to fail (one line, `kInitialStreamCapacity =
   1 << 22`, run in the emulator) removes the object pass, and the panel becomes **byte-identical** to
   batch 102's arena-only crop (pixel sha `f9af51b6…`, `docs/evidence/2026-11-09-L7-mesh-alloc-failure.png`;
   `panel_geom.py geom` on that pair prints `0 px` in the map and `0 px` in colour and does NOT refuse,
   because the arena-only panel has 58464 clear pixels — this is what a real 0 px control looks like).
   So `painted-left = 48000` is the objects, and the open question is what seven doors/BEAMS/statics are
   drawing that covers a whole half-panel to the pixel, not whether they draw. Review-9's open question 1
   stands: the crop cannot distinguish "placed correctly" from "a surface occludes the frame".

3. **NEW, and it is measured, not suspected: every painted pixel of the console's panel has R = 255.**

       python3 - <<'EOF'
       from PIL import Image; from collections import Counter
       px = list(Image.open("docs/evidence/2026-11-09-L7-shader-w-current.png").convert("RGB").getdata())
       print(Counter(p[0] for p in px)[255], "of", len(px))   # -> 96000 of 96000
       EOF

   **[MEASURED AGAINST A CONTROL BY BATCH 105, and it is now a sharper statement.]** The sentence
   above had no control — a fully painted panel cannot show what it excludes. Batch 105 re-ran the
   forced-allocation-failure mutant, whose panel is the arena-only panel with **clear colour
   `(16, 32, 48)`**, and the separation is exact:

       arena-only mutant: 37536 painted pixels, 37536 with R = 255; 58464 clear, 0 with R = 255
       arena-only mutant: 117 distinct painted colours, R = 255 on every one of them
       shipped build:     96000 painted, 96000 with R = 255; 130 distinct colours
                          top (255,0,0) 21287x, (255,44,141) 11262x, (255,0,28) 7753x

   So the claim is not "the panel is red". **Every fragment the renderer paints arrives with its R
   channel at 1.0 while G and B take 117–130 distinct values, and the only other colour path into
   the frame — the clear word — delivers R = 16 correctly.** That excludes the clear word (the
   precedent batch 104 pointed at, `pica_matrix.h:302-319`) and points at the fragment/vertex colour
   upload: a texture format whose bytes land R-last, or a vertex colour written in the wrong order,
   or a `C3D_TexEnv`/colour-source selection putting alpha where red belongs. It also means the
   noise pair's "R identical, B swinging 0..255" is not a phase: R never varies because R is not
   carrying colour. Review-11's open question 1 asked exactly this and got an answer the same batch
   did not have to spend a run on.

   The same count on the Linux frame of the same core (`docs/frames/2026-11-09-b103-*.png`, level 3) is
   9758 of 76800, so this is the 3DS colour path and not MDK's art. Every committed console crop has it,
   including the arena-only ones, so it predates the object pass and is a THIRD defect, not a symptom of
   the two above: the panel is painted, and what it is painted with has its red channel pinned to 1.0. The
   shape of the numbers points at the byte order of a colour reaching the fragment stage — the differing
   pixels of the noise pair are `(255,0,0) <-> (255,44,141)`-style pairs with R identical and B swinging
   the full 0..255, which is what an alpha landing in red looks like — and this project has already been
   bitten by exactly that class of bug at the other end of the frame: `pica_matrix.h:302-319` records that
   the PICA's clear word is `0xRRGGBBAA` (red in the TOP byte) while the seam passes `0xAARRGGBB`, and
   passing it straight through painted the alpha byte as red. `AGENTS.md` §12 names a second colour-order
   trap in `../forsaken/3DS_PORT_NOTES.md`. This is the cheapest open lead in the file: it is
   one upload format and one shader read, it is measurable without a geometry theory, and if it is the
   texture format then the one-sided view may be a colour bug wearing a geometry costume.

What is CLOSED for it, so it is not re-derived: the cull enum, the depth map/test/clear triple, the queue,
the MVP construction, a deleted rotation (Iteration 106), the sheared display transfer, and now the
vertex attribute's `w` (batch 103, measured byte-identical). What is NOT closed: the viewport/attribute
upload path, and the fact that the identity-MVP diagnostic triangle still paints nothing even with `w`
forced — that one is unexplained and it is the only measured GPU behaviour in the tree that no theory
currently covers.

**The instrument rule, restated by review-10 F1, and it binds every future console A/B.**
`tools/panel_geom.py geom` is the picture claim **only where it does not refuse**, and on the current panel
it refuses: the object pass paints all 96000 pixels, so the painted/clear map is a constant, a constant
cannot differ, and a `geom: 0 px` there was arithmetic (batch 103 bound the loop to exactly that). It now
exits 3 and says so, and `tests/scripts/panel_geom_test.sh` gates the refusal. The honest state is worse
than batch 103 wrote and narrower in one place: two runs of ONE ELF differ by 53515 px of 96000 in full
colour on the arena+objects panel, while batch 102's two independent runs of the arena-only panel were
byte-identical — **a noise floor is only valid for the scene it was measured on**, and batch 103 quoted a
floor from the wrong scene at its own A/B (review-10 F2). The mechanism of the 53515 px is NOT established:
R is identical in every differing pixel while G moves on 41462 and B on 53512 with a full 0..255 swing, and
batch 102's `current` (deadline frame 2520) and batch 103's `final2` (deadline frame 2820) produced
byte-identical panels, which is hard to reconcile with an arbitrary phase in a ~60-frame window. **Batch
104 localised it further: the arena-only scene is byte-identical across three runs in three lanes**
(batch 102's two controls and the forced-allocation-failure run, all `f9af51b6…`), so the 53515 px belongs
to the **object pass's output**, not to the capture and not to the arena. Five runs of one ELF, clustered,
is still what would settle the mechanism, but the search is now inside `ObjectDraw`/`ObjectView` and not
inside `emurun.sh`. A colour claim still needs a frame-locked capture, and the
named blocker for that is that the 3DS backend implements no screenshot path at all (`render.h`:
`screenshot_path` is "Linux only") — landing one is a
batch of its own, and it is what would let the next level-7 capture be diffed against the oracle's frame
instead of against itself. Do not ship a sixth census with the old instrument.

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

**Audio and scripts, in priority order after the renderer items.** (a) **DONE — batch 102.** The
object pass is on the console: the C#'s `DrawObjects` between the arena and Kurt, at the blended
transform, over an `objectLook` cache. The linear question it owed is answered with a measurement and
not a hope: level 7 makes the ONE stream mesh once at 4096 corners = **80 KB linear** and never grows
it (the frames reach 570 corners / 13 draws, which is `L7_t1000_draw.txt`'s number exactly), against
the C#'s 5.2 MiB `Renderer.StreamVertices`. It also surfaced and fixed a lifetime defect the pass
would have shipped into **both** frontends — see the `Last updated` line and
`main_3ds_flow_test.sh` check 15. What it did NOT do is the thing this paragraph predicted: no script
sound reached the sink (`plays` stays at Kurt's own 1, `voices=0`), because level 7's seven objects
never fire one; the sink's script-sound row is **still unmeasured**, and a level whose scripts do play
at the spawn is what would close it. (b) **the bank still has no eviction bound** — the
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

**Deferred review findings:** **review-11 F5 (Medium) is deferred with cause, and it is an
operator action, not a batch one.** `ralph-pi.sh`'s `guard()` now waits forever (`while : … sleep
300`) both when qwen is down — the operator's 2026-11-05 decision — and when `$PI_CMD` does not
exist at all, so a fresh box or a second machine parks at `phase route-health` silently, and because
the epoch is rewritten every 600 s the 900 s watchdog never fires. The fix is one fail-fast before
the wait: `[ -x "$PI_CMD" ] || { log "worker wrapper $PI_CMD is missing or not executable — BLOCKED, "
"not a route outage"; exit 7; }`. It is not applied here because `ralph-pi.sh` **is the process
executing this batch**, bash reads a running script incrementally, and the harness pins its own hash
so a mid-run edit is caught as stale; it is also outside `AGENTS.md` §8. Related, same owner queue:
`docs/OWNER_DIRECTIVE_*.md` is not in `gates_batch_integrity.PROTECTED`, which is how batch 104 was
able to put a reviewer-independence exception into a directive with no gate noticing (review-11 F3),
and `tools/reclaim_qwen_slots.sh --kill-live` reaches holders shared with two sibling campaigns
(review-11 open questions 4).
**Review-12 open question 1 is deferred with cause, and it is a measurement gap, not a code gap.**
The re-arm review-11 F7 asked for can in principle spam: if frame N needs more than `capacity_` and
the grow fails, the working mesh has already been released, so frame N+1 with a small count succeeds
and clears the latch, and frame N+2 needs the big count and refuses again — one line per cycle. No
shipped scene shows it (level 8's need is steady at 5616 corners, level 3's at 480, and both are
measured printing once), and the only way to close it is a capture of a scene whose
`objectStream.vertices.size()` alternates under real linear pressure, which no level in the corpus is.
If it ever becomes real, the fix is to key the edge trigger on `attemptedCapacity()` instead of a
boolean, so each distinct refusal prints once whatever the frame did in between — and that changes
what the console log means, so it needs the capture first, not a gate that can already pass both
ways.
**Review-4 of run-20261105-142911 F1/F2** (the geometric solid-arena pin stands in for the unported
DTI connection crossing; the fall probe hard-codes the start and omits the camera stage) need a
placed-fall oracle in full order plus a walk-out golden, and are deferred to a collision batch.
**Review-6 F1 is that same gap, and it is now the front end's whole collision story**: `currentArena_`
is frozen at the spawn arena until `FindConnections` + `CrossedArena` are ported, and
`ArenaSpace::setSolid` takes one name where the oracle passes current + active second. That port is
the collision batch, and it unblocks three things at once — the door-walk golden review-8 F1 wants,
the camera's rest pitch, and the per-object crossing (`ScriptRuntime.cs:776-787`). It is not a cheap
follow-on and it needs a doorway-crossing oracle, not a hand-written expectation. **Batch 101 widened
the gap rather than narrowing it: BOTH frontends now stand the geometric arena under the feet up for
the scripts' drawn list** (`Viewer.cs:533` passes `SolidArenas`), so the collision batch has two
frontends to land on and one shared gate (`frontend_solid_pin_test.sh`) that will have to change when
it does. Review-7's open question 1 (is `mdk_linux` in behavioural scope?) was answered by fixing it
rather than by documenting a divergence: the Linux frontend takes the pinned frame, so it is in scope.
**New from batch 97: the MVE movie decoder has no player.** `formats/mve.{h,cpp}` is ported and
now covered (PCM both widths, DPCM, silence, the broken-size shapes) but nothing calls it, so it
is gc-sectioned out of the shipped ELF and `MISC/FLIC/MDKBZK.MVE` never plays. Wiring it needs a
video presenter (palette + indices → a canvas or a texture), which is the same canvas seam item 5b
is blocked on, so it is not a cheap follow-on. Original-3DS behaviour remains unmeasured.

## Standing facts a batch must not re-derive

* **`ScriptRuntime::option` is 1, not 0, and it is the oracle's default rather than a port choice
  (batch 100, review-6 F2).** `ScriptRuntime.cs:214` initialises the field to 1 and the Viewer
  overwrites it from `Settings.Gore`, which also defaults to true (`Settings.cs:75`, `Viewer.cs:234`;
  `:874` is the `TOOSCARYFORME` gore **cheat**, not the resume path — the resume writer is
  `ScriptRuntime.Snapshot.cs:142`, review-7 F4). Two ported behaviours read it: opcode 232 `if_option` compares for **equality** (an Option
  of 2 must not take the `if_option 1` branch) and opcode 129 `blow_off_parts` mode 2 is skipped while
  it is 0. The gore **debris** half (`ScriptRuntime.cs:1754-1764`, 32×4 RNG draws) is NOT ported, so
  the value does not move the random stream today — which is also why **no golden distinguishes 0 from
  1**: the suite is green either way, so the suite cannot be used as evidence for either value and a
  future batch must not "fix" this back on the strength of a green run.

* **`ScriptRuntime` OWNS its `SoundMixer`, so a frontend that has a runtime has exactly one mixer to
  mix and it is the runtime's (batch 99).** Every script sound is written into that mixer
  (`script_vm.cpp:1258` opcode 89 → `ScriptRuntime::playSound` → `mixer_.play/playAt`), so a frontend
  that keeps mixing its own standalone mixer produces a level that runs its scripts, drains its audio
  queue, prints a healthy ledger and is **silent** — no crash, no error, nothing in the log. And because
  the mixer is a member of the runtime, **rebuilding the runtime rebuilds the `SoundDevice`**: the sink
  has to be handed to the new one on every respawn or the level after the first death is silent. Both
  are pinned by `main_3ds_flow_test.sh`; neither is visible to a compiler or to a single-frame capture.
* **`ArenaSpace::setSolid("")` CLEARS the pin, and `ScriptRuntime::currentArena()` is empty until the
  runtime's first completed tick** (`script_runtime.cpp:371-379` sets it inside the arena-change
  branch, and a 1/60 s call only completes a tick every second step). So a frontend that pins from the
  runtime must guard on non-emptiness, or it drops the spawn pin for the first step or two — which is
  the fall-out-death failure review-8 F1 measured, arriving from a third direction (batch 99).
* **The console boots level 7, not level 3** (`flow: screen=4 level=7`), so every 3DS emulator
  measurement in this file is a level-7 measurement while the goldens and the pinned frame are level 3.
  Do not read a level-7 emulator line as evidence about level 3, or the other way round.
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

* **A guard is only pinned if a case can REACH it, and a sanitizer being quiet is not evidence that
  one fired.** `Mve`'s four file/length bounds each need a different fixture, and three of them
  need a file that is **cut**: `end` comes from the opcode's declared length, so an honest tail
  makes `p < end` stop the loop before any file bound is consulted, and the guard is unreachable.
  `truncateTail()` in `mve_audio_tests.cpp` is the fixture that expresses it. Deleting
  `p + 2 > fileSize` (the seed loop) or `p + step > fileSize` (`decodePcm`) was **silent under both
  sanitizers** — the bytes are still inside the movie vector's own allocation — so only a value
  assertion can see those two, and before batch 98 nothing made one. The reusable habit: run the
  mutation battery, do not trust "the suite is green" as "the guards are covered". (review-4
  F1/F2 / batch 98.)

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

* **A P8 block is the harness's verdict block, not a summary of it** (review-4 F3). Quote the
  `emurun.sh` invocation as typed, the `display`, the emulator md5, the harness's own
  `guest log :` path, the frame figures and the exit code. A one-line summary is not auditable
  once the `/tmp` lane is collected, and a batch cannot reconstruct the invocation afterwards —
  batch 118 learned this and batch 119 recorded the rule.
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
