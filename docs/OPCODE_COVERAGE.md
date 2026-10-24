# Script VM opcode coverage audit (Phase 2 §3.1)

Method and reachability evidence in the final Phase 2 report. Summary:

- The C# `ScriptVm.Execute` owns 238 numeric cases; the C++ `execute()` owns
  145 after the Phase 2 coverage batches. 93 opcodes remain unimplemented and
  fall through to `noteUnimplemented` (decoded, no-op, scripts stay in sync).
- Reachability probe (C++ `unimplemented()` after every golden level/tick):
  only opcodes **99, 138, 168, 175** fire in levels 3-8 through the golden
  frontier (t600). Their current no-op treatment matches the oracle's
  observable state, so they are documented gaps, not golden divergences.

## Ported in this phase (each verified against the C# case body, goldens, gates, and the ARM target build)

119, 250, 246, 87, 114, 171, 209 (runtime/entity state + Kurt/platform probes),
79, 211, 120, 122, 160, 185, 38, 127, 103, 237, 56, 34, 183 (pitch/pos/health/setters and compare/box/branch opcodes),
39, 201, 80, 126, 157 (velocity shifts and aim pitch; `firstOther` gained an optional instance filter),
20, 21, 33, 91 (path slice), 62, 234, 191 (angle/kurt-delta),
110, 218, 197, 164 (delete, pitch alias, DTI advance spots, path speed by Kurt).

## Remaining 93, by blocking subsystem

- **Kurt/player/HUD (needs Kurt items, ammo, health, platform, firing, camera state):**
  87, 109, 112, 171, 173, 174, 175, 178, 181, 189, 194, 209, 215, 240, 241, 244, 247
- **Arena/world/groups (needs group-hit/destructible arrays, arena counters, texture/sky/fan/wind state):**
  28, 99, 130, 133, 137, 138, 139, 140, 142, 143, 159, 162, 163, 168, 190, 202, 223, 224
- **Effects/combat/debris/partners (needs parts, debris, shatter, Items.Bomb, chain/partner links, raycast normals):**
  32, 81, 115, 121, 128, 129, 132, 136, 158, 167, 172, 179, 180, 184, 192, 195, 208, 213, 214, 228
- **Movement/AI leftovers (needs partner links, rolling-basis math, jump/swing mechanics):**
  85, 204, 206, 217, 226, 229
- **Audio (no mixer port):** 27, 107, 233, 249
- **Camera/cutscene/debug (no camera/cutscene port):** 13, 77, 131, 203, 215, 243
- **Not identified in the reference (no safe port exists):** 199, 210, 239
- **Needs its subsystem or data the tree does not have (deferred with cause):**
  alarm/trigger state 15, object attach 74, group textures 140, arena spawn
  helpers 156/159, sky state 202, range-based object search over arena + Items
  interfaces 245, waypoint tables 221, partner links 213/214, projectiles 219,
  point-line debug telemetry 242, sniper-stance player gating 220, item-chain
  links 29/49
- **Needs `FindObject`/Items range logic:** 245

Do not implement any line of the list above without its subsystem and a test
that exercises it; an unverifiable blind port risks a latent gameplay bug that
is worse than the current documented no-op.
