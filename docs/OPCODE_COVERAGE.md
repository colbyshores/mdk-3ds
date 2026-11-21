# Script VM opcode coverage audit (Phase 2 §3.1)

Method and reachability evidence in the final Phase 2 report. Summary:

- **Town timer, shake, sky modes, group textures (2026-11-20, PORT_GAPS package 15):** no
  opcode added (the count stays C# **238**, C++ **238**); three dispatches gain the consumers
  they were partial for. 135 (`raise_573aa8`): `ScriptRuntime::raiseShake` now raises the
  C#'s ShakeRaised, which the follow camera takes (`FollowCamera::raiseShake` / UpdateShake /
  Turn) -- **complete**. 140 (`group_set_texture`): the arena is recorded
  (`retexturedArenas`) and both frontends rebuild its view (`ArenaViews::refresh` ->
  `LevelView::refresh`, the C#'s LevelView.Rebuild) -- **complete**. 202 (`set_574304`): both
  frontends draw their backdrop from `skyMode` (`game/sky_modes.h`, the C#'s SkyModes in
  Viewer.Background) -- partial now only for the save/load snapshot's `sky_mode`, as 203 is.
  Pinned end to end by `golden/L3_screen_hmo3.txt` (HMO_3's 202 -1, its XG's 135 2, the 140s
  on groups 2 and 9-12), `L4_screen_second.txt` (MEAT_6's 202 1) and the town goldens
  (`L3_screen_town`, `L5_screen_last`), byte-identical to `tests/scripts/screen_probe`.
  Retail sites (the C++ decoder over every reachable script): 140 only in L3 HMO_3 (groups 2-12,
  all to -1027, the glass); 202 in L3 HMO_3/HMO_4/HMO_10/CHMO_9, L4 MEAT_3/5/6/7/10/CMEAT_5,
  L6 OLYM_3, L7 DANT_3/4/5/CDANT_3/4; 135 in every level.

- **Bullet holes (2026-11-20, PORT_GAPS package 16):** 130 (`special_130`) ported:
  `ScriptRuntime::stampBulletHole` (scripts/bullet_holes.cpp, BulletHoles.cs whole), pinned
  end to end by `golden/L8_holes_guntam.txt` and `golden/L6_holes_xb2.txt` (sniper rounds hit
  L8 GUNT_10's XGUNTAM and L6 OLYM_10's XB2, whose arena-script loops gosub special_130 on
  `if_hit_part ANY`; the retail sites are L5 MUSE_2/MUSE_5, L6 OLYM_10 x2, L8 GUNT_10 x2).
  Recount (numeric `case` labels of both VMs, set difference), after package 8's 224:
  C# **238**, C++ **238**, remaining **0** -- every C# VM case is dispatched.

- **Sniper frontends and HUD (2026-11-20, PORT_GAPS package 7):** no opcode: the scope's view,
  sniper mode's screen and the round cameras are frontend and HUD work. Counts unchanged.
- **Sniper rounds (2026-11-20, PORT_GAPS package 6):** 28 (`bomb_follow_path`) ported:
  `SniperRounds::guideLastMortar` (scripts/sniper_rounds.cpp), pinned end to end by
  `golden/L7_rounds_mortar.txt` (LEVEL7 DANT_6's group 12 hit script guides a mortar round
  along path 109157). The rounds are also the last writer of 81's `hit_direction` and 195's
  `hit_type` (weapon types 0-4) that was missing. Recount after package 8 too: C# **238**, C++ **237**, remaining
  **1** (130).
- **Updrafts, slide and wind (2026-11-20, PORT_GAPS package 8):** 224 (`wind_zone`)
  ported, after package #3's 114; with package 6 (28): C# **238**, C++ **237**, remaining **1** (130). Details under
  "Remaining" below.

- **Menus and settings (2026-11-20, PORT_GAPS package 20a):** no opcode added or
  changed; the counts below stand. What changed is an INPUT the opcodes read:
  `ScriptRuntime::option` (if_option and opcode 129's gore arm) is now the settings'
  Gore (`Option = ui.Settings.Gore ? 1 : 0`, Viewer.cs:234) on both frontends, still on
  by default, and Kurt's difficulty (the town timer the scripts read) has its writer
  (`Kurt::setDifficulty` from the settings, Viewer.cs:231).
- **Combat and hazard batch (2026-11-20, PORT_GAPS package 4):** 158, 159, 167,
  172, 178 and 184 ported, with the consumer of 164's flag; recount (top-level
  `case` labels of both brace-matched spans, set semantics): C# **238**, C++ **234**,
  remaining **4** (28, 114, 130, 224), recounted after rebasing onto the end of a level
  (131). Details under "Remaining" below.
- Counts recounted from the brace-matched method spans (C# `ScriptVm.Execute`
  vs C++ `ScriptVm::execute`, numeric `case` labels, set semantics): the C#
  owns **238** cases, the C++ has **226** unique labels (225 true dispatches — one
  `case 0:` is opcode 249's inner-switch arm, not a dispatch), leaving **13**
  unimplemented. The
  earlier version of this document claimed 155/83 and listed 114, 171 and 209
  as ported; none of the three is in the C++ `execute()` — the old counts and
  the old "ported" list were wrong. The remaining cases fall through to
  `noteUnimplemented` (decoded, no-op, scripts stay in sync).
- The reachability probe (C++ `unimplemented()` after running each level to
  t30000) was rerun after the ammo-condition batch: none of the remaining opcodes
  on that list fires in levels 3-8, so the probe is clean on every level. The
  synthetic `vm_tests` harness is the route for those opcodes.
- **Parts batch (2026-10-26):** 32/129 ported (`script_vm.cpp`, reusing
  `partsMask`/`findPart`/`locked_parts`/`option`); the brace-matched recount
  is C# **238** / C++ **175** / remaining **64**. The C++ count carries the
  same nested-switch artifact as the 173 before it: the 175th label is the
  `case 0:` mode arm *inside* opcode 249's switch, not a dispatch — true
  dispatches are 174, and 174 + 64 = 238. Verified by recount script both
  sides; the remaining list below is the exact set difference.
- **Rope-swing batch (2026-10-26):** 226/242 ported (`script_vm.cpp`, pure
  object-state over the existing swing/rope fields); the recount is C#
  **238** / C++ **177** / remaining **62** (true dispatches 176;
  176 + 62 = 238). 226/242 never fire in levels 3-8, so they land through
  `vm_tests` like the parts pair (no C# counterpart exists to twin — the
  header comment records this). Opcode 85 was deferred until the object
  rotation helper landed in iteration 41.
- **Spawn/parts-to-push batches (2026-10-26):** 156 (`spawn_at_point`), 81
  (`push_hit_dir`) and 206 (`spawn_along_path`) ported through `vm_tests`;
  recount C# **238** / C++ **180** / remaining **59** (true dispatches 179,
  as §"Counting note" records). Count reconciled 2026-10-26: the header at
  the top of this file had gone stale at 177/62 while the batches landed.
- **Stats slice (2026-10-26):** 217 (`global_573c4c_add`) ported through
  `vm_tests` with the new `GameStats` struct; recount C# **238** / C++
  **181** / remaining **58** (true dispatches 180).
- **Value-slot batch (2026-10-26):** 199 (`set_104`) and 210 (`set_2c0`)
  ported through `vm_tests`; at the time, recount C# **238** / C++ **184** /
  remaining **55** (true dispatches 183). Both are the C#'s own unidentified
  object slots (`ScriptVm.cs:1663-1668`): the write IS the oracle behaviour,
  the `MdkObject` fields already existed (`object.h:136-137`), and nothing in
  C++ reads them yet. Neither fires in levels 3-8; goldens unchanged.
- **Kurt/screen batch (2026-10-26):** 215 (`screen_flash`), 240
  (`set_hurt_flash`) and 244 (`set_global_573bd4`) add explicit Kurt fields and
  setters, pinned by `vm_tests`; these do not fire in levels 3-8. Their visual,
  knock-down and timer consumers remain partial subsystems (see table below).
- **Opcode 239 batch (2026-10-26):** explicit C# no-op `set_27c` is now a C++
  no-op case; a synthetic test supplies all six F32 operands, checks the
  following instruction executes, and verifies it is not counted unimplemented.
- **Opcode 77 batch (2026-10-26):** `debug_msg` is an explicit no-op in the
  C# VM. C++ now dispatches it as a no-op; `vm_tests` supplies its U8 and
  Pascal-string operands and checks the following instruction executes and
  opcode 77 is not reported unimplemented. Current recount: C# 238, C++ 188
  true dispatches, 50 remaining (before opcode 85).
- **Debris slice (2026-10-25):** 137/138/139 (`shatter_group` family) are
  ported with `mdk_core/src/scripts/debris.{h,cpp}` (a 1:1 port of
  `Debris.cs`: Shatter/Spark/Update, pooled pieces, seeded-RNG order) and
  the runtime's `ShatterPoint`/`ShatterDirection` state. The script dump
  now carries a `DEBRIS pieces=... list=...` line on both sides (oracle
  driver + `tick1.cpp`), and all 340 goldens were regenerated from the
  oracle. **Oracle harness fix (same batch):** the driver built
  `TriangleGroups` but never called `Add` (only the viewer's `LevelView`
  does), so `GroupFacets` always returned null and Shatter was a no-op in
  every golden. The driver now registers every arena, as `LevelView`
  does; with that, L5's 138 (t482) produces 134 pieces and the DEBRIS
  line pins them. BreakUp, Trail (Effects) and Updraft (Fans) remain
  unported with their owning slices; neither perturbs the RNG stream
  (see `debris.h`).
- **Chain-link batch (2026-10-27):** 29 (`spawn_chain`) and 49
  (`release_children`) are ported, together with movement command 30 — the
  chain's own motion (`ScriptRuntime::updateChain`, `ObjectMotion.cs:464`),
  which had no C++ counterpart. The links are numbered after the highest
  instance id of the object's live followers (`LastFollowerId`), the release
  takes the highest-id chain link (`LastChainLink`), and the motion hangs
  each link's reference point 0 at the previous link's reference point 1,
  turning the links 90 degrees apart; the link with id 0 drives the chain
  and a gap in the chain makes the later links let go. Three `vm_tests`
  tests pin it. Recount: C# 238, C++ 198 true dispatches, 40 remaining.
- **Alarm-state batch (2026-10-27):** opcode 27 (`if_alarm`) and movement
  command 15 are ported over the runtime's shared `alarmTicks` countdown. The
  countdown decays once per tick, command 15 sets it to ten for an object in
  Kurt's current arena and plays `ALERT` every 32 ticks, and opcode 27
  branches while the countdown is positive. One `vm_tests` case pins the
  dispatch, arena guard, true/false branch, expiry, and sound cadence.
  Recount: C# 238, C++ 199 true dispatches, 39 remaining.
- **Object-avoidance batch (2026-10-27):** opcode 204 (`avoid_objects`) is
  ported over the existing world-bounds and object-list seams. It turns the
  receiver away from the first live, solid, same-arena object whose strict
  world bounds overlap the receiver's, by at most half a turn times one tick.
  One `vm_tests` case pins the exclusions, both turn directions, first-object
  order, the angle clamp, and the dispatch. Recount: C# 238, C++ 200 true
  dispatches, 38 remaining.
- **Turn-and-jump batch (2026-10-28):** opcode 229
  (`turn_and_jump_to_dest`) and movement command 229 are ported together over
  the existing destination, yaw, velocity, gravity, friction, parameter and
  contact fields. The receiver turns by at most the requested rate until it
  faces the destination, then receives the oracle's ballistic velocity and
  flight time; the flight command stops on floor contact after half of the
  recorded flight time. One `vm_tests` case pins the turn and branch, the arc
  and friction-adjusted speed, the first motion tick, and the floor-landing
  stop. Recount: C# 238, C++ 201 true dispatches, 37 remaining.
- **Floor-below batch (2026-10-28):** opcode 121 (`if_floor_below`) is ported
  over the existing arena raycast seam. It drops a segment of the operand's
  length from the receiver's position and branches when any triangle of the
  oracle's active ray arenas crosses it. One `vm_tests` case pins the
  blocked and clear branches and the operand's drop distance. Recount: C# 238,
  C++ 202 true dispatches, 36 remaining.
- **No-flat-floor batch (2026-10-28):** opcode 192
  (`if_no_flat_floor_at`) is ported over the same raycast seam, now extended to
  carry the selected triangle's geometric normal. The receiver rotates its
  forward/side offset by yaw, probes from one unit above that point down by the
  operand drop plus one, and branches when the ray misses or the hit normal's
  absolute Z is below the flatness limit. One `vm_tests` case pins the flat,
  absent and steep cases, both threshold branches, the drop and one-unit
  clearance, and the yaw rotation. Recount: C# 238, C++ 203 true dispatches,
  35 remaining.
- **Hit-weapon batch (2026-10-28):** opcode 195 (`if_hit_weapon`) is ported
  over the existing `MdkObject::hit_type` field. It compares the receiver's
  last-hit weapon type with the signed-byte operand and branches on exact
  equality. One `vm_tests` case pins negative and positive matches, mismatches,
  sign-crossing mismatches, and the oracle's initial zero state. The C++
  group-hit scratch writer exists in `ScriptRuntime::hitGroup`, but `hitGroup`
  has no C++ caller. The C# group-hit call sites — Kurt's group touch
  (`HitTypeKurt = -11`), the chain-gun group hit and `HitGroupAt` wrapper,
  the sniper group hits, and the Items blast group hit — remain deferred, as do
  the direct object-hit writers for mortars, Items, sniper rounds and the cow.
  Recount: C# 238, C++ 204 true dispatches, 34 remaining.
- **Screen-shake batch (2026-10-28):** opcode 135 (`raise_573aa8`) is ported
  over a new per-tick `ScriptRuntime::shake` maximum. The runtime clears the
  value at tick start and a stronger request replaces a weaker one, matching
  `ScriptRuntime.cs:899,1986-1989`. One `vm_tests` case pins the dispatch,
  the maximum semantics, the fall-through instruction, and the next-tick
  reset. The C# also raises `ShakeRaised` for the follow camera; that camera
  consumer is not ported, so the dispatch is classified partial. Recount: C#
  238, C++ 205 true dispatches, 33 remaining.
- **Sky-mode batch (2026-10-28; review-15's citation NIT and review-16's
  snapshot-consumer NIT fixed):** opcode 202
  (`set_574304`) is ported over the runtime's shared `skyMode` state. One
  `vm_tests` case pins the signed-byte operand, 0/1/other values, the
  following-instruction alignment, and that the mode survives the next tick;
  an unsigned-byte mutation fails exactly the -1 case. The renderer's
  `SkyModes.BackdropOf` and save/load snapshot consumers are not ported, so
  the dispatch is classified partial. Recount: C# 238, C++ 207 true dispatches,
  31 remaining.
- **Camera-track batch (2026-10-28):** opcode 203 (`camera_track`) is ported
  over a new runtime pitch/ticks seam. `ScriptRuntime::cameraTrack` follows
  `ScriptRuntime.cs:1188-1216`: mode 0 uses 70% of the way to the model's
  bounds top, mode 1 uses height × scale, the angle fades over the first 90
  degrees of Kurt's facing error, and the result is clamped to 30 degrees up
  and the arena's rest pitch. The runtime copies a bound `Player`'s feet and
  yaw at tick start, exactly as `ScriptRuntime.cs:886-889`, and decrements the
  two-tick lifetime once per tick. `CameraTrackCalculatesPitchAndExpires`
  pins both modes, the clamp, turn-away falloff, the behind fallback, live
  Player yaw, fall-through, and expiry. The Linux frontend integration below
  constructs the runtime, binds that Player and consumes the follow-camera
  request; the save/load snapshot consumer remains deferred, so the dispatch is
  classified partial. Recount: C# 238, C++ 208 true dispatches, 30 remaining.
- **Place-near-Kurt batch (2026-10-28):** opcode 220
  (`place_x_near_player`) is ported over the runtime's seeded RNG, live object
  list and newly added observed `KurtVelocity` field. The first draw selects
  the random/current/predicted-X branch; only the random branch consumes a
  second draw. An X below the minimum wraps once by the range, then up to
  100 passes move it twelve units left of every live same-arena, same-Y
  object. The runtime's observed velocity is the copied feet delta times 30,
  written before object scripts exactly as `ScriptRuntime.cs:945-946` does;
  it is distinct from the live `Kurt.Velocity` used by opcode 170. One
  `vm_tests` case pins the tick field, all three choices, both range/Y
  fallbacks, the wrap, exclusions, ordered spacing pushes and fall-through.
  The Linux frontend integration below binds its `Player`, so the dispatch is
  complete. Recount: C# 238, C++ 209 true dispatches, 29 remaining.
- **Gun-aim batch (2026-10-28):** opcode 219 (`if_gun_aim`) is ported over the
  runtime's seeded RNG, live object list, `yawTo` seam and observed
  `KurtVelocity` field. `ScriptRuntime::gunAim` follows
  `ScriptRuntime.cs:1084-1134`: the aim is the bearing to Kurt led by 1.67 s of
  observed sideways velocity, jittered by a quarter of a degree; if that
  bearing is inside the 270°±3 cone a roll of 70 against
  round(|vx|) + 10 (halves away from zero) fires at Kurt; otherwise up to four
  live same-arena objects within 2 in Y and 3 in Z of the target row and inside
  the cone are collected and one of their bearings is picked at random; failing
  that the gun faces 270° and fires on 29% of rolls. `GunAimAimsAndFires` pins
  21 outcomes — both fire branches, the raised-target pick and its four-object
  cap, the dead/cross-arena/Y-window/Z-window exclusions, the lead, and how many
  draws each path consumes. Every expectation was reproduced against the C#
  oracle itself (a scratch driver calling `ScriptRuntime.GunAim` over the same
  geometry and seeds; recipe recorded in `PROGRESS_LOG.md`), not from the C++
  output. The Linux frontend integration below supplies the same observed
  velocity from its bound `Player`, so the dispatch is complete. Recount:
  C# 238, C++ 210 true dispatches, 28 remaining.
- **Frontend runtime integration batch (2026-10-28):** the Linux frontend now
  loads the level CMI and sound bank, constructs `ScriptRuntime` with the live
  `Player`, and calls it after Kurt's 1/60-second step and before the follow
  camera, matching `Viewer.cs:505-535`. `ScriptRuntime::update` now keeps the
  C#'s own 1/30-second accumulator (`ScriptRuntime.cs:378-390`) instead of
  treating every call as one tick. The camera uses `cameraTrackPitch` while
  its request lives and otherwise the scripts' current arena pitch; the solid
  arena is refreshed after the camera step, as in the oracle. A respawn builds
  a fresh runtime. `FrontendTickCadenceCopiesLivePlayer` pins the cadence and
  live Player copy. This realizes 170/219/220 and the follow-camera half of
  203. Dispatch counts are unchanged. The runtime's combat/inventory `Kurt`
  remains separate from movement `Player`, and ordinary runs use a fresh seed
  through the ported legacy RNG because .NET's unseeded modern generator is
  not ported.
- **Ammo-condition batch (2026-10-28):** opcode 174 (`if_ammo`) is ported
  over new `Kurt` fields for the super chain gun's remaining ticks and the
  five sniper-ammo entries. Kind 0 reads the super-chain-gun timer, kinds 1-5
  read the corresponding ammo entry, and every other kind reads as zero. One
  `vm_tests` case pins the dispatch, the two state sources, every ammo slot,
  the out-of-range guard, strict/between/outside comparisons, the unknown-
  comparison false branch, and fall-through. The pickup writers and the
  chain-gun/sniper combat writers remain deferred, so the dispatch is
  classified partial. Recount: C# 238, C++ 211 true dispatches, 27 remaining.
- **Kurt-velocity batch (2026-10-28; corrected by review-13, hardened by
  review-14):** opcode 170 (`inherit_kurt_vel`) is ported over a borrowed live
  `Player` pointer. The receiver's velocity gains each component of the C#
  `Kurt.Velocity` source multiplied by the F32 operand. `Player::velocity()`
  supplies that source in u/s and includes the horizontal knock-down push term
  from `Kurt.cs:133`; the runtime reads it at dispatch time rather than caching
  a snapshot. `ScriptRuntime` now requires the Player argument explicitly
  rather than defaulting it to null, so every construction states whether it
  has a live source. One `vm_tests` case pins the dispatch, literal scaling,
  all three components, preservation of existing velocity, a negative scale,
  fall-through, a live Player change, the push term and its drain, and the
  C# `Kurt.cs:221-234` order in which the walk move captures the pre-drain push
  before `DrainPush` runs and before the vertical update consumes the move.
  Kurt movement itself was already ported as
  `mdk::Player`; the Linux frontend integration below constructs
  `ScriptRuntime` and binds that Player, so the dispatch is complete.
  Recount: C# 238, C++ 206 true dispatches, 32 remaining.
- **Trigger-box batch (2026-11-11):** opcode 223 (`arena_set_neighbour`, arena_load
  0x41a2d0) is ported over the runtime's second-arena slot: the operand names an
  arena and it takes the second slot INACTIVE through the same `preloadArena` the DTI
  type-3 trigger box drives (`ScriptRuntime.cs:602-612`), with `""`, `"NONE"` and the
  arena that already *is* the second as the C#'s three no-ops. One `vm_tests` case
  pins the dispatch through the VM, the inactive second, the resident-arena change and
  that a preload does not draw the room; the same case walks level 7's shipped
  `DANT_10` type-1 boxes, which is what executes the show arm and the `id = -1` clear
  arm an opcode alone cannot reach. Recount: C# 238, C++ 212 true dispatches, 26
  remaining.

## Ported so far (verified present in `ScriptVm::execute`)

Earlier Phase 2 batches: 119, 250, 246, 87, 79, 211, 120, 122, 160, 185, 38,
127, 103, 237, 56, 34, 183, 39, 201, 80, 126, 157, 41, 63, 169, 177, 205, 20,
21, 33, 91, 62, 234, 191, 110, 218, 197, 164.

The group-state batch adds: 98, 99, 140, 162, 163, 168, 194 (triangle-group
state, hit flags/scripts/counters, floor-group probe), plus 6, 15, 245, and
re-checks of 87, 246, 250, 62, 207 against the oracle. `find_object` is
factored into a runtime helper to keep enumeration and RNG semantics together.
Per-opcode tests are still needed for edge cases beyond the end-to-end golden
suite; the complete-vs-partial classification of the owned cases is now done
(see "Owned-case classification" at the end of this file).

The object-avoidance batch adds: 204 (`avoid_objects`), over the existing
world-bounds and object-list seams.

The turn-and-jump batch adds: 229 (`turn_and_jump_to_dest`) and movement
command 229, over the existing destination, velocity and contact seams.

The floor-below batch adds: 121 (`if_floor_below`), over the existing arena
raycast seam.

The no-flat-floor batch adds: 192 (`if_no_flat_floor_at`), over the same seam
with the hit triangle's normal.

The hit-weapon batch adds: 195 (`if_hit_weapon`), over the existing
`MdkObject::hit_type` field.

The screen-shake batch adds: 135 (`raise_573aa8`), over the runtime's new
per-tick screen-shake maximum.

The Kurt-velocity batch adds: 170 (`inherit_kurt_vel`), over the runtime's
borrowed live `Player` velocity source.

The sky-mode batch adds: 202 (`set_574304`), over the runtime's shared
`skyMode` state; its renderer backdrop and save/load snapshot consumers are
deferred.

The camera-track batch adds: 203 (`camera_track`), over the runtime's pitch
and ticks state; its renderer follow-camera and save/load snapshot consumers
are deferred.

The place-near-Kurt batch adds: 220 (`place_x_near_player`), over the runtime's
seeded RNG, live object list and observed per-tick `KurtVelocity` field.

The gun-aim batch adds: 219 (`if_gun_aim`), over the runtime's seeded RNG,
live object list, `yawTo` seam and the same observed `KurtVelocity` field.

The ammo-condition batch adds: 174 (`if_ammo`), over `Kurt`'s super-chain-gun
timer and five sniper-ammo entries; its pickup and combat writers remain
deferred. (2026-11-20: the pickups write the ammo since the items port, and
sniper mode's clip now spends it -- `Kurt::useAmmo` -- so 174 sees a sniper
round fired; no dispatch changes.)

The movement-command batch adds no dispatch: it lands the two arms
`ScriptRuntime::updateCommand` was still missing, so **6 (`move_to_target`) is
complete** and `command_objects` command 1 finally moves its followers.
Movement command 6 is `ObjectMotion.Chase` (`ObjectMotion.cs:894`, the runtime's
target 8 above the feet, speed in units per *tick*), and command 1 is
`ObjectMotion.FollowFormation` (`ObjectMotion.cs:929`, the slot in the leader's
frame). Both dispatches were already ported and both silently did nothing: the
opcode-6 row below said so, and command 1's gap was not recorded anywhere. Two
`vm_tests` cases pin them (`ChaseFliesAtTheRuntimeTarget`,
`FormationFollowersKeepTheirSlot`); neither command fires in levels 3-8, so all
460 goldens are byte-identical.

The trigger-box batch adds: 223 (`arena_set_neighbour`), over the runtime's second
arena slot and `preloadArena` -- the same routine the DTI type-3 trigger box drives,
which is why the two landed together. It also lands `checkTriggers`, `loadArena` and
`pullDoors` on the runtime side, and the level-7 walk that pins all three.

The teleport batch adds: 112 (`teleport_player`) and 173 (`teleport_player_keep`),
which are one runtime method (`TeleportKurt`, 0x41bce4) with two operand spellings, so they
land together or not at all. 112 is `[arena, x, y, z, yaw]` and yields only when it named an
arena; 173 is the C#'s `op173_target` operand (already decoded by `readOp173Target`) and
always yields. On the runtime side the batch lands `enterArena`, `teleportSecond`,
`isCorridorLoaded` — the only reader of `_residentArena` — and the `_teleported` first-tick
rule, and it pays the `LoadArena(neighbour)` that batch 139 recorded as owed at
`ScriptRuntime.cs:516`. Pinned by three `vm_tests` cases: the oracle's own
`ArenaSwitchTests.FirstTickKeepsTeleport` and `TeleportMakesUnreachableArenaSolid`, its
`DoorArenaTests.TeleportLoadsCorridorArena`, `TeleportKeepsLoadedCorridor` and
`LoadedArenaPullsItsDoors` (the last of which batch 139 could only reach with built doors),
and a bytecode case for the two dispatches' operand order and yields. Neither opcode fires
in levels 3-8, so the goldens are the ones that decide whether the corpus notices.

## Remaining 24 (verified set)

13 left this list with the synthetic-harness batch (port 13 is an explicit
false branch, `ScriptVmTests.UnimplementedConditionsAreFalse` pins it, and the
C++ `unimplemented()` bucket stays clean). 32 (`clear_parts_mask`) and 129
(`blow_off_parts`) left with the parts batch: pure object-state ops over the
existing parts fields, pinned by three `vm_tests` tests with a synthetic
model (no C# counterpart exists to twin). 226 (`jump_to`) and 242
(`set_2d0_block`) left with the rope-swing batch the same way: pure
swing/rope object-state, pinned by two `vm_tests` tests. 156 (`spawn_at_point`) left with the spawn slice: `runtime_.spawn` at `reference_point`, pinned by a `vm_tests` test against BEAMS in DANT_1. 81 (`push_hit_dir`) left with the 81 batch: pure velocity math over existing fields, pinned by a `vm_tests` test. 206 (`spawn_along_path`) left with this batch: the path API (`pathKeyCount`/`pathKeyFrame`/`pathPosition`) and flagged `spawn` both already ported, pinned by a `vm_tests` test on LEVEL7.CMI path 53729. 217 (`global_573c4c_add`) left with the stats slice: the new `GameStats` struct (`game_stats.h`: counters, `IsEnemy`/`CountEnemy`, `Percent`) with a `stats` member on the runtime, pinned by a `vm_tests` test. 243 (`if_point_sees_player`) left with the vision-condition batch: `reference_point`, `kurtPosition` and `raycastClear` all already ported, pinned by a `vm_tests` test (range gate, ref-point selection, clear-line true at the L7 spawn, blocked-from-below false). 199 (`set_104`) and 210 (`set_2c0`) left with the value-slot batch: the C# itself treats them as unidentified object slots and does nothing but `obj.Value104/Value2c0 = Value(obj, o[0])` (ScriptVm.cs:1663-1668) — a pure object-state write like 205/218, with the fields already on `MdkObject` and no C++ reader to diverge from, pinned by a `vm_tests` test (literal and variable operand paths). Opcode 85 (`set_rolling`) was ported with a synthetic transition test for basis capture. Opcode 241 (`if_player_health`) was ported with a synthetic test pinning true and false comparison branches against Kurt's health after damage; the receiver's health is 100 (its default, now also pinned explicitly), so the assertions reject reading the receiver instead of Kurt; the pin itself adds no branch-discriminating strength today. Opcode 74 (`attach_to`) was ported with a synthetic VM test covering the oracle's explicit `BEAMS_1000` identity, case-insensitive matching, exact `TYPE_ID` equality (near-match IDs 999 and 10001 reject range/prefix resolvers, and a different-type ID-1000 candidate rejects ID-only matching), last-match selection among duplicate IDs, self/dead/cross-arena exclusion, attachment points, and preservation of seeded receiver state on a miss. Opcodes 213 (`if_partner_distance`) and 214 (`if_partner_angle`) were ported together over the existing linked-object field, with synthetic coverage for no partner, a dead partner, the live distance floor of one, the full three-dimensional distance measured from the receiver, and `wrap(receiverYaw - bearingToPartner)` (the negated turn required to face the partner). All three oracle `Linked` writers are now ported: `find_object` (245), `spawn_partner` (179), and `link_partners` (228), so partner pairs can feed these conditions. Opcodes 179 (`spawn_partner`), 180 (`snap_to_partner`), and 228 (`link_partners`) were ported together over the existing linked field and spawn machinery. 179 spawns the named type at the receiver's position with yaw zero and links both objects; 180 copies a live partner's position and then applies its optional offset; 228 pairs live same-arena objects of the named type in stable list order when their squared distance is below 10000, while its list form with a zero second value clears only the receiver's link. Synthetic `vm_tests` pin both link directions, dead/no-link behavior, case-insensitive type matching, dead/cross-arena/already-linked exclusions, the strict 100-unit boundary, list order, and the asymmetric clear. Opcodes 29 (`spawn_chain`) and 49 (`release_children`) left with the chain-link batch: the chain that hangs off an object, plus movement command 30 (the chain's own motion), pinned by three `vm_tests` tests (the link numbering after the highest live follower id, the dead follower that does not count, the chain assembly and its 90-degree turns, the broken chain, the dead leader, and the release order with the branch when the chain runs out). Opcode 27 (`if_alarm`) left with the alarm-state batch: the shared countdown is decremented once per tick, movement command 15 sounds it for ten ticks from an object in Kurt's current arena and plays `ALERT` every 32 ticks, and opcode 27 branches on the countdown. One `vm_tests` case pins the dispatch, current-arena guard, true/false branch, ten-tick expiry, and sound cadence. The C#'s Rides subsystem is the other writer of the same countdown and remains deferred with Rides. Opcode 204 (`avoid_objects`) left with the object-avoidance batch: the receiver turns away from the first live, solid, same-arena object whose strict world bounds overlap its own, by at most half a turn times one tick. One `vm_tests` case pins the self/dead/cross-arena/non-solid/touching exclusions, both turn directions, first-object order, and the angle clamp. Opcode 229 (`turn_and_jump_to_dest`) left with the turn-and-jump batch: the receiver turns towards its move destination by at most the requested rate, then launches with the oracle's ballistic velocity, arc top and friction-adjusted flight time; movement command 229 stops on floor contact after half of the recorded flight time. One `vm_tests` case pins the turn and false branch, the launch and true branch, the arc constants, the first motion tick, and the floor landing. Opcode 121 (`if_floor_below`) left with the floor-below batch: it drops a segment of the operand's length from the receiver's position and branches when any triangle of the oracle's active ray arenas crosses it, pinned by a `vm_tests` case with blocked and clear rays. Opcode 192 (`if_no_flat_floor_at`) left with the no-flat-floor batch: it rotates a forward/side offset by the receiver's yaw, probes one unit above that point down by the operand drop plus one, and branches when the ray misses or the hit triangle's normal is steeper than the limit, pinned by a `vm_tests` case against level 7's flat floor and sloping DANT_1 face. Opcode 195 (`if_hit_weapon`) left with the hit-weapon batch: it compares the receiver's existing `hit_type` field with the signed-byte operand and branches on exact equality, pinned by a `vm_tests` case for negative, positive, sign-crossing, mismatched, and default-zero hit types. Opcode 135 (`raise_573aa8`) left with the screen-shake batch: the runtime's per-tick maximum is implemented and tested, while the follow-camera consumer remains deferred. Opcode 170 (`inherit_kurt_vel`) left with the Kurt-velocity batch: the dispatch is implemented and tested over the live `Player` source, and the Linux frontend integration now constructs `ScriptRuntime` and binds that Player. Opcode 202 (`set_574304`) left with the sky-mode batch: the runtime's shared sky mode is implemented and tested, while the renderer's backdrop consumer remains deferred. Opcode 203 (`camera_track`) left with the camera-track batch: the runtime's requested pitch and two-tick lifetime are implemented and tested, and the Linux follow camera consumes them; the save/load snapshot consumer remains deferred. Opcode 220 (`place_x_near_player`) left with the place-near-Kurt batch: the dispatch, observed velocity and spacing pass are implemented and tested, and the Linux frontend now binds the live `Player` source. Opcode 219 (`if_gun_aim`) left with the gun-aim batch: the dispatch, lead, jitter, raised-target selection and fire rolls are implemented and pinned against the oracle, and the Linux frontend now binds the live `Player` source. Opcode 174 (`if_ammo`) left with the ammo-condition batch: the dispatch and all five ammo entries are implemented and pinned, while the pickup and combat writers remain deferred with their subsystems. 133 (`arena_texture_frame`), 189 (`lob_to_kurt`) and 247 (`hud_message`)
left with the texture-frame batch (2026-11-15): the AnimatedTextures
state machine is ported (`scripts/animated_textures.{h,cpp}`, frame
counts from the level's archives), 189's ballistic lob is the oracle's
own math over the object's fields, and 247 queues the runtime's HUD
messages (the drawing consumer stays with the Messages slice), each
pinned by a `vm_tests` case. 142/143/190 (fan_create/remove/enable)
followed with the Fans subsystem (`scripts/fans.{h,cpp}`: the DTI
hotspot boxes, the lift curves, the wobble; the updraft consumers in
Kurt's and the objects' motion ask `Fans::query` from their own
slices). 128/132/136 (attach_effect, spawn_effect, spawn_debris)
followed with the Effects pool (`scripts/effects.{h,cpp}`: wounds,
drops, bubbles, pops, trails; their billboard draw is the view's next
slice). 221 (`pick_waypoint8`) landed next: a weighted-random pick of the
arena's DTI type-8 waypoints becomes the destination (command 221) and the
opcode branches on whether one was found. It never fires in levels 3–8, so
the goldens are unchanged; it is covered by a synthetic `WPTEST` arena in
`vm_tests`. 171 (`if_is_573c30`, "the object Kurt rides") landed next: it is a
one-line branch on `Rides.Ridden == obj`, and the runtime already carries that
state as `ridesRidden_` (the ride slice is unported, so it is null and the branch
is false for every object — the value the C# reads on foot). It never fires in
levels 3–8; a synthetic `vm_tests` case pins the dispatch and the false branch.
Remaining 0 (after the chain-gun batch, the end of a level, the combat and
hazard batch, package #3 (114), the sniper rounds (28), the updraft/wind batch (224) and the
bullet holes (130) below): none.

**Updrafts, slide and wind (2026-11-20, docs/PORT_GAPS.md package 8):** 224
(`wind_zone`, ScriptVm.cs:1166 -> `ScriptRuntime::windZone`, ScriptRuntime.cs:2302, in
`scripts/updraft_wind.cpp`): while Kurt's tick position is in one of the arena's DTI
type-9 boxes, on a floor or already sliding he starts the slide (Kurt.Slide.cs, in
`Player`), faces the wind and is pushed by it; in the air he only falls faster; off, it
stops the slide. 142/143/190's fans now reach Kurt too (`Kurt.Updraft.cs`,
`ScriptRuntime::kurtUpdraft`: his arena's fans, else the second arena's) and let out their
sparks (`FanSpark`), which the debris' updraft already lifted. 224 is NOT corpus-silent
(×9 in LEVEL6, every wind tunnel); 142 is in every level. Pinned by six goldens from the
C# probe `tests/scripts/wind_probe` (`golden/L*_wind_*.txt`, `tick1 dump wind`) and two
re-pinned hazards goldens (`L4_hazards_fan`, `L6_hazards_balls`); each breaks when the
piece it names is broken (see docs/WORK_LEDGER.md).

**End of a level (2026-11-20, package 1 of `docs/PORT_GAPS.md`):** 131
(`special_event`) is dispatched: `ScriptRuntime::specialEvent` (ScriptRuntime.cs:1009).
50 and below are `EndTheLevel` (LevelOver, the town flags, StopFiring, NUKE and TORNADO)
and `scripts/end_level.{h,cpp}` (EndLevel.cs whole), pinned end to end by
`golden/L3_end_level.txt`, `L7_end_level.txt`, `L7_end_air.txt` and `L8_end_level.txt`
(byte-identical to the C# probe `tests/scripts/end_probe`); the dispatch and the control
flow are pinned by `vm_tests` (`SpecialEventEndsTheLevelAndKeepsSync`).

**Cutscenes (2026-11-20, package 13):** the arms above 50 are ported
(`ScriptRuntime::specialEvent`, `startCutscene`, `endCutscene`, `cutsceneTick`,
`updateCutsceneCamera`, `findObjectNamed`; ScriptRuntime.cs:1009-1077, :1337-1492): 52 (the
dog, XBN), 53/92 (EndCutscene), 55 (shot 2), 61 (Gunter, XGUNTAM), 81 (the end of the game,
`gameFinished()`), 91 and 93 (the boss intros), with RunTick's Cutscene branch and the
objects' and Kurt's Visible. Pinned by `golden/L5_cut_{gunter,dog,dog_long,strike,end}.txt`,
`L7_cut_boss.txt` and `L8_cut_boss.txt` (byte-identical to the C# probe
`tests/scripts/cutscene_probe`). **51 is whole since package 14 (2026-11-20)**: its
full-screen strike (`playStrikeScene(Kurt, WithPilot)`, X_STRIKD) plays first, the frontends
wait on it, then its teleport, cutscene and shot run (`golden/L5_strike_event51.txt`); no
value of 131 counts as unimplemented any more. The script carries on to the next
instruction, exactly as the C# does (the opcode has no branch). A value the C#'s
switch has no case for (54) is its own no-op and is not counted.

**Combat and hazard batch (2026-11-20, docs/PORT_GAPS.md package 4):** 158
(`touch_damage`, over the new `ScriptRuntime::touchDamage`, ScriptRuntime.cs:2758),
159 (`spawn_box`, over `ScriptRuntime::spawnBox` and its box models,
ScriptRuntime.cs:1593-1647; mode 1 is unreachable in the retail scripts, which use
modes 0 and 2), 167 (`move_to_bomb`, drawing the game's random for the aside at the
C#'s point), 172 (`explosion`), 178 (`explosion_damage`, a counted blast) and 184
(`explode`, an uncounted blast, then `kill`), with `ScriptVm.Point` (op172_position;
the retail scripts use modes 0 and 3). 164's flag now has its consumer,
`ObjectMotion.UpdatePathSpeed`, in `motionPath`, together with the path push
(`FlagPathPushes`, ObjectMotion.cs:309-310) that the snowboarders ride. These are
NOT corpus-silent: decoding every reachable script of LEVEL3-8 counts 158 ×68,
167 ×84, 184 ×38, 178 ×36, 172 ×32, 159 ×15 — the reachability probe's Kurt
stands still in the first arena, which is why it never saw them. Pinned by nine
goldens from the C# probe `tests/scripts/hazards_probe`
(`golden/L*_hazards_*.txt`, `tick1 dump hazards`); each breaks when the piece it
names is broken (see docs/WORK_LEDGER.md).

**The next level (2026-11-20, package 2):** no opcode changes. What follows a level that
131 ended is now played: both frontends report the level to `GameFlow`, skip the screens this
port lacks the C#'s way (`golden/flow_next_level.txt`) and load the next level in the order of
play.

**Chain-gun batch (2026-11-19):** 181 (`boss_bar`) and 209
(`if_player_on_me_low`) ported with Kurt's firing (`Kurt::firing()`, written by
`Player::updateFiring`). 181 shows the health bar through the new
`ScriptRuntime::showBarValues` only while Kurt fires (mode 0 a group counter,
mode 1 an arena variable, rounded away from zero, capped at the maximum); 209 is
`GetKurtPlatform() == obj && Kurt.Firing && KurtPosition.Z <= top`, and
`Kurt.Platform` is not ported, so its platform half answers null (the C#'s answer
on the arena's own floor) and the branch is not taken until the platform footing
lands. Neither fires in levels 3-8 to t30000; both are pinned by `vm_tests`
(`BossBarShowsOnlyWhileKurtFires`, `IfPlayerOnMeLowNeedsThePlatform`).

**Package #3, solid objects and platforms (2026-11-20):** 114 (`if_player_on_me`,
ScriptVm.cs:1416) is `GetKurtPlatform() == obj`, and `GetKurtPlatform` now reads the
ported `Kurt.Platform` (the fall that landed Kurt on an object, `Player::platform()`,
while he is on a floor) -- which also completes 209's platform half. Pinned by
`vm_tests` `IfPlayerOnMeIsFalseWithoutAPlatform` and, on a real platform, by
`golden/L3_plat_keeper.txt` (L3's hover ship waits on 114 at restart 117823 while Kurt
stands beside it, and goes to 118098 the step he lands on it). **The default case now
matches the C#'s** (ScriptVm.cs:1799-1806): an unhandled opcode with a branch action
takes it as false, so a GosubElse gosubs to its else-target where the port used to
return `ins.next` (`vm_tests` `UnhandledGosubElseCallsTheElseTarget`, over opcode 225,
which neither VM handles). Every existing golden is unchanged by it: for Goto, Gosub
and Return a false branch IS `ins.next`, and no default-case opcode in the corpus
carries a GosubElse that fires before its own port lands.

**Reachability, re-probed to t30000 (2026-10-28, `tick1 probe 30000`):**
after the Debris slice the only remaining opcode that fires anywhere in
levels 3–8 is **208** (`if_part_hit`, L5 ×9134), now ported; the probe is
clean on every level. The remaining opcodes do not fire in this corpus at all —
they stay documented no-ops until a corpus or a synthetic test exercises
them (the rule at the end of this file). The synthetic harness
(`tests/scripts/vm_tests`, twin of `tests/Mdk.Game.Tests/ScriptVmTests.cs`)
is the second route in: 13 came through it without ever firing in the corpus.
208's neighbours 132/136 (`spawn_effect`/`spawn_debris`) stay with the
Effects slice: both draw RNG and delegate to `Effects.SpawnBubble`/`SpawnDrop`
(an effect list with textures, frame counts and per-tick update), which the
C++ does not have — a bigger subsystem slice needing its own dump line, not a
two-line dispatch.

By blocking subsystem (carried grouping, corrected for this batch):

- **Kurt/player/HUD (needs Kurt items, ammo, platform, firing, camera
  state):** 189,
  215, 240, 244, 247 (181 and 209 landed with the chain-gun batch, 114 with
  package #3).
  (171 (`if_is_573c30`) landed over the runtime's `ridesRidden_` (the
  `Rides.Ridden` equivalent): it is not gated by the Kurt items/platform/firing
  subsystem at all, only by the ride slice, and it reads the same null the C#
  reads on foot.)
  (109 `hurt_kurt` and 175 `if_inventory` landed with the Kurt health/inventory
  slice; 240/244 read the same Kurt state but do not fire in levels 3-8, so
  they stay deferred. 135's runtime state landed, but its camera consumer is
  still deferred. 170, 219 and 220 landed over the already-ported `Player`
  movement source, and the Linux frontend now constructs `ScriptRuntime` and
  binds that Player, realizing them in integrated play. 174 `if_ammo` landed
  over the new ammo fields, but its pickup and combat writers remain deferred.
  112 and 173 landed with the teleport batch over `TeleportKurt`; what they
  still do NOT reach is the C#'s ONE ArenaSpace (see `docs/CONTINUATION.md`'s
  open item: the port has a scripts' space and a Player's space, so
  `enterArena` makes an unreachable arena answer the arena RULE and not yet
  the collision) and `Kurt.Keeper`, whose `_keeper = null` the port cannot do.
  A full save's LOAD closes that hole for itself (package #19): the frontends
  add `ScriptRuntime::enteredArenas()` to Kurt's space, pinned by
  `golden/L3_save_teleport.txt`. The opcodes' own teleports still do not.)
- **Arena/world (needs arena texture/sky/fan/wind state, spawn helpers):**
  133, 142, 143, 190 (130 landed with the bullet holes, package 16: `ScriptRuntime::stampBulletHole`; 28 landed with the sniper rounds, package 6: `SniperRounds::guideLastMortar`; 224 landed with the updraft/wind batch, package 8; 159 landed with the combat and hazard batch; 156 landed with the spawn slice: it needed only `runtime_.spawn` and `reference_point`, both already ported; 202 landed with the sky-mode batch, but its renderer backdrop consumer is still deferred; 223 `arena_set_neighbour` left with the trigger-box batch: it is `PreloadArena`'s other caller, so it landed with the DTI type-3 box that drives the same routine, pinned by a `vm_tests` case on shipped level-7 geometry)
- **Effects/combat/partners (needs parts, Items.Bomb,
  chain/partner links, raycast normals):** 128, 132,
  136 (158, 167, 172 and 184 landed with the combat and hazard batch; 121 landed with the floor-below batch over
  the existing raycast seam; 192 landed with the no-flat-floor batch over the
  same seam extended to carry the hit normal; 195 landed with the hit-weapon
  batch over the existing `hit_type` field, but its only C++ writer is the
  uncalled `ScriptRuntime::hitGroup`, so it remains partial until the group-hit
  and direct combat writers land; 32/129 landed with the parts batch; 179/180/228 landed with the partner-link slice; 29/49 landed with the chain-link batch, together with movement command 30 that moves the chain; 81 landed with the 81 batch: only the `hit_direction` FIELD is ported — its writers live in ChainGunHit, the mortar path, Twister, sniper rounds and TouchDamage, none of which has a C++ counterpart yet, so in integrated play 81 always pushes due east until those slices land; each of those slices' acceptance checks must re-verify 81 end-to-end)
- **Movement/AI leftovers:** none. 220 landed with the place-near-Kurt
  batch; 229 with the turn-and-jump batch; 204 with the object-avoidance
  batch; 206 with the spawn_along_path batch; 217 with the stats slice; 85
  (`set_rolling`) with object rotation math; and 226/242 with the rope-swing
  batch.
- **Audio — ported with the mixer slice:** 89 (`play_sound`), 107
  (`set_loop_sound`), 233 (`if_sound_playing`) and 249 (`if_own_sound`) landed
  with `mdk_core/src/audio/` (the sound bank, the mixer voice table, and the
  device control plane); the goldens pin the voice table and each object's
  tracked/loop voices.
- **Alarm state (ported with the alarm batch):** movement command 15 and opcode
  27 (`if_alarm`) now use the oracle's shared `alarmTicks` countdown. The C#'s
  Rides subsystem also writes that field (`Rides.cs:219,224`); Rides remains
  deferred with its own player/vehicle slice, so the riding vehicle's alarm
  source is not present yet.
- **Camera/cutscene/debug:** 131 is ported whole, 51's strike scene included (package 14;
  see the 2026-11-20 entries above)
  (77 `debug_msg` is a reference no-op, now pinned by `vm_tests`; 13 `if_cheat_key` is an explicit false branch -- the
  C#'s own (ScriptVm.cs:1709, "cheat keys aren't done"), re-checked with the cheats of gap package #21, which
  are typed words and console switches and never reach this opcode -- so
  no camera/cheat subsystem is needed — ported with the harness twin. 243
  `if_point_sees_player` left the same way: it reads only `reference_point`,
  `kurtPosition` and `raycastClear`, none of which needs the camera —
  ported with the vision-condition batch.)
- **Not identified in the reference:** opcode 239 (`set_27c`) is an explicit
  C# no-op (`ScriptVm.cs:1783`); the C++ case now preserves that no-op while
  consuming the decoder's six-float record, pinned by `vm_tests`. Opcodes 199
  and 210 are also unidentified downstream slots, but their C# behavior is a
  pure value write (see the value-slot batch above).
- **Needs its subsystem or data the tree does not have (deferred with cause):**
  item-chain links (29/49 landed with the chain-link batch). 219
  (`if_gun_aim`) landed with the gun-aim batch: the C# case only aims the gun
  and answers whether it fires, so no projectile subsystem is needed.

Do not implement any line of the list above without its subsystem and a test
that exercises it; an unverifiable blind port risks a latent gameplay bug that
is worse than the current documented no-op.

## Owned-case classification (Task 2 remainder)

Method (reproducible): brace-match the C# `ScriptVm.Execute` and the C++
`ScriptVm::execute` method spans, take the set of numeric `case` labels in each,
and compare as sets. Current count: C# **238**, C++ **214 labels / 213 unique**,
remaining **26**. The C++ set has the nested `case 0:` mode arm inside opcode
249's switch, which is not a dispatch, so there are **212 true dispatches** and
212 + 26 = 238. Recounted 2026-11-11 after opcode 223 was added (it was 213
labels / 212 unique / 211 true dispatches / 27 remaining after opcode 174); the raw C++
label count includes opcode 249's nested `case 0` and `case 1` arms, with the
latter duplicating top-level opcode 1.

Two axes, kept separate:

- **Completeness** — does the dispatch case do everything the C# case does *and
  is its gameplay effect realized by the subsystems present in this tree?* A
  case that faithfully mirrors the C# operand layout but delegates its effect to
  a partial or unported subsystem is **partial**, not done (the CONTINUATION's
  "move command 6 on partial motion is NOT done").
- **Test coverage** — the end-to-end (level, tick) goldens in
  `tests/scripts/tick1.cpp` compare the full object table and script state
  against the oracle byte-for-byte through t600 on levels 3-8. Per-opcode unit
  tests now exist for 44 opcodes: the 11 the
  harness twin covers (1, 12, 13, 64, 65, 66, 67, 68, 71, 252, 253) plus
  32/129, 226/242, 156, 81, 206, 217, 243, 77, 85, 74, 213/214,
  179/180/228, 29/49, 15/27, 204, 229, 121, 192, 195, 135, 170, 202, 203, 220,
  219 and 174, which pin the
  oracle directly (no C# counterpart exists); the other owned cases are
  golden-covered only where they fire.

**Stubs: 0.** No owned case is a fake implementation. (248 `push_kurt` was the only
no-op case until the knock-down slice landed and gave its push a writer; it is now a
faithful dispatch, listed as partial below for the single output the unported firing
subsystem still swallows. 89 `play_sound` was the other honest no-op until the mixer
slice landed; it is now a real implementation.
Opcode 170 is a faithful dispatch over the borrowed live `Player` source, now
realized by the Linux frontend integration.)

**Partial — faithful dispatch, effect gated behind a partial/unported subsystem
(7 cases since package 15 completed 135 and 140), each verified against the code. The rule is consistent for input
fields and output consumers: a required input with no reachable writer, or an
output with no consumer, is partial even when the dispatch itself is exact.
Thus 81/174/195 are partial for unwritten inputs, and
202/203 for missing snapshot consumers (135 and 140 left the table with package 15: the camera shakes and the views rebuild). (16/56, 165/166
and 215 left this table on 2026-11-20, re-verified with the combat and hazard
batch: `kill` runs Explode's sound, flash, break-up and explosion
(`ScriptRuntime::explode`); `itemsBomb` is written by the Items slice
(items.cpp, golden/L8_items_bomb.txt, golden/L6_hazards_bomb.txt); and the white
flash is drawn by `hud_view.cpp` from `Kurt::whiteFlash()` on both frontends.) (240 and 244 are
not in this table and no longer belong to that second group: 240 writes
`knockDamage` and 244 writes `invulnerable`, and both fields are live state that
`Player` declines each tick and the `L3_kurt_knock`/`L3_kurt_invuln` goldens pin, so
their consumers landed with the Kurt combat-state slices. 24 and 151 left this table with the
mixer call-sites batch: the frame-sound trigger and the door sounds now play,
so both are complete. 6 left it with the movement-command batch: `Chase` is
portable with no new subsystem, and it is ported.)**

| Opcode | Name | Gating subsystem (verified) |
|---|---|---|
| 81 | push_hit_dir | reads `hit_direction`; the chain gun, the blast, the mortar, Twister, touch_damage and the sniper rounds (package 6, `SniperRounds::hitObject`) write it |
| 174 | if_ammo | reads `Kurt`'s super-chain-gun timer and five sniper-ammo entries; the pickups write them and sniper mode's clip spends them (`Kurt::useAmmo`, 2026-11-20) |
| 202 | set_574304 | writes the runtime's shared `skyMode`, which both frontends' backdrop reads (package 15, `game/sky_modes.h`); the save/load snapshot's `sky_mode` is not ported |
| 203 | camera_track | writes the runtime's `cameraTrackPitch`/`cameraTrackTicks`, and the Linux follow camera consumes them, but the save/load snapshot's `camera_track_*` fields are not ported |
| 131 | special_event | ported whole: the <= 50 arm (package 1), the arms above 50 (package 13) and 51's strike scene (package 14); none counts as unimplemented |
| 195 | if_hit_weapon | reads `hit_type`; the chain gun, the blast, the mortar, the cow, touch_damage, `hitGroup` and the sniper rounds (package 6: the round's type on an object, the type as the group hit's weapon) write it |
| 248 | push_kurt | knocks Kurt down and shoves him, from the object's frame or away from it (golden/L3_kurt_push.txt); the C#'s `StopFiring()` inside KnockDown is inert because the firing/muzzle subsystem is not ported |

**Complete — effect realized with the subsystems present (225 cases, plus 135 and 140
since package 15):** every
owned case not in the partial table above (234 true dispatches − 9 = 225, recounted
2026-11-20 after package 1's 131 and package 4). These are the pure
state, condition, orientation/aim, movement-parameter, handled-movement-command,
path, animation-state, variable/flag, control-flow, group-state and spawn cases, exercised by byte-for-byte goldens
through t600 on levels 3-8 or by the synthetic tests where the corpus cannot reach them.
Representative members, by group: orientation/aim (8, 40, 60, 62, 87, 101, 104,
105, 120, 122, 126, 134, 160, 193, 207, 234, 235, 246); movement parameters
(50-55, 82, 84, 106, 169, 177); handled movement commands (43, 46, 61, 78, 88,
93, 157, 197, 229, 231 — `stuck_count` is maintained at script_runtime.cpp:1583/1623);
object avoidance (204); turn and jump (229); path (2, 20, 21, 33, 91, 102, 124, 164); animation (3, 17, 19, 58, 59, 92, 118,
154, 186 — `advanceAnimation` is ported and called); stats counters (217);
conditions/flags/variables
(10, 11, 14, 18, 22, 23, 25, 26, 31, 34-39, 41, 42, 44, 45, 47, 48, 54, 57, 63,
65-73, 75, 76, 79, 80, 83, 86, 96, 97, 100, 103, 108, 110, 111, 113, 116, 117,
119, 121, 123, 127, 161, 176, 185, 187, 188, 191, 192, 198, 200, 201, 205, 211, 212, 213,
214, 216, 218, 222, 230, 232, 236, 237, 238, 243, 245, 250, 251, 252, 253); control flow (1, 4,
9, 12, 64, 94, 95, 125, 183); explicit no-ops (77, 239); group state (98, 99, 162, 163, 168, 194); spawn
(113, 149, 150, 152, 153, 156, 206); partner links (179, 180, 228); chain links
(29, 49 — with movement command 30, which moves the chain); alarm state (15, 27). The authoritative complete set is the owned set minus
the 7 partial cases in the table above, not this illustrative grouping. (121 joined the conditions group with the floor-below batch; 192 joined it with the no-flat-floor batch; 206 joined the spawn group with the 206 batch; 217 joined the new stats-counters group with the stats slice; 243 joins the conditions group with the vision-condition batch. 81, 174 and 195 are partial until their input writers exist; 202 and 203 are partial until their snapshot consumers exist (135 and 140 joined the complete set with package 15); 170, 219 and 220 joined the complete set when the Linux frontend constructed `ScriptRuntime` and bound the live `Player`.)

All three oracle `Linked` writers are now ported: `find_object` (245),
`spawn_partner` (179), and `link_partners` (228). The partner-condition cases
213/214 therefore no longer have a producer caveat, and partner pairs created
by either new writer can reach them. Removal keeps those links exactly as the
oracle does: the C# `Remove` clears no inbound reference, so `remove()` does
not either (see the iteration-59 narrative in `docs/PROGRESS_LOG.md`); a widow
keeps its dead partner, and `RemoveKeepsInboundReferences` pins the same for
`leader`. The chain links follow the same rule: `release_children` (49) is the
only OPCODE that clears an inbound `leader`, and it clears exactly the links it
lets go of. The chain motion's own let-go (command 30,
`ObjectMotion.cs:470,480`) is the only other place the oracle clears one; a
removed leader keeps its links until that motion sees the dead leader and lets
them go.

Note on 251 (`set_target_mode`): its own effect (set `target_mode`, set/clear
the alien target) is realized, so it is classified complete; the known
decoy/`AlienTarget` targeting gap lives in the targeting *selection*, not in this
dispatch case.

Closing the partial cases requires their subsystems (the
death path, the renderer material remap, the Items.Bomb slice, the camera
shake/flash and sky-backdrop consumers, the combat writers for `hit_direction`
and `hit_type`, and the remaining camera/snapshot consumers), each of which is
its own increment with a test — not a blind port.
