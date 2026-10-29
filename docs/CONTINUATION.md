# Phase 2 continuation checkpoint

Last updated: 2026-10-28 (opcode 174 ammo condition).
Current coverage: 211 true C++ dispatches / 238, 27 remaining. Every remaining
opcode needs a deferred subsystem; the next increment is P5.0, not another
blind opcode port.

This is persistent handoff. Keep it under ~150 lines; batch narrative goes in
`docs/PROGRESS_LOG.md`.

## PRIORITY ORDER (owner directive 2026-10-25)

1. **DONE — mixer call-sites** (`ObjectMotion.cs:133`, `ObjectBehaviors.cs:90-115`).
2. **DONE — `Debris.Shatter` + opcodes 137/138/139**, dump line and oracle
   goldens. BreakUp/Trail/Updraft await Effects/Fans.
3. Continue opcode conversion subsystem by subsystem; tests/goldens in the same
   batch. Opcodes 208 (reachability probe), harness + 13, parts 32/129,
   rope-swing 226/242, spawn 156/206, push-hit-dir 81, stats 217, vision 243,
   value slots 199/210, Kurt/screen 215/240/244, no-ops 239/77, object
   avoidance 204, turn-and-jump 229, floor-below 121, no-flat-floor 192,
   hit-weapon 195, screen-shake 135, inherit-Kurt-velocity 170, sky-mode
   202, camera-track 203, place-near-Kurt 220, gun-aim 219 and ammo 174 are
   implemented and pinned. The Linux frontend now constructs `ScriptRuntime`,
   binds the live `Player`, runs its 30 Hz script clock after each 60 Hz
   player step, and consumes `camera_track`/the scripts' current arena in the
   follow camera. That completes 170/219/220 and the follow-camera half of 203.
   Opcode 202's backdrop/snapshot consumers and opcode 203's snapshot
   consumer remain deferred. All three oracle `Linked` writers
   (245/179/228) are ported, so partner pairs can feed the 213/214
   conditions. 132/136 still need Effects; frame-blend integration is a
   separate future slice. The exact remaining 27 are in
   `docs/OPCODE_COVERAGE.md`; each needs a deferred subsystem, so opcode
   conversion is exhausted for the C# oracle's currently supported behavior.
4. **P5.0 — frame-golden harness** (`SPEC-COMPLETION.md` §3 item 4) before
   renderer batches. The reference source is DECIDED: the C# renderer,
   headlessly under Linux/lavapipe (see `SPEC-COMPLETION.md`). The harness
   itself is not built; no renderer batch until its gate is green.

Deferred: 248 `push_kurt` is an honest no-op pending Kurt knock-back. The
C#'s Rides subsystem also writes the alarm countdown and remains deferred with
Rides. Opcode 135's follow-camera consumer, opcode 202's renderer backdrop and
snapshot consumers, opcode 203's snapshot consumer, opcode 195's group-hit/
direct combat writers and opcode 81's hit-direction writers remain partial
with those integrations. Old-3DS timing remains UNMEASURED.

## Current tree state (VERIFIED)

- Opcode 174 (`if_ammo`) now reads `Kurt`'s super-chain-gun tick count and
  five sniper-ammo entries. Kind 0 selects the timer, kinds 1-5 select the
  entries, and any other kind reads zero. The fields and their test seam are
  present, but pickup collection and chain-gun/sniper combat writers are not,
  so integrated play sees zero and the opcode is partial.
- The Linux frontend now loads `LEVEL3.CMI` and the level sound bank,
  constructs `ScriptRuntime` with the live `Player`, and runs it after Kurt
  moves and before the camera, matching `Viewer.cs:505-535`. The runtime
  keeps the C#'s separate 1/30-second tick clock, so the frontend's 1/60-
  second calls complete a script tick every second call. A fresh runtime is
  built on respawn, as the C# level reload does.
- The follow camera now consumes the runtime's `cameraTrackPitch` while its
  two-tick request lives and otherwise uses the scripts' current arena's
  pitch (`Viewer.cs:746-747`). The collision world's solid arena is refreshed
  after the camera step, as in the oracle.
- `FrontendTickCadenceCopiesLivePlayer` pins the 60 Hz call cadence, the
  30 Hz tick boundary and the live Player copy. The existing gun-aim,
  place-near-Kurt, camera-track and inherited-velocity tests remain green.
- Opcode 170 now has its live `Player` source in integrated play; 219/220 now
  receive the observed velocity written from that Player. The three are
  classified complete. 203 remains partial only because its save/load
  snapshot consumer is deferred. Review-21's single-arena caveat is recorded
  below: the frontend refresh matches the oracle's timing, not its full
  `SolidArenas` list.
- Review-20's FP correction: VFPv2 `VMLA.F32` is fused and GCC emits it even
  with `-ffp-contract=off`; the TU is byte-identical (161 `vmla`/`vmls`, 0
  `vfma`). ARM11 VMLA-versus-C# rounding remains UNMEASURED.
