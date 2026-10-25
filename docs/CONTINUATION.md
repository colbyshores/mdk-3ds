# Phase 2 continuation checkpoint

Last updated: 2026-10-25 (iteration 11: the pickup/chute/death lifecycle slice
is ported — `Kill`'s death-script branch and `Remove`'s live-`Attached` cascade
mirror the C# — and the golden frontier moved from ~t400-900 to the END of the
corpus: L3/L6/L7/L8 match the oracle byte for byte (table AND script) to t30000
with live objects, L4 to t10000 (empty arena), L5 to t2302. The new L5 blocker
is a BOLT spawn at t2303 (leads recorded below). Review-8's depth finding is
recorded as an open item gated on hardware verification.)
This file is the persistent handoff for every fresh context window. Keep it under
~150 lines: current state + priorities only; append batch narratives to
docs/PROGRESS_LOG.md (archived, not loaded by default).


## PRIORITY ORDER (owner directive 2026-10-25 — port code, goldens follow)

The owner's directive: churn through the C#→C++ conversion. Port subsystems
in this order; extend goldens in the same iteration as the code that unlocks
them. Do not spend an iteration on docs/tests alone.

1. **BOLT spawn slice (the new L5 frontier, t2303)** — at t2303 the oracle
   spawns a 16th BOLT in MUSE_1 that the C++ does not spawn (15 vs 16 objects;
   everything matches at t2302). Evidence for the next context: the new BOLT's
   flags are exactly `ProjectileFlags` 0x00080820 (ScriptRuntime.cs:25), while
   the six BOLTs that DO match carry 0x000808A0 = ProjectileFlags |
   FlagNoBanking. C# `Spawn(..., Spawning.Plain)` applies NO static flags
   (cs:1570 — only `Spawning.Flagged` does), and the `{arena}${type}` init
   script runs at creation (cs:1579-1584) — so the NoBanking on the matching
   BOLTs comes from the init script, and the t2303 BOLT's absence means the
   XG's fire DECISION diverged, not the spawn flags: the C++ `fire()`
   (script_runtime.cpp:1531) already matches C# `Fire` (cs:2659) operand for
   operand. Leads, in order: (a) an RNG-gated branch (opcode 47 `if_chance`)
   in the XG attack script — check the C++ `.NET Random` port's draw sequence
   against the oracle's at ~t2303; (b) a visibility/raycast predicate; (c) a
   Kurt-state input. Disassemble the MUSE_1 XG attack script around the
   opcode-61 fire to find the predicate. Then extend L5 past t2302.
2. **Kurt health/inventory state** — the fields opcodes 109/175 read (they
   fire in L3/L5/L6 goldens). Port the state + dump it in the golden table
   so the opcodes become verifiable, then implement them.
3. **Audio slice for opcode 249** — C# `Audio/SoundBank.cs`/`SoundMixer.cs`
   → a C++ sound seam (3DS: no backend yet is fine; the SCRIPT-visible state
   is what the goldens pin). Then implement 249.
4. Continue the remaining 77 opcodes subsystem by subsystem, same rule.

Deferred with evidence: alarm (move_command 15 never fires in the corpus —
verified 0 occurrences in goldens), opcodes whose subsystems are not yet
reachable. Old-3DS hardware timing stays open (hardware-only).


## Current tree state (VERIFIED)

- The kill/remove lifecycle slice is implemented and committed. Verified on the
  final tree:
  - `make -C tests/scripts` → `All cases match the oracle.` — 341 PASS, 0 SKIP,
    including the 36 new deep cases (L3 t480-30000, L4 t660-10000, L5 t720-2302,
    L6 t960-10000, L7 t960-10000, L8 t420-30000; table AND script dumps).
  - `bash tools/check.sh` → `CHECK: ALL GATES PASSED` (`gates_smdh` SKIPs: no
    built `.cia`)
  - `make -C mdk_3ds` (with `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM`)
    → links `mdk.elf`, builds `mdk.3dsx`
  - `cmake --build build-linux --target mdk` → `Built target mdk` — both targets
    build the changed core.
