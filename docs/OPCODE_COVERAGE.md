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
suite, and the complete-vs-partial classification below is not done per opcode.

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
