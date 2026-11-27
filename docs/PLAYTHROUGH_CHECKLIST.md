# Playthrough checklist: the 11-26/27 combined build (New 3DS)

Build: `test/3ds-combined-11-26` at fbfa9459. Everything below comes from
`docs/WORK_LEDGER.md`, `docs/OPCODE_WATCHLIST.md`, `docs/MIP_GLITCH.md` and
`docs/3DS_ARCHITECTURE.md`. Nothing here was checked on a New 3DS before tonight.

## How to use it

Play every level in order. For each bug, write down: **level, arena (if known),
what you did, what you saw**, and your toggles (HD, EDGE SMOOTHING, MIPMAPS,
ROUND CAMERAS, SCOPE IN 2D, 3D slider). Then look it up here. If a change below
touched that place or system, write its hash next to the bug. The coordinator
first compares that spot against the build **before** that merge
(`<hash>^1`) before anyone root-causes it. A bug with no matching item is
probably older than tonight.

The game log is `sdmc:/3ds/mdk/mdk.log`. Copy it off the card after the session.

## The merges, in order (first-parent line of test/3ds-combined-11-26)

| Hash | What |
|---|---|
| c95310c9 | One streaming worker on core 1: music, file reads, log (direct commit) |
| f5c94f6c, 17889b9d | HD through the door bubble: 512 everywhere, raw HD pack, 30 MiB linear (direct commits) |
| 49db95d9 | Upstream gameplay fixes, and the level 2 (LEVEL6) cannon |
| c5221a5b | Upstream presentation fixes (trails, explosions, HUD, fall pause, arena sounds, board) |
| 2315ec29 | Decals: coplanar details stop flickering (depth pull) |
| d2ba986f | Bullet cams drawn on the 3DS; ROUND CAMERAS toggle |
| 59150ee1, 7802a292 | Loading bar moves; bar follows the HD window's wait |
| b1bf4428 | Review fixes: decals inside the bullet cams; the bar never goes back |
| 598a2bbc | Bullet cams drawn in one eye, copied into the other |
| d4438bb5 | LEVEL4 ride end: the camera's arena drawn over the other one |
| 2216f98e | Fall pause menu: fresh each fall; its OPTIONS pages work |
| ff13fd11 | Every frame loop (menus too) checks the run's end |
| 6c5b3bcb | Every sound effect placed in the DSP buffer at level load |
| 88d083f2 | Script opcodes: pendulum, comm screen, dog tethers, box sprites |
| cd367cba | Heap: reloads no longer raise the heap's top |
| 12118fd0 | Doorways round 1: second bubble hop, hints built in order, objects warmed |
| 5ec993b7 | Mip glitch fix (non-square ETC1 stops at its 16 level) |
| d71b8904 | No texture file read on core 0 in play; fonts made at load; effects warm restarts |
| fbfa9459 | Doorways round 2: no in-play shadow readback, view builds under 3 ms, texture index, layouts warmed |

No code: 20e2521f (arena-entry stutter, already in), dd5317df (L7 "regression"
report), f517348b / 92011514 (ledger).

---

## Look for these

Game order: LEVEL7, LEVEL6, LEVEL3, LEVEL4, LEVEL8, LEVEL5. Each level starts with
its fall; run the **Every fall** list (under "Every level") on at least one.

### Menus and loading

- [ ] **Loading bar moves** -- 59150ee1, 7802a292, b1bf4428
  - Where: the loading screen before every level.
  - Right: the bar steps up through the load (about 23, 42, 57 ... 72 %), then
    fills to 100 % while the HD files arrive. It never goes backwards. HD off:
    72 % jumps to full at the end.
  - Wrong: bar stuck at 0 or at one step, goes backwards, or the loading picture
    vanishes mid-load.
- [ ] **Fall pause opens with START** -- c5221a5b (upstream 75ec50e2)
  - Where: any fall, press START.
  - Right: menu (Resume, Options, Main menu, Quit) over the frozen fall. START
    or A picks, B resumes. The fall carries on from where it stopped.
  - Wrong: START skips the fall (the old test shortcut), or the fall keeps moving.
