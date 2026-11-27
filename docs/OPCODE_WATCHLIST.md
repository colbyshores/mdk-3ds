# Script opcode watch list for the playthrough (2026-11-26)

This list covers the places where **our C++ script VM does less than upstream's C++ 2.0**. It is
sorted in level order, so it can be read as a checklist during a full playthrough. It is a
coverage list, not a behaviour diff. A row means that one side has handling the other side lacks.
It does not say which side is right where both sides implement an opcode.

- **Ours:** `test/3ds-combined-11-26` at f517348b. The dispatch is `ScriptVm::execute` in
  `mdk_core/src/scripts/script_vm.cpp`, plus the runtime and frontend code that consumes the
  state it writes.
- **Upstream C++:** `origin/main` 367cc6fc (v2.0.0+). The dispatch is `ScriptVm::Execute` in
  `src/game/scripts/ScriptVm.cpp`.
- **C# authority:** our `src/` (1.1.1, d5bb66ec) and upstream v1.2.6.

## Fixed on `fix/opcodes-parity` (2026-11-26)

Owner decision: match the C# like for like on all four partial opcodes. The fixes follow the
**C# as the authority** (our `src/` 1.1.1 plus the upstream fixes this tree has already taken,
checked against upstream's C# v1.2.6). Upstream's C++ was used only to find what was missing,
never as the specification. Each fix's oracle comparison is in `docs/WORK_LEDGER.md`.

| Op | Commit | What now matches the C# |
|---|---|---|
| 226 | (the commit that adds this row) | `ObjectMotion.Swing` runs every tick for a `FlagSwinging` object (pitch, place, plane turn), and its rope is drawn (`RopeView`, pair 0). `L6_hazards_swing` equals the C# oracle byte for byte for 1200 steps, through the PENDHIT knock-down. |
| 133 | (the commit that adds this row) | The level draw passes each animated batch the frame the scripts set for its texture in its arena (`LevelView.cs:139`, `FrameOf`), so M_COMM runs at 6 frames/s and 12 once hit. `L4_hazards_comm` and `L7_hazards_comm` (the frame and the drawn frame, through the chain-gun hit) equal the C# oracle byte for byte. |

## Counts

Both VMs dispatch the same **238** retail opcode labels. Our VM has no case that theirs lacks,
and theirs has no case that ours lacks. The differences are in depth: whether the state an opcode
writes is used, drawn or moved by anything.

| Class | Retail opcodes | Notes |
|---|---:|---|
| Both implemented | 234 | |
| Ours a stub or no-op, theirs implements | 0 | Plus 5 opcodes from the 1996 demo, listed below. None of them is in the retail game. |
| Ours partial, theirs handles more | **4** | 133, 159, 226 and 242 |
| Ours implemented, theirs not | 0 | |

The static census found **236 distinct opcodes** in the retail scripts. Opcode 5 is one of them,
and no implementation handles it (see "Logged as unhandled" below).

## The checklist, in level order

| Where (level, arena, object) | Op | Name | Ours | Upstream C++ | C# | What to look for |
|---|---|---|---|---|---|---|
| LEVEL3: HMO_1, HMO_3, HMO_5, HMO_6, HMO_8 (11 boxes)<br>LEVEL4: MEAT_5 (2)<br>LEVEL5: MUSE_5 (2) | 159 | spawn_box | The box, its hazard and its sprite are all ported. The sprite is drawn at the image's own aspect, scaled by `max(sizeX, sizeZ)/width` (`game/effects_view.cpp:654-657`). | The image is stretched over the box's width and height, using separate X and Z texels (`src/game/objects/EffectsView.cpp:55-74`, upstream 84ad71e4). | 1.1.1 keeps the aspect. 1.2.x stretches it (84ad71e4: "LEVEL4's bones were 9 x 5 instead of 9 x 6"). | The FIRE and PULSE hazard boxes may draw squat or thin compared with their hazard box. The same draw path serves the sprite boxes that `spawn_flagged` makes for SW_EWJ and BONEFLC: the bones and worms on LEVEL4's snowboard run, and 2 EWJ in LEVEL8. A bone draws 9x5 instead of 9x6. This is cosmetic. |
| LEVEL4: MEAT_3 (2 sites)<br>LEVEL7: DANT_5 (2 sites) | 133 | arena_texture_frame | **Fixed (see "Fixed on fix/opcodes-parity").** Before: the VM writes the frame into `runtime.animatedTextures` (`script_vm.cpp:2069`). Nothing reads it back: the level draw always passes `draw.frame = 0` (`game/level_view.cpp:357`). | `LevelView` draws `frames.FrameOf(arena, batch.Animated)` for each animated batch (`src/game/level/LevelView.cpp:113`). | `ScriptVm.cs:1042`. `LevelView.cs:143` draws the frame. | The **M_COMM** texture, the comm device's screen. The scripts set it to 6 frames/s (mode 1), and to 12 once it is hit. It should animate until the device is destroyed. In ours it stays frozen on frame 0. |
| LEVEL5: MUSE_5 (4 sites)<br>LEVEL8: GUNT_10 (2 sites); object XBN, the dog (DOGHOWL) | 242 | set_2d0_block | The VM writes `rope_mask` and `rope_points`, and save/load keeps them (`script_vm.cpp:2399-2421`, `script_snapshot.cpp:473`). No frontend draws them, and nothing else reads `rope_mask`. | `RopeView` draws a line from each of reference points 1-4 to its point (`src/game/objects/RopeView.cpp:17-60`). | `ScriptVm.cs:1148`. `Objects/RopeView.cs` draws them. | The dog is held by **four tether lines**, which the script drops one at a time (the mask goes 4 lines, then 3, then 2, then 0) before the dog howls. In ours the lines are invisible, so the dog looks free from the start. The logic still runs. |
| LEVEL6: OLYM_6; object XSWINGB, the pendulum | 226 | jump_to | **Fixed (see "Fixed on fix/opcodes-parity").** Before: the VM sets the pivot, speed and gain and raises `FlagSwinging` (`script_vm.cpp:2379-2385`). Nothing swings the object: no tick code reads the flag or the `swing_*` fields. | `ObjectMotion::Swing` runs each tick while the flag is set (`ObjectMotion.cpp:53-55`, `:480-511`). It also sets `RopeMask = 1` so that RopeView draws the pendulum's rope. | `ScriptVm.cs:1140`. `ObjectMotion.cs:86` and `Swing` at `:537`. | The **pendulum** (loop sound PENDULUM; PENDHIT on touch, through `touch_damage` and `if_touching_kurt`) hangs still about 2000 units under its pivot. It has no rope, and it hits Kurt only when he walks into it. In the original it swings across the path. This changes gameplay: expect the room to be easier, or a timing puzzle to be missing. |

