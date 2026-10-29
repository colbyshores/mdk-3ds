# Script VM opcode coverage audit (Phase 2 §3.1)

Method and reachability evidence in the final Phase 2 report. Summary:

- Counts recounted from the brace-matched method spans (C# `ScriptVm.Execute`
  vs C++ `ScriptVm::execute`, numeric `case` labels, set semantics): the C#
  owns **238** cases, the C++ has **212** numeric labels / 211 unique labels (210 true dispatches), leaving **28** unimplemented. The
  earlier version of this document claimed 155/83 and listed 114, 171 and 209
  as ported; none of the three is in the C++ `execute()` — the old counts and
  the old "ported" list were wrong. The remaining cases fall through to
  `noteUnimplemented` (decoded, no-op, scripts stay in sync).
- The reachability probe (C++ `unimplemented()` after running each level to
  t600) was rerun after this batch: of the remaining set, only **138** (L5)
  fires in levels 3-8 — 109 (L3, L5), 175 (L6), 249 (L5), and 77 do not
  exercise this route; 77 is covered by `vm_tests`. (99 and 168, which fired
  before this batch, are now implemented.)
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
  Player yaw, fall-through, and expiry. No shipped frontend constructs the
  runtime and binds that Player yet; the renderer's follow-camera consumer and
  the save/load snapshot consumer are also not ported, so the dispatch is
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
  No shipped frontend constructs the runtime and binds its `Player`, so the
  dispatch is classified partial. Recount: C# 238, C++ 209 true dispatches,
  29 remaining.
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
  output. The observed velocity it leads with needs the same missing frontend
  `Player` binding as 170/220, so the dispatch is classified partial. Recount:
  C# 238, C++ 210 true dispatches, 28 remaining.
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
  `mdk::Player`; the missing integration is that no shipped frontend constructs
  `ScriptRuntime` and binds that Player yet, so the dispatch remains partial.
  Recount: C# 238, C++ 206 true dispatches, 32 remaining.

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

## Remaining 28 (verified set)

