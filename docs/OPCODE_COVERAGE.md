# Script VM opcode coverage audit (Phase 2 §3.1)

Method and reachability evidence in the final Phase 2 report. Summary:

- Counts recounted from the brace-matched method spans (C# `ScriptVm.Execute`
  vs C++ `ScriptVm::execute`, numeric `case` labels, set semantics): the C#
  owns **238** cases, the C++ has **203** numeric labels / 202 unique labels (201 true dispatches), leaving **37** unimplemented. The
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
- **Turn-and-jump batch (2026-10-27):** opcode 229
  (`turn_and_jump_to_dest`) and movement command 229 are ported together over
  the existing destination, yaw, velocity, gravity, friction, parameter and
  contact fields. The receiver turns by at most the requested rate until it
  faces the destination, then receives the oracle's ballistic velocity and
  flight time; the flight command stops on floor contact after the apex. One
  `vm_tests` case pins the turn and branch, the arc and friction-adjusted
  speed, the first motion tick, and the floor-landing stop. Recount: C# 238,
  C++ 201 true dispatches, 37 remaining.

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

## Remaining 37 (verified set)

13 left this list with the synthetic-harness batch (port 13 is an explicit
false branch, `ScriptVmTests.UnimplementedConditionsAreFalse` pins it, and the
C++ `unimplemented()` bucket stays clean). 32 (`clear_parts_mask`) and 129
(`blow_off_parts`) left with the parts batch: pure object-state ops over the
existing parts fields, pinned by three `vm_tests` tests with a synthetic
model (no C# counterpart exists to twin). 226 (`jump_to`) and 242
(`set_2d0_block`) left with the rope-swing batch the same way: pure
swing/rope object-state, pinned by two `vm_tests` tests. 156 (`spawn_at_point`) left with the spawn slice: `runtime_.spawn` at `reference_point`, pinned by a `vm_tests` test against BEAMS in DANT_1. 81 (`push_hit_dir`) left with the 81 batch: pure velocity math over existing fields, pinned by a `vm_tests` test. 206 (`spawn_along_path`) left with this batch: the path API (`pathKeyCount`/`pathKeyFrame`/`pathPosition`) and flagged `spawn` both already ported, pinned by a `vm_tests` test on LEVEL7.CMI path 53729. 217 (`global_573c4c_add`) left with the stats slice: the new `GameStats` struct (`game_stats.h`: counters, `IsEnemy`/`CountEnemy`, `Percent`) with a `stats` member on the runtime, pinned by a `vm_tests` test. 243 (`if_point_sees_player`) left with the vision-condition batch: `reference_point`, `kurtPosition` and `raycastClear` all already ported, pinned by a `vm_tests` test (range gate, ref-point selection, clear-line true at the L7 spawn, blocked-from-below false). 199 (`set_104`) and 210 (`set_2c0`) left with the value-slot batch: the C# itself treats them as unidentified object slots and does nothing but `obj.Value104/Value2c0 = Value(obj, o[0])` (ScriptVm.cs:1663-1668) — a pure object-state write like 205/218, with the fields already on `MdkObject` and no C++ reader to diverge from, pinned by a `vm_tests` test (literal and variable operand paths). Opcode 85 (`set_rolling`) was ported with a synthetic transition test for basis capture. Opcode 241 (`if_player_health`) was ported with a synthetic test pinning true and false comparison branches against Kurt's health after damage; the receiver's health is 100 (its default, now also pinned explicitly), so the assertions reject reading the receiver instead of Kurt; the pin itself adds no branch-discriminating strength today. Opcode 74 (`attach_to`) was ported with a synthetic VM test covering the oracle's explicit `BEAMS_1000` identity, case-insensitive matching, exact `TYPE_ID` equality (near-match IDs 999 and 10001 reject range/prefix resolvers, and a different-type ID-1000 candidate rejects ID-only matching), last-match selection among duplicate IDs, self/dead/cross-arena exclusion, attachment points, and preservation of seeded receiver state on a miss. Opcodes 213 (`if_partner_distance`) and 214 (`if_partner_angle`) were ported together over the existing linked-object field, with synthetic coverage for no partner, a dead partner, the live distance floor of one, the full three-dimensional distance measured from the receiver, and `wrap(receiverYaw - bearingToPartner)` (the negated turn required to face the partner). All three oracle `Linked` writers are now ported: `find_object` (245), `spawn_partner` (179), and `link_partners` (228), so partner pairs can feed these conditions. Opcodes 179 (`spawn_partner`), 180 (`snap_to_partner`), and 228 (`link_partners`) were ported together over the existing linked field and spawn machinery. 179 spawns the named type at the receiver's position with yaw zero and links both objects; 180 copies a live partner's position and then applies its optional offset; 228 pairs live same-arena objects of the named type in stable list order when their squared distance is below 10000, while its list form with a zero second value clears only the receiver's link. Synthetic `vm_tests` pin both link directions, dead/no-link behavior, case-insensitive type matching, dead/cross-arena/already-linked exclusions, the strict 100-unit boundary, list order, and the asymmetric clear. Opcodes 29 (`spawn_chain`) and 49 (`release_children`) left with the chain-link batch: the chain that hangs off an object, plus movement command 30 (the chain's own motion), pinned by three `vm_tests` tests (the link numbering after the highest live follower id, the dead follower that does not count, the chain assembly and its 90-degree turns, the broken chain, the dead leader, and the release order with the branch when the chain runs out). Opcode 27 (`if_alarm`) left with the alarm-state batch: the shared countdown is decremented once per tick, movement command 15 sounds it for ten ticks from an object in Kurt's current arena and plays `ALERT` every 32 ticks, and opcode 27 branches on the countdown. One `vm_tests` case pins the dispatch, current-arena guard, true/false branch, ten-tick expiry, and sound cadence. The C#'s Rides subsystem is the other writer of the same countdown and remains deferred with Rides. Opcode 204 (`avoid_objects`) left with the object-avoidance batch: the receiver turns away from the first live, solid, same-arena object whose strict world bounds overlap its own, by at most half a turn times one tick. One `vm_tests` case pins the self/dead/cross-arena/non-solid/touching exclusions, both turn directions, first-object order, and the angle clamp. Opcode 229 (`turn_and_jump_to_dest`) left with the turn-and-jump batch: the receiver turns towards its move destination by at most the requested rate, then launches with the oracle's ballistic velocity, apex and friction-adjusted flight time; movement command 229 stops on floor contact after the apex. One `vm_tests` case pins the turn and false branch, the launch and true branch, the arc constants, the first motion tick, and the floor landing. Remaining 37:
28,
112, 114, 121, 128,
130, 131, 132, 133, 135, 136, 142, 143, 158, 159, 167, 170,
171, 172, 173, 174, 178, 181, 184, 189, 190, 192, 195, 202,
203, 209, 219, 220, 221, 223, 224,
247

**Reachability, re-probed to t30000 (2026-10-25, `tick1 probe 30000`):**
after the Debris slice the only remaining opcode that fires anywhere in
levels 3–8 is **208** (`if_part_hit`, L5 ×9134), now ported; the probe is
clean on every level. The remaining 37 do not fire in this corpus at all —
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
  209, 215, 240, 244, 247; 170 (inherit_kurt_vel) needs Kurt's velocity;
  135 (raise_573aa8) needs the screen-shake state. (109 `hurt_kurt` and 175
  `if_inventory` landed with the Kurt health/inventory slice; 240/244 read
  the same Kurt state but do not fire in levels 3-8, so they stay deferred.)
- **Arena/world (needs arena texture/sky/fan/wind state, spawn helpers):**
  28, 130, 133, 142, 143, 159, 190, 202, 223, 224 (156 landed with the spawn slice: it needed only `runtime_.spawn` and `reference_point`, both already ported)
- **Effects/combat/partners (needs parts, Items.Bomb,
  chain/partner links, raycast normals):** 121, 128, 132,
  136, 158, 167, 172, 184, 192, 195 (32/129 landed with the parts batch; 179/180/228 landed with the partner-link slice; 29/49 landed with the chain-link batch, together with movement command 30 that moves the chain; 81 landed with the 81 batch: only the `hit_direction` FIELD is ported — its writers live in ChainGunHit, the mortar path, Twister, sniper rounds and TouchDamage, none of which has a C++ counterpart yet, so in integrated play 81 always pushes due east until those slices land; each of those slices' acceptance checks must re-verify 81 end-to-end)
- **Movement/AI leftovers:** none. 229 landed with the turn-and-jump
  batch; 204 with the object-avoidance batch; 206 with the spawn_along_path
  batch; 217 with the stats slice; 85 (`set_rolling`) with object rotation
  math; and 226/242 with the rope-swing batch.
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
- **Camera/cutscene/debug (no camera/cutscene port):** 131, 203
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
  sky state 202, waypoint tables 221,
  projectiles 219, sniper-stance player gating
  220, item-chain links (29/49 landed with the chain-link batch)

Do not implement any line of the list above without its subsystem and a test
that exercises it; an unverifiable blind port risks a latent gameplay bug that
is worse than the current documented no-op.

## Owned-case classification (Task 2 remainder)

Method (reproducible): brace-match the C# `ScriptVm.Execute` and the C++
`ScriptVm::execute` method spans, take the set of numeric `case` labels in each,
and compare as sets. Current count: C# **238**, C++ **203 labels / 202 unique**,
remaining **37**. The C++ set has the nested `case 0:` mode arm inside opcode
249's switch, which is not a dispatch, so there are **201 true dispatches** and
201 + 37 = 238. Recounted 2026-10-27 after opcode 229 was added; the raw C++
label count includes one nested switch arm and one duplicate label in the nested
switch.

Two axes, kept separate:

- **Completeness** — does the dispatch case do everything the C# case does *and
  is its gameplay effect realized by the subsystems present in this tree?* A
  case that faithfully mirrors the C# operand layout but delegates its effect to
  a partial or unported subsystem is **partial**, not done (the CONTINUATION's
  "move command 6 on partial motion is NOT done").
- **Test coverage** — the end-to-end (level, tick) goldens in
  `tests/scripts/tick1.cpp` compare the full object table and script state
  against the oracle byte-for-byte through t600 on levels 3-8. Per-opcode unit
  tests now exist for 34 opcodes: the 11 the
  harness twin covers (1, 12, 13, 64, 65, 66, 67, 68, 71, 252, 253) plus
  32/129, 226/242, 156, 81, 206, 217, 243, 77, 85, 74, 213/214,
  179/180/228, 29/49, 15/27, 204 and 229, which pin the oracle
  directly (no C# counterpart exists); the other owned cases are golden-covered
  only where they fire.

**Stubs: 0.** No owned case is a fake implementation. The only no-op case (248
`push_kurt`) is a documented honest no-op because its subsystem (Kurt's
knock-back) does not exist yet; it decodes and advances the PC exactly as the
C# does and writes no state the goldens read. (89 `play_sound` was the other
honest no-op until the mixer slice landed; it is now a real implementation.)

**Partial — faithful dispatch, effect gated behind a partial/unported subsystem
(10 cases), each verified against the code. (24 and 151 left this table with
the mixer call-sites batch: the frame-sound trigger and the door sounds now
play, so both are complete.)**

| Opcode | Name | Gating subsystem (verified) |
|---|---|---|
| 6 | move_to_target | sets `move_command = 6`; `ScriptRuntime::updateCommand` (script_runtime.cpp:1781) has no case for 6 → no motion |
| 16 | set_health | on health 0 calls `kill()`, which is a death-slice placeholder (script_runtime.cpp:2944 sets only `dead`; death script / debris / drops unported) |
| 56 | add_health | on death calls the same `kill()` placeholder |
| 140 | group_set_texture | sets the group material VM state; the renderer ignores it (open item 1, dynamic scene updates) |
| 165 | if_no_bomb | reads `itemsBomb`, which is never assigned (script_runtime.h:226, `= nullptr` only) → always "no bomb" until the Items slice |
| 166 | if_bomb_visible | same `itemsBomb` gap → always false |
| 215 | screen_flash | writes whiteFlash, but fade and render consumers are not ported |
| 240 | set_hurt_flash | writes knockDamage, but Kurt's knock-down state machine is not ported |
| 244 | set_global_573bd4 | writes invulnerability, but per-tick timer decay is not ported |
| 248 | push_kurt | honest no-op; Kurt knock-back unported (no observable state the goldens read) |

**Complete — effect realized with the subsystems present (191 cases):** every
owned case not in the partial table above (201 true dispatches − 10 = 191). These are the pure
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
119, 123, 127, 161, 176, 185, 187, 188, 191, 198, 200, 201, 205, 211, 212, 213,
214, 216, 218, 222, 230, 232, 236, 237, 238, 243, 245, 250, 251, 252, 253); control flow (1, 4,
9, 12, 64, 94, 95, 125, 183); explicit no-ops (77, 239); group state (98, 99, 162, 163, 168, 194); spawn
(113, 149, 150, 152, 153, 156, 206); partner links (179, 180, 228); chain links
(29, 49 — with movement command 30, which moves the chain); alarm state (15, 27). The authoritative complete set is the owned set minus
the 10 partial cases, not this illustrative grouping. (206 joined the spawn group with the 206 batch; 217 joined the new stats-counters group with the stats slice; 243 joins the conditions group with the vision-condition batch.)

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
death path, the renderer material remap, the Items.Bomb slice),
each of which is its own increment with a test — not a blind port.
