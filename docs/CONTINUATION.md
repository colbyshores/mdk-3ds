# Phase 2 continuation checkpoint

Last updated: 2026-10-25 (ralph loop bootstrap). This file is the persistent
handoff for every fresh context window. Update it at the end of every batch.

## Current tree state (VERIFIED)

- Branch `main`, HEAD `7a2fc27`. Uncommitted work-in-progress (group-state
  slice) in: `docs/OPCODE_COVERAGE.md`, `mdk_core/src/formats/{arena.h,arena.cpp,mto.h,mto.cpp}`,
  `mdk_core/src/game/bsp.{h,cpp}`, `mdk_core/src/render/arena_scene.cpp`,
  `mdk_core/src/scripts/script_runtime.{h,cpp}`, `mdk_core/src/scripts/script_vm.cpp`.
- **THE TREE DOES NOT COMPILE.** `make -C tests/scripts` fails at
  `script_runtime.cpp` `setGroupState`: dead code after `return;` redeclares
  `triangleFlags` and references an undeclared `state` (lines ~988-996).
  This is the first task: fix by rewriting `setGroupState` per the C# below.
- No test run has passed since the group-state edits began. Do not trust any
  earlier "all green" claim.

## Task 1 — finish the group-state slice (exact C# semantics, verified)

Oracle files: `src/Mdk.Game/Level/TriangleGroups.cs`,
`src/Mdk.Game/Scripts/ScriptRuntime.cs`, `src/Mdk.Game/Scripts/ScriptVm.cs`.

1. **`TriangleGroups.SetState` (TriangleGroups.cs:90-123)**: the group's state
   is a STORED per-group value (`Group.State`), NOT derived from triangle bits.
   - Requires the group to EXIST (a triangle with that top-byte number exists)
     and `number != 0`; otherwise no-op. Group numbers are the full top byte
     (1..255), not limited to 16.
   - Operation on the stored state: 0 → `|Hidden|NotSolid`; 2 → `|Hidden`;
     3 → `&~Hidden`; 4 → `|NotSolid`; 5 → `&~NotSolid`; any other → `None`
     (clears both bits). Hidden=0x10, NotSolid=0x20.
   - Then write `(flags & ~(Hidden|NotSolid)) | state` to EVERY triangle of the
     group and store the state.
   - C++ fix: store per-group state on `Arena` (e.g. `groupStates_` map next to
     the existing `groupMaterials_`), add a `hasGroup()` existence check, and
     rewrite `ScriptRuntime::setGroupState` accordingly. The current C++
     version wrongly derives state per-triangle and clamps groups to ≤16.
2. **Load-time `Disable` (TriangleGroups.cs:52-81)**: when an arena is
   registered, every triangle with flag bit `0x2` gets `Hidden|NotSolid`
   ORed into its flags IF `ClipFlag == 0`. Retail arenas always have
   ClipFlag 0 (only `BetaDemo.cs:319` sets it; C++ `beta_demo.cpp:624` sets it
   AFTER `Arena::parse`, so the conversion must NOT live inside `Arena::parse`
   — apply it where arenas are registered for play, after clip flags are set).
   This is a real fidelity gap: LEVEL8 GUNT_2's invisible diagonal wall.
3. **`GetKurtFloorGroup` (ScriptRuntime.cs:410-414)**: `Raycast(kurt+(0,0,1),
   kurt-(0,0,10))`, return hit group or 0. C# `Raycast` (ScriptRuntime.cs:2886)
   uses **SegmentMode.Any + Clip.PassesThrough** over `RayArenas()`: all arenas
   if `CurrentArena` empty, else `{Current, Second}` — Second is included
   **regardless of SecondActive**. The current C++ `raycastGroup` diverges:
   it uses `kSegmentFloor` and requires `secondActive_`. Mirror
   `raycastPoint` exactly (kSegmentAny, second unconditional, all-arena
   fallback) with `Bsp::segmentWithGroup`. Then implement
   `getKurtFloorGroup` with it (currently returns 0 always, which makes
   opcode 194 a silent no-op).
4. **`HitGroup` (ScriptRuntime.cs:1909-1960)** — declared in
   `script_runtime.h`, NOT implemented. Semantics: group outside 1..16 → 0;
   `flags = GroupHitFlags[group-1]`; if `flags & kind`: destructible(0x80) →
   SetState(Show=3); always(0x40) → `amount = max(amount,1)`; stops(0x20) →
   result |= 2. If `GroupHitScripts[i]==0 || ((GroupHitMasks[i]&kind)==0 &&
   !always)` → return result. Else `GroupCounters[i] += amount`, then run the
   script on the arena's `HitScripts` scratch object (HitType=weapon,
   HitEvent=0, WaitTime=0, gosub stacks cleared, Restart=script, Vm.Run).
   Implement it or remove the declaration and record the deferral with the
   subsystem it waits on (needs the VM scratch-object run path).
5. **`SetGroupTexture` (ScriptRuntime.cs:1983 → TriangleGroups.SetMaterial)**:
   requires group exists and `number != 0`; stores the group's material.
   `Arena::setGroupMaterial` exists but only guards group==0 — add the
   existence check. `ScriptRuntime::setGroupTexture` is an empty stub; wire it.
   NOTE: `buildArenaScene` reads `groupMaterial` only at scene-build time;
   dynamic retexture/hide visibility needs a scene rebuild — record as an
   open rendering-integration item, do not fake it.
6. **Renderer skip parity (LevelView.cs:126,294)**: C# skips drawing triangles
   with `flags & (ClipFlag | 0x2)` and groups whose stored State has Hidden.
   `buildArenaScene` does neither; add both at build time.
7. **Audit the other uncommitted opcodes against C# before trusting them**:
   245 `find_object` (mode-2 uses object flag 0x1000 — verify against C#
   `FindObject`, which the earlier audit said needs Items/FlagThrown state),
   246 `turn_to_linked`, 250 `if_in_box`, 87 `kurtLooksAt` (constants vs
   ScriptRuntime.cs:2701), movement commands 6/15 (does C++ ObjectMotion
   actually implement commands 6 and 15? if not, they are partial), 207
   (verify C# `Branch` behavior when `Action` is null vs the new
   `Kind::None` early return), 62 (matches C# ScriptVm.cs:691).
   VM group cases 98/99/140/162/163/168/194 match C# operand layout
   (`GroupIndex = (group-1)&15` masking verified at ScriptVm.cs GroupIndex).

## Task 2 — coverage recount

Recompute opcode coverage from the exact C# `Execute` method span vs the C++
`ScriptVm::execute` span; reconcile `docs/OPCODE_COVERAGE.md` (its counts are
untrusted). Classify: complete+validated / insufficiently tested / partial /
stubbed / missing / out-of-scope-with-evidence. A dispatch case that calls a
stub is NOT implemented.

## Task 3 — known open divergences

- L5 `XG[004]` golden divergence around tick 577 (root cause unknown;
  `findCoverSpot` did not explain it).
- C++/oracle DTI record discrepancy; incomplete decoy/`AlienTarget` targeting.
- Original Old 3DS hardware timing: UNMEASURED. A devkitARM build is not
  hardware validation. This stays open until measured on Old 3DS hardware.

## Verification commands (run after every batch)

```
make -C tests/scripts        # builds + goldens ("All cases match the oracle")
bash tools/check.sh          # the six contract gates
make -C mdk_3ds              # devkitARM target (mdk.3dsx)
```

Rules: `src/Mdk.Game/**` is read-only oracle. Keep the monolithic
`ScriptVm::execute()` switch (dispatch investigation resolved). Never weaken
a gate or test to pass. Commit per verified batch.