- [ ] **Fall pause -> MAIN MENU -> another level** -- 2216f98e
  - Where: a fall, START, MAIN MENU, then start another level.
  - Right: briefing, then that level's fall.
  - Wrong: briefing, then straight back to the main menu (the bug this fixed).
- [ ] **Fall pause OPTIONS pages** -- 2216f98e
  - Where: a fall, START, OPTIONS, each page (Graphics, Audio, Game).
  - Right: every row has its label; a change (e.g. volume) applies at once in
    the fall and is still set after the fall and after a restart.
  - Wrong: blank labels, a change that does nothing or is not saved.
- [ ] **Quit and HOME from menus** -- ff13fd11
  - Where: QUIT from the main menu; HOME -> close from a menu.
  - Right: clean exit back to HOME.
  - Wrong: hang, black screen, or the game sitting in the menu.
- [ ] **Text on first show** -- d71b8904 (font atlases made at load)
  - Where: HUD text, START menu, briefings, the first time each appears.
  - Right: text looks as before, no hitch the first time a menu opens.
  - Wrong: missing, garbled or blurry text.
- [ ] **START menu after a death** -- cd367cba (menu fonts now belong to the level)
  - Where: die, Continue, open the START menu in the reloaded level.
  - Right: menu text normal.
  - Wrong: empty or garbled START menu text, or a crash on open.
- [ ] **ROUND CAMERAS row** -- d2ba986f
  - Where: START menu (level).
  - Right: the row exists, on by default; off = empty bullet-cam windows, steady 30 fps.

### LEVEL7 (game level 1)

- [ ] **Mip glitch: the start tunnel** -- 5ec993b7
  - Where: the start tunnel (DANT_1), the legs of the inner archway (C7_WALL).
    Check with EDGE SMOOTHING **on** and **off**, MIPMAPS on, HD on.
  - Right: the leg is smooth at every distance.
  - Wrong: a hard-edged dark/white block on one leg (a small copy of the texture).
- [ ] **Mip glitch: practice-room targets through the scope** -- 5ec993b7
  - Where: DANT_2, the practice room. Scope in, then zoom **out** until the
    bullseye targets are small (roughly 15-30 pixels across). Edge smoothing on
    and off.
  - Right: clean bullseyes at every zoom.
  - Wrong: a small bullseye in the lower-left of a target, or a smear.
- [ ] **Bullet cams** -- d2ba986f, 598a2bbc, b1bf4428
  - Where: practice room (DANT_2) or anywhere you snipe: fire, watch the three
    ovals at the top of the scope.
  - Right: an oval shows the round's chase view while it flies and after a hit.
    3D slider up: the oval looks the same in both eyes, flat (no depth). Slider
    down, or SCOPE IN 2D: same picture.
  - Wrong: empty ovals with ROUND CAMERAS on; one eye's oval rotated, shifted,
    garbled or empty (close one eye at a time); flickering details inside an oval.
  - Expected: ~25 fps while an oval is watching (known, accepted).
- [ ] **Doorway into the practice room** -- 12118fd0, fbfa9459
  - Where: CDANT_1 -> DANT_2 (first sunlit arena on this level).
  - Measured worst frame (Old 3DS): 59.5 -> 65.1 -> **35.0 ms**.
  - Right: at most one dropped frame. Shadows look as before.
  - Wrong: a visible hitch, missing or garbled shadows.
- [ ] **Barricade soldiers sing and taunt** -- c5221a5b (arena sounds, upstream 683a53ef), 6c5b3bcb
  - Where: DANT_2 barricade soldiers (SING, TAUNT1).
  - Right: you hear them, once, on cue.
  - Wrong: silence, or a late sound.