13 left this list with the synthetic-harness batch (port 13 is an explicit
false branch, `ScriptVmTests.UnimplementedConditionsAreFalse` pins it, and the
C++ `unimplemented()` bucket stays clean). 32 (`clear_parts_mask`) and 129
(`blow_off_parts`) left with the parts batch: pure object-state ops over the
existing parts fields, pinned by three `vm_tests` tests with a synthetic
model (no C# counterpart exists to twin). 226 (`jump_to`) and 242
(`set_2d0_block`) left with the rope-swing batch the same way: pure
swing/rope object-state, pinned by two `vm_tests` tests. 156 (`spawn_at_point`) left with the spawn slice: `runtime_.spawn` at `reference_point`, pinned by a `vm_tests` test against BEAMS in DANT_1. 81 (`push_hit_dir`) left with the 81 batch: pure velocity math over existing fields, pinned by a `vm_tests` test. 206 (`spawn_along_path`) left with this batch: the path API (`pathKeyCount`/`pathKeyFrame`/`pathPosition`) and flagged `spawn` both already ported, pinned by a `vm_tests` test on LEVEL7.CMI path 53729. 217 (`global_573c4c_add`) left with the stats slice: the new `GameStats` struct (`game_stats.h`: counters, `IsEnemy`/`CountEnemy`, `Percent`) with a `stats` member on the runtime, pinned by a `vm_tests` test. 243 (`if_point_sees_player`) left with the vision-condition batch: `reference_point`, `kurtPosition` and `raycastClear` all already ported, pinned by a `vm_tests` test (range gate, ref-point selection, clear-line true at the L7 spawn, blocked-from-below false). 199 (`set_104`) and 210 (`set_2c0`) left with the value-slot batch: the C# itself treats them as unidentified object slots and does nothing but `obj.Value104/Value2c0 = Value(obj, o[0])` (ScriptVm.cs:1663-1668) — a pure object-state write like 205/218, with the fields already on `MdkObject` and no C++ reader to diverge from, pinned by a `vm_tests` test (literal and variable operand paths). Opcode 85 (`set_rolling`) was ported with a synthetic transition test for basis capture. Opcode 241 (`if_player_health`) was ported with a synthetic test pinning true and false comparison branches against Kurt's health after damage; the receiver's health is 100 (its default, now also pinned explicitly), so the assertions reject reading the receiver instead of Kurt; the pin itself adds no branch-discriminating strength today. Opcode 74 (`attach_to`) was ported with a synthetic VM test covering the oracle's explicit `BEAMS_1000` identity, case-insensitive matching, exact `TYPE_ID` equality (near-match IDs 999 and 10001 reject range/prefix resolvers, and a different-type ID-1000 candidate rejects ID-only matching), last-match selection among duplicate IDs, self/dead/cross-arena exclusion, attachment points, and preservation of seeded receiver state on a miss. Opcodes 213 (`if_partner_distance`) and 214 (`if_partner_angle`) were ported together over the existing linked-object field, with synthetic coverage for no partner, a dead partner, the live distance floor of one, the full three-dimensional distance measured from the receiver, and `wrap(receiverYaw - bearingToPartner)` (the negated turn required to face the partner). All three oracle `Linked` writers are now ported: `find_object` (245), `spawn_partner` (179), and `link_partners` (228), so partner pairs can feed these conditions. Opcodes 179 (`spawn_partner`), 180 (`snap_to_partner`), and 228 (`link_partners`) were ported together over the existing linked field and spawn machinery. 179 spawns the named type at the receiver's position with yaw zero and links both objects; 180 copies a live partner's position and then applies its optional offset; 228 pairs live same-arena objects of the named type in stable list order when their squared distance is below 10000, while its list form with a zero second value clears only the receiver's link. Synthetic `vm_tests` pin both link directions, dead/no-link behavior, case-insensitive type matching, dead/cross-arena/already-linked exclusions, the strict 100-unit boundary, list order, and the asymmetric clear. Opcodes 29 (`spawn_chain`) and 49 (`release_children`) left with the chain-link batch: the chain that hangs off an object, plus movement command 30 (the chain's own motion), pinned by three `vm_tests` tests (the link numbering after the highest live follower id, the dead follower that does not count, the chain assembly and its 90-degree turns, the broken chain, the dead leader, and the release order with the branch when the chain runs out). Opcode 27 (`if_alarm`) left with the alarm-state batch: the shared countdown is decremented once per tick, movement command 15 sounds it for ten ticks from an object in Kurt's current arena and plays `ALERT` every 32 ticks, and opcode 27 branches on the countdown. One `vm_tests` case pins the dispatch, current-arena guard, true/false branch, ten-tick expiry, and sound cadence. The C#'s Rides subsystem is the other writer of the same countdown and remains deferred with Rides. Opcode 204 (`avoid_objects`) left with the object-avoidance batch: the receiver turns away from the first live, solid, same-arena object whose strict world bounds overlap its own, by at most half a turn times one tick. One `vm_tests` case pins the self/dead/cross-arena/non-solid/touching exclusions, both turn directions, first-object order, and the angle clamp. Opcode 229 (`turn_and_jump_to_dest`) left with the turn-and-jump batch: the receiver turns towards its move destination by at most the requested rate, then launches with the oracle's ballistic velocity, arc top and friction-adjusted flight time; movement command 229 stops on floor contact after half of the recorded flight time. One `vm_tests` case pins the turn and false branch, the launch and true branch, the arc constants, the first motion tick, and the floor landing. Opcode 121 (`if_floor_below`) left with the floor-below batch: it drops a segment of the operand's length from the receiver's position and branches when any triangle of the oracle's active ray arenas crosses it, pinned by a `vm_tests` case with blocked and clear rays. Opcode 192 (`if_no_flat_floor_at`) left with the no-flat-floor batch: it rotates a forward/side offset by the receiver's yaw, probes one unit above that point down by the operand drop plus one, and branches when the ray misses or the hit triangle's normal is steeper than the limit, pinned by a `vm_tests` case against level 7's flat floor and sloping DANT_1 face. Opcode 195 (`if_hit_weapon`) left with the hit-weapon batch: it compares the receiver's existing `hit_type` field with the signed-byte operand and branches on exact equality, pinned by a `vm_tests` case for negative, positive, sign-crossing, mismatched, and default-zero hit types. Opcode 135 (`raise_573aa8`) left with the screen-shake batch: the runtime's per-tick maximum is implemented and tested, while the follow-camera consumer remains deferred. Opcode 170 (`inherit_kurt_vel`) left with the Kurt-velocity batch: the dispatch is implemented and tested over the live `Player` source, while frontend construction of `ScriptRuntime` and the Player binding remain missing. Opcode 202 (`set_574304`) left with the sky-mode batch: the runtime's shared sky mode is implemented and tested, while the renderer's backdrop consumer remains deferred. Opcode 203 (`camera_track`) left with the camera-track batch: the runtime's requested pitch and two-tick lifetime are implemented and tested, while the renderer's follow-camera consumer remains deferred. Opcode 220 (`place_x_near_player`) left with the place-near-Kurt batch: the dispatch, observed velocity and spacing pass are implemented and tested, while frontend construction of `ScriptRuntime` and its live `Player` binding remain missing. Opcode 219 (`if_gun_aim`) left with the gun-aim batch: the dispatch, lead, jitter, raised-target selection and fire rolls are implemented and pinned against the oracle, while frontend construction of `ScriptRuntime` and its live `Player` binding remain missing. Remaining 28:
28,
112, 114, 128,
130, 131, 132, 133, 136, 142, 143, 158, 159, 167,
171, 172, 173, 174, 178, 181, 184, 189, 190,
209, 221, 223, 224,
247

**Reachability, re-probed to t30000 (2026-10-28, `tick1 probe 30000`):**
after the Debris slice the only remaining opcode that fires anywhere in
levels 3–8 is **208** (`if_part_hit`, L5 ×9134), now ported; the probe is
clean on every level. The remaining 28 do not fire in this corpus at all —
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
  state):** 112, 114 (if_player_on_me), 171, 173, 174, 178, 181, 189,
  209, 215, 240, 244, 247.
  (109 `hurt_kurt` and 175 `if_inventory` landed with the Kurt health/inventory
  slice; 240/244 read the same Kurt state but do not fire in levels 3-8, so
  they stay deferred. 135's runtime state landed, but its camera consumer is
  still deferred. 170's dispatch landed over the already-ported `Player`
  movement source, but a shipped frontend must construct `ScriptRuntime` and
  bind that Player before the inherited velocity is realized in integrated
  play. 220's observed velocity uses the same missing frontend binding, and so
  does 219's shot lead.)
- **Arena/world (needs arena texture/sky/fan/wind state, spawn helpers):**
  28, 130, 133, 142, 143, 159, 190, 223, 224 (156 landed with the spawn slice: it needed only `runtime_.spawn` and `reference_point`, both already ported; 202 landed with the sky-mode batch, but its renderer backdrop consumer is still deferred)
- **Effects/combat/partners (needs parts, Items.Bomb,
  chain/partner links, raycast normals):** 128, 132,
  136, 158, 167, 172, 184 (121 landed with the floor-below batch over
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
- **Camera/cutscene/debug (no camera/cutscene port):** 131
  (77 `debug_msg` is a reference no-op, now pinned by `vm_tests`; 13 `if_cheat_key` is an explicit false branch, so
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
  waypoint tables 221,
  item-chain links (29/49 landed with the chain-link batch). 219
  (`if_gun_aim`) landed with the gun-aim batch: the C# case only aims the gun
  and answers whether it fires, so no projectile subsystem is needed.

Do not implement any line of the list above without its subsystem and a test
that exercises it; an unverifiable blind port risks a latent gameplay bug that
is worse than the current documented no-op.

## Owned-case classification (Task 2 remainder)

Method (reproducible): brace-match the C# `ScriptVm.Execute` and the C++
`ScriptVm::execute` method spans, take the set of numeric `case` labels in each,
and compare as sets. Current count: C# **238**, C++ **212 labels / 211 unique**,
remaining **28**. The C++ set has the nested `case 0:` mode arm inside opcode
249's switch, which is not a dispatch, so there are **210 true dispatches** and
210 + 28 = 238. Recounted 2026-10-28 after opcode 219 was added; the raw C++
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
  tests now exist for 43 opcodes: the 11 the
  harness twin covers (1, 12, 13, 64, 65, 66, 67, 68, 71, 252, 253) plus
  32/129, 226/242, 156, 81, 206, 217, 243, 77, 85, 74, 213/214,
  179/180/228, 29/49, 15/27, 204, 229, 121, 192, 195, 135, 170, 202, 203, 220
  and 219, which pin the
  oracle directly (no C# counterpart exists); the other owned cases are
  golden-covered only where they fire.

**Stubs: 0.** No owned case is a fake implementation. The only no-op case (248
`push_kurt`) is a documented honest no-op because its subsystem (Kurt's
knock-back) does not exist yet; it decodes and advances the PC exactly as the
C# does and writes no state the goldens read. (89 `play_sound` was the other
honest no-op until the mixer slice landed; it is now a real implementation.
Opcode 170 is a faithful dispatch over the borrowed live `Player` source; its
missing frontend integration is recorded partial below.)

**Partial — faithful dispatch, effect gated behind a partial/unported subsystem
(18 cases), each verified against the code. The rule is consistent for input
fields and output consumers: a required input with no reachable writer, or an
output with no consumer, is partial even when the dispatch itself is exact.
Thus 81/165/166/170/195/219/220 are partial for unwritten inputs, and
135/202/203/215/240/244 for missing visual or movement consumers. (24 and 151 left this table with the
mixer call-sites batch: the frame-sound trigger and the door sounds now play,
so both are complete.)**

| Opcode | Name | Gating subsystem (verified) |
|---|---|---|
| 6 | move_to_target | sets `move_command = 6`; `ScriptRuntime::updateCommand` (script_runtime.cpp:1805) has no case for 6 → no motion |
| 16 | set_health | on health 0 calls `kill()` (script_runtime.cpp:2976); its death-script and removal accounting are ported, but the Explode branch's sound/flash/debris/explosion effects are not |
| 56 | add_health | on death calls the same `kill()` placeholder |
| 81 | push_hit_dir | reads `hit_direction`; its chain-gun, mortar, Twister, sniper-round and touch-damage writers are unported, so integrated play uses the default east direction |
| 135 | raise_573aa8 | writes the runtime's per-tick maximum, but the C# `ShakeRaised` follow-camera consumer is not ported |
| 140 | group_set_texture | sets the group material VM state; the renderer ignores it (open item 1, dynamic scene updates) |
| 165 | if_no_bomb | reads `itemsBomb`, which is never assigned (script_runtime.h:295, `= nullptr` only) → always "no bomb" until the Items slice |
| 166 | if_bomb_visible | same `itemsBomb` gap → always false |
| 170 | inherit_kurt_vel | reads the borrowed `Player`'s live velocity, including the C# knock-down-push term; no shipped frontend constructs `ScriptRuntime` or binds the Player yet, so integrated play has no script runtime |
| 202 | set_574304 | writes the runtime's shared `skyMode`, but the renderer's `SkyModes.BackdropOf` and save/load snapshot consumers are not ported |
| 203 | camera_track | writes the runtime's `cameraTrackPitch`/`cameraTrackTicks`, but no shipped frontend binds the live `Player` source and the follow-camera pitch consumer plus the save/load snapshot's `camera_track_*` fields are not ported |
| 220 | place_x_near_player | writes the receiver's X from the exact RNG/range/wrap/spacing rules; its predicted-X branch reads the observed `kurtVelocity` field, whose live `Player` source is not bound by any shipped frontend |
| 219 | if_gun_aim | aims the gun and answers whether it fires exactly as the oracle does; the lead and the fire roll read the observed `kurtVelocity` field, whose live `Player` source is not bound by any shipped frontend |
| 195 | if_hit_weapon | reads `hit_type`; its only C++ writer is `ScriptRuntime::hitGroup`, which has no caller. The C# group-hit call sites (including Kurt's `HitTypeKurt = -11` touch) and direct combat writers remain deferred |
| 215 | screen_flash | writes whiteFlash, but fade and render consumers are not ported |
| 240 | set_hurt_flash | writes knockDamage, but Kurt's knock-down state machine is not ported |
| 244 | set_global_573bd4 | writes invulnerability, but per-tick timer decay is not ported |
| 248 | push_kurt | honest no-op; Kurt knock-back unported (no observable state the goldens read) |

**Complete — effect realized with the subsystems present (192 cases):** every
owned case not in the partial table above (210 true dispatches − 18 = 192). These are the pure
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
the 18 partial cases, not this illustrative grouping. (121 joined the conditions group with the floor-below batch; 192 joined it with the no-flat-floor batch; 206 joined the spawn group with the 206 batch; 217 joined the new stats-counters group with the stats slice; 243 joins the conditions group with the vision-condition batch. 81 and 195 are partial until their input writers exist; 135 and 203 are partial until their camera consumers exist; 170, 219 and 220 are partial until a shipped frontend constructs `ScriptRuntime` and binds the live `Player`.)

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

Closing the partial cases requires their subsystems (movement command 6, the
death path, the renderer material remap, the Items.Bomb slice, the camera
shake/flash and sky-backdrop consumers, the combat writers for `hit_direction`
and `hit_type`, and shipped-frontend construction of `ScriptRuntime` with its
live `Player` binding), each of which is its own increment with a test — not a
blind port.