- `docs/OPCODE_COVERAGE.md` was recounted from the brace-matched `Execute`
  spans: C# 238 cases, C++ 161, remaining 77 (the old 155/83 and the old
  "ported" list were wrong — 114, 171, 209 were never in the C++). The
  reachability probe was rerun to t600: of the remaining set only 109 (L3, L5),
  138 (L5), 175 (L6), 249 (L5) fire.


## Testability map (why the remaining opcode work is blocked, not merely deferred)

The goldens compare a FIXED field set (`tick1.cpp` `dumpTable`/`dumpScript`):
pos, yaw, hp, flags, dead, move_command/parameter, connects, path/time/speed/stop,
script_flags, variables, restart, wait, death_script, command_target, gosub stacks,
level_timers, global vars. They do **not** observe: `target_position`/`target_yaw`,
`alien_target`, `alarm_ticks`, `Items.Bomb`/`Items.Decoy`, velocity, or any audio
state. Combined with the reachability probe (only 109/138/175/249 fire among the
remaining 77), every remaining owned-partial or unimplemented opcode falls in one of
three buckets, none of which is testable in the current tree:

1. **Gated behind an unported subsystem** (the subsystem must land first): 109
   (Rides + `Kurt.Hurt`), 138 (`Debris.Shatter`), 175 (`Kurt.Inventory`), 249/107/
   233 (audio `Mixer`), 27 (alarm-ticks state machine), 165/166 (`Items.Bomb`),
   decoy targeting (`Items.Decoy`), 16/56 death path (death script/debris/drops).
2. **Fires but its effect is not in the dumped field set** — e.g. movement command 6
   (`Chase`) and 15 (`AlarmTicks`): if 6 fired and moved an object the goldens would
   already FAIL, so it does not fire in levels 3-8/t600; 15's only observable effect
   (`alarm_ticks`) is not dumped. Implementing them cannot be checked.
3. **In the dumped set and already matches** — the goldens pass byte-for-byte, so any
   owned opcode that fires and writes a dumped field is already verified.

The project rule (OPCODE_COVERAGE.md) forbids porting an opcode without a test that
exercises it, so buckets 1 and 2 must NOT be blind-ported. **To unblock the next
tier**, the highest-leverage move is to extend BOTH the oracle driver's `script`
dump (`/tmp/kilo/oracle/Driver/Program.cs`) and `tick1.cpp` `dumpScript` to observe
`alarm_ticks`, `target_position`/`target_yaw`, `alien_target`, and `Items.Bomb`/
`Decoy`, then regenerate the script goldens — that turns bucket 2 into a real test
and makes the Items/alarm subsystem slices verifiable when they land. That is a
deliberate golden-set change (touches the oracle harness and regenerates every
script golden), so it is its own increment, not a drive-by.

**Progress (iteration 7):** `target_position`/`target_yaw`/`alien_target` are DONE —
dumped and locked into all 114 script goldens (the C++ already matched, so no
behaviour change). The remaining dump fields are each gated behind an unported
subsystem and must be added *with* that subsystem, not before it: `alarm_ticks`
with the alarm-ticks state machine (the per-tick decrement + the movement-command-15
/ Rides setters), and `Items.Bomb`/`Decoy` with the `Items` slice (which must also
become reachable from the oracle driver). Adding either field to the dump before its
subsystem lands would make the goldens diverge and force a blind port.

**Progress (iteration 8):** the alarm-ticks unblock is now shown to be a dead end in
*this* corpus, not merely deferred: `move=15/` (the only `AlarmTicks` setter) appears
**0 times** across every golden, so dumping `alarm_ticks` would lock an all-zeros
column. The deep-tick frontier was extended instead (see the iteration 8 batch note):
the corpus now reaches each level's first unported-lifecycle divergence (L8 t420,
L3 t480, L5 t720; L4 empties past t600). The next real unblock is a subsystem whose
setter actually fires in levels 3-8 — the pickup/chute/death lifecycle is the first
one on that frontier (it is what bounds L8 at t420), so porting it (with a dump field
for the removal) is the highest-leverage next slice, not the alarm.


## Open items (do not lose these)