## Logged as unhandled when it runs

- **Opcode 5, `raise_573_91c` (operand 0.75).** It appears once, in LEVEL7 DANT_2, in object
  XBANG's script, between a `return` and `set_flag 1 3`. No implementation dispatches it: ours,
  upstream's C++, C# 1.1.1 and C# 1.2.6 all lack it. In ours it reaches `default:`, where
  `noteUnimplemented(5)` counts it. It has no branch action, so the script simply goes on to
  the next instruction. Upstream's VM counts it in `Unimplemented` the same way. Our
  `game/scope.h` identifies 0x57391c as the sniper zoom, so this is probably a zoom nudge. That
  is a guess.
- **LEVEL3 HMO_9's arena script** starts with byte 141, which is not a retail opcode. The decoder
  rejects it and the script stops at once (`restart = 0`, `script_vm.cpp:229-232`), as it does
  upstream (`ScriptVm.cpp:83-88`). It is not counted as unimplemented, because it fails before
  dispatch. Both ports behave the same here, and it is the original data's own quirk.
- **Run-time probe.** `tests/scripts/build/tick1 probe 18000` (built from this tree) ran 10 minutes
  of game time per level on LEVEL3-8 and reported **no unimplemented opcode on any level**. Kurt
  was not placed, so only the scripts that run without him were exercised. Opcode 5 was not
  reached.

## Not in the retail game: the 1996 demo's opcodes

Upstream's VM handles the demo dialect's own numbers, and ours has no case for them:

| Op | Name | Upstream |
|---|---|---|
| 300 | Nothing | no-op |
| 302 | FollowPath | `FollowBetaPath` |
| 305 | IfField108 | false |
| 328 | IfAlarmEnded | `AlarmEndedTicks`, from `ScriptRuntime.Beta.cpp` |
| 361 | Fire | `FireBolt` |

These rows are unreachable in ours. The demo's formats are parsed (`Cmi::parseBeta`,
`scriptDecoderFor`, Linux build only), but no frontend or runtime loads the demo, and the
runtime has no `IsBeta` or `BetaArena`. They matter only if the beta levels are ever wired up.

## Leads that turned out equal (the comments are stale)

Several of our comments say a subsystem is missing, but the code has since been written. The
audit checked each one against the code:

| Op | What exists now |
|---|---|
| 112, 173 | Teleport is ported (`script_runtime.h:1393` still says it is not) |
| 251 | The decoy is ported (`script_runtime_squads.inc:9`) |
| 128, 132, 136 | The effects are drawn (`script_runtime.h:882`) |
| 159 | The spawn_box pool is ported (`effects_view.h:19`) |
| 140 | The group retexture is rebuilt by both frontends (`script_vm.cpp:1773`) |
| 202, 203 | Both are saved and loaded (`docs/OPCODE_COVERAGE.md`'s partial table) |
| 248 | `knockDown` calls `stopFiring()` |
| 131 | `specialEvent` handles every arm upstream does, so its `noteUnimplemented` never fires |

## Method

- **Dispatch sets:** we took the numeric `case` labels of both `Execute` switches and compared
  them as sets. They are identical (238 each). The case bodies were extracted side by side, and
  every opcode's helpers were followed one to two calls deep, into the runtime and the
  frontends, on both trees. Three read-only reviewers each took one third of the range. Every
  partial row above was re-checked by hand.
- **Where it is used:** a scratch program (not committed) linked against this tree's own
  `formats/cmi` and `formats/script_decoder` objects. For each level, LEVEL3-8, it walks every
  arena, object and alien script from its entry point, following `next` and
  `ScriptDecoder::targets`. In that walk, 236 distinct opcodes appear at 107,856 reachable
  (script, pc) pairs. Site counts above are unique pcs per level. Arena names include the
  objects their scripts spawn. The object names (XBN, XSWINGB) come from the `spawn` that owns
  the code.
