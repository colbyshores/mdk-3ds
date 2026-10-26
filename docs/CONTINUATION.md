# Phase 2 continuation checkpoint

Last updated: 2026-10-25 (iteration 17: the audio slice is ported —
`mdk_core/src/audio/` (SoundBank, SoundMixer voice table, SoundDevice control
plane), opcodes 89 `play_sound`, 107 `set_loop_sound`, 233 `if_sound_playing`,
249 `if_own_sound`, and the goldens now pin an `AUDIO` voice-table line plus each
object's tracked/loop voice (`tsnd/tvoice/tplay/lsnd/lvoice/lplay`). The corpus
still matches the oracle byte for byte to t30000 on every level, table AND
script.)
This file is the persistent handoff for every fresh context window. Keep it under
~150 lines: current state + priorities only; append batch narratives to
docs/PROGRESS_LOG.md (archived, not loaded by default).


## PRIORITY ORDER (owner directive 2026-10-25 — port code, goldens follow)

The owner's directive: churn through the C#→C++ conversion. Port subsystems
in this order; extend goldens in the same iteration as the code that unlocks
them. Do not spend an iteration on docs/tests alone.

1. **DONE — wire the mixer's remaining call-sites.** The motion frame-sound
   trigger (`ObjectMotion.cs:133`) now calls `playSound` before clearing
   `frame_sound`, and `updateDoor` plays `door_sounds[0..3]` at the same
   transitions as `ObjectBehaviors.cs:90-115` (empty/`"NONE"` names skipped).
   Verified 2026-10-25: `make -C tests/scripts` → "All cases match the
   oracle"; `bash tools/check.sh` → ALL GATES PASSED; `make -C mdk_3ds`
   links. The corpus did not change, so no golden regeneration was needed.
   Adversarial review (2026-10-25) found no semantic divergence from the
   C#; its MAJOR — that the goldens cannot observe either call — is
   rebutted with evidence: the corpus matched the oracle byte-for-byte
   both BEFORE this wiring (C++ played neither sound) and AFTER it, so
   neither call produces a voice in levels 3–8 to t30000. The wiring is
   verified by direct semantic diff against `ObjectMotion.cs:133` and
   `ObjectBehaviors.cs:90-115,418-424` (same indices, same `NONE` rule,
   same call-then-clear order), not by the corpus.
2. **DONE — `Debris.Shatter` + opcodes 137/138/139.** `mdk_core/src/scripts/
   debris.{h,cpp}` ports `Debris.cs` (Shatter/Spark/Update, pooled pieces);
   the runtime carries `ShatterPoint`/`ShatterDirection` and wires the Ray
   and GroupFacets delegates. The script dump has a `DEBRIS` line on both
   sides; the oracle driver now registers its triangle groups as the
   viewer's `LevelView` does (it never did, so Shatter was a no-op in every
   golden), and all 340 goldens were regenerated from the oracle. Verified
   2026-10-25: `make -C tests/scripts` → "All cases match the oracle" (L5's
   138 at t482 produces 134 pieces, pinned by the DEBRIS line);
   `bash tools/check.sh` → ALL GATES PASSED; `make -C mdk_3ds` links;
   Linux `Built target mdk`. Coverage: C++ 171/238, remaining 68.
   BreakUp/Trail/Updraft stay with their owning slices (see `debris.h`).
3. Continue the remaining opcodes subsystem by subsystem, same rule.
   **Opcode 208 (`if_part_hit`) DONE** — the reachability probe
   (`tests/scripts/tick1 probe 30000`, a new permanent harness mode) showed
   it was the only remaining opcode firing anywhere in levels 3–8
   (L5 ×9134); it is ported and the probe is now clean on every level.
   Coverage: C++ 172/238, remaining 67. The other 67 do not fire in this
   corpus (133 `AnimatedTextures` and the Effects spawns were probed to
   t30000 and never trigger), so per the no-blind-port rule they wait for
   a synthetic per-opcode test harness (open item 2) or a corpus that
   exercises them. Next verifiable work is therefore P5 (golden-frame
   tests for the Vulkan renderer) or that synthetic harness.

Deferred with evidence: alarm (move_command 15 never fires in the corpus —
verified 0 occurrences), 240/241/244 (Kurt's state exists in `game/kurt.h` but
none fires in levels 3-8 — do not blind-port), 248 `push_kurt` (honest no-op:
`state=0` in every golden). Old-3DS hardware timing stays open (hardware-only).