- Coverage: C# 238, C++ 213 labels / 212 unique / 211 true dispatches, 27
  remaining; classification is 16 partial, 195 complete.
- Verification on this tree:
  - `make -C tests/scripts clean && make -C tests/scripts` exited 0 after 47
    VM tests and the complete oracle suite (`All cases match the oracle.`,
    354 golden cases).
  - `bash tools/check.sh` exited 0 (`CHECK: ALL GATES PASSED`); SMDH was
    skipped because no CIA was built.
  - `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C
    mdk_3ds` exited 0, linked `mdk.elf`, staged 69 RomFS files (79M), and
    built `mdk.3dsx`.
  - `cmake --build build-linux --target mdk -j8` exited 0. A 120-frame
    headless run with `DISPLAY=:99` and `--seed 1` exited 0 and reported the
    script assets/runtime ready.
  - `MDK_DATA_DIR=<repo>/MDK tests/scripts/build/tick1 probe 30000` exited 0
    with no unimplemented opcodes on levels 3-8.
- No golden changed. The runtime's combat/inventory `Kurt` is still separate
  from the movement `Player`, so health/items/object rendering and the 3DS
  frontend remain deferred. Ordinary C++ runs use a fresh seeded legacy RNG
  because .NET's unseeded modern generator is not ported; `--seed` makes runs
  reproducible, but one seed is reused for every respawn in a process, unlike
  the C#'s fresh unseeded generator per level load. Review-21 also records two
  frontend divergences: the post-camera solid-arena refresh pins only the
  scripts' current arena rather than the C#'s current-plus-active-second
  `SolidArenas` list, and scripts continue during the 4-second death beat
  rather than ending with the level as the C# does. No emulator or hardware
  run was performed; Old-3DS timing is unmeasured.
- Next increment: P5.0 frame-golden harness. All 27 remaining opcodes need a
  deferred subsystem (Effects, Fans, Rides, SniperRounds, teleport, HUD,
  wind, waypoints, arena preload, pickups/combat writers); do not blind-port.

## Testability and standing open items

- Oracle/goldens compare fixed table + script fields, not velocity,
  `alarm_ticks`, `Items.Bomb`/`Decoy`, or all combat/effect state. Use
  `vm_tests` for otherwise unreachable opcode behavior; don't port without a
  test. The tick corpus matches byte-for-byte through t30000 on levels 3-8.
- Oracle driver: `/tmp/kilo/oracle/Driver/Program.cs`; run with
  `DOTNET_ROOT=/tmp/kilo/dotnet10`, `MDK_DATA_DIR=<repo>/MDK`, driver args
  `L T 1 script`. Source oracle `src/Mdk.Game/**` is read-only. To call a
  runtime method directly (the 219 route), copy `src/`, `assets/` and
  `Directory.Build.props` to a scratch tree plus `src/Mdk.Engine/obj/shaders`,
  and build with `NUGET_PACKAGES=/home/coleshores/.nuget/packages dotnet build
  -p:PrecompiledShaders=spirv`; the repo's own C# `obj/` is not writable here.
- The Linux frontend hardcodes level 3 in three places; a future level
  argument must replace all three.
- Dynamic scene refresh exists at scene level; no GameEngine exists to wire
  refresh on opcode 140. Do not fake call-site wiring.
- Known divergences: incomplete decoy/`AlienTarget` target selection; opcode
  207's `MathF.Sign(NaN)` exception is accepted because C++ exceptions are
  banned; degenerate-path guards return 0 rather than C# out-of-range behavior.
  The Linux frontend now runs scripts, but its runtime `Kurt` combat/inventory
  state is not yet the live `Player` state, and the 3DS frontend has no
  `Player`/runtime binding yet.
- C3D depth state still differs from C#: depth test disabled in C3D versus
  LESS_OR_EQUAL depth test/write in C#. Resolution needs projection remap,
  `C3D_DepthMap`, test function and clear value plus capture. DepthLayer/
  DepthPull are unported and depend on the textured renderer path.
- Old 3DS hardware timing UNMEASURED. Never claim OG readiness without actual
  Old 3DS capture. Emulator GUI runs must use explicit `DISPLAY=:99` and save
  captures under `/tmp/kilo`.
- Documentation drift noted earlier: `AGENTS.md`/`SPEC.md` cite absent
  `docs/DOCTRINE.md`; SPEC §9.4 names an unwritten `gates_romfs`. Flag to owner,
  do not invent policy/gates unilaterally.

## Verification commands

```
make -C tests/scripts clean && make -C tests/scripts
bash tools/check.sh
DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds
```

Tests do not track header dependencies: run `make -C tests/scripts clean` after
header changes. Keep the monolithic `ScriptVm::execute()` switch. Never weaken
a gate or test.