1. **Dynamic scene updates — DONE at the scene level.** `buildArenaScene` tags
   each batch with its group and breaks runs at group boundaries; Hidden is a
   draw-time check (`arenaBatchVisible`), so `group_set_state` hide/show needs no
   rebuild, and `LevelView::refresh()` rebuilds the scene+mesh for a
   `group_set_texture` (the C# `Rebuild`). Locked by the scene test in
   `tests/scripts/tick1.cpp`. **Remaining:** the `GameEngine` that runs the
   scripts must call `LevelView::refresh()` when a `group_set_texture` fires (no
   engine exists yet); the VM state and the scene mechanism are both correct and
   tested, only the per-opcode call-site wiring is pending. Do not fake it.
2. **Opcode classification (Task 2 remainder) — DONE.** The 161 owned cases are
   classified in `docs/OPCODE_COVERAGE.md` §"Owned-case classification": 0 stubs,
   11 partial (each verified against the gating subsystem), 150 complete. Per-opcode
   unit tests beyond the end-to-end goldens are still needed (the goldens are the
   only coverage today).
3. **Reachable unimplemented opcodes**: 109, 138, 175, 249 fire in levels 3-8
   to t600 and are real gaps. Each is gated behind an unported subsystem
   (109 → Rides + `Kurt.Hurt`; 138 → `Debris.Shatter`; 175 → `Kurt.Inventory`;
   249 → the audio `Mixer`), so none is meaningfully testable until that
   subsystem lands — do not blind-port them (see coverage doc).
4. **Known divergences**: the former "C++/oracle DTI record discrepancy" is
   **CLOSED** — reproduced with a raw-IEEE-bit dump of every arena's records for
   levels 3-8 from both parsers, byte-identical (see the batch note above), and
   locked by `tests/scripts/golden/L{3..8}_dti.txt`. Remaining known divergences:
   incomplete decoy/`AlienTarget` targeting; opcode 207 — C# `MathF.Sign` throws
   on NaN (C++ returns 0; exceptions are banned here, accepted divergence). (The
   former "L5 `XG[004]` divergence ~t577" is CLOSED — see the batch note above.)
5. **Old 3DS hardware timing: UNMEASURED.** A devkitARM `.3dsx` is not
   hardware validation. Never claim an OG pass without a capture.
6. **C3D depth state diverges from the oracle (review-8 finding, verified).**
   The C# renderer depth-tests and depth-writes mesh geometry in the original
   look: `enable_depth_test = geometry == Mesh && pass not Overlay/OnTop`,
   `enable_depth_write = Mesh && !blended && pass != OnTop`, compare
   LESS_OR_EQUAL (`src/Mdk.Engine/Render/Renderer.cs:603-608` — note the
   renderer oracle lives in `src/Mdk.Engine`, not `src/Mdk.Game`). The C3D
   backend disables the depth test outright (`mdk_3ds/src/render/renderer_c3d.cpp:624`,
   `C3D_DepthTest(false, GPU_ALWAYS, 0)`), and its comment's claim that "the
   C#'s original-look path" draws back-to-front without depth is REFUTED by
   the oracle above. The fix is NOT a one-line flip: PICA's depth range is
   [-1,0] (opposite of D3D's [0,1]), the comment documents an observed
   invisible-geometry trap from a desktop-style clear, and AGENTS.md P6
   requires a hardware/emulator capture for render-path changes (no emulator
   is available in this environment). The next render pass must map the C#
   LESS_OR_EQUAL semantics onto PICA (projection remap at renderer_c3d.cpp:496
   + `C3D_DepthMap` + test function + clear value) and verify on hardware or
   a working emulator before claiming it.
7. **DepthLayer/DepthPull unported (review-8 finding).** The C# pairs
   `Layers.Of` (`src/Mdk.Game/Level/Layers.cs`) with `DepthPull.cs` to push
   per-triangle depth layers (a poster over its wall); `render_types.h`
   documents `DepthPull`/`depth_layer` but no C++ code computes or applies
   them. Gated on item 6 (they are depth-buffer semantics) and on the textured
   renderer path.
8. **Doc/spec drift (review-8 observations, not code).** `AGENTS.md`/`SPEC.md`
   cite `docs/DOCTRINE.md`, which does not exist; SPEC §9.4 lists a
   `gates_romfs` gate that was never written. Both are documentation/spec
   corrections, not porting work — flag to the owner rather than inventing a
   gate or doctrine text unilaterally.


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
