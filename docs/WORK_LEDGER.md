# Work ledger — what was done, and why

The owner asked on 2026-11-20 for every piece of work to be logged with its justification, so it can be referred back to later. Each entry says:
- **what** was done,
- **why** (the owner's direction, a measurement, or a rule in AGENTS.md / SPEC.md),
- **evidence**,
- the **commit**, or why there isn't one.

Newest entries are at the bottom. Performance experiments keep their own table in `docs/3DS_PERF_EXPERIMENTS.md`, with the protocol for measuring them; this file links to that table rather than copying it.

## 2026-11-20

### Recovery after the NAS outage (I/O errors on /mnt/nas, the second outage on 2026-11-19)
- **What:**
  - `git fsck --no-dangling` was clean.
  - All four agents that were stopped mid-work checked their own files for damage (empty, cut short or containing NUL bytes). None was damaged.
  - The gun port, left staged when the outage hit, had its checks re-run: release, formats, the full scripts suite and `tools/check.sh`.
- **Why:** the earlier run of those checks failed on NAS reads, not on code. A commit is only made after its checks pass (AGENTS.md §0).
- **Commit:** **fc3a869**, "Kurt fires the chain gun", cherry-picked from `port/kurt-firing` c775e92.

### The console stays with ../forsaken; functional checks move to Mandarine
- **What:** every 3DS change is checked in Mandarine r1.2.1 (md5 0d2adab6):
  - always through `mdk_3ds/tools/emurun.sh` on `DISPLAY=:99`;
  - always through the scratchpad wrapper `emu_one.sh`, which takes one lock so only one MDK emulator ever runs.
- **Why:**
  - The owner lent the console to Forsaken.
  - AGENTS.md P8: when there's no hardware, every batch still gets emulator evidence.
  - The owner capped MDK at one Mandarine instance (2026-11-19).
- **What the emulator can't show:**
  - Timing: it is never used to measure speed.
  - The geometry shader: Mandarine has none.
  - Core reservation: it emulates a New 3DS.
  - Clipping at the near plane: its OpenGL renderer clips there whether or not the console would.

### Emulator builds turn the geometry-shader lighting off
- **What:** emulator payloads of builds that light through the geometry shader are built with `TOGGLES='enableLighting=0'`.
- **Why:**
  - The owner's direction: "you could always toggle it off … we can get the rest of the game finished up and not be blocked."
  - With the lighting on, Mandarine draws those faces black, which hides everything else a functional check is looking for.
- **Note:** lighting itself is judged on hardware only. E11's baked path draws correctly in Mandarine either way.

### E11 (baked light and shadow) checked in the emulator, then parked
- **What:** run on levels 3 to 8, with captures. Every level is lit and shadowed, the bake loads in 39–135 ms, nothing is split by the geometry shader, and nothing crashed.
- **Why parked:** it's a performance experiment, and it gets merged on an Old-3DS profile A/B (AGENTS.md §2; the protocol in `docs/3DS_PERF_EXPERIMENTS.md`). The owner has also put gameplay porting first.
- **Evidence:** `docs/evidence/2026-11-19-3ds-baked-sun-shadows.txt` on the E11 branch (uncommitted there).
- **Commit:** none on the main branch.

### Core-1 workers (E14) checked in the emulator, then parked
- **What:** branch 6807a5f, run on level 7 for 3,000 frames:
  - both workers start on core 1;
  - the CPU-time limit reads back as 30%;
  - the music never dropped or restarted.
- **Why parked:** the owner's 2026-11-19 decision puts both workers on core 1 on both models. Merging it depends on the Old-3DS A/B showing that audio isn't starved on the time-limited core.

### Walk harness (E13) merged
- **What:** `WALK=` scripted input plus an arena-entry profile table.
- **Why:**
  - It reproduces the owner's room-entry hitch without anyone holding the console.
  - The release binary is byte-identical to before.
- **A failure that wasn't a code failure:** `linux_arg_parse` failed because `build-linux/mdk` was a Nov-19 binary from before `--walk` existed, and the suite doesn't rebuild it. The scratchpad gate script now rebuilds the Linux binary before running the suite.
- **Commit:** **09c8ed8**.

### E12 merged: chute stall fix, pause menu, controls file
- **What:**
  - Kurt's chute frames are shrunk to fit 256×256 and preloaded along with the jump frames.
  - While a frame's bytes are still loading, Kurt's previous frame is held on screen.
  - START opens a pause menu instead of quitting.
  - `sdmc:/3ds/mdk/controls.ini` is written with the owner's default layout when it's missing.
- **Why:**
  - Owner-reported hitches when the chute opens and when Kurt jumps.
  - The owner's control layout, held in a file so a remapping screen can be built on it later.
  - "R must fire the gun."
  - "I need a pause menu overlay to toggle on and off settings."
- **The preload bound went from 3 to 5 MiB.** Taken under the owner's rule of memory over texture precision: about 13 MB of linear memory stays free. The owner can ask for 4 MiB back.
- **Merge fixes:**
  - The walk harness's action names gain `sniper`.
  - The look-rate divisor in `input_3ds.cpp` is named as a float local. Its constant moved into the header, where the R11 integer-division screen can't see it; it is a float divide either way.
  - `kurt_stream_tests` passes the body mesh to `drawFrame`.
- **Evidence:** in the emulator on level 7, the controls file is written on first boot, all 164 start frames load (4.84 MB, 277 ms), and the route matches Linux.
- **Commit:** **e8c49e4**.

### Pickups, inventory and items merged
- **What:** CollectPickups, the inventory, the THROW state, and Items.cs with every item, all checked against 11 recordings from the C# version.
- **Why:** gameplay parity.
- **Commit:** **627e6bc**.

### The blue wedge when firing is not a bug
- **What:** in Mandarine, a large blue fan appeared from Kurt while he fired.
- **Finding:** it's an enemy BIGBOLT plasma bolt hitting Kurt. Linux draws the same model (frame 355, `/tmp/kilo/bolt-compare.png`).
- **Commit:** none needed.
- **Open, hardware only:** whether the PICA200 clips at this port's near plane. Queued as a console probe.

### Owner direction: port all gameplay before any more graphics work
- **What:** "I want everything else working including things like kurts machine gun, all enemies, doors, etc lets get everything ported over from the C# code in to C++ and then we can revisit the graphics."
- **Consequences:**
  - A gap audit, C# compared with C++, ranks the remaining port jobs by what stops a level being finished.
  - Port agents run in parallel only where they don't share the same files.
  - Every port is checked against recordings from the C# version.
  - Graphics work is parked: E11, E14, the console A/Bs, HD imports and point lights.
  - The menu sliders and 30 fps cap job, already running, finishes, but its merge waits behind the gameplay ports.
- **In flight:**
  - HUD pickup messages and the tornado (Twister) trail.
  - The level-7 corridor wall: is it a port divergence?
  - The gap audit.

### Gap audit, C# vs C++; the first wave of gameplay ports started
- **What:** a read-only audit compared the C# oracle `src/` with `mdk_core` and both frontends. Its result is `docs/PORT_GAPS.md`: 21 packages, ranked by what stops a level being finished, with the files they share and the order they merge in.
  - Its opcode counts come from decoding every reachable script in LEVEL3-8. They correct `OPCODE_COVERAGE.md`'s claim that "none of the 11 fire": that held only because the reachability probe's Kurt stands still.
- **Why:** the owner's 2026-11-20 direction to port everything from the C# before graphics work resumes.
- **The headline:** no level can be finished today. Opcode 131 (`special_event` → `EndTheLevel`) is unported, and neither frontend can load the next level.
- **Wave 1:** three agents in separate worktrees, all following the same written brief: golden-gated against C# probes, full gates, a ledger entry each.
  - #1 end of level;
  - #3 solids, platforms and lifts, which also fixes the VM default case: the C# branches to the else-gosub, the C++ skipped it;
  - #4 the combat and hazard opcodes 158/159/167/172/178/184, plus path speed set by Kurt.
- **Merge order:** they merge one at a time; #2 (loading the next level) starts after #1 lands.
- **Open for the owner:** packages #19 (save/load) and #20 (menus and screens) were cut by the owner's 2026-11-05 directive. Whether "port everything" brings them back is the owner's call.
- **Commit:** this entry, with `docs/PORT_GAPS.md`.

### Level 7, DANT_1 -> CDANT_1: the second arena is faithful; Kurt walks through closed doors
- **What:** reproduced the walk harness's report on Linux headless: DANT_1 stays the active second arena from frame 227 to frame 683.
  - Wrote the C# probe `tests/scripts/corridor_probe` and its C++ twin, `tick1 dump corridor`. Both print, per step, Kurt's arena, the second arena, the solid list, his feet, and the wall he ran into.
  - Pinned `golden/L7_corridor_middle.txt` (800 steps down the middle of CDANT_1's north leg) and added a note to `docs/OPEN_ITEMS.md`.
  - No behaviour change.
- **Why:** to find out whether DANT_1 staying solid and blocking the corridor is the C#'s behaviour or a bug in the port's second-arena logic.
- **Evidence:**
  - The C# and C++ output is byte-identical over 800 steps, and over the harness's 1,400-step route.
  - Breaking the door-close release makes the golden fail.
  - The divergence happens earlier, at the door. X7DOOR starts locked. In the C# a locked door is solid (SolidsWithin, Solids.cs), so Kurt stops at y=478.4. The C++ has no Kurt-vs-object collision, so he walks through. The door never closes behind him, DANT_1 is never released, and its triangle 60 then blocks the middle of the corridor.
  - The fix is package #3 (solids, in flight). The probe's Kurt is let through by clearing the door's locked bit at step 40, so this golden must stay identical after #3.
- **Commit:** 202d811 on the agent's branch, cherry-picked here.

### HUD pickup messages and twister ribbons
- **What:**
  - Ported `Hud/Messages.cs` to `game/messages.{h,cpp}`: a fixed queue with no heap use after load, drawn last by `HudView` on both frontends.
  - `Inventory.PickedUp` became `Kurt::PickupHook`, a plain function pointer.
  - `ScriptRuntime.Messages` became `ScriptRuntime::hudMessages`.
  - Ported `Ribbon.cs` `Triangles` and `ScaleOf` to `Ribbon::triangles`. `EffectsView` draws each ribbon with the WMIBTEX texture in one draw call (`AddRibbon`).
- **Why:** parity with the C# (`Hud/Messages.cs`, `Viewer.cs:245/249/710`, `Scripts/Ribbon.cs`, `Objects/EffectsView.cs` `AddRibbon`). These were the two pieces `bef19e1` left open, under the owner's 2026-11-20 direction to port all gameplay.
- **Evidence:**
  - `items_probe hud|ribbon` produces `golden/L8_hud_queue.txt`, `golden/L7_hud_collect.txt` and `golden/L8_ribbon_tornado.txt`, and `tick1 dump hud|ribbon` matches all three byte for byte. The eleven items goldens are unchanged.
  - Breaking the code fails them: the queue time not doubled, no small font, the pickup message pushed unzoomed, ScaleOf's divisor changed, the ribbon's last segment on the wrong texture row.
  - On merge, the full set of checks was re-run on the main branch.
- **Commit:** 74d9b8f on the agent's branch, cherry-picked here.
- **Merge fix:** the corridor dump in `tick1.cpp` (d02e8ca) still called `parseItemsSchedule` with three arguments, but this port added a fourth (pushes). The dump now passes it and refuses a schedule that uses it.
- **Still open:** the town-flattened message (FlattenTown). Watch the 3DS `main()` stack, which grew about 540 bytes.

### Pause menu: sliders, choices, five look dials and a 30 fps cap
- **What:** `EnhancedMenu` gained t2k-style toggle, slider and choice items, bound to config fields in one table (no templates). The list scrolls: 20 rows, 12 visible.
  - **Five dials,** each a percent of today's value, default 100. Each scales a number the 3DS renderer already uploads.
    - haze density
    - glow strength
    - shadow darkness
    - exposure
    - tint saturation
  - **Shadow map choices:** its size, and how often it refreshes.
  - **30 FPS CAP toggle,** off by default. It uses `C3D_FrameRate(30)`, citro3d's own pacing, with no extra vblank waits. Gameplay keeps its speed because the fixed-step loop runs two steps per frame.
  - **Edge smoothing** stays on/off (owner: t2k's 2x1 only).
- **Why:**
  - The owner's dial-in workflow from t2k: tune live on hardware, fix the defaults, then cut what hurts.
  - The 30 fps cap: the owner wanted it "if it is part of the toggles and the change is meaningful".
  - Merged now, ahead of the gameplay ports: it touches none of their files, so waiting would only invite conflicts.
- **Evidence:**
  - `tests/formats`: sliders step and clamp, choices wrap, the list scrolls, the config round-trips and clamps, and every glyph the menu draws exists in the font.
  - PicaMatrix: with every dial at 100 the output is byte-identical, and each dial moves only its own number.
  - `gates_frames` is unchanged, so the default look is unchanged.
  - No hardware run yet. Owner checks are listed in the agent's report: the menu fits, each dial moves its effect live, and the cap holds a steady 33.3 ms.
- **Commit:** 5e8d9c9 on the agent's branch, cherry-picked here.

### Package 1: the end of a level
- **What:** ported the end of a level from the C#, operation for operation:
  - opcode 131 (`special_event`): the <= 50 arm, `EndTheLevel`, and `EndLevel.cs` whole (`mdk_core/src/scripts/end_level.{h,cpp}`), updated at its RunTick position; its `Finished` is the runtime's `levelEnded()` latch (the C#'s `LevelEnded`);
  - Kurt's `Frozen`, `Rising` and `Pose` (K_TAKEOF, K_FLOATC), and the follow camera's rising;
  - the torn arena and its flying pieces on Linux and the 3DS, through the existing arena and effects draws;
  - both frontends log the end and leave 0.5 s after it with the exit reason "level ended", at a marked hook for package 2;
  - Linux `--event=N` and the 3DS harness `EVENT=N`, the C#'s `--event` test option.
  - The arms above 50 (cutscenes, Gunter, the end of the game) are package 13: still counted as unimplemented, with the script's control flow the C#'s.
- **Why:** the owner's 2026-11-20 direction to port all gameplay from the C#. Every retail level ends through this opcode, so until now no level could be finished (L3-L8).
- **Evidence:**
  - The C# probe `tests/scripts/end_probe` and `tick1 dump end` are byte-identical on four goldens: `L3_end_level` (Kurt firing and turning, then frozen with keys held; a second special_event ignored), `L7_end_level` (665 pieces), `L7_end_air` (frozen mid-fall, the air pitch held at 0), `L8_end_level` (value 50). `vm_tests` pins the dispatch.
  - The goldens bite: no Frozen branch fails all four; no Rising hold fails `L7_end_air`; no LevelOver guard fails `L3_end_level`; an unfused piece rotation fails all of them.
  - `tests/formats` `ATornArenaIsRewrittenEveryFrameWithItsTrianglesCollapsed` pins the torn arena's mesh (the dynamic swap, a rewrite every drawn frame, the collapsed corners); leaving one corner uncollapsed fails it.
  - Linux headless, level 3, `--event=1`: the end starts at frame 60, LevelEnded at 459, exit "level ended" at 489; captures show the torn arena, Kurt floating and the white-out.
  - Not pinned: the camera's clearance skip while frozen (no golden reaches a blocked view). Open items in `docs/OPEN_ITEMS.md`.
- **Commit:** 22a74e0 on the agent's branch, cherry-picked onto the main branch after the HUD messages and the menu dials. Merge fixes: two additive conflicts, the tick1 includes and this ledger, with both sides kept. The end dump's `parseItemsSchedule` call now passes d7e91e4's fourth argument (pushes) and refuses a schedule that uses it.

### Combat and hazard opcodes (PORT_GAPS package 4)
- **What:** ported, operation for operation, the six opcodes that fell to the VM's default case, and the consumer of a seventh's flag:
  - 158 `touch_damage` (ScriptVm.cs:1528) over the new `ScriptRuntime::touchDamage` (ScriptRuntime.cs:2758, `mdk_core/src/scripts/hazards.cpp`): Kurt hurt once per visible part touching him, the other objects' hit and damage, death on hit, the branch on hit.
  - 159 `spawn_box` (ScriptVm.cs:1024) over `ScriptRuntime::spawnBox` and its box models (ScriptRuntime.cs:1593-1647). `EffectsView` now draws the boxes' FIRE/PULSE sprites, as EffectsView.cs:66-76 does, on both frontends.
  - 167 `move_to_bomb` (ScriptVm.cs:885), drawing the game's random for the aside where the C# does.
  - 172 `explosion`, 178 `explosion_damage` (a counted blast) and 184 `explode` (an uncounted blast, then the kill) (ScriptVm.cs:1544-1572), with `ScriptVm.Point` (op172_position).
  - 164's flag now has its consumer, `ObjectMotion.UpdatePathSpeed` (ObjectMotion.cs:189), in `motionPath`. The path push it needs (`FlagPathPushes`, ObjectMotion.cs:309-310: the path drives Push instead of the position) was also missing, so it is ported with it: LEVEL4's snowboarders ride their paths that way.
- **Why:** the owner's 2026-11-20 direction to port all gameplay from the C#. Decoding every reachable script of LEVEL3-8 counts 158 ×68, 167 ×84, 184 ×38, 178 ×36, 172 ×32, 159 ×15. Without them nothing hurt Kurt by contact, objects that should explode kept running their scripts, explosions did no damage, aliens ignored the bomb, and the fire/pulse hazards were missing.
- **Evidence:**
  - The C# probe `tests/scripts/hazards_probe` (the items probe plus `tp:` teleport, `run:` — a spawn that enters the level's own bytecode at an opcode — and `watch:`) and its twin `tick1 dump hazards` print the same bytes for nine goldens, `tests/scripts/golden/L*_hazards_*.txt`: L6 bomb (167), L5 touch (158 on Kurt), L8 bang (184), L8 kill (184's uncounted kill), L4 snow (164 and the path push), L4 blast (178's counted kill, a BIGBOLT's 158, XC's FIRE boxes), L4 touch (158's branch on hit), L3 boxes (159 modes 0 and 2, 172 mode 0) and L8 points (172 at a model's reference point). Every existing golden is unchanged.
  - Breaking the code fails them; 14 of 15 mutants are caught: no Kurt hurt (L5_touch), no object damage and never dies on hit (L4_blast), no branch on hit (L4_touch), 184 counted and 184 without its kill (L8_kill, L8_bang), 172 mode 3 at the position and no explosion (L8_points, L3_boxes), 178 uncounted (L4_blast), 159 mode 2 ignored (L3_boxes), 167 without its random draw (L6_bomb), half the path-speed easing and no path push (L4_snow), a full-size box model (L3_boxes, L4_blast). The one survivor, touch_damage's untouchable mask, is recorded in `docs/OPEN_ITEMS.md` with why no retail object can show it.
  - `docs/OPCODE_COVERAGE.md`: the six move to ported (C# 238, C++ 234 with package 1's 131, remaining 28, 114, 130, 224), and its stale partial entries are fixed after checking the code: 16/56, 165/166 and 215 are complete; 81/195 stay partial for the sniper rounds alone.
- **Commit:** the agent branch's commit, rebased onto 9853f81 (its hash is in the agent's report). Merge fixes: `effects_view.cpp` keeps package 1's `addEndPieces` and this batch's `queueSprite`; the build lists keep both `end_level.cpp` and `hazards.cpp`; the hazards schedule passes `parseItemsSchedule`'s fourth argument (d7e91e4) and refuses a `push:` token, which the hazards probe has no HUD for; 131 joins the partial table (its arms above 50 are package 13).

### The stream's simulation (package #18a): Flight, StreamTube, TubeBasis, WatcomRandom
- **What:**
  - Ported `src/Mdk.Game/Stream/{WatcomRandom,TubeBasis,StreamTube,Flight}.cs` to `mdk_core/src/stream/` (`watcom_random.h`, `tube_basis.{h,cpp}`, `stream_tube.{h,cpp}`, `stream_flight.{h,cpp}`), operation for operation and in the C#'s tick order. The tube's per-ring arrays are fixed (no per-frame allocation); the events and sounds are fixed lists.
  - `StreamRun` builds the generator, the tube and the flight in StreamScreen's order; `streamIndexOf`/`streamKindOf` are StreamScreen's index clamp and Gunter test. The seed is a parameter: `WatcomRandom::kDefaultSeed` (1) for a stream started directly, the tick count otherwise.
  - Not ported here: StreamScreen and StreamView (package #18b).
- **Why:** the owner's 2026-11-20 direction to port all gameplay from the C#. The stream sits between every level and the statistics, and after LEVEL8 it carries Kurt's health into LEVEL5 (or ends the game).
- **Evidence:**
  - `tests/scripts/stream_probe` runs the C# simulation (its four files compiled straight from `src/`) with fixed seeds and scripted steering. It prints every float as its IEEE bits, every step: Kurt, the camera, the fade, the background, the bonus or Gunter, Bones, the sprites, the events and sounds, and the generator's state. It also prints each new ring (origin, basis, a hash of its points, its segment's colours and a hash of its planes), the walk's angles, radius and shade, each spawn, and every 15 steps the transforms, the animation frames and a hash of `AddTriangles`/`Rgba` over the drawn tube.
  - `tick1 dump stream|streamrand` (tests/scripts/stream_dump.cpp) matches all seven goldens byte for byte: `stream_rand` (four seeds), `stream_L7_weave` (41 wall hits to 1 health, rescue), `stream_L7_bonus` (the health bonus, Bones at segment 177, 150 health out), `stream_L8_planet` (Gunter, straightening, the planet, Survived), `stream_L8_death` (Hard, Died), `stream_L3_rescue` (1 health), `stream_L6_stop` (Esc). They run in tick1's case list.
  - They bite. An unfused Vector3.Lerp fails all six flights (`.NET 10` lowers Lerp to a fused multiply-add on this host: 200000 of 200000 cases; the port uses `std::fma`). Damping the walk by the double 0.8 instead of the float 0.8f fails bonus and planet. Wall damage without its random draw fails four. A rand increment off by one fails all seven.
  - `otherFrame`'s `(int)Frame % FrameCount` is `(int)fmod(Frame, FrameCount)`, which is equal for a non-negative frame, so there is no runtime integer divide (R11). The libm trig sites carry R6 pins (glibc matches .NET's MathF/Math bit for bit, 20000 cases).
- **Commit:** this entry's commit.
- **Still open (#18b):** see `docs/OPEN_ITEMS.md`, "The stream's screen".

### Sniper mode: the scope and the mode (gap package #5)
- **What:** Kurt can enter and leave sniper mode, ported operation for operation from the C#.
  - `Kurt.Sniper.cs` -> `game/player_sniper.cpp` plus `Player::sniperMove` in `player.cpp`: the sniper key, entering (on a floor, alive, not knocked down or throwing), leaving (the key with SNIPEROFF; death, falling and `StopFiring` silently), the sidestep at a quarter speed, the BREATH/ZOOM loops, SNIPRELD/SNIPERSHOT, and the mouse look through the scope.
  - `Scope.cs` -> `game/scope.{h,cpp}`: the look, the zoom (keys and wheel), the clip of 3, the ammo keys, the eye and line of sight, and `ToScreen`/`Locks`/`LockLimit` for the target lock.
  - `Inventory.SelectAmmo`/`UseAmmo`/`NextAmmo` -> `Kurt`.
  - `FollowCamera.SniperView`/`CameraMath.ScopeView`/`ScopeProjection`/`UpOf` -> `follow_camera` (`buildSniperCamera`); the follow camera's clearance and mouse look skip a sniping Kurt.
  - `kKeyZoomIn`/`kKeyZoomOut` after `kKeySniper` (walk script `zoomin`/`zoomout`); the 3DS writes them `none`; Linux binds the C#'s Page Up/=, Page Down/- and the right mouse button.
  - Seams: `Player::setSniperHooks` for #6's `FireSniper`, `Scope::zoomLimit` and the lock helpers for #6's `UpdateSniperTarget`, `buildSniperCamera` and the public scope state for #7.
- **Why:** the owner's 2026-11-20 direction to port all gameplay from the C#. Every level has sniper-hit checks (`if_hit_weapon 0`); this package lets Kurt into the scope, and #6 (the rounds) builds on it to reach those targets.
- **Evidence:**
  - The C# probe `tests/scripts/sniper_probe` (Kurt alone, one schedule, levels 3 and 7: enter, look down, sidestep, zoom by keys and wheel, fire a strike round until the clip falls back to bullets, fire bullets, look up, turn, zoom out, the ammo keys, homing rounds until they run out, leave, a refused entry in the air) gives `golden/L3_sniper.txt` and `golden/L7_sniper.txt`; `tick1 dump sniper` matches both byte for byte, every float as its bits, the scope view's view-projection and projection included.
  - Two System.Numerics facts were measured on the way: `Vector3.Dot` is unfused (an fmaf chain is one ULP off the view's translation at L3 step 60), and `CreatePerspectiveOffCenter` multiplies by the reciprocals (dividing is one ULP off its M32).
  - Breaking the code fails them: `NextAmmo` stopping on an empty type (at the first ammo key, step 390), the eye's forward shift scaled by one ULP (from step 50), SNIPEROFF played on the silent exits instead of the key's (L7 step 540), the projection divided instead of reciprocal-multiplied (step 1), the fmaf-chain dot (step 60). The shot gap does not bite: at 60 Hz it never binds (OPEN_ITEMS).
  - No existing golden changed.
- **Commit:** see the git log ("Sniper mode: the scope and the mode").
- **Still open:** the rounds (#6), the frontends' scope view, overlay and listener (#7), the 3DS zoom buttons (owner), the beta helmet, the saves' clip fields.

### The fall before a level: the simulation (gap package 17a)
- **What:**
  - Ported `Fall/FallSim.cs` with its partials `FallSim.Missiles.cs`, `FallSim.Pickups.cs` and `FallSim.Radar.cs` to `mdk_core/src/fall/fall_sim.{h,cpp}`, and `Fall/FallMissile.cs` to `fall/fall_missile.{h,cpp}`. They are ported operation for operation, in the C#'s tick order. The parts are: the intro in space, steering and the pull back to the centre, the camera and its braking, the radars, the missiles and their hits, the explosions, the pickups on their chutes, Bones, the palette effects, and the end (Landed or GameOver).
  - The C# `Sound`/`Message` events became `FallSim::Hooks`, which are function pointers. The input is `FallSim::Input`: steering x/y (−1/0/1) and skip. The seeded `System.Random` is `mdk::Random`. Every place where the C# draws twice inside one call's arguments has those draws put in order explicitly.
  - The fall's inventory is a `Kurt`. Its health makes the round trip the way `Kurt.Collect` does. `Kurt::setDifficulty` is new, and is the C#'s `Inventory.Difficulty = setup.Difficulty`.
  - The handoff to the level is two calls. `FallSim::carry(FallCarry*)` is `FallScreen.Next` on Landed. `fall::carryIntoLevel(carry, Kurt*)` is `Viewer.CarryFromFall`: it collects the pickups again with no messages, then sets the health.
  - `fall::fallSetup(bni, level, difficulty, Setup*)` is the setup `FallScreen` builds: the clamped index, `FALLPU_n`, and the KURTANIM, KURT_HIT and BONESANM animations.
  - The view's parts are package 17b and are not here.
- **Why:** the owner's 2026-11-20 direction to port all the gameplay from the C#. The fall is the skydive before LEVEL7/6/3/4/8. It decides the health Kurt starts each of those levels with, and the pickups he carries into it. Until now the C++ had only the data parser (`formats/fall3d.cpp`), and the flow carried nothing (`game_flow.cpp`, `play()`).
- **Evidence:**
  - A new C# probe, `tests/scripts/fall_probe`, is built against the C# FallSim. It wrote `golden/L6_skydive_chase.txt`, `L8_skydive_hard.txt`, `L3_skydive_slow.txt` and `L7_skydive_skip.txt`. `tick1 dump skydive` matches all four byte for byte. They cover:
    - three pickups taken and carried into the level's inventory;
    - Bones, and a hard-mode death ending in GameOver with no carry;
    - 0.15 s frames clamped to three ticks a step, through the 30-33 s pull-back and fade;
    - the FALL_T1 message and Esc.
  - Each step prints an FNV-1a hash of all the public state, so a single-ULP drift fails. The full state prints every 10th step and on every event.
  - Beyond the goldens, 36 more probe-vs-C++ runs matched byte for byte, over every level (5 clamps to 8's fall), difficulty, a negative seed, 0.021 s frames and the full mode.
  - Breaking the code fails the goldens:
    - the pickup's x and y draws swapped: all four fail at the first P_FALL;
    - `carryIntoLevel` without its health: L6 and L3 fail on the `level` line;
    - the missile's homing pull at 0.21: all four fail.
  - There are no opcode changes, so `docs/OPCODE_COVERAGE.md` is unchanged.
- **Commit:** 48f570a on the agent's branch (on e0d79d4), cherry-picked onto the main branch after sniper mode (9fb8235); the only conflict was this ledger ("The fall before a level: the simulation").

### Package 2: the next level
- **What:** after a level's end, play goes on into the next level, as the C#'s `Game` does:
  - `GameFlow::skipScreen` / `skipUnported` (`mdk_core/src/game/game_flow.{h,cpp}`): the screens this port lacks are skipped the way the C# skips them. Stream: `Flight.Stop`, health kept, the menu below 1. Statistics: `StatsScreen.EndPhase` moves the level on. Fall: `FallSim.Skip` gives a carry of 100 and no pickups. Save prompt: closed. Each one is logged as "flow: skipped X (package #N not ported)".
  - Both frontends report the level (`runtime->stats`, Kurt's health), take `Event::LevelEnded`, skip, and load the level the flow names, in the C#'s order 7, 6, 3, 4, 8, 5. A level is entered at `flow.levelHealth()`: 100 after a skipped fall, LEVEL8's end health into LEVEL5. The menu after LEVEL5 ends the run.
  - A level loop in `mdk_linux/src/main.cpp` and `mdk_3ds/src/main_3ds.cpp`. The body is not re-indented, to keep the diff mergeable. The teardown runs between frames, in this order: Kurt's pack read settled (`KurtSprite::settleStream`), the music, the DSP path closed while the bank lives, the runtime, the pose caches, the arena views; then the level block's objects in reverse order; then `mdk_render_release` of the level's scope, newest first; then two empty frames on the 3DS (four on Linux), so the deferred frees land before the next level allocates.
  - The 3DS backend's `mdk_render_mark/release` was a no-op. It now keeps a creation log like the Vulkan one, skipping the renderer's own canvas mesh and white texture, and it reuses released palette and mesh slots. The Vulkan backend now frees a released texture's index copy.
  - The C#'s test options are the first level's only (Game.cs:487): `--event`, `--at`, and the 3DS `EVENT=` / `AT=`. `--event-every-level` (Linux) and `EVENT_EVERY=1` (3DS) repeat the event in every level. `--replay-level` (Linux) reloads the level that ended, as a leak test.
- **Why:** the owner's 2026-11-20 direction to port all gameplay from the C#. Before this, finishing a level stopped the game.
- **Evidence:**
  - `golden/flow_next_level.txt` is the C# probe `tests/scripts/flow_probe`'s output: the real `Flight`, `FallSim`, `LevelFlow` and `GameState`, for 7 levels × 4 end healths. `vm_tests dump-flow` matches it byte for byte. `flow_tests` also pins `skipUnported`'s trail.
  - The goldens bite. A fall carry of 99 fails the golden and the trail test. A statistics page that does not advance the level fails both.
  - `tests/scripts/level_chain_test.sh` runs headless on DISPLAY :99 with lavapipe. Level 3 chains to 4, 8 and 5, then exits at the menu, with every skipped screen logged.
  - The chained frame 600 (level 4) and frame 1100 (level 8) are byte-identical to direct `--level=4` and `--level=8` runs at their own frames 111 and 122.
  - Four level-3 teardowns leave the heap at 67.43, 67.47, 67.48 and 67.48 MB, with 42 fds and the renderer log at 0 each time.
  - Without `mdk_render_release`, the test fails: the renderer log climbs by 95 a level and the heap by 2 MB a level.
  - 3DS: the release and harness builds compile. No emulator run here; the parent runs it.
- **Commit:** this entry's commit.

### Owner un-cuts save/load and the menus and screens
- **What:** packages #19 and #20 in `docs/PORT_GAPS.md` are back in scope.
  - #19: save and load, the snapshot, LASTGAME, the save prompt.
  - #20: the main menu, briefings, the statistics screen and slideshow, the end movies, intro and loading screens, and settings (difficulty, gore).
  - The owner's 2026-11-05 directive had cut both.
- **Why:** the owner, 2026-11-20, asked whether "port everything" includes them: "yes I want them back".
- **Plan:**
  - Both start once #2 (the level-to-level flow) lands, because they hang off its screens.
  - #20 splits into three agents: menus/UI/settings, the statistics screen and slideshow, and the movies (the FLC/MVE parsers already exist).
  - `docs/OWNER_DIRECTIVE_2026-11-05.md` is not edited. This entry records the reversal.
- **Commit:** lands with the next merge.

### Package #3: solid objects, platforms, lifts and the keeper; the VM default case
- **What:** ported from the C#, operation for operation:
  - `Collision/Solids.cs` (Walk, Platform, Touching) as `game/solids.{h,cpp}`; `ArenaSpace.Move(..., solids)` and `ArenaSpace.Touching` as a delimited block in `game/arena_space`.
  - Kurt's half in `game/player`: `Nearby`, `FindPlatform`, the platform landing in `FallMove` (`Platform`, 0.05 lift), `Contact`/`TakeContacts`, `UpdateTouched` (also in `UpdateSpecial`), `Shove` against the objects, `Carry` (the transform measured on .NET 10: one rounded product, then one fused multiply-add), and all of `Kurt.Keeper.cs` (CheckKeeper, KeptPlatform, KeepOn, StayOnKeeper, the kept Kurt not walking, the teleport letting go).
  - The runtime's half in the new `scripts/platforms.cpp`: `SolidsWithin`, `MdkObject.Footing`, `GetKurtPlatform`, `KeeperAt`, `KurtTouchesGroups` (called after UpdateArenas, as the C#), the object loop's Carry, and `ClipView`, wired into `FollowCamera` and both frontends (Viewer.cs:237).
  - VM: opcode 114 (`if_player_on_me`), 209's platform half, and the default case: an unhandled opcode with a branch action now takes it as false (`ScriptVm.cs:1799`), so a GosubElse calls its else-target.
- **Why:** the owner's 2026-11-20 direction to port all gameplay from the C#. Before this, Kurt walked through doors and object walls, fell through lifts, lifts waiting on opcode 114 never moved and floor triggers never fired (L3, L4, L6, L7, L8).
- **Evidence:** C# probe `tests/scripts/platform_probe` and `tick1 dump platforms`, byte for byte:
  - `L3_plat_keeper` (700 steps): the empty hover ship waits on 114 (false while Kurt stands beside it), Kurt lands on it, 114 turns true, it keeps him and flies him through HMO_9, turning him with it.
  - `L7_plat_tank` (300): carried on the tank's turret. `L6_plat_gun` (600; it stopped at 66 until package #4's combat opcodes landed, and now agrees to step 1200): kept by the flying gun.
  - `L7_plat_door` (300) and `L7_corridor_locked` (the corridor probe without its unlock): the locked X7DOOR stops Kurt at y = 478.4, he slides along it, and it clips the camera's sight. `L7_corridor_middle` is unchanged.
  - `L4_plat_trigger` (60): landing on MEAT_1's hatch hits its group (hit type -11), it opens, he falls through.
  - `vm_tests`: `IfPlayerOnMeIsFalseWithoutAPlatform`, `UnhandledGosubElseCallsTheElseTarget` (opcode 225, in neither VM).
  - Every existing golden passes unchanged.
  - The goldens bite. Each of these breaks fails the named goldens: 114 always false (keeper, gun); no KurtTouchesGroups (trigger); no Carry (keeper, tank, gun); no object pass in the move (corridor_locked, door); the old default case (the vm_test); no ClipView (door); no StayOnKeeper (keeper, gun). Not pinned: Carry's fused-vs-unfused rounding (no golden turns a carried, unkept Kurt).
- **Still open:** the ride parts of the same code (package #9); the slide branch of FallMove (#8); the upstream C#'s later triangle-platform and kept-in-air changes; Old-3DS cost unmeasured. All in `docs/OPEN_ITEMS.md`.
- **Commit:** this entry.

### Sniper rounds (gap package #6): the rounds, the lock, the mortar puzzle
- **What:** a sniper shot now flies and hits, ported operation for operation from the C#.
  - `Scripts/SniperRounds.cs` -> `scripts/sniper_rounds.{h,cpp}`: the three slots and the five round types (bullet, homing, grenade, homing grenade, mortar), straight flight tested against the objects (world box, then the pose's faces in model space) and the arena, homing (`Steer`), the mortar's drag, gravity, bounces and box sweep, `bomb_follow_path`'s guided flight, `HitObject` (hit event, hit type = the round's type, hit direction = its yaw, shot part/point, 8 damage under 65000, sparks, the kill), the explosions (two `Items.Blast`es, the effect, EXPLODE), the round camera and the round models' state.
  - `ScriptRuntime.UpdateSniperTarget`, `FireSniper` and `Sweep` -> ScriptRuntime members (in `sniper_rounds.cpp`); `runTick` updates the rounds after the debris and the lock after the end of the level, where the C# does; the runtime binds `Player::setSniperHooks` and unbinds it in its destructor.
  - Opcode 28 (`bomb_follow_path`) -> `script_vm.cpp`.
  - System.Numerics' `Matrix4x4.Invert` twin moved from `mdk_linux/src/render/` to `mdk_core/src/util/` so the rounds' face test can use it.
- **Why:** the owner's 2026-11-20 direction to port all gameplay from the C#. Every level has sniper-hit checks (`if_hit_weapon 0`), the homing and mortar rounds reach targets the chain gun cannot, and LEVEL7's DANT_6 puzzle (groups 10-13) is only solvable with a guided mortar round.
- **Evidence:**
  - The C# probe `tests/scripts/rounds_probe` (Kurt and the runtime at 1/60 s; enter the scope, aim by yaw/pitch bits, fire, give ammo, teleport, set a target's health) gives `golden/L8_rounds_ship.txt` (bullets on XBSHIP's faces, the lock, a homing round, a grenade, a homing grenade), `golden/L8_rounds_kill.txt` (a bullet kill, then a wall shot) and `golden/L7_rounds_mortar.txt` (a mortar on DANT_6's group 12, opcode 28's path 109157 to the blast, then a bouncing mortar); `tick1 dump rounds` matches all three byte for byte, every float as its bits.
  - Breaking the code fails them: the homing yaw acceleration 540 -> 541 (L8_rounds_ship line 522), opcode 28 a no-op (L7_rounds_mortar line 312), the hit type written as 0 (L8_rounds_ship line 583), the mortar bounce 1.75 -> 1.5 (L7_rounds_mortar line 168), the vector dot fused (L8_rounds_kill line 49). The kill's explosion facing does not bite (the killed XG has a death script; OPEN_ITEMS).
  - No existing golden changed: the full `tick1` suite passes with the hooks bound.
- **Commit:** see the git log ("Sniper rounds").
- **Still open:** the strike arm (#14), drawing the round models and the round cameras (#7), the wall's bullet hole (#16); see OPEN_ITEMS.

### Kurt in updrafts, the slide and the wind (PORT_GAPS package 8)
- **What:** ported, operation for operation:
  - `Kurt.Updraft.cs` into `Player` (`updateUpdraft`, called where the C# calls it: after the floor/air branch in `updateVertical` and in `updateDeath`): inside a fan's box the fan sets his vertical speed and opens his chute (CHUTEOUT, the FAN loop); out of every fan he rises at most 40 u/s, except while knocked down. `updateAir` no longer closes the chute while a fan holds him.
  - `Kurt.Slide.cs` into `Player` (`startSlide`, `enterArena`, `stopSlide`, `slideAccel`, `updateSlide` and its helpers), taking the step where the C# does (`Kurt.cs:185`, ahead of the Hang/HardLand branch); FallMove's Sliding branch (`Kurt.cs:540`); `CameraRoll::slide` (CameraRoll.cs:54). The slide's heading is libm (the C#'s `MathF`), pinned R6.
  - `ScriptRuntime::kurtUpdraft` (his arena's fans, else the second arena's), `fanSpark` (the fans' still fire spark, now bound: `Fans.Spark = FanSpark`) and `windZone`, with opcode 224 (`ScriptVm.cs:1166`), in the new `mdk_core/src/scripts/updraft_wind.cpp`; `Kurt.EnterArena()` at the arena crossing (`ScriptRuntime.cs:926`).
  - The 3DS frontend now loads Kurt's level animations (K_SLIP, K_SLIDE, K_FSLIDE, K_BSLIDE, K_SURF, K_SURFJ) from the SNI archives as Linux already did, so a sliding Kurt has frames there and the Player's frame counts match.
  - The debris half package 4 asked about was already ported (`Debris::move`'s updraft); what was missing was the spark the fans let out, now bound. There is no crouch-slide in the C#: the slide is the wind tunnels'.
  - On package 3's solids: `slideMove` moves against the arena and the solid objects nearby through `nearby`/`contact`, and the slide step runs `updateTouched`, exactly as `Kurt.Slide.cs` does.
  - Sniper mode's `canSnipe` (package 5) now refuses while sliding (`Kurt.Sniper.cs:61`'s `!Sliding`, which package 5 took as true while the slide was missing); the slide takes the step after the sniper's (`UpdateSniper(...) || UpdateSlide(...)`).
- **Why:** the owner's 2026-11-20 direction to port all gameplay from the C#. Fans are in every level (opcode 142 ×23) and gate vertical routes (LEVEL3's CHMO_6 shaft into HMO_8, LEVEL6's COLYM_7 shaft into OLYM_8); LEVEL6's wind tunnels (opcode 224 ×9) are its route down. Without this the fans lifted nothing but sparks that did not exist and LEVEL6's tunnels were walked, not slid.
- **Evidence:**
  - The C# probe `tests/scripts/wind_probe` and its twin `tick1 dump wind` (Kurt's state, feet, yaw, vertical speed, floor/chute/updraft/slide flags, the slide's velocity, cap and counters, the roll, the fans and their wobble, the debris sums, the voices) match byte for byte on six goldens: `L3_wind_shaft` (rise through CHMO_6 into HMO_8 on the second arena's fan, hover, drift out), `L8_wind_fan` (GUNT_3, strafing out mid-rise at the 40 u/s cap), `L6_wind_tunnel` (COLYM_1: slip, slide, fast, brake, turn, air), `L6_wind_arena` (COLYM_5 into OLYM_6: EnterArena's 15 ticks, fall, land), `L6_wind_fan` (COLYM_7: the wind slides him to the fan, wind off, the fan lifts him into OLYM_8), `L6_wind_wall` (COLYM_2: SlideMove's wall turn).
  - Package 4's two fan-dependent hazards scenarios are re-pinned against its own probe: `L4_hazards_fan` (MEAT_10's XBN_BOMBs) and `L6_hazards_balls` (OLYM_2, COLYM_2's fan as the second arena). Its third, the GUNT_2 explode beside a victim, is NOT a fan divergence (GUNT_2 has no fan) and stays open in `docs/OPEN_ITEMS.md`.
  - .NET 10's `float.Lerp` is fused on this host (200000 of 200000 random cases equal `MathF.FusedMultiplyAdd(a, 1 - t, b * t)`), so `CameraRoll::slide` uses `fmaf`; the unfused spelling put the tunnel's roll a 4th-decimal unit off at step 205.
  - The goldens bite; every mutant is caught: no second-arena fallback (L3_wind_shaft, L6_wind_fan), no 40 u/s exit cap (L8_wind_fan), fan sparks unbound (four wind goldens and both re-pinned hazards goldens), no EnterArena (L6_wind_arena), no FallMove Sliding branch (three L6 goldens), the chute closing in an updraft (three fan goldens), an unfused Lerp (L6_wind_tunnel), wind_zone off ignored (L6_wind_fan), the wall turn dropped (L6_wind_wall).
  - `vm_tests` `SlideEndsInTheAirAndAtRest` pins the two endings no retail route reaches within a golden (20 air ticks, at rest on a floor) and a dead Kurt's slide.
  - Gates: `make -C mdk_3ds`, `tests/formats`, the Linux build, `make -C tests/scripts test` and `tools/check.sh` (see the commit). No emulator or hardware run (the parent runs Mandarine).
- **Commit:** this entry's commit.
- **Still open:** `docs/OPEN_ITEMS.md`, "Kurt in updrafts, the slide and the wind".

### Package 20a: the menus, the briefing and the settings
- **What:** the C#'s main menu, its options, the pause menu, the settings file and the briefing, ported operation for operation, on both frontends.
  - `game/menu_items.{h,cpp}` ports `Menu/MenuItems.cs`: the rows (36 high from y 5, smaller and the small font past 9 items), the 65 % items growing at 2.1 a second, Up/Down/Left/Right/Accept/Esc, the held-key repeat (0.4 s, then 0.06 s), the options pages. The C#'s lambdas are action ids the owner acts on where the C# would invoke them (`handle`, the owner, `grow` is `Update`'s order). The options pages are filtered per host as the C# filters them per platform (`DisplayMenu.Items`): the frontends show Graphics (Gore), Audio (three volumes, the music filter) and Game (Difficulty); `Host::Oracle` lists the C#'s eight desktop pages for the golden.
  - `game/main_menu.{h,cpp}` ports `MainMenu.cs`: Continue, New Game, Level: n, Saved Game, Beta Levels, Options, Quit; "Really Quit?"; New Game is `GameState.NewGame(index)` then `Event::Briefing`. The saves are #19's: `menu::MenuSaves` is three null hooks (list, peek, load) that read as an empty folder, which is the C#'s menu on a fresh user folder.
  - `game/pause_menu.{h,cpp}` ports `PauseMenu.cs` (Resume, Options, Main menu, Quit over the dimmed game). Linux shows it on Esc, as the C#. On the 3DS the owner's START menu (`game/enhanced_menu.{h,cpp}`) keeps every row and gains the C# pause menu's two missing ones, OPTIONS and MAIN MENU, just above QUIT; OPTIONS opens `PauseMenu::openOptions` (the same pages), MAIN MENU leaves the level for the menu.
  - `game/game_settings.{h,cpp}` ports `Flow/Settings.cs` whole: every field, `Parse`/`Set` with .NET's parsers' rules, `Format` in the C#'s order and spelling (shortest round-trip floats), `Apply`'s audio half. Linux reads and writes the C#'s `settings.cfg` in `$MDK_USER_DIR` (or `./`); the 3DS keeps the game's lines (volumes, music filter, difficulty, gore) in `sdmc:/3ds/mdk/config.ini` after the toggles. Each level applies them as the C# Viewer does: `Kurt::setDifficulty` (the writer `player.h` lacked), `ScriptRuntime::option` from gore, the volumes on the runtime's mixer (the pause menu's changes reach it live).
  - `game/briefing.{h,cpp}` ports StatsScreen's Briefing page and `Typesetter.cs`: L<n>_MAP fading in, BRIEF<n> typed at 15 a second (60 with Fire/Jump), the cursor, a key to fade out, then `Event::Fall` (skipped: #17b) and the level. `game/screen_view.{h,cpp}` is the C#'s ScreenView (600 x 360 fitted on the canvas) and PalettedImage.
  - The flow: `GameFlow::skipUnported` stops at the Briefing now that it is shown; the Menu and the Briefing are package 0 in the skip log.
  - Linux: `mdk_linux/src/menu_screens.{h,cpp}` runs the menu and the briefing; no `--level` starts at the menu; `--press=Menu.Down@0.5,...` (the C#'s TestPresses, `game/menu_presses.{h,cpp}`) drives them headless; the window reports the C#'s menu keys (`windowTakeMenuKeys`). A level that ends into the menu (after LEVEL5) shows it.
  - 3DS: `mdk_3ds/src/menu_screens_3ds.{h,cpp}`; a release build and a harness build without `LEVEL=` boot into the menu (D-pad, A/START, B); `make AUTOQUIT=.. MENU=...` presses keys in the menus; `CAPTURE=` works there too.
  - Not ported, with cause: MDK12.FLC behind the menu, the slideshow and the INTRO1A splash (#20b/#20c; the C#'s own fallback MDKOPT is drawn), the menus' sounds, the pointer, the typed keys (Y/N, SEETHEWHOLEGAME: #21), the desktop's Display/3D Stereo/Keyboard/Gamepad/Mods pages, the pause menu's quick-save hint (#19).
- **Why:** the owner's 2026-11-20 direction to port all gameplay from the C#, and the un-cut of the menus: before this a run ended at "Menu follows, which this frontend cannot show", nothing chose a level or a difficulty, and `Kurt::difficulty_` had no writer.
- **Evidence:**
  - `tests/scripts/menu_probe` drives the C#'s own MainMenu, MenuItems, PauseMenu, Settings, Typesetter and StatsScreen through a real headless Ui (SDL offscreen, lavapipe) by key schedules at 1/60 s, printing each frame's page, selection, every item's growth, the rows, the events and the settings file. `golden/menu_{settings,typeset,main,pause,briefing}.txt` (91, 55, 559, 113 and 432 lines); `vm_tests dump-menu <section>` prints the same bytes (`PASS MenusMatchTheOracle`).
  - The goldens bite, one mutation at a time: the growth rate 2.17 fails main and pause; volumes wrapping at 90 fail main; `\n` one unit lower fails typeset; typing at 16 a second fails briefing; bools written lower case fail settings and main; NextLevel keeping the top row fails main; Esc always resuming fails pause.
  - Frames: the Linux headless main menu (frame 60), its Game options page and the level-7 briefing mid-typing (frame 240) are BYTE-IDENTICAL (0.000 %, max delta 0) to the C# app's (`--menu --original --wait=..`, the same `--press`) at 1280 x 960, run with MDK12.FLC hidden so the C# draws its MDKOPT fallback too. Pair (C# left, port right): `docs/evidence/2026-11-20-menus/*-pair.png`; the pinned level-3 frame is `docs/frames/2026-11-20-menus.png`.
  - `tests/scripts/menu_frontend_test.sh`: the entry menu, the options' file written and read back, New Game -> briefing -> fall skipped -> level 7 -> Esc -> Main menu -> the menu again, Really Quit? -> Yes, and `--level` still starts in the level. `level_chain_test.sh` now ends at the menu after LEVEL5 ("Level: 6", as the C#'s).
  - 3DS: release and harness builds compile; no emulator run here (the parent's).
- **Commit:** this entry's commit.

### Package 13: the cutscenes and special events 51-93
- **What:** ported the cutscenes from the C#, operation for operation:
  - opcode 131's arms above 50 (`ScriptRuntime::specialEvent`, ScriptRuntime.cs:1009-1077): the dog (52), the end of a cutscene (53, 92), shot 2 (55), Gunter (61), the end of the game (81), the boss intros (91 at L8 GUNT_10, 93 at L7 DANT_10), and 51's cutscene half (the teleport, the strike cutscene, its shot);
  - `StartCutscene`, `EndCutscene`, `FindObjectNamed`, `CutsceneTick` (RunTick's Cutscene branch: which objects run and show, Kurt's `Visible`) and `UpdateCutsceneCamera` (shots 0, 1, 2, 11 and 12); RunTick's object motion became one function, `motionUpdate`, shared by both ticks;
  - `Kurt.Visible` (`Player::visible`, read by `KurtSprite::draw`), and `CutsceneCamera.cs` (`game/cutscene_camera.{h,cpp}`), which both frontends use in the C# Viewer's places;
  - `GameFinished` is the runtime's `gameFinished()` latch: both frontends log it and end the run "game finished", at a hook beside package 2's LEVEL-END HOOK.
  - 51's full-screen strike (`PlayStrikeScene`) is package 14, a marked hook; until it lands 51 alone still counts opcode 131 as unimplemented.
- **Why:** the owner's 2026-11-20 direction to port all gameplay from the C#. Without this the boss intros (L7, L8), Gunter's scene and the end of the game (L5) did nothing, and the game could not finish.
- **Evidence:**
  - The C# probe `tests/scripts/cutscene_probe` and `tick1 dump cutscene` are byte-identical on seven goldens, each driving the event the way the retail script does (from the controller of Kurt's arena): `L7_cut_boss` (93, 92), `L8_cut_boss` (91, 92), `L5_cut_gunter` (61, 53), `L5_cut_dog` (52, 54 the no-op, 55, 53), `L5_cut_dog_long` (the orbit's 1800-tick ease-in running out), `L5_cut_strike` (51, 53), `L5_cut_end` (81). The camera's yaw, pitch, distance, position and point are compared as bit patterns.
  - The goldens bite: no Cutscene branch in RunTick fails all seven; Kurt always visible fails four; the pickups not hidden fails `L7_cut_boss`; the settled weights swapped fails `L5_cut_dog_long`; the blend never letting go of the point fails `L5_cut_dog_long`; the orbit angle off by 10 degrees fails four; 51 not moving its object fails `L5_cut_strike`.
  - `vm_tests` pins the dispatch (51 counted, 52/53/54/81 not; 81 latches `gameFinished()`) and `CutsceneCamera` (Active from the first shot, the blend, the jump and turn cuts).
  - Linux headless: `--level=5 --at=465,-6,-2263,0 --event=81` ends "game finished" at frame 61; captures of 93, 91, 52 and 61 show each shot (Gunter framed in his).
  - Not pinned: `CutsceneBoss` keeping the stored position (shot 12's point is the stored one); a moving target's shot. The oracle's targets look wrong (the controllers, at the origin) and are ported as they are. Open items in `docs/OPEN_ITEMS.md`.
- **Commit:** see `git log` (the commit that adds this entry).

### Bullet holes (gap package #16): special_130, the last unported VM opcode
- **What:** a sniper round that hits a special_130 object now leaves a hole in that object's texture, ported operation for operation from the C#.
  - `Scripts/BulletHoles.cs` -> `scripts/bullet_holes.{h,cpp}`: `Nearest` (the textured face of the hit part nearest the point -- the C#'s approximation, kept: the original keeps the face its round found), `Weights` (clamped, normalised barycentrics), `Stamp` (the hole centred on the rounded UV, index 0 transparent, wrapping both ways).
  - `ScriptRuntime.StampBulletHole` / `ArenaTexture` -> runtime members in `bullet_holes.cpp`: the hit turned into the model's frame (the runtime's fused `Vector3.Transform`), BHOLE or BHOLE2 by the gore option. Opcode 130 -> `script_vm.cpp`. The retail sites are arena-script loops (`if_hit_part ANY` -> gosub special_130) in L5 MUSE_2/MUSE_5, L6 OLYM_10 (XB2) and L8 GUNT_10 (XGUNTAM). The C#'s rounds stamp nothing on a WALL, so neither does the port: #6's marker in `moveStraight` was a guess, and the oracle says no.
  - The C#'s `TextureStamped` event -> the runtime's `stampedTextures()` list, which both frontends take after each step and pass to the new `SurfaceCache::refresh` (the C#'s `MaterialResolver.Refresh`, Viewer.cs:263) -> `mdk_render_update_texture`; plus a synchronous hook for the golden harness.
  - A stamped texture is pinned (`formats::Texture::pinPixels`): the arena stream would otherwise drop its pixels when Kurt leaves the arena and read the hole-less ones back from the MTO. The C# keeps every texture for the level, so the hole stays there too.
  - The 3DS path: `mdk_render_update_texture` takes the stamped indices into the backend's system-memory source at once (so an eviction comes back with the hole), unlinks the texture's HD-pack copy (the PC drops a changed texture's replacement the same way, the C#'s `Renderer.Changed`), and re-expands the GPU copy at the frame boundary, ONE restamped texture a frame, after `C3D_FrameBegin`'s wait. Why this path: the arena and object textures are palette-indexed sources expanded to 16-bit (or RGBA8) stores, never ETC1 -- ETC1/ETC1A4 is only Kurt's frames and the HD pack -- so re-expanding the one texture is exact and simple; a decal quad would not give the C#'s result (the hole is on the texture, so every object sharing it shows it). The cost is one texture's expansion per hit, deferred and budgeted (owner rule: no main-thread stalls).
- **Why:** the owner's 2026-11-20 direction to port all gameplay from the C#. special_130 was the last unported VM opcode (224 landed with package 8): every C# VM case is now dispatched; its effect is cosmetic, on the shot turret, robot and guards of L5, L6 and L8.
- **Evidence:**
  - The C# probe `tests/scripts/holes_probe` (the rounds probe's stepping and schedule, plus `opt:` and `stampall:`; every texture copied at load, each stamp printed as the texture's FNV-1a before/after, the changed rectangle and count) gives `golden/L8_holes_guntam.txt` (two rounds on GUNT_10's XGUNTAM, a miss, then BHOLE2 on all 15 parts' nearest faces, wrapping in x and y) and `golden/L6_holes_xb2.txt` (two misses at OLYM_10's glass, two rounds on XB2, then BHOLE on all 20 parts across four textures); `tick1 dump holes` matches both byte for byte. `holes_pinned` (tick1 suite) drives the arena stream: a pinned texel survives GUNT_10's trim and re-read while an unpinned sibling is dropped and read back.
  - Breaking the code fails them: the weights unclamped (both), the gore option ignored (L8), no wrap at the texture's edge (both), the first face kept instead of the nearest (both); `adoptPixels` ignoring the pin fails `holes_pinned` (back=0). Writing the world-to-model turn unfused does NOT bite (the texel rounds the same; OPEN_ITEMS).
  - No existing golden changed.
- **Emulator check for the parent (not run here):** `make -C mdk_3ds AUTOQUIT=60 FIXED=1 LEVEL=8 AT=20,3360,-427.01,123.8016 WALK='sniper@30-32;forward@40-78;fire@100-110' CAPTURE=200` -- Kurt on GUNT_10's floor facing XGUNTAM, the scope raised ~26 degrees by 38 frames of forward (the oracle hits with 37-39), one round into XGU_BODY; the capture should show the dark hole on the turret's body. Found with the C# probe (`up:160-197` / `-199` both stamp XGU_BODY; `-195` and `-201` miss).
- **Commit:** this entry.
- **Still open:** no capture of a hole yet; the Old-3DS cost of the re-expansion is unmeasured; L5's MUSE sites have no golden of their own (the same XGUNTAM model and path). See OPEN_ITEMS.
### Package #19: save and load
- **What:** the C#'s saves, byte for byte, and what a death does.
  - `mdk_core/src/util/json.{h,cpp}`: JSON written as System.Text.Json writes it. Floats are the shortest round-trip decimal in .NET's form (`1E+09`, `1E-05`, `-0`), strings use JavaScriptEncoder.Default's escapes (`\u003C`, `\u002B`...). The reader acts as JsonDocument does: 64 levels deep, the last duplicate name wins, each value's raw span kept.
  - `game/save_games.{h,cpp}` (Flow/SaveGames.cs): the SaveGame record (ToJson, Parse and its fallbacks) and the `<NAME>.sav` folder (Write, Read, Delete, List, QuickSlot, LASTGAME). A write goes to `.tmp`, is flushed and checked, then renamed over the save. Where rename will not replace a file (the 3DS FS), the old save steps aside to `.bak`, and a read falls back to it. A short read is an error.
  - `game/game_state` gains Load and Save; `GameFlow` does `--load`, `--save`, KurtDied's LASTGAME write, and the menu's "Continue" as its skip (it loads LASTGAME, MainMenu.Load).
  - `scripts/script_snapshot.cpp` (ScriptRuntime.Snapshot.cs, Snapshot.cs, Kurt.Snapshot.cs, Fans and TriangleGroups' Snapshot/Restore): `capture()`, `load()`, `canSnapshot()`, `snapshotHash()`. MdkObject's 107 public fields are listed in the C#'s reflection order; one list does both directions. The sniper clip fields are saved (#5's leftover). The runtime keeps its arena states' creation order (the C#'s Dictionary order) and counts TownTicks down (FlattenTown is #15's).
  - `game/save_prompt.{h,cpp}` (Menu/SavePrompt.cs): the prompt's decisions, with `layout()` giving the C#'s text runs for whoever draws it (#20a).
  - `game/viewer_saves.{h,cpp}`: the Viewer's OnDied, OpenSnapshot/SnapshotSave, QuickLoad, RestoreSnapshot and ShowNotice.
  - The one-ArenaSpace hole, for loads: a restore's arenas (`ScriptRuntime::enteredArenas`) go into Kurt's space too.
  - The harness's start in an arena only a teleport reaches: `ARENA=NAME` with `AT=` (3DS), `--arena=NAME` with `--at` (Linux).
  - Linux: a death writes LASTGAME and the level reloads from its start. F2 opens the prompt (FONTBIG on black, a typed name, Enter saves); F9 loads. New options: `--save-dir`, `--load`, `--save` and `--die` (the C#'s test death). Saves go to `--save-dir`, else `$MDK_USER_DIR/saves`, else the C#'s Linux folder `~/.local/share/mdk-sdl/saves`.
  - 3DS: saves go to `sdmc:/3ds/mdk/saves`. A death reloads the level as on Linux. In the pause menu, X saves under the level's number (F2 + Enter) and Y loads (F9). Harness options: `DIE=1` and `SAVETEST=1` (X at 5 s, Y at 8 s).
- **Why:** the owner's 2026-11-20 direction to port all gameplay from the C# (and to have #19 back). Before this there were no saves, and a death respawned Kurt in place without reloading the level, so a group a script had hidden stayed hidden.
- **Evidence:**
  - `tests/scripts/save_probe` (the C# probe) plays a level, captures it, writes the save file, parses it into a fresh world and runs on, printing a whole-state hash each step. `tick1 dump save` prints the same bytes for six goldens:
    - `L3_save_box`: 46 objects, a box, a second arena;
    - `L3_save_teleport`: HMO_7, reached only by teleport;
    - `L7_save_decoy`: the inventory, the clip, the decoy;
    - `L7_save_bomb`;
    - `L4_save_groups`;
    - `L5_save_idle`;
    - `L3_save_fan`: HMO_6's fan (after the rebase onto package 8).
  - The cross-load: each golden's C#-written file, loaded by the C++ (`tick1 dump save-load`), prints the C#'s restore lines. That includes `L3_save_fan` (a fan), a full golden since package 8's updrafts made the port's HMO_6 play match. The other way, the C++'s files (`tick1 dump save-file`) loaded by `Probe load` print the same lines, measured for L3_save_box and L7_save_decoy (98 542 and 20 514 bytes, identical files).
  - `golden/save_flow.txt` (`Probe flow`) pins the record: the light saves, 15 Parse cases, the folder's list, read and delete, and a death's LASTGAME. `golden/json_floats.txt` holds 20 000 floats as the C# writes them; the formatter matched all 199 589 it was tried on.
  - `save_tests` also checks the prompt (typing, the cursor, Space vs Enter, the layout) and the death, then the menu's Continue, through GameFlow. It checks the write's safety too (the `.bak` read, a folder under a file refused with a reason).
  - The goldens bite. Each of these breaks fails a golden:
    - Yaw and Pitch packed in swapped order fails L3_save_box (line 2);
    - the groups not restored fails L4_save_groups (line 6);
    - the arenas' Started not restored fails L5_save_idle (line 6);
    - the loaded arenas kept out of Kurt's space fails L3_save_teleport: Kurt falls at the first step;
    - the float exponent switching at 1E+10 fails json_floats;
    - deaths written before health fails save_flow.
  - Linux headless: `--die` writes LASTGAME and reloads level 3 at frame 317. `--load` of the C#'s L3_save_box file logs "restored hash cf41a6be998bcfc2, objects 46", the C#'s own hash.
  - The 3DS release and `DIE=1 SAVETEST=1` harness builds compile. The parent's Mandarine runs: `DIE=1` on L7 ("Kurt died at frame 316", LASTGAME loaded, level 7 again, deaths 1) and `SAVETEST=1` on L3 ("Saved game 3 (30148 bytes, 57.6 ms)", loaded, restored hash 77c80424abe4ca48, 13 objects, Kurt back in HMO_1) pass. `AT=` into HMO_7 fell forever: `AT` moves only the feet, so an arena only a teleport reaches was never entered or loaded. `ARENA=NAME` (3DS, with `AT`) and `--arena=NAME` (Linux, with `--at`) now enter it the way a script's teleport does (`teleportKurt`, the arena added to Kurt's space; the C# console's map does the same). On Linux `--at=677.5,2125,41,96 --arena=HMO_7` stands Kurt at z 0 in HMO_7, and without `--arena` he falls past -1964 by frame 598.
  - The main menu's save hooks (#20a's `MenuSaves`: list, peek, load) are `flow::menuListSaves/menuPeekSave/menuLoadSave` in `game/viewer_saves`, pinned in `save_tests`.
  - Not saved, because the C# does not save them: the random stream, voices, Kurt's motion. There are no opcode changes; `docs/OPCODE_COVERAGE.md` notes the load's ArenaSpace fix.
- **Update (rebased onto the menus, fca4633):**
  - The menus (#20a) are shown now, so the flow no longer skips the main menu. A death follows the C#: LASTGAME is written and the main menu follows (Game.Handle(KurtDied) -> Handle(Menu)), Continue first. Taking it reloads the level from LASTGAME. Linux headless: `--level=3 --die --press=Menu.Accept@9` shows "Continue | New Game | Level: 3 | Saved Game | Options | Quit", then level 3 again. On the 3DS, `DIE=1` now stops at the menu; add `MENU=Menu.Accept@2` to continue.
  - The main menu's saves are wired in both frontends (`FrontScreens::saves`, `FrontScreens3ds::saves` -> `flow::menuListSaves/menuPeekSave/menuLoadSave`). `golden/menu_saves.txt` is `menu_probe saves`: a folder with LASTGAME, A, B and an invalid BAD gives Continue first and the Saved Game page "A  (Level 3) | B  (Level 4) | BAD  (Invalid/Corrupt File) | LASTGAME  (Level 2)". B loads into its briefing (level 4, deaths 2, the strike). A second menu's Continue loads LASTGAME into level 6. menu_tests' dumpSaves prints the same bytes; a load hook that drops "before level" fails it.
  - Linux, menu start: the C#'s L3_save_box file on the Saved Game page restores with "restored hash cf41a6be998bcfc2, objects 46", the C#'s own.
- **Update (rebased onto bullet holes, 438773e):** the C# saves nothing of a stamped texture (Snapshot has no texture), and its load is a new Viewer over a new LevelData that reads the textures from the archives again, so a loaded game has no bullet holes. The port matches by construction: a load is a level-loop pass with a fresh LevelData (unpinned textures) and a fresh runtime (empty stamped list), and `ScriptRuntime::load` now also refuses a runtime whose stamped list is not empty.
- **Commit:** this entry's commit.


### The air strike and the strike scene (gap package #14): L5's final fight
- **What:** Bones' air strike and the full-screen strike scene, ported operation for operation from the C#:
  - `Scripts/AirStrike.cs` -> `scripts/air_strike.{h,cpp}`: `FindTarget` (the crosshair's point with open sky 1000 above), `Call` (the raspberry without a target or when used up; the dive and `UsedUp` after the fourth level of the order), `Start` (the five points, `ClearPoint`, the curve), `Update` (150 u/s along the curve, the heading, the dive's velocity and gravity, the nine X_TOOTH grenades one every 15 units), `Crash` (450 within 80 on objects and groups, 67 on Kurt, hit type -6, the explosion), `DropTooth`, and `BezierPath` (64 samples a segment, `Sample` by .NET's binary search, `ClosestOffset`). System.Numerics as the oracle's host computes it (fused Lerp, left-to-right dots).
  - `Scripts/StrikeScene.cs` -> `scripts/strike_scene.{h,cpp}`: `Play` (X_STRIKB from the items' animations, X_STRIKD from the arena's; the plane's parts only for the dive), `Update`, `Skip`, the camera on the model's reference points turned by `CreateRotationZ`; `ScriptRuntime.PlayStrikeScene` beside it. The C#'s `StrikeSceneChanged` event has no subscriber in `src/`, so it is not ported.
  - The runtime: `AirStrike.Update(1f)` after the rounds in RunTick (ScriptRuntime.cs:957), `FireSniper`'s strike arm (:1284), special_event 51's `PlayStrikeScene(Kurt, WithPilot)` (:1022): 131 is whole, nothing counts it as unimplemented.
  - The frontends (`game/strike_view.{h,cpp}`, SniperView.DrawStrike + Viewer.Update): while the scene plays the game waits, the frame is the sky and the model from the scene's camera, Esc (Linux) / START or SELECT (3DS) skips it instead of pausing. Both draw the model through the ordinary object pass, whose GPU state is set per draw on the 3DS. `StrikeUsed` is read when the runtime is made and written back on a death (Viewer.cs:236, :891).
  - The probe's twin `tick1 dump strike`; `vm_tests` now expects 51 not to count as unimplemented.
- **Why:** the owner's 2026-11-20 direction to port all gameplay from the C#. Without this Kurt's strike ammo did nothing (the clip kept the round), the dive that hits MUSE_4's XGUNTAM with if_hit_weapon -6 never came, and 51's full-screen strike was missing from the end of L5.
- **Evidence:**
  - The C# probe `tests/scripts/strike_probe` (Kurt then the scripts at 1/60 s; the game waits on the scene as Viewer.Update does; AirStrike's and StrikeScene's private state by reflection) and `tick1 dump strike` are byte-identical on three goldens: `L5_strike_gunter` (the dive: X_STRIKB without Bones, the curve, the crash into XGUNTAM, hit type -6, then the used-up raspberry), `L3_strike_teeth` (the scene skipped, the curve over the target, nine teeth and their blasts on XS, the curve's end), `L5_strike_event51` (51 end to end: X_STRIKD, then the cutscene until 53).
  - They bite: the tooth bias 4 fails `L3_strike_teeth`; the turn rate 44 fails all three; an unfused Lerp fails `L5_strike_gunter` and `L3_strike_teeth`; hit type -7 fails `L5_strike_gunter`; the plane's parts not hidden fails `L5_strike_gunter`. Normalize as a reciprocal multiply does NOT bite (OPEN_ITEMS).
  - No existing golden changed (`L5_cut_strike` still passes: the cutscene probe never waits on the scene).
  - Linux headless `--level=5 --at=465,-6,-2263,0 --event=51 --fixed-step`, frame 150: Kurt in X_STRIKD over the sky, the same picture as the oracle's `--wait=2.5` frame (43% of pixels differ: the model turns and the two strike clocks are not on the same step).
- **Emulator check for the parent (not run here):** `make -C mdk_3ds AUTOQUIT=60 FIXED=1 LEVEL=5 AT=465,-6,-2263,0 EVENT=51 CAPTURE=150` -- special_event 51 at frame 60; frames 61-300 should show only the sky and Kurt in his X_STRIKD plane, turning; START skips it; then 51's cutscene from 10 behind the controller.
- **Commit:** this entry.
- **Still open:** #7's overlay hook (`SniperView::aim` -> `airStrike().findTarget`), the 3DS's first-frame bake of the scene's animation, the harness cannot give SW_BONES. See OPEN_ITEMS.
