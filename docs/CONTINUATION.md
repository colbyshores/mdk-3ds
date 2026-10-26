# Phase 2 continuation checkpoint

Last updated: 2026-10-26 (iteration 35: stats slice committed — opcode
217 `global_573c4c_add` via new `GameStats` (`game_stats.h`, exact
33-entry table re-verified against GameStats.cs) + `stats` on
ScriptRuntime + the R11 pin on `percent` (level-end only, oracle
truncation shape). Coverage now C++ 181/238, remaining 58. All three
gates green — see Verified below. Next: one opcode family per batch
through the harness.)
This file is the persistent handoff for every fresh context window. Keep it under
~150 lines: current state + priorities only; append batch narratives to
docs/PROGRESS_LOG.md (archived, not loaded by default).


## PRIORITY ORDER (owner directive 2026-10-25 — port code, goldens follow)

The owner's directive: churn through the C#→C++ conversion. Port subsystems
in this order; extend goldens in the same iteration as the code that unlocks
them. Do not spend an iteration on docs/tests alone.

1. **DONE — mixer call-sites** (`ObjectMotion.cs:133`, `ObjectBehaviors.cs:90-115`).
   Verified 2026-10-25, all gates green; unobservable in the corpus (byte-match
   before AND after), verified by semantic diff, not the corpus. Details in
   `docs/PROGRESS_LOG.md`.
2. **DONE — `Debris.Shatter` + opcodes 137/138/139** (`debris.{h,cpp}`, DEBRIS
   golden line, oracle group registration, all goldens regenerated). Verified
   2026-10-25, all gates green. BreakUp/Trail/Updraft stay with their owning
   slices (see `debris.h`). Details in `docs/PROGRESS_LOG.md`.
3. Continue the remaining opcodes subsystem by subsystem, same rule.
   **Opcode 208 (`if_part_hit`) DONE** — the reachability probe
   (`tests/scripts/tick1 probe 30000`) showed it the only firing unported
   opcode (L5 ×9134); ported, probe clean since. **Synthetic harness DONE
   (slice 1)** — `tests/scripts/vm_tests` twins `ScriptVmTests.cs` (7 tests,
   11 opcodes: 1/12/13/64-68/71/252/253) and runs inside
   `make -C tests/scripts`; **opcode 13 (`if_cheat_key`) DONE** through it
   (explicit false branch; the twin pins the value AND the clean
   `unimplemented()` bucket). **Parts pair DONE (slice 2, iter 24)** —
   32 reuses `partsMask` with hide=false, 129 validates the gore mode and
   locks+hides via `findPart`; three `vm_tests` tests pin them against a
   synthetic 3-part model (no C# counterpart exists to twin — header notes
   this). **Rope-swing DONE (slice 3, iter 28)** — 226 + 242, pinned
   against `ScriptVm.cs:1139-1166`. **Spawn slice** — 156 (arena-model
   spawn at a reference point) and **206 DONE (slice 5, iter 32)** —
   flagged spawn per step over the path API, pinned on L7 path 53729
   (iter 33 hardened it: 0.1f accumulation + absolute positions).
   **81 (`push_hit_dir`) DONE (slice 4, iter 31)** — field-only port,
   writers deferred to the combat slices. 85 (`set_rolling`) stays out:
   `SetRolling` snapshots the full `Rotation` matrix; `rotation()` is
   deferred to the FrameBlend slice. Coverage: C++ 181/238, remaining 58
   (true dispatches 180; the 181st label is the nested `case 0:` in 249).
   132/136 stay with the Effects slice (RNG + effect list + dump line).
   The rest never fire in levels 3-8 to t30000 — they land through the
   harness now, one family per batch. P5 golden-frame tests stay parallel.

Deferred: alarm (move 15 fires 0x), 240/241/244 (Kurt state exists, none
fires — do not blind-port), 248 `push_kurt` (honest no-op). HW timing open.


## Current tree state (VERIFIED)

- Iteration 35 (stats slice: two code commits + this state commit).
  Verified on the final tree:
  - `make -C tests/scripts` → `vm_tests` 16/16 PASS (new
    `HeadShotsAddOnlyWhenEnabled` pins 217: +3/+2 accumulate, first-operand
    0 counts nothing, plus `isEnemyType`/`countEnemy`/`percent` unit pins)
    then `All cases match the oracle.` (goldens unchanged — the counters
    are not dumped)
  - `bash tools/check.sh` → `CHECK: ALL GATES PASSED` (`gates_smdh` SKIPs:
    no built `.cia`). Needed one fix mid-iteration: the R11 screen flagged
    `percent`'s `hits * 100 / shots` (file-level loop inference from
    `isEnemyType`'s scan); pinned R11 with a level-end-only rationale,
    same surface as renderer_c3d.cpp:1130 — no gate relaxed.
  - `make -C mdk_3ds` → links `mdk.elf`, builds `mdk.3dsx`
  - Coverage: C++ 181/238, remaining 58 (217 landed with the stats slice)
