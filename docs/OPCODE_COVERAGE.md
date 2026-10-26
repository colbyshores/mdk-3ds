# Script VM opcode coverage audit (Phase 2 §3.1)

Method and reachability evidence in the final Phase 2 report. Summary:

- Counts recounted from the brace-matched method spans (C# `ScriptVm.Execute`
  vs C++ `ScriptVm::execute`, numeric `case` labels, set semantics): the C#
  owns **238** cases, the C++ owns **173**, leaving **66** unimplemented. The
  earlier version of this document claimed 155/83 and listed 114, 171 and 209
  as ported; none of the three is in the C++ `execute()` — the old counts and
  the old "ported" list were wrong. The remaining cases fall through to
  `noteUnimplemented` (decoded, no-op, scripts stay in sync).
- The reachability probe (C++ `unimplemented()` after running each level to
  t600) was rerun after this batch: of the remaining set, only **138** (L5)
  fires in levels 3-8 — 109 (L3, L5), 175 (L6) and 249 (L5) are now ported.
  (99 and 168, which fired before this batch, are now implemented.)
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

## Remaining 66 (verified set)

13 left this list with the synthetic-harness batch (port 13 is an explicit
false branch, `ScriptVmTests.UnimplementedConditionsAreFalse` pins it, and the
C++ `unimplemented()` bucket stays clean): 27, 28, 29, 32, 49, 74, 77, 81,
85, 112, 114, 121, 128, 129,
130, 131, 132, 133, 135, 136, 142, 143, 156, 158, 159, 167, 170,
171, 172, 173, 174, 178, 179, 180, 181, 184, 189, 190, 192, 195, 199, 202,
203, 204, 206, 209, 210, 213, 214, 215, 217, 219, 220, 221, 223, 224,
226, 228, 229, 239, 240, 241, 242, 243, 244, 247

**Reachability, re-probed to t30000 (2026-10-25, `tick1 probe 30000`):**
after the Debris slice the only remaining opcode that fires anywhere in
levels 3–8 is **208** (`if_part_hit`, L5 ×9134), now ported; the probe is
clean on every level. The other 66 do not fire in this corpus at all —
they stay documented no-ops until a corpus or a synthetic test exercises
them (the rule at the end of this file). The synthetic harness
(`tests/scripts/vm_tests`, twin of `tests/Mdk.Game.Tests/ScriptVmTests.cs`)
is the second route in: 13 came through it without ever firing in the corpus.
208's neighbours in the parts
bucket (32 `clear_parts_mask`, 129/132/136 and friends) are the natural
next slice when a test can exercise them.

By blocking subsystem (carried grouping, corrected for this batch):

- **Kurt/player/HUD (needs Kurt items, ammo, platform, firing, camera
  state):** 112, 114 (if_player_on_me), 171, 173, 174, 178, 181, 189,
  209, 215, 240, 241, 244, 247; 170 (inherit_kurt_vel) needs Kurt's velocity;
  135 (raise_573aa8) needs the screen-shake state. (109 `hurt_kurt` and 175
  `if_inventory` landed with the Kurt health/inventory slice; 240/241/244 read
  the same Kurt state but do not fire in levels 3-8, so they stay deferred.)
- **Arena/world (needs arena texture/sky/fan/wind state, spawn helpers):**
  28, 130, 133, 142, 143, 156 (spawn_at_point), 159, 190, 202,
  223, 224
- **Effects/combat/partners (needs parts, Items.Bomb,
  chain/partner links, raycast normals):** 32, 81, 121, 128, 129, 132,
  136, 158, 167, 172, 179, 180, 184, 192, 195, 213, 214, 228
- **Movement/AI leftovers (needs partner links, rolling-basis math,
  jump/swing mechanics):** 85, 204, 206, 217, 226, 229
- **Audio — ported with the mixer slice:** 89 (`play_sound`), 107
  (`set_loop_sound`), 233 (`if_sound_playing`) and 249 (`if_own_sound`) landed
  with `mdk_core/src/audio/` (the sound bank, the mixer voice table, and the
  device control plane); the goldens pin the voice table and each object's
  tracked/loop voices.
- **Alarm-ticks state machine (no `AlarmTicks` runtime field; command 15 / Rides
  unported):** 27 (`if_alarm`). This is a branch on `runtime.AlarmTicks > 0`
  (ScriptVm.cs:1693), NOT an audio opcode — the earlier grouping under Audio was
  wrong. Its gate is the alarm-ticks state machine: a runtime `alarm_ticks` field,
  the per-tick decrement (`AlarmTicks = max(AlarmTicks-1, 0)`, ScriptRuntime.cs:896),
  and the setters in movement command 15 (ObjectMotion.cs:385) and Rides
  (Rides.cs:219). None of that exists in the C++ yet, and 27 does not fire in
  levels 3-8 to t600, so there is no golden to exercise it — do not blind-port it.
- **Camera/cutscene/debug (no camera/cutscene port):** 77, 131, 203, 243
  (13 `if_cheat_key` left this group: the C# is an explicit false branch, so
  no camera/cheat subsystem is needed — ported with the harness twin.)
- **Not identified in the reference (no safe port exists):** 199, 210, 239
- **Needs its subsystem or data the tree does not have (deferred with cause):**
  object attach 74, sky state 202, waypoint tables 221, partner links 213/214,
  projectiles 219, point-line debug telemetry 242, sniper-stance player gating
  220, item-chain links 29/49

