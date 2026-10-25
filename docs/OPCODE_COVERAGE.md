# Script VM opcode coverage audit (Phase 2 §3.1)

Method and reachability evidence in the final Phase 2 report. Summary:

- Counts recounted from the brace-matched method spans (C# `ScriptVm.Execute`
  vs C++ `ScriptVm::execute`, numeric `case` labels, set semantics): the C#
  owns **238** cases, the C++ owns **161**, leaving **77** unimplemented. The
  earlier version of this document claimed 155/83 and listed 114, 171 and 209
  as ported; none of the three is in the C++ `execute()` — the old counts and
  the old "ported" list were wrong. The remaining cases fall through to
  `noteUnimplemented` (decoded, no-op, scripts stay in sync).
- The reachability probe (C++ `unimplemented()` after running each level to
  t600) was rerun after this batch: of the remaining set, only **109** (L3, L5),
  **138** (L5), **175** (L6) and **249** (L5) fire in levels 3-8. Their stateful
  meanings remain unported; the harness dumps do not observe their effects.
  They are real gaps, not confirmed harmless no-ops. (99 and 168, which fired
  before this batch, are now implemented.)

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

## Remaining 77 (verified set)

13, 27, 28, 29, 32, 49, 74, 77, 81, 85, 107, 109, 112, 114, 115, 121, 128, 129,
130, 131, 132, 133, 135, 136, 137, 138, 139, 142, 143, 156, 158, 159, 167, 170,
171, 172, 173, 174, 175, 178, 179, 180, 181, 184, 189, 190, 192, 195, 199, 202,
203, 204, 206, 208, 209, 210, 213, 214, 215, 217, 219, 220, 221, 223, 224, 226,
228, 229, 233, 239, 240, 241, 242, 243, 244, 247, 249

By blocking subsystem (carried grouping, corrected for this batch):

- **Kurt/player/HUD (needs Kurt items, ammo, health, platform, firing, camera
  state):** 109, 112, 114 (if_player_on_me), 171, 173, 174, 175, 178, 181, 189,
  209, 215, 240, 241, 244, 247; 170 (inherit_kurt_vel) needs Kurt's velocity;
  135 (raise_573aa8) needs the screen-shake state.
- **Arena/world (needs arena texture/sky/fan/wind state, spawn helpers):**
  28, 130, 133, 137, 138, 139, 142, 143, 156 (spawn_at_point), 159, 190, 202,
  223, 224
- **Effects/combat/debris/partners (needs parts, debris, shatter, Items.Bomb,
  chain/partner links, raycast normals):** 32, 81, 115, 121, 128, 129, 132,
  136, 158, 167, 172, 179, 180, 184, 192, 195, 208, 213, 214, 228
- **Movement/AI leftovers (needs partner links, rolling-basis math,
  jump/swing mechanics):** 85, 204, 206, 217, 226, 229
- **Audio (no mixer port):** 27, 107, 233, 249
- **Camera/cutscene/debug (no camera/cutscene port):** 13, 77, 131, 203, 243
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
and compare as sets. This yields C# **238**, C++ **161**, remaining **77** (the
C++ owns no case the C# lacks). The same extraction is what produced the counts
above, so the owned set here is the same 161, not a re-count from memory.

Two axes, kept separate:

- **Completeness** — does the dispatch case do everything the C# case does *and
  is its gameplay effect realized by the subsystems present in this tree?* A
  case that faithfully mirrors the C# operand layout but delegates its effect to
  a partial or unported subsystem is **partial**, not done (the CONTINUATION's
  "move commands 6/15 on partial motion is NOT done").
- **Test coverage** — every owned case is exercised only by the end-to-end
  (level, tick) goldens in `tests/scripts/tick1.cpp`, which compare the full
  object table and script state against the oracle byte-for-byte through t600 on
  levels 3-8. There are **no per-opcode unit tests**, so in that sense all 161
  are insufficiently-tested; the goldens give end-to-end coverage for whatever
  actually fires in those levels/ticks.

**Stubs: 0.** No owned case is a fake implementation. The only no-op cases (89
`play_sound`, 248 `push_kurt`) are documented honest no-ops because their
subsystems (the audio mixer, Kurt's knock-back) do not exist yet; they decode
and advance the PC exactly as the C# does and write no state the goldens read.

**Partial — faithful dispatch, effect gated behind a partial/unported subsystem
(11 cases), each verified against the code:**

| Opcode | Name | Gating subsystem (verified) |
|---|---|---|
| 6 | move_to_target | sets `move_command = 6`; `ScriptRuntime::updateCommand` (script_runtime.cpp:1561) has no case for 6 → no motion |
| 15 | alarm | sets `move_command = 15`; same — no `updateCommand` case for 15 |
| 16 | set_health | on health 0 calls `kill()`, which is a death-slice placeholder (script_runtime.cpp:2451 sets only `dead`; death script / debris / drops unported) |
| 24 | set_id_and_name | stores `frame_sound`; playback needs the unported mixer |
| 56 | add_health | on death calls the same `kill()` placeholder |
| 89 | play_sound | honest no-op; mixer unported (no observable state the goldens read) |
| 140 | group_set_texture | sets the group material VM state; the renderer ignores it (open item 1, dynamic scene updates) |
| 151 | door_set_names | stores door sound names; playback needs the unported mixer |
| 165 | if_no_bomb | reads `itemsBomb`, which is never assigned (script_runtime.h:226, `= nullptr` only) → always "no bomb" until the Items slice |
| 166 | if_bomb_visible | same `itemsBomb` gap → always false |
| 248 | push_kurt | honest no-op; Kurt knock-back unported (no observable state the goldens read) |

**Complete — effect realized with the subsystems present (150 cases):** every
owned case not in the partial table above (161 − 11 = 150). These are the pure
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
the 11 partial cases, not this illustrative grouping.

Note on 251 (`set_target_mode`): its own effect (set `target_mode`, set/clear
the alien target) is realized, so it is classified complete; the known
decoy/`AlienTarget` targeting gap lives in the targeting *selection*, not in this
dispatch case.

Closing the partial cases requires their subsystems (motion commands 6/15, the
death path, the audio mixer, the renderer material remap, the Items.Bomb slice),
each of which is its own increment with a test — not a blind port.