- Earlier slices (details in `docs/PROGRESS_LOG.md`): iters 23/24 (harness +
  opcode 13, parts pair), iters 28/29/30 (rope-swing, spawn 156, review-29
  hardening), iter 31 (opcode 81, review-30) and iter 32 (opcode 206,
  review-31) — all gates green at each commit.
- Corpus frontier: every level 3-8 matches the oracle byte for byte (table AND
  script) to t30000. The tick axis is spent; unfired opcodes now land through
  `vm_tests`, one family per batch.
- The oracle driver (`/tmp/kilo/oracle/Driver/Program.cs`, outside the repo; run
  with `DOTNET_ROOT=/tmp/kilo/dotnet10`, `MDK_DATA_DIR=<repo>/MDK`,
  `bin/Debug/net10.0/mdkoracle L T 1 script`) emits the `AUDIO` line and the
  per-object audio fields; `tick1.cpp` `dumpScript` mirrors both. Regenerate
  script goldens with: for each `{L, T}` in the `cases[]` table,
  `mdkoracle L T 1 script > golden/L{L}_t{T}_script.txt`.
- `docs/OPCODE_COVERAGE.md` recounted from the brace-matched sets: C# 238 /
  C++ 181 / remaining 58 (re-run 2026-10-26 for the stats batch; the 181
  carries the nested `case 0:` inside opcode 249 — true dispatches 180).


## Testability map

Goldens compare a FIXED field set (table + script incl. AUDIO/KURT/mixer);
they do **not** observe velocity, `alarm_ticks`, `Items.Bomb`/`Decoy`,
debris. Porting an opcode without a test is forbidden. Two templates:
dump-field (port state, dump, regenerate) and harness (`vm_tests`
twins/pins the oracle — no golden change). `alarm_ticks` is a dead end
(move=15 fires 0x); `Items.Bomb`/`Decoy` land with the Items slice.

## Open items (do not lose these)

1. **Dynamic scene updates — DONE at the scene level.** `buildArenaScene` tags
   each batch with its group; Hidden is a draw-time check; `LevelView::refresh()`
   rebuilds on `group_set_texture`. **Remaining:** the `GameEngine` that runs the
   scripts must call `refresh()` when a `group_set_texture` fires (no engine
   exists yet); the VM state and scene mechanism are both tested, only the
   per-opcode call-site wiring is pending. Do not fake it.
2. **Opcode classification — DONE** in `docs/OPCODE_COVERAGE.md`. Per-opcode
   unit tests beyond the goldens are still needed.
3. **Known divergences**: the DTI record-layout question is CLOSED (raw-bit
   goldens). Remaining: incomplete decoy/`AlienTarget` targeting; opcode 207 —
   C# `MathF.Sign` throws on NaN (C++ returns 0; exceptions banned, accepted);
   degenerate-path guards (`pathKeyCount`/`pathKeyFrame` return 0 where C#
   reads out-of-range bytes — garbage mid-buffer, throws only at the edges;
   inherited by 206 — unreachable in shipped scripts, 206 never fires in the
   corpus).
4. **Old 3DS hardware timing: UNMEASURED.** A devkitARM `.3dsx` is not
   hardware validation. Never claim an OG pass without a capture.
5. **C3D depth state diverges from the oracle (review-8 finding, verified).**
   The C# depth-tests/depth-writes mesh geometry (compare LESS_OR_EQUAL,
   `src/Mdk.Engine/Render/Renderer.cs:603-608`); the C3D backend disables the
   depth test outright (`mdk_3ds/src/render/renderer_c3d.cpp:624`). The fix is
   NOT a one-line flip: PICA's depth range is [-1,0] (opposite of D3D's [0,1]),
   there is a documented invisible-geometry trap, and AGENTS.md P6 requires a
   hardware/emulator capture (none available here). The next render pass must map
   the C# semantics onto PICA (projection remap at renderer_c3d.cpp:496 +
   `C3D_DepthMap` + test function + clear value) and verify before claiming it.
6. **DepthLayer/DepthPull unported (review-8 finding).** The C# pairs
   `Layers.Of` (`src/Mdk.Game/Level/Layers.cs`) with `DepthPull.cs` for
   per-triangle depth layers; `render_types.h` documents them but no C++
   computes them. Gated on item 5 and the textured renderer path.
7. **Doc/spec drift (review-8 observations, not code).** `AGENTS.md`/`SPEC.md`
   cite `docs/DOCTRINE.md`, which does not exist; SPEC §9.4 lists a
   `gates_romfs` gate that was never written. Documentation/spec corrections —
   flag to the owner rather than inventing a gate or doctrine text unilaterally.


## Verification commands (run after every batch)

```
make -C tests/scripts        # builds + goldens ("All cases match the oracle")
bash tools/check.sh          # the six contract gates
make -C mdk_3ds              # devkitARM target (mdk.3dsx)
```

The tests Makefile has no header dependency tracking — after editing headers,
`make -C tests/scripts clean` first, or stale objects can link against old
signatures.

Rules: `src/Mdk.Game/**` is read-only oracle. Keep the monolithic
`ScriptVm::execute()` switch (dispatch investigation resolved). Never weaken
a gate or test to pass. Commit per verified batch.