Do not implement any line of the list above without its subsystem and a test
that exercises it; an unverifiable blind port risks a latent gameplay bug that
is worse than the current documented no-op.

## Owned-case classification (Task 2 remainder)

Method (reproducible): brace-match the C# `ScriptVm.Execute` and the C++
`ScriptVm::execute` method spans, take the set of numeric `case` labels in each,
and compare as sets. This yields C# **238**, C++ **173**, remaining **66**
(the C++ owns no case the C# lacks; re-run 2026-10-26 for the 13 batch, which
also corrected this section's stale 171/68). The same extraction is what
produced the counts above.

Two axes, kept separate:

- **Completeness** — does the dispatch case do everything the C# case does *and
  is its gameplay effect realized by the subsystems present in this tree?* A
  case that faithfully mirrors the C# operand layout but delegates its effect to
  a partial or unported subsystem is **partial**, not done (the CONTINUATION's
  "move commands 6/15 on partial motion is NOT done").
- **Test coverage** — every owned case is exercised only by the end-to-end
  (level, tick) goldens in `tests/scripts/tick1.cpp`, which compare the full
  object table and script state against the oracle byte-for-byte through t600 on
   levels 3-8. Per-opcode unit tests now exist for the 11 opcodes the harness
  twin covers (1, 12, 13, 64, 65, 66, 67, 68, 71, 252, 253); the other 162
  owned cases are golden-covered only where they fire.

**Stubs: 0.** No owned case is a fake implementation. The only no-op case (248
`push_kurt`) is a documented honest no-op because its subsystem (Kurt's
knock-back) does not exist yet; it decodes and advances the PC exactly as the
C# does and writes no state the goldens read. (89 `play_sound` was the other
honest no-op until the mixer slice landed; it is now a real implementation.)

**Partial — faithful dispatch, effect gated behind a partial/unported subsystem
(8 cases), each verified against the code. (24 and 151 left this table with
the mixer call-sites batch: the frame-sound trigger and the door sounds now
play, so both are complete.)**

| Opcode | Name | Gating subsystem (verified) |
|---|---|---|
| 6 | move_to_target | sets `move_command = 6`; `ScriptRuntime::updateCommand` (script_runtime.cpp:1561) has no case for 6 → no motion |
| 15 | alarm | sets `move_command = 15`; same — no `updateCommand` case for 15 |
| 16 | set_health | on health 0 calls `kill()`, which is a death-slice placeholder (script_runtime.cpp:2451 sets only `dead`; death script / debris / drops unported) |
| 56 | add_health | on death calls the same `kill()` placeholder |
| 140 | group_set_texture | sets the group material VM state; the renderer ignores it (open item 1, dynamic scene updates) |
| 165 | if_no_bomb | reads `itemsBomb`, which is never assigned (script_runtime.h:226, `= nullptr` only) → always "no bomb" until the Items slice |
| 166 | if_bomb_visible | same `itemsBomb` gap → always false |
| 248 | push_kurt | honest no-op; Kurt knock-back unported (no observable state the goldens read) |

**Complete — effect realized with the subsystems present (165 cases):** every
owned case not in the partial table above (173 − 8 = 165). These are the pure
state, condition, orientation/aim, movement-parameter, handled-movement-command,
path, animation-state, variable/flag, control-flow, group-state and spawn cases,
each exercised by the byte-for-byte goldens through t600 on levels 3-8.
Representative members, by group: orientation/aim (8, 40, 60, 62, 87, 101, 104,
105, 120, 122, 126, 134, 160, 193, 207, 234, 235, 246); movement parameters
(50-55, 82, 84, 106, 169, 177); handled movement commands (43, 46, 61, 78, 88,
93, 157, 197, 231 — `stuck_count` is maintained at script_runtime.cpp:1403/1411);
path (2, 20, 21, 33, 91, 102, 124, 164); animation (3, 17, 19, 58, 59, 92, 118,
154, 186 — `advanceAnimation` is ported and called); conditions/flags/variables
(10, 11, 14, 18, 22, 23, 25, 26, 31, 34-39, 41, 42, 44, 45, 47, 48, 54, 57, 63,
65-73, 75, 76, 79, 80, 83, 86, 96, 97, 100, 103, 108, 110, 111, 113, 116, 117,
119, 123, 127, 161, 176, 185, 187, 188, 191, 198, 200, 201, 205, 211, 212, 216,
218, 222, 230, 232, 236, 237, 238, 245, 250, 251, 252, 253); control flow (1, 4,
9, 12, 64, 94, 95, 125, 183); group state (98, 99, 162, 163, 168, 194); spawn
(113, 149, 150, 152, 153). The authoritative complete set is the owned set minus
the 8 partial cases, not this illustrative grouping.

Note on 251 (`set_target_mode`): its own effect (set `target_mode`, set/clear
the alien target) is realized, so it is classified complete; the known
decoy/`AlienTarget` targeting gap lives in the targeting *selection*, not in this
dispatch case.

Closing the partial cases requires their subsystems (motion commands 6/15, the
death path, the audio mixer, the renderer material remap, the Items.Bomb slice),
each of which is its own increment with a test — not a blind port.