- [ ] **Comm screen animates** -- 88d083f2 (opcode 133)
  - Where: DANT_5, the comm device's screen (M_COMM).
  - Right: the screen animates (6 frames/s); faster (12) once you hit it.
  - Wrong: frozen screen. (A thin bleed at the screen's edge row is known.)
- [ ] **Window cannon** -- 49db95d9 (upstream d14ed0b5, objects run newest first)
  - Where: DANT_6, the cannon XD6GUN.
  - Right: it fights normally.
  - Wrong: it blows up as soon as Kurt comes near (the old bug).
- [ ] **Decals: windows and tube lights** -- 2315ec29
  - Where: DANT_6 windows (D6_WIN1); DANT_7 ceiling tube lights and their rims.
  - Right: steady while you move and turn.
  - Wrong: flicker / z-fight, or a light visible through a wall.
- [ ] **Doorway CDANT_3 -> DANT_4** -- 12118fd0, fbfa9459
  - Measured worst frame: 100.2 -> 46.5 -> **47.5 ms**.
  - Right: at most one dropped frame. Wrong: a clear stall.
- [ ] **Chains and the boss** -- 49db95d9 (upstream 94bf1d0b, a48e64c7, 3005da70)
  - Where: DANT_3, the ship XE's chain by the tank; the boss XB1.
  - Right: chain hangs from the ship's hook; XB1's bolts leave his gun.
  - Wrong: chain from the wrong point; bolts start in the floor.

### LEVEL6 (game level 2)

- [ ] **The cannons on the row** -- 49db95d9 (419613a6, upstream 3b072d2a)
  - Where: OLYM_4, stand on the middle cannon, hold fire.
  - Right: it fires every shot; on the third it flies through the window
    (COLYM_4) and drops Kurt in OLYM_5.
  - Wrong: nothing happens when Kurt fires on it.
- [ ] **Jump on the flying cannon** -- 49db95d9 (upstream 7f0dfca8)
  - Where: the cannon in flight, jump.
  - Right: Kurt stays over it. Wrong: left up to ~10 units behind.
- [ ] **Decals: hub rims, mirror tiles** -- 2315ec29
  - Where: OLYM_5 hub rims; OLYM_7 mirror/floor tiles.
  - Right: steady. Wrong: flicker.
- [ ] **Pendulum** -- 88d083f2 (opcode 226)
  - Where: OLYM_6, the pendulum (XSWINGB) over the path.
  - Right: swings on a white rope from far above, with its sound; it can knock
    Kurt down. The room is **harder** than before (it used to hang still).
  - Wrong: hangs still, no rope, rope detached from the bob, rope too thick or
    missing in one eye.
- [ ] **Steep slope** -- 49db95d9 (upstream 3f9315c5)
  - Where: OLYM_8 slope, run downhill.
  - Right: Kurt keeps running. Wrong: keeps dropping into the fall animation.
- [ ] **Chute in wind** -- 49db95d9 (upstream dce8d904)
  - Where: wind zones in the air (e.g. under OLYM_3's hatch, COLYM_3).
  - Right: chute stays shut while blown. Wrong: chute opens in the wind.

### LEVEL3 (game level 3)

- [ ] **Start position** -- 49db95d9 (upstream 3bb211b6)
  - Where: level start.
  - Right: Kurt stands **on** the start pad (1.81 units higher than before).
  - Wrong: sunk into the pad, or falling at start.
- [ ] **Hover ship deck** -- 49db95d9 (upstream e1fde558)
  - Where: the hover ship XGS.
  - Right: Kurt stands on the deck. Wrong: floating level with the fins.
- [ ] **Box sprites** -- 88d083f2 (opcode 159, upstream 84ad71e4)
  - Where: the HMO rooms (HMO_1, 3, 5, 6, 8): the FIRE / PULSE hazard boxes.
  - Right: the sprite fills its hazard box (stretched).
  - Wrong: squat or thin compared with the hazard, or missing.

### LEVEL4 (game level 4)

- [ ] **Comm screen animates** -- 88d083f2 (opcode 133)
  - Where: MEAT_3, the comm device's screen. Same as LEVEL7 DANT_5.
- [ ] **Snowboard: board stays whole** -- c5221a5b (upstream a27ca2f4, 03ee9031)
  - Where: the board runs (CMEAT_3 bends, MEAT_1 flat, the tubes).
  - Right: the board always shows whole under Kurt.
  - Wrong: board half under the snow, or vanishing.
- [ ] **Snowboard: bones and worms** -- 49db95d9 (upstream e1d13fdf), 88d083f2
  - Where: the board runs.
  - Right: red bones and EWJ pickups are there (20 bones, 11 EWJ) and can be
    picked up; a bone is 9 x 6, a row taller than before.
  - Wrong: no bones/worms, or squashed ones.
- [ ] **Second board run** -- 49db95d9 (upstream 6ccc0cf4)
  - Right: ends in MEAT_5. Wrong: Kurt stuck in MEAT_4's bank / over no floor.
- [ ] **Ride end: Kurt and board whole** -- d4438bb5, 88d083f2 (back objects)
  - Where: end of the board tube (CMEAT_1, by the doorway to MEAT_3). Also walk
    back up the tube.
  - Right: tube snow and walls; Kurt and board whole.
  - Wrong: a flat dark grey plane over the floor, Kurt cut at the knees; or
    MEAT_3 objects / glass drawn wrong through the doorway.
- [ ] **Doorway CMEAT_5 -> MEAT_5** -- 12118fd0, fbfa9459
  - Where: from MEAT_6 through the short connector CMEAT_5 into MEAT_5 (first
    sunlit arena). Try at turbo.
  - Measured worst frame: 336.9 -> 84.5 -> **45.5 ms**.
  - Right: at most one dropped frame; MEAT_5 textures sharp at once (no pop-in);
    shadows as before.
  - Wrong: a stall, blurry-then-sharp textures, missing shadows.
- [ ] **Box sprites** -- 88d083f2
  - Where: MEAT_5 (2 boxes). Same check as LEVEL3.
- [ ] **Door jump** -- 49db95d9 (upstream e96fb32e)
  - Where: MEAT_7, jump over the board through the closed X4DOOR.
  - Right: Kurt stops at the door. Wrong: passes through it.
- [ ] **FLY sound** -- c5221a5b, 6c5b3bcb
  - Right: heard. Wrong: silent.

### LEVEL8 (game level 5)

- [ ] **Doorway CGUNT_1 -> GUNT_2** -- 12118fd0, fbfa9459
  - Measured worst frame: 202.1 -> 97.0 -> **68.5 ms** (on the line; the
    arena's scripts).
  - Right: one dropped frame, maybe two. Wrong: a clear stall.
- [ ] **Walkway** -- 49db95d9 (upstream 3bb211b6)
  - Where: GUNT_7 walkway with no side wall.
  - Right: Kurt walks on it. Wrong: sunk ~2 units into it.
- [ ] **Dog tethers** -- 88d083f2 (opcode 242)
  - Where: GUNT_10, the dog (XBN) on its perch.
  - Right: four white tether lines; they drop (4, 3, 2, 0) before the howl.
  - Wrong: no lines, lines from the wrong point, lines that never drop.
- [ ] **EWJ pickups** -- 49db95d9 (upstream e1d13fdf)
  - Right: 2 EWJ worms present. Wrong: missing.
- [ ] **Gunter's tube (stream)** -- 49db95d9 (upstream 8fceff8c), 6c5b3bcb
  - Right: Gunter flies head first down the stream; sounds play.
  - Wrong: Gunter lying sideways; missing sounds.
- [ ] **XFORK sound** -- c5221a5b, 6c5b3bcb
- [ ] **Heavy stretches** -- see "Known, not fixed": frame drops on L8 are expected.

### LEVEL5 (game level 6)

- [ ] **Dog tethers** -- 88d083f2
  - Where: MUSE_5. Same check as LEVEL8 GUNT_10.
- [ ] **Box sprites** -- 88d083f2
  - Where: MUSE_5 (2 boxes). Same check as LEVEL3.
- [ ] **Decals: flat panels** -- 2315ec29
  - Where: MUSE_3 / MUSE_4 flat-colour panels.
  - Right: steady. Wrong: flicker.
- [ ] **Re-entering MUSE_1** -- d71b8904 (effects warm restarts)
  - Where: leave MUSE_1, come back later.
  - Right: drops and trails appear with no hitch (was a 238 ms frame, now 73 ms).
  - Wrong: a stall the first time a drip or trail shows; missing effects.
- [ ] **SMASH sound** -- c5221a5b, 6c5b3bcb

### Every level

**Every fall**

- [ ] **Missile trails** -- c5221a5b (upstream 04c25852)
  - Right: white ribbons from the tail fins, tapering, yellower at the missile.
  - Wrong: a flat grey band, or no trail.
- [ ] **Explosions face the camera** -- c5221a5b (upstream 8bc42528)
  - Right: a missile hitting Kurt shows a fireball at Kurt.
  - Wrong: a thin yellow sliver beside him.
- [ ] **Missiles home as before** -- 49db95d9 (upstream 93743b33)
  - Steering is now per game tick, not per drawn frame.
  - Right: missiles chase as on the PC. Wrong: missiles turning much harder or softer.
- [ ] **Fall sounds** -- 6c5b3bcb (placed when the fall opens)
  - Right: every fall sound on time. Wrong: missing or late sounds.
- [ ] START pause, MAIN MENU, OPTIONS: see "Menus and loading".

**In play, everywhere**

- [ ] **HD 512 everywhere, no pop-in** -- f5c94f6c, 17889b9d (with 12118fd0, d71b8904, fbfa9459)
  - Where: every doorway, teleport, ride and scripted reveal, HD on.
  - Right: every arena sharp (512) when you first see it. No blurry-then-sharp.
    No freeze at a doorway waiting for files.
  - Wrong: a low-res texture that sharpens a moment later; a short freeze at a
    doorway; a surface with a wrong or missing texture.
- [ ] **Doorways** -- 12118fd0, fbfa9459
  - Right: at most one dropped frame crossing any doorway (L8 GUNT_2 may be two).
  - Wrong: a stall, or an arena drawn late. Note the doorway names.
- [ ] **Level load time** -- f5c94f6c, d71b8904, 6c5b3bcb
  - Measured on the Old 3DS, HD on: L3 ~14.5 s, L4 ~13.8 s, L5 12.2 s, L6 13.4 s,
    L7 15.6 s, L8 15.9 s (was 17-36 s before the HD window; L3/L4 from the HD
    A/B, L5-L8 from d71b8904's runs). Fonts at load and sounds at load each added
    a few tenths. The New 3DS should be no slower.
  - Wrong: clearly longer, or a hang on the loading screen.
- [ ] **Sound effects** -- 6c5b3bcb, c5221a5b
  - Right: every effect plays on cue, including the first time.
  - Wrong: a missing sound, a sound late the first time, a crackle, or a wrong sound.
- [ ] **Music** -- c95310c9
  - Right: never stutters or stops, including through doorways and loads.
  - Wrong: gaps, repeats, music stopping.
- [ ] **Shadows** -- fbfa9459 (in-play shadow readback removed)
  - Where: sunlit arenas, especially each level's first (L4 MEAT_5, L7 DANT_2, L8 start).
  - Right: same as before.
  - Wrong: shadows missing, garbled, or flashing on entering a sunlit arena.
- [ ] **Object textures** -- fbfa9459 (texture-name index, layouts warmed)
  - Right: every object and pickup has its usual texture.
  - Wrong: an object with another object's texture.
- [ ] **Decals in general** -- 2315ec29, b1bf4428
  - Right: posters, lights, panels on walls and ceilings steady; steady inside
    the bullet cams too.
  - Wrong: flicker, or a decal showing through a wall far away.
- [ ] **Dead-Kurt camera** -- 49db95d9 (C# FollowCamera !Dead)
  - Where: die with a wall behind Kurt.
  - Right: the corpse stays put for the ~4 s death beat.
  - Wrong: the corpse sliding, or pushed through a wall.
- [ ] **Look limits** -- 49db95d9 (upstream 47395c03)
  - Right: stick look stops at 30 degrees down / 60 up. This is new and intended.
  - Wrong: Kurt's sprite lying flat behind the floor when looking up.
- [ ] **Chute at the top of a held jump** -- 49db95d9 (upstream a91c0870)
  - Right: hold jump: the chute opens at the top of the jump at once (it waited
    ~0.4 s before).
  - Wrong: late chute, a jump that looks stepped, ledge climbs failing.
- [ ] **Camera through doors** -- 49db95d9 (upstream c81dafc1)
  - Right: the camera eases through tight doors. Wrong: a jerk or snap.
- [ ] **Kurt's sprite** -- 49db95d9 (upstream 8a3740ac, 32b1e1b0)
  - Right: gun stays in his right hand strafing left; slide brake holds its last
    frame. Wrong: gun swaps hands; brake flickers.
- [ ] **Objects newest first** -- 49db95d9 (upstream d14ed0b5)
  - The game's script order now matches the original. If an enemy, door or
    trigger behaves differently from your memory of the PC game, note it with
    this hash.
- [ ] **HUD** -- c5221a5b (upstream 235b1792, f0fa4c17)
  - Right: the dog's icon is not magenta; the HUD takes the arena's colours.
  - Note: with HD on the timer ring around the health stays full (known gap).
- [ ] **Animation smoothness** -- c5221a5b (upstream ae746b5d, 7740deb1, a73a5cd2)
  - Right: monsters, items, debris and rounds move as before or smoother.
  - Wrong: a pose snapping, a piece jumping between two spots.
- [ ] **Long session: many deaths and reloads** -- cd367cba
  - Where: die and Continue 10+ times, switch levels, load saves.
  - Right: no slowdown, no crash, no "out of memory".
  - Wrong: crashes or growing hitches after several reloads.
- [ ] **HOME and lid sleep** -- c95310c9 (never tested on a console)
  - Where: mid-level and in a fall: HOME and back; close and open the lid.
  - Right: picture, music and sounds come back; the game did not run on while away.
  - Wrong: hang, no music, double music, a jump in time.

---

## Nothing to see -- measured instead

Old 3DS appliance (`old3ds-lan167`, 268 MHz, L2 off, stereo on, HD on), one run
a side unless said.

| What | Before -> after | Hash |
|---|---|---|
| Sound effects decoded on core 0 in play (L7 route) | 23 -> **0** | 6c5b3bcb |
| Effect copies into the DSP buffer in play | 23 (21 ms) -> **0** | 6c5b3bcb |
| Effect samples on the main heap in play | 483 KB, growing -> **0** | 6c5b3bcb |
| Effect placement at load (L7) | +237 ms of load, 79 effects, 1.14 MB | 6c5b3bcb |
| Texture-file reads on core 0 in play (L5 / L6 / L7 / L8) | 3 / 1 / 0 / 1 -> **0 / 0 / 0 / 0** | d71b8904 |
| Main-thread file reads in play (L4, L7, L8) | 0 on every run | fbfa9459 |
| Core-0 texture work in play (the font atlases) | ~2.1 MB, ~480 ms over 45 frames -> 2-9 KB | d71b8904 |
| Linear free in play | +0.52-0.58 MB every level | d71b8904 |
| Heap top in play after a reload (Mandarine, 15 deaths, LEVEL3) | 30.69 MiB -> **25.58-25.71 MiB**; in-use 0 bytes per reload; cost +63 KB | cd367cba |
| Core-1 worker busy, max per 10 s (HD window A/B) | 84-89 % -> **40-49 %** | f5c94f6c |
| Core-1 worker busy, steady (L7 route) | 6-8 % (music 5.7-7.3 %) | 6c5b3bcb |
| Music underruns / DSP dropped frames | **0 / 0** | c95310c9, 6c5b3bcb |
| HD: transitions that waited / draws without the HD copy | **0 / 0** on every level | f5c94f6c |
| GPU command words per frame (L7): decals, bullet cams, load bar, ride end | 7,224 -> 7,225 | dd5317df |
| L8 linear free minimum in play (each merge's own before/after run) | 2.45 -> 2.29 (12118fd0), 2.25 -> 2.83 (d71b8904), 2.85 -> **3.18 MB** (fbfa9459) | 12118fd0, d71b8904, fbfa9459 |
| Bullet cams, one eye + copy vs both eyes | 44.4 -> **39.5 ms** while watching | 598a2bbc |

The HD window's A/B (worker busy, load times, 0 waits) is in perf/3ds-hd-bubble's
ledger (b52d2159, 5f04c266), two ledger-only commits not merged into this line.

Not measured on a console: the heap fix (Mandarine only), the fall's and the
stream screen's sound placement, LEVEL3/LEVEL4 sound and texture census.

---

## VRAM placement and load overlap (measurement pending)

> **Placeholder -- the coordinator fills this section.**

---

## Known, not fixed

- **Bullet cams cost frames:** ~39.5 ms a frame (~25 fps) while an oval watches,
  stereo on. Accepted by the owner. Off (ROUND CAMERAS) holds 30 fps.
- **LEVEL8 is heavy:** whole run ~1,840 frames over 33.3 ms, mean ~24.9 ms.
  CGUNT_1 -> GUNT_2 at 68.5 ms is the arena's own scripts (~29 ms) and first spawns.
- **Opcode 5 is unimplemented everywhere** (ours, upstream C++, both C#):
  LEVEL7 DANT_2, object XBANG. Probably a scope zoom nudge (a guess). The script
  goes on past it.
- **LEVEL3 HMO_9's arena script** stops at once on a bad first byte. Original
  data; upstream does the same.
- **Comm screen edge row** may bleed (ETC1 block across a frame boundary).
- **Timer ring stays full with HD on** (the HD copy of SC_STAT is not stamped);
  the sniper and bomber overlays keep the level's palette.
- **Ride end, not like upstream:** the other arena's effects, debris and ropes
  are not marked; Kurt and the camera arena's objects stay depth-tested against
  the other arena; a bullet cam replays the frame camera's marks.
- **Bullet cam sky** is the 3DS dome centred on the scope's eye; the sun's
  shadow map stays centred on Kurt.
- **Rope width** on the 3DS (~1.5-pixel quad) is a judgement, never looked at on
  a console.
- **Dog tether drops** (4, 3, 2, 0) were never reached in a test run.
- **An object killed by its own script** still gets one gravity/velocity step
  (the C# stops at once). No retail case checked.
- **LEVEL7 CDANT_3 costs more CPU** (script runtime 4.3 -> ~6 ms a frame): the
  original's object order (upstream d14ed0b5). The game's cost, not a bug.
- **Pose blending** costs ~0.3 ms a frame in the object pass (absorbed by vsync).
- **Heap:** a level needing more than the reserved table sizes grows a table
  once mid-level; with HD on the HD-copies table grows per new file (not measured).
- **Scripts decoded on first run** (part of L8 GUNT_2's entry cost). Preloading
  costs 2-4.8 MB of heap a level, so not done.
- **L4 MEAT_5's view** is built during the approach in MEAT_6 (one step a frame).
- **Fall pause menu** is the C# pause menu, not the level's START menu; its
  options page plays no song.
- **Not verified on the New 3DS:** everything. The mip fix in particular:
  if the glitch still shows with the fix, send `mdk.log`, the edge-smoothing
  setting and a photo (`docs/MIP_GLITCH.md` section 7). `mipprobe-owner.3dsx`
  (40 s, writes `sdmc:/3ds/mipprobe/probe.log`) answers whether the New 3DS GPU
  misreads the same level.
- **HOME / lid sleep** never tested on a console since the core-1 worker (c95310c9).
- **CIA:** `mdk_3ds/cia/mdk.rsf` has AffinityMask 1; if the kernel refuses core 1 there,
  the log says `OFF THE TWO-CORE LAYOUT`.

## Decisions waiting

- **Mip cap on wide 2:1 and 16:1 ETC1 textures:** measured right in the test
  program, capped anyway with the other non-square shapes (default: capped).
  Uncap them?
- **Pose blending on the 3DS:** pass alpha 1 (saves ~0.3 ms a frame, no frames
  gained on the L7 route), or keep the smoother animation.
- **touchDamage bounds cache:** an exact per-tick cache to win back the L7 cost
  of the newest-first order. A separate measured branch, if wanted.
- **Script decode preload at level load:** smooths L8 GUNT_2's entry, costs
  2-4.8 MB of heap (host figure) against L8's ~4.9 MB main-heap minimum.
- **L4 view build on core 1:** move the depth-layer phase to the worker
  (needs cancellation and HOME/sleep checks).
- **The play reserve:** L8 took ~10 MB of linear in play beyond the start window
  (7 MiB assumed); re-measure rather than assume.
- **Version string in the main menu** (upstream 25a11c52): needs build plumbing.

## Log lines worth a glance (`sdmc:/3ds/mdk/mdk.log`)

- `stream: worker running on core 1` -- the two-core layout is on.
- `level: loaded in N ms` -- load time per level.
- `hd: level end: 0 waits ..., 0 draws without their HD copy` -- no pop-in.
- `audio: dsp close: ... 0 underrun restart(s) ... 0 IN PLAY` -- music never
  starved, no effect copied during play.
- No `pack: MAIN-THREAD read` lines -- core 0 read no texture file in play.