## Current tree state (VERIFIED)

- The audio slice is implemented and committed (032f999). Verified on the final
  tree:
  - `make -C tests/scripts` → `All cases match the oracle.` — 341 PASS, 0 SKIP,
    including the 170 regenerated script goldens with the `AUDIO` line and the
    per-object audio fields. Six goldens (L3/L5/L6/L8/L4 spread) were re-diffed
    against a freshly run oracle binary and match byte for byte.
  - `bash tools/check.sh` → `CHECK: ALL GATES PASSED` (`gates_smdh` SKIPs: no
    built `.cia`)
  - `make -C mdk_3ds` (with `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM`)
    → links `mdk.elf`, builds `mdk.3dsx`
  - `cmake --build build-linux --target mdk` → `Built target mdk` — both targets
    build the changed core.
- Corpus frontier: every level 3-8 matches the oracle byte for byte (table AND
  script) to t30000. The tick axis is spent; what is left is subsystem depth.
- The oracle driver (`/tmp/kilo/oracle/Driver/Program.cs`, outside the repo; run
  with `DOTNET_ROOT=/tmp/kilo/dotnet10`, `MDK_DATA_DIR=<repo>/MDK`,
  `bin/Debug/net10.0/mdkoracle L T 1 script`) now emits the `AUDIO voices=N
  list=id:NAME,...` line (via reflection on `_voices`) and the per-object audio
  fields; `tick1.cpp` `dumpScript` mirrors both. Regenerate script goldens with:
  for each `{L, T}` in the `cases[]` table,
  `mdkoracle L T 1 script > golden/L{L}_t{T}_script.txt`.
- The headless harness never calls `SoundMixer::update` (neither does the C#
  driver), so in the corpus a voice ends only when stopped — L3's voice table
  fills to the 64 cap with RICO1 on both sides. The per-frame 3D update and the
  device's frame-consumption lifecycle are ported but only the headless half is
  golden-verified; the lifecycle lands with the audio backend's own checks.
- `docs/OPCODE_COVERAGE.md` recounted from the brace-matched sets: C# 238 / C++
  168 / remaining 71 (the doc had also been stale on 115, ported at iter 11).
  Reachability (to t600): of the remaining set only **138** fires in levels 3-8.


## Testability map (why the remaining opcode work is gated, not merely deferred)

The goldens compare a FIXED field set (`tick1.cpp` `dumpTable`/`dumpScript`):
pos, yaw, hp, flags, dead, move_command/parameter, connects, path/time/speed/stop,
script_flags, variables, restart, wait, death_script, command_target, gosub stacks,
level_timers, global vars, target_position/target_yaw/alien_target (iter 7), Kurt's
combat/inventory state (iter 14), and the mixer voice table + tracked/loop voices
(iter 17). They do **not** observe: velocity, `alarm_ticks`, `Items.Bomb`/`Decoy`,
or debris. The project rule (OPCODE_COVERAGE.md) forbids porting an opcode without
a test that exercises it, so each remaining opcode lands WITH the dump field that
makes it observable — the AUDIO/KURT lines are the template: port the subsystem
state, dump it, regenerate, implement the opcode, verify.

`alarm_ticks` is a dead end in this corpus (move=15 setter fires 0 times).
`Items.Bomb`/`Decoy` land with the Items slice. Debris state lands with the
shatter slice (priority 2).


## Open items (do not lose these)

1. **Dynamic scene updates — DONE at the scene level.** `buildArenaScene` tags
   each batch with its group; Hidden is a draw-time check; `LevelView::refresh()`
   rebuilds on `group_set_texture`. **Remaining:** the `GameEngine` that runs the
   scripts must call `refresh()` when a `group_set_texture` fires (no engine
   exists yet); the VM state and scene mechanism are both tested, only the
   per-opcode call-site wiring is pending. Do not fake it.
2. **Opcode classification — DONE** in `docs/OPCODE_COVERAGE.md` §"Owned-case
   classification": 0 stubs, 10 partial, 158 complete (of 168). Per-opcode unit
   tests beyond the goldens are still needed.
3. **Known divergences**: the DTI record-layout question is CLOSED (raw-bit
   goldens). Remaining: incomplete decoy/`AlienTarget` targeting; opcode 207 —
   C# `MathF.Sign` throws on NaN (C++ returns 0; exceptions banned, accepted).
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
