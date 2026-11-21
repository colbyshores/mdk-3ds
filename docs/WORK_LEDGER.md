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


### The town timer, the shake, the sky modes and the group textures (gap package #15)
- **What:** four things the scripts asked for and nothing drew, ported operation for operation from the C#:
  - The town timer (ScriptRuntime.cs:38-47, 366-370, 901-908, 2139-2154): `ScriptRuntime::townTicks` from Kurt's difficulty (81000/54000/36000; none in the last level of the order), counted down each game tick, and `flattenTown` at 0 -- shake 5, the OOT_L<n>[A] message zoomed in front for 5 s, the flattened-town global flag (the second town's with FlagSecondTown), which the debriefing reads through `stats.townFlags`. #19 counted the timer down (for its saves) but nothing happened at 0; the C# raises no GameOver for a flattened town, and neither does the port.
  - The shake (FollowCamera.cs:59-62, 94, 137-139, 194-244): `raiseShake` raises the C#'s ShakeRaised (`setShakeRaisedHook`), which both frontends bind to `FollowCamera::raiseShakeHook`; `updateShake` and `followCameraTurn` (Turn's shake half, then the roll) in `update` and `blend`. The C#'s camera Random is time-seeded (`new Random()`); the port's is `mdk::Random`, the oracle's seeded generator, seeded by the frontend with the scripts' seed.
  - The sky modes (Level/SkyModes.cs, Viewer.Background/SkyColour): `game/sky_modes.h`, read every frame by both frontends into the new `MdkFrameParams::backdrop` and the clear colour. Linux skips the sky unless Sky and clears to the DTI's sky colour (black in mode 1); the 3DS skips its dome unless Sky, clears black in mode 1, and under Keep clears only the depth.
  - The group textures (opcode 140's consumer, TriangleGroups.Changed -> LevelView.Rebuild): `setGroupTexture` records the arena (`retexturedArenas`), and both frontends rebuild its view (`ArenaViews::refresh` -> `LevelView::refresh`) after the views' sync.
  - A save's restored group materials (#19's `script_snapshot.cpp`) record their arena for the same rebuild, as the C#'s Restore -> SetFlags -> Changed does.
  - On the way: the 3DS frontend reset its follow camera after binding the runtime's ClipView to it, which dropped package #3's object pass on every spawn there; the reset now comes first.
- **Why:** the owner's 2026-11-20 direction to port all gameplay from the C#. Opcode 135 fires in every level and shook nothing; the minecrawler's towns were never flattened; level 3's HMO_3 glass never appeared before it shattered; the scripts' black and hidden skies were drawn as sky.
- **Evidence:**
  - The C# probe `tests/scripts/screen_probe` (Kurt, the scripts and the camera at 1/60 s; the camera's Random swapped for `new Random(camseed)` by reflection; hooks `diff:`, `town:`, `gflag:`, `tp:`, `kill:`, `avar:`) and `tick1 dump screen` are byte-identical on four goldens: `L3_screen_town` (Easy's 81000 ticks, set to 7, the flattening, OOT_L3, the shake's 16 offsets and drain), `L3_screen_hmo3` (HMO_3's sky -1 on entry, its XG's death script's shake 2 and group 1, the 140s on groups 2 and 9-12 and their shattering), `L4_screen_second` (Hard's 36000, MEAT_6's sky 1, OOT_L4A and the second town's flag, camera seed 7), `L5_screen_last` (no town in the last level).
  - The goldens bite: the shake never draining, the Y offset drawn before the X, no ShakeRaised and no countdown each fail all three shaking goldens; the second-town flag swapped fails `L4_screen_second`; Keep drawn as Clear and the group material not stored each fail `L3_screen_hmo3`.
  - `screen_retextured_arenas` (tick1 suite): group 0 and a missing group record nothing, two groups of HMO_3 record it once until taken. `ARetexturedGroupRebuildsItsArenasView` (tests/formats): HMO_3's view rebuilt by `ArenaViews::refresh` draws group 2 with exactly the resolver's -1027 surface (not before), every other group's batches unchanged, one mesh released and one made; no view, nothing made.
  - No existing golden changed.
  - Linux headless (`--headless --fixed-step --level=3 --at=-56,320,104,90 --event=0 --capture-frame=66`, 320x240): the end of the level's shake 5 moves the view by a few pixels against the same frame from the pre-#15 binary; `--level=3 --at=-60,1410,-960,0` logs `sky: mode -1 (Keep) at frame 1`. The retail arenas that set 1 or -1 (L3 HMO_3/HMO_10, L4 MEAT_6, L7 DANT_4 checked) are closed rooms: their frames are pixel-identical with and without the sky, so the new `sky: mode` log line is how a run shows the change.
  - Gates: `make -C mdk_3ds`, `tests/formats`, a fresh `build-linux`, `tests/scripts test` and `tools/check.sh` (ALL GATES PASSED, the frame gate included).
- **Emulator check for the parent (not run here):** `make -C mdk_3ds AUTOQUIT=40 FIXED=1 LEVEL=3 AT=-56,320,104,90 EVENT=0 CAPTURE=66` -- special_event 0 at one second ends the level, and its StartShake (5) shakes the view for ~32 frames: frame 66's capture is offset by a few pixels from an unshaken one (compare with `CAPTURE=58`, or the same build without the camera hook); then `LEVEL=3 AT=-60,1410,-960,0` (HMO_3) must log `sky: mode -1 (Keep)` and draw no dome while Kurt is in HMO_3.
- **Commit:** this entry.
- **Still open:** Vulkan draws Keep as Clear (no load-op pass); the 3DS's Sky clear stays 0xFF102030; no capture of HMO_3's glass (it needs its XG killed and three sub-objects destroyed); see OPEN_ITEMS.

### Linux: the level's sound reaches the speakers
- **What:**
  - `mdk_linux/src/main.cpp`: each level's (and each respawn's) runtime mixer device is given the SDL stream, `setSink(audioSink, nullptr, audioQueued)`, right after the settings' volumes and music filter are applied to it. Music and effects both go through it, and the pause menu's volume changes reach the same device live.
  - `--mute` (the C#'s option, Program.cs:186 → `Output.Muted`). `--headless` implies it, so every test and capture runs on the muted device's `owed` accounting, as before; a real queue drains at wall-clock pace and would make which voices are still playing depend on the host.
  - `audio_sdl`: `audioStats()` (frames sent, peak sample). The log says `[audio] the level's mixer -> the SDL stream (master, music, effects)` when a level wires it, `[audio] level N: V voices started; F frames sent (peak P)` at its teardown, and the totals at close.
- **Why:** the owner's 2026-11-20 direction to port all gameplay from the C#. Package #18b found the level silent on Linux: the stream was opened (`[audio] 44100 Hz stereo F32LE`) but nothing called `setSink`, so every level mixed into the muted branch. The C#'s Viewer mixes into the game's one `AudioDevice` (Game.cs:206), whose `Update` pushes to its SDL stream (AudioDevice.cs:214-266). The 3DS was not affected (the DSP path).
- **Evidence:**
  - Level 3 windowed on `DISPLAY=:99`, lavapipe, `SDL_AUDIO_DRIVER=disk`, `--fixed-step --frames 400 --walk=forward@30-200;fire@60-300;jump@200-220`: `[audio] the level's mixer -> the SDL stream (master 0.80, music 0.50, effects 0.70)`, `[audio] level 3: 7 voices started; 287318 frames sent to the stream (peak 0.592)`. The disk driver's file holds 589 824 samples, 566 596 of them non-zero, peak 0.592.
  - Headless: `[audio] muted (--headless) ...`, `0 frames sent`.
  - The full scripts suite is green (no golden or frame changed). `tools/check.sh`: four gates pass; core parity's only findings were package #21's new files, not yet in the Linux list in the worktree it ran on (that package's commit adds them).
  - Frame: `docs/frames/2026-11-21-linux-level-sound.png`.
- **Open:** the fall screen (#17b) is not in this base; when it lands on Linux its device takes the same `setSink` line (docs/OPEN_ITEMS.md, the fall's section).
- **Commit:** c6a8573, "Linux: the level's sound reaches the speakers".

### Package #21: the cheats
- **What:**
  - `game/cheats.{h,cpp}` (Flow/Cheats.cs): `Cheats::type`, the 15-letter window that completes `TOOSCARYFORME` (the settings' gore flipped, not saved) and `SEETHEWHOLEGAME` (GameState's debug keys), and `cheatLetters`, Viewer.TypeCheats' filter (upper-cased ASCII letters).
  - Kurt.Cheats.cs: `Kurt::setMortality` (god: `damage()` already refused under it; `Player::fall` now skips the fall-out death, Kurt.cs:494), and `Player::setClipping` with `updateNoclip` (C# UpdateNoclip, operation for operation: 60 u/s, turbo x3, jump up and the new `kKeyDown` down, the turn keys still turn; no collision, no gravity). `updateTurning` is C# UpdateTurning, now shared by the walk and noclip.
  - The main menu's typed keys (MainMenu.HandleKeys/DebugKey): `menu::Keys::typed`; with SEETHEWHOLEGAME on, 3-8 play that level, F the fall, S the stream, D the statistics with random counts; "Really Quit?" takes Y/J/O/S/T and N.
  - Opcode 13 (`if_cheat_key`) stays the C#'s explicit false (ScriptVm.cs:1709, "cheat keys aren't done"): ported faithfully, nothing to change.
  - Linux: the text input is on from the window's creation (Window.cs:71-75), so the cheats are typed in a level (Viewer.TypeCheats' place in the frame: not under the prompt or the pause menu) and the debug keys in the main menu; Q is `kKeyDown`. `--god` and `--noclip` are the C# console's `god`/`noclip` held for the session (every level's and respawn's Kurt, Viewer.cs:286-288). `--type=TEXT@SECONDS` types text in the first level (a test aid).
  - 3DS: the START menu has a **CHEATS** row (above OPTIONS) that opens a page of toggles, `game/cheats_menu.{h,cpp}`: GORE (TOOSCARYFORME's effect: the settings' gore and the running level's script Option), GOD MODE and NOCLIP (held for the session). A page of toggles, not swkbd: the swkbd applet would need its own APT round trip and a typed word for three switches. SEETHEWHOLEGAME is left out on the 3DS: the main menu cannot be typed at. `controls.ini` gains `down` (unbound; the owner picks it). Harness: `make AUTOQUIT=.. GOD=1 NOCLIP=1`; `WALK=` takes `down`.
- **Why:** the owner's 2026-11-20 direction to port all gameplay from the C#. The PORT_GAPS row: the gore toggle and god mode (player.cpp's "God mode is not ported"); noclip and the debug keys came with them.
- **Evidence:**
  - `tests/scripts/cheats_probe` (the C# probe: Cheats, Viewer.TypeCheats, MainMenu.DebugKey, Kurt with LevelCommands.SetGod/SetNoclip). Byte-identical twins:
    - `golden/cheats_typed.txt` (9 typed strings, letter by letter: the cheat, gore, debug keys, the Option, the window) and `golden/cheats_menu.txt` (14 typed texts) vs `vm_tests dump-cheats typed|menu`;
    - `golden/cheats_L3_noclip.txt` (noclip through the floor, then the fall-out death), `cheats_L3_god.txt` (no damage, survives the fall out, dies at once when mortal), `cheats_L7_hurt.txt` (a hurt lands, none under god, one again when mortal) vs `tick1 dump cheats`.
  - `cheats_tests.cpp` also pins the CHEATS page (rows, wrap, flips, BACK, close) and the main page's frame() reading the typed keys.
  - The goldens bite. Each break fails a golden: the god guard dropped from `fall` (cheats_L3_god); jump and Down swapped in noclip (cheats_L3_noclip, cheats_L3_god); the window 14 letters (cheats_typed); DebugKey's fall index 3 (cheats_menu); the god guard dropped from `Kurt::damage` (cheats_L3_god, cheats_L7_hurt).
- **Commit:** "The cheats: TOOSCARYFORME, SEETHEWHOLEGAME, god mode and noclip (#21)", the commit after c6a8573.

### The stream's screen (package #18b): StreamScreen, StreamView, the flow's stream
- **What:**
  - `Stream/StreamScreen.cs` -> `mdk_core/src/stream/stream_screen.{h,cpp}`: STREAM.BNI, STREAM.MTI and the palette (SYS_PAL's 64 colours, then the BNI's PAL), the clips, the WIND loop, the "Stream after LEVELn" line; Frame's order (Back -> `Flight::stop`, `update`, PlaySounds with its 8 players and HITSIDE not restarted while it plays, Report); `SteeringOf`; `Health`; the Died -> Menu / StreamEnded event. The frame is split at the renderer seam: `update()` before `begin_frame` (it sets the camera), `draw()` inside.
  - `Stream/StreamView.cs` -> `stream/stream_view.{h,cpp}`: the background quad 900 ahead, KURT/BONES/SWH150/GUNTA in their poses (Model.Parse NoFlags/Named, ModelAnimation Bake, Build's corner walk and batches), the tube far to near (Blended, vertex RGBA), the planet and the additive lights (256 sprites), the health box (SC_STAT, SNIP_TXT, the blink at 20), the fade (red added, white, black), and ViewOf's camera.
  - The sounds go through the shared `SoundMixer`: the BNI's WAVs are laid out as a one-archive SNI in memory (full volume, gain 1 = Ui.Play's) for a `SoundBank`, so the SDL sink and the 3DS's DSP channels need nothing new.
  - The flow (`game_flow`): `streamDirect()`/`streamSeed(clock)` (Game.cs:515-516: DefaultSeed for `--stream`, else the clock) and `setStreamHealth()` (AfterStream's `StreamScreen.Health`, LEVEL5's entry after the Gunter stream). `skipScreen`'s Stream arm stays for the flow golden; the frontends no longer skip the stream.
  - Rebased onto the menus (#20a) and the cutscenes: after a stream the flow's main menu (Kurt died in the Gunter tube) is shown by #20a's front screens, and the level chain now ends at the menu after LEVEL5 instead of exiting.
  - Linux: `--stream=N` and `--health=N`; the level-end hook tears the level down and the level loop's top plays the stream (its own renderer scope, released, four empty frames), handles its event, skips what is still unported, loads the next level. A `--fixed-step` run seeds every stream with the default seed; the `--walk` keys steer it. Sound to the SDL sink when there is a window.
  - 3DS: the same hookup in `main_3ds.cpp` (`runStream3ds`): aptMainLoop, the resume rebase, the harness deadline/budget/capture/walk, the circle pad (past a third of its throw) added to the keys, START = Back; the screen and its `StreamRun` on the heap; the DSP path opened on the stream's bank and closed before it goes. `make AUTOQUIT=.. STREAM=1 LEVEL=n` starts in the stream (the C#'s `--stream=N`); FIXED=1 seeds every stream with the default seed.
  - Two 3DS-specific pieces, through the existing renderer API: the PICA vertex has no colour, so `MdkCapabilities::vertex_colours` (false on the 3DS) makes the tube draw from a 64x6 RGBA table of exactly `StreamTube::rgba`'s colours, each corner's UV its (colour, level) texel; and the 600x360 background is cut at the image's edges (the 3DS pads it to 1024x512, so a repeat would read padding). On Linux the cut is pixel-identical to the uncut quad (measured). `pica::alphaTestReferenceFor` no longer applies the filtered half cover to Blended draws: their alpha is translucency (the tube is 0x0F-0x5A; the glass's one-pixel texture too), which the half cover deleted whenever filtering was on.
- **Why:** the owner's 2026-11-20 direction to port all gameplay from the C#. The stream sits between every level and the next; after LEVEL8 it decides LEVEL5's health or ends the game. Package 2 skipped it.
- **Evidence:**
  - `golden/stream_screen_L{7,8,3}.txt` are the C# app's own stdout (`mdk_sdl --hidden --original --gpu=vulkan --size=320x240 --mute --wait=50 --stream=N`, the oracle build in a private namespace on :99). `tests/scripts/stream_screen_test.sh` runs `build-linux/mdk --headless --fixed-step --stream=N` and matches the report byte for byte: the tube's line, the first wall hit (segment, t, x, z, yaw, speed, health), Bones, the end. It also checks LEVEL5 entered at the Gunter stream's 63 health and the stream's renderer scope back at its mark.
  - They bite: without `setStreamHealth` LEVEL5 enters at the wrong health (L8 fails); reporting before the update drops the end line (all three fail).
  - `golden/stream_view_models.txt` is `tests/scripts/stream_view_probe` (the C#'s Mdk.Formats): the stream's five images and every (model, animation) pair the view draws, every baked frame's corners hashed; `tick1 dump streamview` matches it byte for byte.
  - Frames: Linux `--stream=7` frame 600 against the C# oracle's screenshot at 10 s (320x240): 76,755 of 76,800 pixels identical; the 45 that differ are one patch inside Kurt. The first comparison found Kurt 12% small: the camera was set after `begin_frame`, which the Vulkan backend reads at `begin_frame`, so the frame drew last frame's eye; fixed by the update/draw split.
  - The chain (`level_chain_test.sh`, updated): 3 -> stream -> 4 -> stream -> 8 -> Gunter stream -> 5 (entered at 63) -> menu; the chained level-4 and level-8 frames still equal direct starts (now at chained frames 2624 and 5159); four level-3 teardowns, each followed by its stream, leave the heap flat (70.78, 70.79, 70.79 MB) with the renderer log at 0.
  - `flow_tests`: the seed choice (direct vs after a level) and the stream's end health into LEVEL5. `pica_matrix_tests`: Blended keeps the oracle's discard with filtering on.
  - 3DS: the release build compiles. No emulator run here (the parent runs it).
- **Still open:** see `docs/OPEN_ITEMS.md`, "The stream's screen": the 45-pixel patch on Kurt, the canvas mods, the 3DS's look and cost unmeasured. The difficulty is the settings' (#20a's GameSettings), as the C#'s ui.Settings.Difficulty.
- **Commit:** this entry's commit.

### Sniper mode's frontends and screen (gap package #7)
- **What:** sniper mode now looks through the scope on both frontends, ported operation for operation from the C#.
  - `Hud/SniperOverlay.cs` -> `game/sniper_overlay.{h,cpp}`, owned and drawn by `HudView` in place of the HUD (`HudView.Draw`): the SNIPERS1 frame (cut by `CutView`), the SNIPERS2 mask, the round cameras' fills and SNIPERGA's miss animation, the air strike's iris and pulse, CROSS, the zoom percent and SNIP_RNG's gauge, the ammo icons/label/count, the health in the frame's panel.
  - `SniperView.cs` -> `game/sniper_view.cpp` (`CameraMath.View`, `ClipPlace`, `OnCanvas`, the round-camera and clip views, `Aim`, the SniperHud) and `game/sniper_view_draw.cpp` (`Draw`, `DrawRoundCamera`, `DrawClip`).
  - `Viewer.SceneView`/`DrawScene` in both frontends: the frame is drawn through `buildSniperCamera` while Kurt snipes, the mixer hears through the scope (`ScopeZoom`, the line of sight and its up, the listener at the scope's eye), the scripts' eye is the scope's, the rounds in flight (`SniperRounds.Visuals`) are drawn with the objects, and `KurtSprite` hides Kurt.
  - `Renderer.Insets.cs` -> the render seam: `Inset::scene` (InsetContent); begin/end record instead of flushing. Vulkan draws Scene insets (the round cameras) and Own insets (the clip) in their viewport and scissor with the depth cleared, in the direct, deferred and post frames (not in stereo: refused once). Citro3D draws Own insets the canvas's way (one matrix onto the rectangle) after the canvas, and refuses Scene insets.
  - Linux: `Input.AddWheel`/`ClearMouse` (the wheel zooms the scope and presses the item keys until the step uses them, `Input::wheelKeys`), and `SniperTest`'s `--sniper[=zoom,pitch]`, `--zoom=s`, `--sniper-fire`.
  - 3DS: the overlay's two full-screen images are cut into 63x63 pieces of every second texel (`SniperOverlay::Budget`, ~0.4 MiB of linear memory instead of 4 MiB); no round-camera insets (a watching camera's window is filled as empty); the clip is drawn. Zoom stays on the unbound zoom_in/zoom_out: the C# pad zooms on the d-pad's up/down, which this layout gives to sniper and use_item (logged once at boot).
- **Why:** the owner's 2026-11-20 direction to port all gameplay from the C#. After #5 and #6 the sniper worked in logic but drew the follow camera and Kurt, so on device it was unusable.
- **Evidence:**
  - The rounds probe's new `hud` and `iris` modes (`tests/scripts/rounds_probe`; the C#'s overlay and renderer made without their constructors, the renderer's canvas lists recording `FillRect`/`DrawImage`) == `tick1 dump sniperhud|sniperiris`, byte for byte: `golden/L3_sniper_iris.txt` (the overlay alone, every quad), `golden/L8_sniper_hud_ship.txt` and `golden/L8_sniper_hud_kill.txt` (watching, hit, kill and miss cameras with their views, the clip's places and view). The probe's default mode still reproduces the three rounds goldens.
  - The goldens bite: DegreesToRadians as a reciprocal (both hud goldens), the iris closing at half speed (iris), the miss frame off by one (iris, kill). Not pinned: Vector3.Lerp fused vs unfused in `ClipPlace`'s scale, and `MathF.Round`'s halves to even for the gauge and percent (no step lands on a half).
  - Linux headless frames (lavapipe) against the oracle's `--sniper` at 640x480 on L3: the scope at 2 s (frame 120), 58 of 307200 pixels differ, all in the scene; a round camera watching at 1.77 s (`--sniper-fire`, frame 106), 376 differ. `--walk` series at 400x240 `--true-aspect` (the 3DS's canvas): package #5's harness walk (sniper, look down, turn right, zoom in, fire, leave) shows the scope, the look down, the zoom gauge to 59%, the miss/hit fills and Kurt back after leaving, but its turn faces a dark wall of HMO_1; `sniper@120-124;zoomin@200-240;fire@260-300;sniper@450-454` shows a round camera watching (frame 275) and then its fills (frame 330).
  - No emulator or hardware run (the parent runs Mandarine).
- **Commit:** this entry's commit.
- **Still open:** `docs/OPEN_ITEMS.md`, "Sniper mode's frontends and screen".

### Ledges: the grab, the hang and the climb (PORT_GAPS package 12)
- **What:** ported, operation for operation:
  - `Kurt.Ledge.cs` whole into the new `mdk_core/src/game/player_ledge.cpp`: `grabLedge` (GrabLedge, Kurt.Ledge.cs:52-74), `hangFrom` (HangFrom, :77-103) and `updateClimb` (UpdateClimb, :106-129), with the climb tables and `Wrap180`. The hang's yaw is `atan2f` (the oracle's MathF.Atan2) converted as .NET's RadiansToDegrees, pinned R6 as the slide's heading is.
  - Wired where `Kurt.Update` has them: `Current is Hang or HardLand` -> `UpdateSpecial` (Kurt.cs:190, after the sniper and slide steps of packages 5 and 8), `UpdateSpecial`'s Hang arm (Kurt.cs:598), and `GrabLedge(before, forward)` after the fall, `StayOnKeeper` and `UpdateTouched` (Kurt.cs:237), with `before` taken where the C# takes it (Kurt.cs:220).
  - `ArenaSpace.Probes.cs`'s `Edge` and `Free` (:35-44) as `ArenaSpace::edge` / `free`; `ArenaSpace::Floor` (the C#'s Surface) now carries the arena and triangle `floorCrossing` found. `Bsp.Edge` and `Bsp.Crossing` (Bsp.cs:368-423) as `Bsp::edge`. Each in a delimited block (packages 3 and 8 edited these files).
  - Solid objects: the C#'s grab is BSP-only (FloorCrossing, Edge, Free), so no solid object stops a grab here either.
  - `FollowCamera::climb()` (read-only, for the goldens). The camera's climb support was already the C#'s generic rise of the feet; it needed nothing.
- **Why:** the owner's 2026-11-20 direction to port all gameplay from the C#. Without it Kurt fell back from every wall top his jump reached, so ledges the levels expect him to climb (LEVEL4's MEAT_3, LEVEL6's OLYM_3 and others) were unreachable that way.
- **Evidence:**
  - The C# probe `tests/scripts/ledge_probe` and its twin `tick1 dump ledge` (Kurt's arena, state, feet, yaw, speeds, floor/chute, frame, climb ticks, hp, the camera's climb / air pitch / arena pitch, the voices) match byte for byte on four goldens: `L6_ledge_climb` (OLYM_3: a running jump at a 16-unit wall, hang at step 56, a 31-tick climb, standing at 120), `L4_ledge_climb` (MEAT_3, 17 units, the chain gun firing: the grab stops MULTIFIRE), `L4_ledge_short` (MEAT_3 from 1.5 before the wall: the hands stay short, no grab) and `L6_ledge_letgo` (the OLYM_3 jump with forward released before the fall: no grab). The ledges come from the probe's `scan`, which keeps to edges along +Y so the grab leaves Kurt at a yaw of exactly 0, where the LUT facing (kept: owner decision 2026-11-20) is exact.
  - The goldens bite: no forward test (L6_ledge_letgo), the climb moving forward instead of back (both climb goldens), no StopFiring (L4_ledge_climb), the grab not wired (both climb goldens), `free` inverted (both climb goldens), hands at 6.0 instead of 4.6 (both climb goldens and L4_ledge_short). Every existing golden still passes.
  - 3DS harness: the scan (`ledge_probe scan <level> <arena> any`, any edge and facing) found no ledge in any level's start arena, and `AT=` keeps the start arena, so there is no harness walk that shows a grab yet (`docs/OPEN_ITEMS.md`, "Ledges").
  - Gates: `make -C mdk_3ds`, `tests/formats`, the Linux build, `make -C tests/scripts test` and `tools/check.sh` (see the commit). No emulator or hardware run.
- **Commit:** this entry's commit.
- **Still open:** `docs/OPEN_ITEMS.md`, "Ledges: what the goldens cannot see".

### Package #9: the rides core and the XD2 (2026-11-20)
- **What:** `Scripts/Rides.cs` and the ride half of `Kurt/Kurt.Ride.cs`, ported operation for operation.
  - `mdk_core/src/scripts/rides.{h,cpp}`: `Rides` (Update, TryMount, MountWalker, UpdateWalker, Follow, Dismount, Lost, RideWalker, Hides, TakesHits, Driven), owned by `ScriptRuntime` as `rides_` in place of the fixed-null `ridesRidden_`/`ridesBoard_`; `Update` in `runTick` after `KurtTouchesGroups`, `Follow` after the tick loop, `Lost` first in `remove`, `TakesHits` in `hurtKurt`, `Hides` in the visibility pass. Opcode 171 now answers true for the XD2 Kurt rides.
  - `Player` (`game/player.{h,cpp}`): `walkMode` (Walk.Riding: no strafe, and the riding return after the walk move), `visible`, `rideKeys`, `rideWalker`, `jumpOff`, `getOffBoard`, `glide`, `updateGun`, and the C#'s `Ride` delegate as `Player::RideHook` with its branch in `update()` and `land()`'s soft landing.
  - `KurtSprite::draw` returns early while Kurt is hidden; both frontends draw the ridden XD2 (`Rides::driven()`) at its current transform, with Kurt.
  - Seams for #10 (snowboard) and #11 (XE bomber): `TryMount` recognises XSNOWB/XE/X_STRIKE and leaves Kurt off them; `Rides::board_`/`bomber_`, `RideHook`, `glide`, `getOffBoard` wait for them.
- **Why:** the owner's 2026-11-20 direction to port all gameplay from the C#. LEVEL7's DANT_9 is played in the XD2: kill its driver, climb in from behind, walk it past the guards to their room, and its script lets you out (opcode 171 drives that script). Before this Kurt bumped into an empty XD2 forever; the board (#10) and the bomber (#11) build on this core.
- **Evidence:**
  - `tests/scripts/ride_probe` (C#) and `tick1 dump ride` (C++) match byte for byte on three goldens: `L7_ride_xd2` (700 steps: firing, he runs into the rideable XD2 and stays out; he lets go and is in, hidden, 171 true; DUMMY while steering, ALERT and the alarm while firing, jump and strafe ignored, a turn to 115.7; teleported beside the guards' rect he drives in, the script clears the flag 3.5 s later and he jumps out; the XD2 blows up and knocks him down), `L7_ride_lost` (two 30-point hits go to the XD2, the second kills it, Rides.Lost: 50 damage and the jump), `L7_ride_test` (Rides.RideWalker on the live XD2, steered at yaw 321 and turned, the flag cleared: out). Per step: Kurt's state, feet, yaw, speeds, walk mode, visibility, ride keys, gun, health, the ridden object, 171's answer, TakesHits, the alarm, Touched, the XD2 (place, yaw, flags, animation rate, health, script position), the follow camera (place, pitch) and the voices.
  - The goldens bite; each mutant is caught: mounting while firing (`L7_ride_xd2` at line 219), no Follow (all three), strafing in the XD2 (`L7_ride_xd2`), no 50 damage on Lost (`L7_ride_lost`), hits to Kurt while riding (`L7_ride_lost`).
  - The camera eases toward the arena's rest pitch in both trees: opcode 203's track pitch is fastAtan2 here and a ULP off (`docs/OPEN_ITEMS.md`, the rides section).
  - Prior-solution lookup: the Obsidian and Cognee MCP servers answered "Error POSTing to endpoint"; the vault on disk (`/mnt/nas/Vaults/port-docs`) has no note on the rides or the XD2. Pi knowledge is not in this session.
  - Gates: `make -C mdk_3ds`, `tests/formats`, the Linux build, `make -C tests/scripts test` and `tools/check.sh` (see the commit). No emulator or hardware run (the parent runs Mandarine).
- **Commit:** this entry's commit.
- **Still open:** `docs/OPEN_ITEMS.md`, "The rides: the core and the XD2".

### Package #10: the snowboard (2026-11-20)
- **What:** `Scripts/Snowboard.cs` whole and the board halves of `Scripts/Rides.cs` and `Kurt/Kurt.Ride.cs`, ported operation for operation on #9's rides core.
  - `mdk_core/src/scripts/snowboard.{h,cpp}`: `Snowboard` (Update, GetOff, Steer, Accelerate, SlopeSpeed, UpdatePitch/Ground, Jump, UpdateSounds, FollowPath, Turn, Place, Ram, Animate, JumpPivot, TurnInput) in the C#'s tick order; its trig is glibc's (`@vfp-exempt R6` pins: gameplay, once a step, bit for bit with MathF here), its path search keeps the C#'s loop (`@vfp-exempt T1,T2`).
  - `scripts/rides.{h,cpp}`: `MountBoard` (flags 0x80800 set / 0x800100 cleared, `leaveSniper`, the board's step as `Player::RideHook`), `Rides::board_` an owned Snowboard, Dismount's `GetOff`, the `board && health == 0` off-test, Lost without damage, TakesHits false. `ScriptRuntime::solidArenas()` now drops the second arena while Kurt rides (the "board drop" the package row named; the branch was written and unreachable).
  - Integration with #19 (landed in parallel with #9): `ScriptRuntime::canSnapshot` read #9's removed `ridesRidden_`; it reads `rides_.ridden()` now (the C#'s `Rides.Ridden == null`, ScriptRuntime.Snapshot.cs:25), so no save is taken on the board or in the XD2.
  - `CameraRoll::follow` (CameraRoll.cs:62, the view rolls toward the board's bank at 45 deg/s) through `Player::followCameraRoll`; `Player::muzzleOffset`'s K_SURF (-42, 12). The K_SURF/K_SURFJ pose and the camera pivot go through #9's `hasPose`/`poseName`/`cameraPivot`, which KurtSprite and the follow camera already read; both frontends already draw the ridden board with Kurt (`Rides::driven()`).
- **Why:** the owner's 2026-11-20 direction to port all gameplay from the C#. LEVEL4's CMEAT_3 -> MEAT_4 -> CMEAT_4 -> MEAT_5 is ridden on the board, and its end box (script 147357) is how the level goes on; before this Kurt landed on the rideable board and stayed on foot.
- **Evidence:**
  - `tests/scripts/board_probe` (C#) and `tick1 dump board` (C++) match byte for byte on five goldens: `L4_board_ride` (700 steps: the mount at step 38 and the one-arena collision, faster, the carve left/right with its ramp and snap, +-30 and SKITURN, the strafe keys winning a tie, braking, a jump off a ledge with K_SURFJ held, the pivot dip, firing stopped in the air, SKILAND and the late landing's K_SURFJ 6-10, the K_SURF muzzle, a held jump key that does not re-jump), `L4_board_off` (the script's flag cleared: the board thrown at 1.1x with gravity 64, Kurt walking on with its speeds, two arenas again), `L4_board_lost` (the board killed under him: off, unhurt), `L4_board_dead` (killed on the board: GetOff), `L4_board_end` (6200 steps from halfway down CMEAT_3: MEAT_4, a jump over its lip, two XTGUN rams at 5 damage each, CMEAT_4, MEAT_5's end box letting him off at 6054, the thrown board dying on its first collision). Per printed step: Kurt (state, feet, yaw, speeds, floor, visible, pose, pivot, roll, gun, health), the ride (Ridden, OnBoard, TakesHits, Touched), the board (place, yaw, pitch, flags, path time/stop, velocity, gravity, friction, script, dead), the Snowboard's fields, the camera's height and pitch, the solid arenas, the voices.
  - The goldens bite; each mutant is caught: no board flags on mount (all five), no solid-arena drop (all five), the steer ramp x1.5 (`ride` line 131), the throw at 1.0x (`off`, `lost`, `dead`, `end`), no ram damage (`end`), a tie going to the turn keys (`ride` 221), no K_SURF muzzle (`ride` 397), Lost hurting on the board (`lost` 151), no off on death (`dead` 151), no SKILAND (`ride`, `end`), CameraRoll.Follow not holding the leveller (all five). `rintf` -> `roundf` (path_stop's ties) survives: no path time lands on .5 in them (OPEN_ITEMS).
  - Not printed: the camera's x/y (Kurt's facing is the LUT's: a 1/64 cell on 3 steps of 700) and the muzzle's random frame/jitter (the oracle's Random is unseeded); see OPEN_ITEMS.
  - No existing golden changed (`make -C tests/scripts test`: every case passes).
  - Prior-solution lookup: the Obsidian and Cognee MCP servers answered "Error POSTing to endpoint"; the vault on disk (`/mnt/nas/Vaults/port-docs`) has no note on the snowboard. Pi knowledge is not in this session.
  - Gates: `make -C mdk_3ds`, `tests/formats`, the Linux build, `make -C tests/scripts test` and `tools/check.sh`. No emulator or hardware run (the parent runs Mandarine).
- **Emulator check for the parent (not run here):** with #19's `ARENA=` (it teleports him into CMEAT_3 as the probe's tp does): `make -C mdk_3ds AUTOQUIT=60 FIXED=1 LEVEL=4 AT=420,4285,-570,90 ARENA=CMEAT_3 WALK='forward@60-120;turnleft@130-160;turnright@165-215;back@240-290;jump@300-306;fire@395-420' CAPTURE=200` -- Kurt dropped above CMEAT_3's board lands on it (~frame 38) and rides it down: K_SURF frames, the board under him, the view banking, SKI/SKITURN; frame 200 should show him carving right on the board.
- **Commit:** this entry's commit.
- **Still open:** `docs/OPEN_ITEMS.md`, "The snowboard".

### Package #11: the XE bomber (2026-11-20)
- **What:** `Scripts/Bomber.cs` whole, the bomber halves of `Rides.cs` and `Kurt.Ride.cs`, FollowCamera's `LookDown` and `Hud/BomberOverlay.cs`, ported operation for operation.
  - `mdk_core/src/scripts/bomber.{h,cpp}`: `Bomber` (Update, MoveCursor, CursorStep, RefillBombs, Drop, Aim, AimDirection, FallTime, FirstObject, PassDamage, Shown), Kurt's ride step through `Player::RideHook`.
  - `scripts/rides.{h,cpp}`: `mountBomber` (TryMount's XE / X_STRIKE), `hides`, `follow`'s and `dismount`'s bomber arms, `bomberRide()`.
  - `game/player.{h,cpp}`: `hasTopDownHeight`/`topDownHeight` (Kurt.TopDownHeight), cleared by `getOffBoard`. `game/follow_camera.{h,cpp}`: LookDown in `update`, a `blend(camera, player, ...)` overload, the top-down guard in `mouseLook`.
  - `game/bomber_overlay.{h,cpp}` (+ `bomber_overlay_mods.cpp`): BOMBTARG, CROSS and the count in the big font; `HudView` draws it first when `HudState::bomber` is set; both frontends fill it from `Rides::bomberRide()` and draw the flown XE at alpha 1 (the C#'s Carrier, see OPEN_ITEMS).
- **Why:** the owner's 2026-11-20 direction to port all gameplay from the C#. L7's DANT_5 is crossed in the XE: shoot the comm device, board the flyer it calls, bomb the guards and generators below from the top-down view. Before this Kurt touched the XE and nothing happened.
- **Evidence:**
  - `tests/scripts/bomber_probe` (C#) and `tick1 dump bomber` (C++) match byte for byte on five goldens: `L7_bomber_mount` (firing refuses the mount, he lets go and is on, the view sinks 50 -> 0 and the XE hides, a hit passes from the XE to Kurt, the unlock), `L7_bomber_bombs` (the cursor on keys, strafe vs turn, the mouse only without keys, held fire drops once, ten bombs run out and refill, the bombs fall and blow up, the overlay's quads), `L7_bomber_target` (four bombs led onto the walking guard XG#3500 kill it, kills 1), `L7_bomber_walk` (no test hooks: Kurt shoots the comm device, walks into the landed XE and bombs), `L7_bomber_end` (the script ends the ride at 6336, Kurt drops and the XE shows). Columns: Kurt, the ride (view, lock, hide, cursor, speed, bombs, refill, fire held), the XE, LookDown's camera, the objects' count/health/kills, every bomb, the voices, the overlay's draw list (count, FNV hash, the quads every 10th step).
  - The goldens bite (`tick1 dump bomber` vs the goldens per mutant): no restart of the cursor's speed the other way (bombs), the hidden XE left solid (all), PassDamage not restoring the XE's 10000 (all), LookDown ignoring the sinking height (all), the cross not rounded (bombs). The miss branch's mutant survives: no drop misses in DANT_5 (OPEN_ITEMS).
  - Rounding, measured: the follow camera off the XE goes 1/64 apart once (step 640) through `fastSin`/`fastCos` and matches with libm swapped in; the goldens print `cam=follow` there and `up=facing` on the XE (OPEN_ITEMS, "The XE bomber").
  - The aim (`Bomber::aimDirection`) takes libm `sinf`/`cosf` under an `@vfp-exempt R6` pin, as `scope.cpp`'s sniper eye does: it sets where each bomb lands, and with `fastSin`/`fastCos` the bombs' velocities move a ULP (`L7_bomber_bombs` and `_target` then differ in the bombs' columns; the guard still dies). Once per fire press.
  - Rebase onto #19 (save/load): `ScriptRuntime::canSnapshot` read the pre-#9 `ridesRidden_`; it now reads `rides_.ridden()` (`scripts/script_snapshot.cpp`), the C#'s `Rides.Ridden == null`.
  - Prior-solution lookup: the Obsidian and Cognee MCP servers answered "Error POSTing to endpoint"; the vault on disk has no note on the bomber. Pi knowledge is not in this session.
  - Gates: `make -C mdk_3ds`, `tests/formats`, the Linux build, `make -C tests/scripts test` and `tools/check.sh` (see the commit). No emulator or hardware run (the parent runs Mandarine).
- **Commit:** this entry's commit.
- **Still open:** `docs/OPEN_ITEMS.md`, "The XE bomber".

### Package 17b: the fall before a level, its screen and its view
- **What:** the C#'s `Fall/FallScreen.cs`, `FallView.cs`, `FallModels.cs`, `FallGround.cs`, `FallPalette.cs` and `FallHud.cs`, ported to `mdk_core/src/fall/` and shown by both frontends.
  - `fall_screen.{h,cpp}` (FallScreen): FALL3D.BNI, FALL3D_n.MTI (index 0 see-through, as `TextureArchive.Load`), FALL3D.SNI decoded at open; the sim with the frontend's seed (the clock; 1 under a fixed step); its Sound event on 8 voices at most at full gain, WINDLOOP from the start and C_GRIND from the fall's start at `windVolume()`/`grindVolume()`; its Message event as a 3 s zoomed message; Esc/START skips; `next()` is FallScreen.Next: Landed stores the carry and is `Event::Play`, GameOver is `Event::Menu`.
  - `fall_view` (FallView: the camera straight down at 250 px focal, the background quads at 7000-6970 below it, the models, radar beam, trails, the HUD, the effects), `fall_models` (FallModels: the C#'s surfaces, batches and UVs; the rest poses built at load, Kurt and Bones written into one dynamic mesh each from their baked animations instead of 199+ cached meshes), `fall_ground` (FallGround), `fall_palette` (FallPalette), `fall_hud` (FallHud), `fall_layout` (FallView's statics and tables: RadarRing with .NET 10's fused Lerp, Orient, NearCamera, HazeBlend, the explosions' growth).
  - The palette's effects are DRAWN: the seam has no palette update (and the 3DS cannot re-expand its textures each flash), so the C#'s `(c k + (256 − k) 255) >> 8` is a white fill of alpha (256 − k)/256 over the frame and the red an additive fill.
  - The 3DS draws it the "lean" way (`FallViewOptions::lean`, `--fall-lean` on Linux): the track as PODn over the ground (no 1 MB re-expansion a frame), the haze as one half-resolution RGBA image on the canvas (a scene draw there takes neither the 600 x 1440 masks nor a material alpha), the radar and trails projected onto the canvas (the half-cover cut deletes their alpha in the scene), EXPLODE's 26 frames a texture each (3328 rows > 1024).
  - The flow: `GameFlow::skipUnported` stops at the Fall (the frontends show it); `play()` keeps the whole carry as `levelCarry()`, and the level's first Kurt takes it through `fall::carryIntoLevel` on both frontends (the pickups again without messages, then the health) instead of `setHealth`.
  - Linux: `menu_screens.cpp` runs the fall between the briefing (or the statistics) and the level; `--fall=N` starts there (the C#'s `--fall`); test aids `--skip-falls` (each fall skipped the C#'s Esc way before its first frame) and `--fall-chase` (the probe's chase steering). 3DS: `menu_screens_3ds.cpp` runs it with the circle pad, the D-pad and the movement actions steering, START skipping, the DSP path opened for the fall's bank and closed before the level's; `make AUTOQUIT=.. FALL=n` starts there.
  - Two Vulkan canvas bugs the fall's fills exposed, both against the C#'s Renderer: a canvas vertex carried the quad's colour as well as the material tint, so every translucent fill drew at alpha² (the C#'s canvas vertices are White, Renderer.cs AddQuad); and `CanvasQuad::additive` (the C#'s AddRect) was ignored. The pinned level-3 frame is unchanged by both.
- **Why:** the owner's 2026-11-20 direction to port all gameplay from the C#. The fall decides the health each of LEVEL7/6/3/4/8 starts with and the pickups Kurt carries in; until now every fall was skipped ("flow: skipped Fall") and only the health of a skipped one reached the level.
- **Evidence:**
  - `tests/scripts/fall_probe` has a `view` mode: each step what the C#'s FallView makes of the sim (FallPalette.Set and its palette's hash, the intro's progress, FallGround.CentreRow / UpdateTrack with a hash of the rows it wrote / Visible / Crawler, the haze's alphas, the models' animations and frames, each missile's Orient by reflection, the pickups' yaw and chutes, the explosions' frame and scale, RadarRing's four rings, the fills). `golden/L6_fallview_chase.txt` and `L8_fallview_hard.txt` (1146 and 797 lines); `tick1 dump fallview` prints the same bytes.
  - The goldens bite, one mutation at a time, each failing both: the track's ramp at 0.7 a row; colour 0 always whitened; explosions growing 2.1x; HazeBlend[12] = 25; RadarRing's Lerp unfused (that one is how the fused form was found).
  - The skydive goldens are unchanged; `flow_tests`' skip-trail test now stops at the Fall and pins `levelCarry()` (64 health and two pickups through Play; a skipped fall's 100 and none; LEVEL5's 55 after the Gunter stream).
  - Frames against the C# app (`--fall=7`, 1280 x 960, lavapipe): the intro at 2 s is BYTE-IDENTICAL; 0.08 s into the fall (the whitening at k = 21) every probed pixel is within 1 (mean delta 0.28); 0.5 s in ("Avoid the RADAR!") mean delta 0.67, the rest the C#'s clock-seeded random. `docs/evidence/2026-11-20-fall/`.
  - `tests/scripts/fall_frontend_test.sh`: `--fall=7 --fall-chase` lands at 33 s and enters LEVEL7 at health 67 with SW_HOME, SW_GATT, SW_HOME in Kurt's inventory (slot 6:1, 16 homing missiles, the super chain gun's 200 on the HUD); New Game -> briefing -> fall -> Esc -> LEVEL7 at 100 with nothing; LEVEL8's fall the lean way. `level_chain_test.sh` and `menu_frontend_test.sh` pass `--skip-falls` and keep their frames.
  - 3DS: release and harness builds compile. No emulator run here (the parent's): `make -C mdk_3ds AUTOQUIT=90 FALL=7` (or `AUTOQUIT=90 MENU=Menu.Accept@2,Menu.Back@4,Menu.Accept@5`, New Game's path, which now reaches the fall before LEVEL7).
  - No opcode changes: `docs/OPCODE_COVERAGE.md` is unchanged.
- **Commit:** see `git log` (the commit that adds this entry).

### Package #20b: the statistics, the Score-O-matic's heads and the slideshow
- **What:** the screens between levels, ported operation for operation from `src/Mdk.Game/Menu/StatsScreen.cs`, `HeadsView.cs` and `Slideshow.cs`, and shown by both frontends.
  - `game/stats_screen.{h,cpp}` is StatsScreen whole: the intermission (L1_INTRM, fading in from white), the debriefing (L<n>_MAP; DEBTOP, DEB<n><result> by the town flags' bits 31-29, DEBBOT, typed), the Score-O-matic (five rows: wait, SNIPER and the row big, wait, count up with CGUN, RICO and ALDIE, slide to its place; then the head shots, one a second, XGHEAD1/2), the save prompt after it (#19's `SavePrompt`, now drawn: the level moves on and is offered under its number), and the next level's briefing; Esc skips, Fire or Jump held runs it all twice as fast. Its sounds are STATS.BNI's WAVs on its own `audio::SoundDevice` (the C#'s `_ui.Audio`), which the frontend updates after the frame (Game.Run). #20a's `Briefing` class is now its Briefing page (`StatsPages::Briefing`), so TELETYPE plays in the briefing too; `game/briefing.{h,cpp}` keeps the Typesetter.
  - `game/heads_view.{h,cpp}` is HeadsView: STATS.BNI's XGHEAD with STATS.MTI's textures through the page palette, at most 16, spinning at 157 degrees a second, one row or two. The C# draws them in a renderer inset; neither backend has an inset rectangle, so the camera's projection is remapped into the view's rectangle (identical on Linux, the identity on the 3DS's 600 x 360 canvas). The draw sets the camera, an unlit rig, no point lights and no glow quads every frame (AGENTS.md P2).
  - `game/slideshow.{h,cpp}` is Slideshow (MDKS_001-008.GIF, 5 s / 4 s / 2 s). The C# starts it at MDK12.FLC's end, which is #20c's VideoPlayer; until then it does not start, as in the C# without the video.
  - The flow: `GameFlow::skipUnported` stops at the Statistics (their package is 0 in the skip log); `skipScreen` keeps their arm.
  - Linux: `menu_screens` shows them (and the briefing) through StatsScreen; a level's end goes there ("flow: the statistics follow"); the C#'s test options `--stats=N --phase=1-4 --counts=... --towns=N --bloodyes/--nobloodno`, and `--skip-statistics` (the C#'s skip, for the level chain's tests). The sounds reach the SDL sink in a windowed run; a headless run keeps the muted device's accounting.
  - 3DS: `menu_screens_3ds` shows them; the keys are the menus' (any button, B skips, Fire/Jump held runs fast; the prompt: D-pad, A, B — A saves under the offered number); the sounds mix on the ARM11 into `audio_3ds`'s one ndsp channel, opened with the screen and closed with it (the DSP path binds a level's mixer and SNI bank).
- **Why:** the owner's 2026-11-20 direction to port all gameplay from the C#, and the un-cut of the menus and screens. Before this every level's end skipped the statistics ("flow: skipped Statistics (package #20 not ported)"), so the player never saw the level's counts, the debriefing, the before-level save prompt or the next briefing.
- **Evidence:**
  - `tests/scripts/menu_probe` gains the sections `stats-all7`, `stats-score4`, `stats-nogore6` and `slideshow`: the C#'s own StatsScreen through a headless Ui, driven by key schedules at 1/60 s with given counts, its audio device updated after each frame, its `new Random()` replaced by a scripted one, the canvas captured in the renderer's Overlay hook. Each frame prints the page's state (fades, typing, rows, slides, heads), the sounds started, stopped and ended, and the canvas's quads (count and FNV hash; the quads on a page's first frame). `golden/stats_{all7,score4,nogore6,slideshow}.txt` (3896, 2065, 1841 and 26 lines: LEVEL7 at normal speed with gore; LEVEL4 from the Score-O-matic with Escs, 20 heads and a save; LEVEL6 from the debriefing (DEB2FS by town bits 6), fast, without gore, the prompt refused; the slideshow's eight GIFs and its timing); `vm_tests dump-stats <section>` prints the same bytes (`PASS StatisticsMatchTheOracle`). The briefing's `golden/menu_briefing.txt` is unchanged and still matches through StatsScreen.
  - The goldens bite, one mutation at a time: the count-up rate 2 instead of 0.5, a ricochet 1 in 7, no TELETYPE, the rows' places unrounded and the closing prompt's frame drawing the page each fail all7, score4 and nogore6; the heads spinning at 158 degrees a second fails all7 and score4; town bits 6 read as "SS" fails nogore6; the first slide held 3 s fails slideshow.
  - Frames: the Linux headless Score-O-matic with its three spinning 3D heads (frame 300 of `--stats=7 --phase=1 --counts=57,31,9,4,12,20,3` with seventeen Escs), the LEVEL4 debriefing typing (frame 200, `--towns=5`) and the save prompt (frame 258) are BYTE-IDENTICAL (0 of 1 228 800 pixels) to the C# app's at 1280 x 960. The intermission's white fade-in (frame 15) differs by the Vulkan canvas's alpha-squared tint, which #17b fixed in the backend (max delta 64 = 0.25 x 255, exactly that bug's). Pairs (C# left, port right): `docs/evidence/2026-11-20-stats/*-pair.png`.
  - `tests/scripts/menu_frontend_test.sh` section 5: LEVEL3 ends, the statistics show, keys run them through, the prompt saves `4.sav`, LEVEL4 loads; `--stats` starts there. `level_chain_test.sh` runs with `--skip-statistics`. `flow_tests` pins the walk's stop.
  - 3DS: release and harness builds compile; no emulator run here (the parent's).
- **Commit:** this entry's commit.

### The C#-vs-C++ divergences, root-caused; the debris and effects bounces fixed
- **What:** each drift the packages recorded was measured to the first step, field and operation where the C++ stops matching the C#. The findings are in `docs/DIVERGENCES.md` (D1-D7), linked from `OPEN_ITEMS.md` and `PORT_GAPS.md`.
  - **Fixed (port bug, D5):** `Debris::move` and `Effects::move` scaled the bounce as `normal * (dot * 1.4)`. The C# does `(normal * dot) * 1.4`. That cost GUNT_2's explode one debris piece: it lived 10 steps longer than the C#'s, the divergence package 4/8 left open.
  - **Fixed (port bug, D5):** the pieces' orientation step now follows .NET 10's `Quaternion.Normalize(a * b)`: a Vector128 product and a pairwise dot. It is `scripts::quaternionMultiplyNormalize`.
  - **Resolved by package 8 (logic, D3/D7):** HMO_6's extra objects and the L3 fan-room drift. Without the fans' `FanSpark`, the runtime's RNG stream fell behind the C#'s, and HMO_6's turret fired an extra BOLT.
  - **Accepted rounding (owner direction, not changed):**
    - D1: Kurt's walk and aim. The trig table in `Player::facing()` is the ONLY cause.
    - D4: Gunter's yaw. He turns toward Kurt's LUT-drifted feet.
    - D2: L8's XG push. Root motion's `Vector3.Transform` is fused on .NET 10; a verified one-line `fmaf` fix is described, not landed.
    - D6: the stream's 45 pixels. A UV sits exactly on a texel row; the matrix grouping's rounding picks the row.
    - D5: the spin's `CreateFromYawPitchRoll`. It takes .NET's `Vector3.SinCos`, not libm.
  - Test support: the hazards probe and `tick1 dump hazards` take a `flying` token (FNV-1a of every piece's and effect's motion). New golden: `golden/L8_hazards_debris.txt`. `system_numerics_tests` gains the quaternion step.
- **Why:** the owner's 2026-11-20 direction to port all gameplay from the C#. The owner doubted that the trig LUT caused the drifts several port agents had blamed on it without testing. The coordinator's re-scope: the LUT stays and rounding-only drift is accepted; logic divergences get fixed with goldens.
- **Evidence:**
  - D1 experiment (reverted): with `cosf/sinf` in `Player::facing()`, 7 walks of 600-900 steps on L3-L8 (`items_probe` vs `tick1 dump items`, feet/yaw/speeds as bits) and the sniper run at start yaws 37/123.5/250 on L3 and L7 all matched the C# with 0 differing lines. With the LUT they differ on 135-871 lines from the first running step (feet.y, 4 ULP).
  - D4: the cutscene probe at 8362320, with XGUNTAM traced. Gunter matches while Kurt stands or walks at yaw 0. He drifts 1 ULP once Kurt walks after a turn (282 trace lines in one 600-step run), and the drift goes to 0 with exact trig.
  - D2: the save probe at 5d1d4ab, with the XG traced. The first difference is XG #1004's `Push.x` at its 140th motion tick, during root motion. The `fmaf` rotation makes the scenario byte-identical in the export.
  - D3: at 5d1d4ab, `L3_save_fan`'s scenario is byte-identical at 200+30 and 1200+60 steps. With the fans' `setSpark` line commented out, the save has 10 objects where the C# has 9.
  - D5: per-piece and per-effect bit dumps of the GUNT_2 run. The first difference is a bounced piece's velocity at s=68, and the effects' at s=66. After the fix, every piece's centre, velocity and ticks and every effect's position, velocity and life match the C# in that run and in `L8_hazards_bang`, `L8_hazards_kill` and `L4_hazards_blast`. The quaternion step was measured against .NET 10 over 2000 random pairs: the old code was wrong on 1225, the new on 0.
  - D6: a fork's capture of the Linux build of 9de8765 against the oracle app (lavapipe, `DISPLAY=:99` in a private namespace). Nudging Kurt's v moves the count 45 -> 35/71.
  - The goldens bite:
    - the old debris bounce fails `L8_hazards_debris` at s=68;
    - the old effects bounce fails it at s=66;
    - the old quaternion step fails `system_numerics_tests` ("row 0 component 2 was 3EF50A3D, .NET 10 says 3EF50A3C").
  - Every existing golden is unchanged and passes.
  - Prior-solution lookup: the Obsidian and Cognee MCP calls failed ("Error POSTing to endpoint"). A read-only grep of `/mnt/nas/Vaults/port-docs` found no prior note on these drifts.

### Package 20c: the INTRO1A splash, MDK12.FLC behind the menu, the end movies, the loading screen
- **What:** the C#'s movie screens, ported operation for operation, on both frontends.
  - `game/intro_splash.{h,cpp}` ports `Menu/IntroSplash.cs`: OPTIONS.BNI's INTRO1A (two palettes, the run-length picture, `Unpack`), the five steps (black -1 s-> palette 1 -3 s-> -2 s-> palette 2 -3 s-> -1 s-> black), `Blend` (weight `MathF.Round(t * 256)`, half to even), any key skips.
  - `game/video_player.{h,cpp}` ports `Menu/VideoPlayer.cs`: Play (an FLC at the original's 34 ms a frame, the first frame at once), PlayMovie (an MVE at its frame time, sooner while its sound runs low), Hold, Brighten (`round(b * 255)` added, at most 255), ShowStill, Stop, Update's catch-up loop, Finish. The C#'s PalettedImage becomes the image's indices and shown palette with a version each; the frontend uploads what moved.
  - `game/end_movie.{h,cpp}` ports `Menu/EndMovie.cs`: MDKEND.FLC with FINISH.BNI's sounds by frame (DOGSHIP looped from 1, stopped at 188; DROP 129, FLYBY 133, EXPLODE1 186 and 196, ENDEXP 194), `Flash` (210-260, white at 233 held 1 s), then MDKBZK.MVE, then the menu; Back gives up.
  - `game/main_menu` gains the C# MainMenu's splash and video: `setMedia(options, data)`, the splash's frame taking the whole frame (its key is the splash's), StartVideo once per menu, `status()`; `splashFrame()` tells the frontend what to draw. Without `setMedia` the menu is #20a's (its goldens are unchanged).
  - `game/loading_screen.{h,cpp}` ports `Menu/LoadingScreen.cs` (LOAD_n.LBB at (200, 25), LOAD_MSG in FONTBIG in the LBB's palette centred at y 260, the bar and its FrameRect; `ScreenView.Fit.Height`). `game/screen_view` gains `layout(view, fitHeight)` (the C#'s `Layout(fit, size)`), a tinted image and a canvas fill.
  - `game/movie_view.{h,cpp}`: the GPU side shared by both frontends (MovieView: a player's picture; SplashView: the splash, by palette updates or by the 3DS's cross-fade).
  - The flow: `GameFlow::skipUnported` stops at the EndMovie, which both frontends now show; GameFinished goes to the end movies, then the menu with the splash. Both frontends draw the loading screen's one frame before each level loads (C# Play).
  - The renderer seam gains `mdk_render_update_paletted` (the C#'s SetIndices/SetPalette on one picture). Vulkan re-makes the index image and the palette image. The 3DS turns the texture into a STREAMED one: an RGB565 store, no mip chain, index 0 black, rewritten in place at the frame boundary from the indices and palette (`pica::streamTexelsRgb565`, host-tested), one streamed texture per boundary, the updates between boundaries collapsing into the newest (drop frames, never stall).
  - Linux: the C#'s `--menu` (the menu without the splash; the splash is the default, as the C#'s), `--splash`, `--end` (Start.EndMovie), and `--capture-loading=PATH` (the loading frame is not a game frame). 3DS: the splash at boot; harness `SPLASH=0` is the C#'s `--menu`; the end movies skip on B, START or A; the START menu's new last row MOVIES HALF RATE (`EnhancedToggles::movieHalfRate`, config `movieHalfRate`).
- **Why:** the owner's 2026-11-20 direction to port all gameplay from the C#, and the un-cut of the screens (row 20). Before this the game booted straight into a menu over MDKOPT, the end of the game ended the run ("game finished"), and nothing showed while a level loaded.
- **3DS cost and the fallback (the owner judges on hardware):** measured on the host (Ryzen 9 5900HX, -O2): an MDK12.FLC frame decodes in 11 us (mean; 322 us for the first, a whole BYTE_RUN) and converts to RGB565 in 198 us (600 x 360 texels). By the usual 50-60x host-to-Old-3DS ratio for integer code with L2 off, that is ~0.7 ms of decode (~20 ms once, at the first frame) and ~10-12 ms of conversion per movie frame, about 30 a second: it fits the menu's frame (nothing else runs) at the C#'s full rate, so full rate is the default. MOVIES HALF RATE uploads every second decoded frame (and always the last one) for the owner's A/B; the decode keeps the 34 ms clock, so the movie keeps its length. The splash costs nothing per frame on the 3DS: its two palettes' pictures are uploaded once each and cross-faded on the GPU with Blend's own weight, which is Blend's arithmetic (a palette blend is per-entry linear, so it commutes with the lookup) up to the framebuffer's rounding. Memory: MDK12.FLC is read whole (632 KB) and decoded a frame at a time into one buffer; the streamed store is 1 MB (1024 x 512 RGB565); the splash's two stores 2 MB, released when the menu closes.
- **Evidence:**
  - `tests/scripts/movie_probe` drives the C#'s own Flc/Mve decoders, IntroSplash, VideoPlayer, MainMenu, EndMovie and LoadingScreen through a real headless Ui at 1/60 s. `golden/movie_{decode,splash,menu,endmovie,loading}.txt` (528, 78, 1012, 849 and 12 lines); `vm_tests dump-movie <section>` prints the same bytes (`PASS MoviesMatchTheOracle`). decode: every frame of MDK12.FLC, pie.flc and shiny.flc (FNV-1a of the indices and the palette; the rewind); MDKEND.FLC and MDKBZK.MVE are absent from the GOG data and both sides say so. menu: 800 frames with no key (the splash's 10 s, then the video to its last frame) and a key that skips the splash. endmovie: the data's (the menu at once), and pie.flc in MDKEND.FLC's place for every frame extra (the sounds, the stop at 188, the flash, the 1 s hold), and Back.
  - The goldens bite, one mutation at a time: a 35 ms frame fails menu and endmovie; Blend's weight truncated fails splash and menu; the step clock carrying its excess fails menu; the splash ignoring keys fails menu; the loop's stop at 187 fails endmovie; a 0.5 s hold fails endmovie; Brighten one short fails endmovie; a DELTA_FLC skip off by one fails decode, menu and endmovie; the text at y 262 fails loading; Fit.Height as Fit.Inside fails loading.
  - Frames: the Linux headless splash at frames 140 and 300 (the logo; the cross-step between the two palettes) and the menu over MDK12.FLC at frames 40 and 240 are BYTE-IDENTICAL (0 pixels, max delta 0) to the C# app's (`--menu [--splash] --original --wait=..`) at 640 x 480. Pairs (C# left, port right) and the level-3 loading screen: `docs/evidence/2026-11-20-movies/`.
  - `tests/scripts/menu_frontend_test.sh` gains the splash (the key that skips it does not choose New Game), `--end` (the end movies, then the menu with the splash) and the loading screen before `--level=3`; its menu runs pass `--menu`, as the C#'s own tests do. `pica_matrix_tests` pins `streamTexelsRgb565` (each texel at its tile slot, index 0 black, the padding untouched). `lighting_tests` pins the new toggle row and its config line.
  - The 3DS release build compiles; no emulator run here (the parent's).
- **Emulator checks for the parent (not run here):** the splash at boot: `make -C mdk_3ds AUTOQUIT=30` (no LEVEL=) -- the INTRO1A logo fading in, the publisher text at ~5 s, then MDK12.FLC behind the menu; `CAPTURE=300` is mid cross-fade. The menu's video alone: `make AUTOQUIT=20 SPLASH=0 CAPTURE=40`. The end movies: `make AUTOQUIT=40 FIXED=1 LEVEL=5 AT=465,-6,-2263,0 EVENT=81` -- "game: finished; the flow shows EndMovie", "flow: the end movies (end movies: done)", then the menu with the splash. The loading screen: any `LEVEL=` run shows LOAD_n's picture, "Loading" and the bar while the level loads ("flow: the loading screen of level N").
- **Integration with #20b (the slideshow):** `MainMenu` now owns #20b's `Slideshow` and starts it from the player's Finished (`video_.setFinished(&MainMenu::videoFinished, this)`, the C#'s `_video.Finished += _slideshow.Start`), keeping the video's last frame as image 0; its Update runs after the video's, a key resets it, its image is shown on the player (`showSlide`, the C#'s Next -> ShowStill) and the bare first slide hides the items (status "main menu: slideshow"). No C# probe runs the menu long enough to print it, so `movie_tests`' `checkSlideshowStart` pins it against Slideshow.cs's rules (end -> image 0 -> 5 s -> MDKS_001 on the player without the items -> a key gives way to MDKS_002 and chooses nothing -> past the last slide, the last frame again); removing the hook fails it (five checks).
- **Commit:** this entry's commit.

### Every package integrated; smoke pass; main fast-forwarded
- **What:**
  - An integrator agent landed ten finished packages as a linear series ending at c9b7292e, in order: #18b stream screen, #7 sniper view, #12 ledges, #9 rides, #10 snowboard, #11 bomber, #17b fall screen, #20b statistics, the drift fixes, #20c movies.
  - It fixed three interaction bugs on the way:
    - On the 3DS, the scope camera overrode the air strike's camera.
    - The rides package's `canSnapshot` fix wasn't in its commit.
    - On Linux, the statistics screen re-read typed text that the cheats had already taken.
  - It also wired the cross-package hooks: the scope's strike-target check, and the slideshow starting from the menu video's end.
  - The working branch was fast-forwarded to c9b7292e. Then, after the smoke pass, local `main` was fast-forwarded to c9b7292e. That was a clean fast-forward: main was an ancestor, 93 commits behind. Nothing was pushed.
- **Why:**
  - The owner's "do 1 - 4" plan: drain the queue, run the gates and a smoke pass, fast-forward main locally, keep working on the experimental branch.
  - The owner's direction to port all gameplay first.
- **Evidence:**
  - All five gates passed on every combined tree the integrator built.
  - The smoke pass and every per-package emulator check are in `docs/evidence/2026-11-21-mandarine-pass.md`. Two checks there were inconclusive, because their walk scripts were tuned in the C# and the 3DS's accepted trig rounding shifts the aim: the bullet-hole shot and the bomber mount. They go to the playthrough pass, with new harness shortcuts.
- **Commit:** this entry. Main points at c9b7292e.
- **Next:** the leftovers (the cutscene save check, the save prompt after the level-8 stream, the 3DS slideshow stall, harness shortcuts), then level-by-level playthroughs in Mandarine, then the hardware session at the end once ../forsaken is done.

### The Linux-binary suites refuse a stale binary
- **What:** a new suite step, `tests/scripts/linux_binary_fresh_test.sh`, runs before the Linux-binary suites in `make -C tests/scripts test`. It fails, naming the rebuild command, when any `mdk_core/src` or `mdk_linux/src` source is newer than `build-linux/mdk`. With no binary it skips, as those suites do.
- **Why:** those suites run `build-linux/mdk` as it is on disk, and nothing rebuilds it. A stale binary reports the current tree's checks with an old program: an unknown new flag reads as "the run exited 1", and every log expectation after it fails. That happened three times on 2026-11-20:
  - the walk-harness merge;
  - the stream screen's chain;
  - the checkpoint commit 2029db50, where the suite went red at a tree that is green with a fresh build.
- **Evidence:**
  - Passes on a fresh build.
  - With `mdk_linux/src/main.cpp` touched, it fails and names the file and `cmake --build build-linux -j8`.
  - After the rebuild it passes.
  - The suite at 2029db50, rebuilt, is green (exit 0).
- **Commit:** this entry.

### Leftovers (2026-11-20): no save while a cutscene runs
- **What:** `ScriptRuntime::canSnapshot` (`mdk_core/src/scripts/script_snapshot.cpp`) gains the C#'s `Cutscene == 0` term. It is now `ScriptRuntime.Snapshot.cs:24-26` term for term: no cutscene, the level not over, no EndLevel, no air strike out, not riding, Kurt alive, in an arena. Package #19 had left the term out because #13's cutscenes were not in yet; they are.
  - Test support: the cutscene probe and `tick1 dump cutscene` take a `snap` token that adds a `can=` column (`runtime.CanSnapshot()`). Without the token the lines are as before, so the existing `L*_cut_*` goldens are unchanged (re-run against the probe: identical).
- **Why:** the owner's 2026-11-20 direction to port all gameplay from the C#. Without the term, F2 on Linux or the 3DS pause menu's save during Gunter's, the dog's or a boss's cutscene wrote a save the C# refuses (Kurt frozen and hidden, the cutscene's camera mid-shot).
- **Evidence:** `golden/L5_cut_snapshot.txt` (`cutscene_probe 5 110 at:465:-6:-2263:0,ev:40:61,ev:100:53,show:36,snap`): `can=True` before Gunter's cutscene, `False` on each of its 60 steps, `True` once 53 ends it; `tick1` prints the same bytes (`PASS L5_cut_snapshot`). The golden bites: without the term, steps 40-99 read `can=True` and the case fails at s=40.
- **Commit:** this entry's commit.

### Leftovers (2026-11-20): the save prompt after the LEVEL8 stream
- **What:** the C#'s SavePromptScreen (`Flow/Game.cs:670-694`), shown by both frontends where they skipped it.
  - `game/save_prompt_screen.{h,cpp}`: `SavePromptScreen` (open: `_state.Save(SaveKind.LevelStart)` offered under `LevelFlow.SaveName(level)`, "6"; frame: the prompt's Update, then `Closed ? Event.Play : Event.None`; draw) and `drawSavePrompt`, SavePrompt.Draw on black in FONTBIG. `StatsScreen::drawPrompt` now calls the same function, so the two prompts share one drawing (#20b's, unchanged).
  - `GameFlow::skipUnported` stops at `Screen::SavePrompt` (shown, package 0); `skipScreen` keeps its Esc arm (Event.Play), as `tests/scripts/flow_probe` closes it.
  - Linux (`menu_screens.cpp`, `main.cpp`): the prompt is a front screen after the Gunter stream (menu keys and typing, as the statistics' prompt), then LEVEL5; `--skip-save-prompt` takes the Esc arm before its first frame (a test aid, as `--skip-statistics`). 3DS (`menu_screens_3ds.cpp`, `main_3ds.cpp`): the same screen; D-pad, A (yes, then save under "6": no keyboard), B (no). A harness `MENU=...Menu.Back@t` closes it.
- **Why:** the owner's 2026-11-20 direction to port all gameplay from the C#. After LEVEL8 the game now offers the save the original offers before LEVEL5, instead of going straight on.
- **Evidence:**
  - `level_chain_test.sh`: arms 1-3 pass `--skip-save-prompt` (the chain's line is now `flow: skipped SavePrompt (--skip-save-prompt)`; its frame numbers are unchanged); new arm 4 runs `--level=8 --event=1 --press=Menu.Accept@8.5,Menu.Accept@9`: the prompt opens after the Gunter stream (health 63) offering "6" at 8.15 s, Yes, Enter, `Saved game 6: level 5`, `6.sav` is the C#'s light save (`"type":3,"level":5,"health":100`), and LEVEL5 (MUSE_1) loads.
  - `flow_tests`: LEVEL8's walk now stops at the SavePrompt with one screen skipped (the stream), LEVEL5 already entered, and the prompt's Esc is Play.
  - A 640x480 headless capture of the prompt (Save Game? / Yes / No, "Yes" grown) looks as the statistics' prompt does.
  - The goldens bite: with `skipUnported` skipping the prompt again, `flow_tests` fails ("LEVEL8 stops at the save prompt") and level_chain arm 4 fails (no prompt line, no 6.sav).
  - 3DS release build compiles; no emulator run here (the parent's).
- **Emulator check for the parent:** `make -C mdk_3ds AUTOQUIT=120 LEVEL=8 EVENT=1 FIXED=1` -- the level ends at 1 s, the Gunter stream plays (~31 s), then "flow: the save prompt before level 5 (offering "6")" and the prompt on black; with `MENU=Menu.Accept@1,Menu.Accept@2` (the menus' clock starts at the prompt) it logs "flow: the save prompt closes ... (saved game 6)" and LEVEL5 loads; `sdmc:/3ds/mdk/saves/6.sav` appears.
- **Commit:** this entry's commit.

### Leftovers (2026-11-20): the 3DS menu's slideshow and video off the main thread
- **What:** the owner's rule (no main-thread stalls) for the main menu's two mid-screen loads on the 3DS.
  - `game/async_read.h`: `AsyncRead`, a frontend's file reader lent to a screen (request by tag with optional work run on the reader's thread, poll by tag, cancel). `Slideshow::setReader` / `VideoPlayer::setReader` / `MainMenu::setReader`.
  - `Slideshow`: with a reader, start() asks for MDKS_001 only; each update takes at most one arrived slide and asks for the next, in Load's order with its stop; an image whose successor is not here stays up (what is resident shows). The decode runs on the reader's thread (`formats::Gif::decodePacked`, a packed block; `fromPacked` on the main thread is a copy). A read still out is cancelled with the slideshow.
  - `VideoPlayer::play`: with a reader, MDK12.FLC is asked for and started by the update it arrives in (the C#'s Play then Update); MDKOPT shows until then; retried while the reader is full; cancelled by stop() or the player's end. EndMovie's player has no reader (unchanged).
  - 3DS: `AsyncFileReader` gains `kChannelFile`, `requestFile` (with the work step), `pollTag`, `discard` (a read in progress is freed by the worker when it ends) and `running`; render.h's `mdk_render_has_file_reader` / `read_file_async` / `poll_file_read` / `cancel_file_read` (the desktop and the format tests' stubs answer false); `menu_screens_3ds.cpp` lends it to the main menu and logs the slide load's start and end.
  - Without a reader (Linux, the host goldens) both read at once, as the C#: `golden/movie_*.txt` and `stats_slideshow` are unchanged.
- **Why:** the owner's rule (no main-thread stalls; decode off-thread). The 3DS read and decoded the eight MDKS_*.GIF (~1.7 MiB decoded) in the one frame the video ended -- by the host measurement (1.4-2.1 ms a slide, ~14 ms all) ~0.7-1 s on an Old 3DS -- and read MDK12.FLC (632 KB) in the frame the splash ended.
- **Evidence:** `movie_tests`' checkLentReader (a fake reader, 3 polls of latency, its work run as the thread would): MDK12.FLC asked for, not read, nothing shown until it lands, then it plays; at the video's end no slide read or decoded; at most one slide a frame; the C#'s slides byte for byte in Load's order (against a Slideshow loaded at once); every decode done as the reader's work; all in before the first is due; on a slow card the video's last frame stays past 5 s and slide 1 shows the frame it arrives; reads still out are cancelled with the slideshow and the player. Bites: the slideshow loading at once fails 4 checks; showing an image whose successor is missing fails 1; the FLC read at once fails 4; the decode left on the main thread fails 4.
- **Found and not changed** (see OPEN_ITEMS): the front screens' `loadAssets` (MDKFONT.FTI, OPTIONS.BNI, STATS.BNI: ~2.7 MB) at each entry, and the statistics', fall's and end movie's loads at their screens' open -- transitions, not stalls inside a running screen; the statistics' one picture per page.
- **Emulator check for the parent:** `make -C mdk_3ds AUTOQUIT=60 SPLASH=0` (no LEVEL=): the menu over MDK12.FLC (~20 s), then the log's "menu: slides loading off the main thread at frame N (0 loaded)" and, a few frames later, "menu: slides loaded at frame M (8 loaded)"; the first slide shows 5 s after the video's end, without the items (CAPTURE= at ~frame (video end + 320)).
- **Commit:** this entry's commit.

### Leftovers (2026-11-20): harness shortcuts for the set pieces (GIVE=, RIDE=, AIM=)
- **What:** test aids that start Kurt at a set piece, for the parent's emulator playthroughs. `game/harness_aids.{h,cpp}`, shared by both frontends; the first level's only, as the C#'s test options.
  - GIVE=SW_BONES[,...] (Linux `--give`): the pickups collected at spawn (`Kurt::collect`, the items probe's `give:`).
  - RIDE=XE|XD2|XD|XSNOWB|X_STRIKE (Linux `--ride`): from 1 s on, Kurt mounted on the nearest live one of the model in his arena through the ported Rides code -- new `Rides::mount`, TryMount's dispatch for one object without the touch scan: a walker by RideWalker (the C#'s `--ride`, RideTest.Start), the board and a bomber once rideable. In DANT_5 an XE not rideable yet is called first, as the C#'s RideTest.StepBomber calls it (DANT_5 group 16 hit by the chain gun); it lands and is mounted ~9 s later. Gives up (logged) after 30 s.
  - AIM=yaw,pitch (Linux `--aim`): Kurt's yaw at spawn, and the scope's yaw and pitch the first step he snipes (entering the scope resets its pitch), so a shot does not depend on turns rounded through the trig table.
  - 3DS: `MDK_HW_GIVE/RIDE/AIM` (`mdk_3ds/Makefile`'s harness section, with the four set pieces' command lines), compiled only under AUTOQUIT; `build_cia.sh` refuses GIVE/RIDE/AIM in the environment and the ELF literal "main: harness aids". Linux: the strike scene's start and end are logged ("strike: the scene plays at frame N").
- **Why:** the owner's 2026-11-20 direction to port all gameplay from the C#: the parent checks the set pieces (the bomber, the XD2, the air strike, the bullet holes) in Mandarine, and reaching them by walking costs minutes and depends on LUT-rounded turns.
- **Evidence:** `tests/scripts/harness_aids_test.sh` (in `make -C tests/scripts test`): the XD2 mounted at 1.02 s and walked; the XE called, landed and mounted (XE_9000 in DANT_5); Bones given and the scope aimed at yaw 0 pitch 1.25 -- the shot plays the strike scene (without --give the same shot does not); the scope aimed at XGUNTAM exactly (`golden/L8_holes_guntam.txt`'s first aim). With `Rides::mount` refusing everything the XD2 and XE arms fail (3 checks). `linux_arg_parse_test.sh` refuses a ride that is no ride, an aim without a pitch or past the scope's 50 degrees, and an empty pickup name, and accepts the three together. A 3DS harness build with GIVE, RIDE and AIM compiles and carries the literal; the release ELF does not. Linux captures (scratch): the bomber's top-down view with the bomb count at 8 after two drops, the XD2 walking, X_STRIKB full screen, the scope on XGUNTAM.
- **3DS lines for the parent** (`make -C mdk_3ds clean` first; FIXED=1 so WALK frame f is step f):
  - the bomber: `make -C mdk_3ds AUTOQUIT=90 LEVEL=7 AT=100,2320,-60,90 ARENA=DANT_5 RIDE=XE FIXED=1 WALK='fire@800-802;turnright@820-840;fire@840-842;forward@850-865;fire@866-868'` -- "harness: RIDE=XE: none rideable yet; called ...", "mounted XE_9000 in DANT_5 ..., 9.88 s in", the top-down view, bombs from frame 800.
  - the XD2: `make -C mdk_3ds AUTOQUIT=60 LEVEL=7 AT=45,4868,-25,90 ARENA=DANT_9 RIDE=XD2 FIXED=1 WALK='forward@120-200;turnleft@200-230;forward@230-280'`.
  - the air strike: `make -C mdk_3ds AUTOQUIT=60 LEVEL=5 AT=250,121,-1622,0 GIVE=SW_BONES AIM=0,1.25 FIXED=1 WALK='sniper@20-22;fire@80-82'` -- X_STRIKB full screen from frame ~81.
  - the bullet holes: `make -C mdk_3ds AUTOQUIT=60 LEVEL=8 AT=20,3360,-360,0 ARENA=GUNT_10 AIM=123.80164,-26.158365 FIXED=1 WALK='sniper@150-152;fire@190-200'` -- BHOLE stamped on XGUNTAM's body.
- **Commit:** this entry's commit.

### Hardware session (2026-11-21): the PROFILE=1 build compiles again (Kurt's held draws in the arena table)
- **What:** the owner handed the Old 3DS appliance back for the end-of-project hardware session. Its first OG profile build (`make -C mdk_3ds AUTOQUIT=90 LEVEL=3 PROFILE=1`) did not compile: `main_3ds.cpp` read `KurtSprite::StreamStats::standIns`, which E12 (e8c49e4e) replaced with the hold (`holds`, `holdMisses`). The arena-entry table's `kSI` column now counts Kurt's held draws (`holds + holdMisses`: draws whose wanted frame was still being read); `HarnessArenaSample::kurtStandIns` is renamed `kurtHolds` and the table's legend says "Kurt held draws".
- **Why:** every OG profile of the session needs this build; and the gates build the release and harness variants but never `PROFILE=1`, so the merge's bit-rot went unseen for a day. (Open: a compile-only gate for the PROFILE/CAPTURE variants.)
- **Evidence:** `make -C mdk_3ds AUTOQUIT=90 LEVEL=3 PROFILE=1` builds; the profile runs that followed came back FRESH on old3ds-lan167 (CFGU model 0, 268 MHz, L2 off, stereo on). All five gates green.

### Level-by-level emulator playthrough (2026-11-21, Sonnet pass)
- **What:** walked levels 3, 4, 8, 5, 6 and 7 in Mandarine, arena by arena (start arena plus the arenas that matter, entered with `AT=`/`ARENA=` at points read from each level's DTI), the ledge grab, the chain's transitions (stream, statistics, save prompt, fall, level end, game end -> menu) and the menu slideshow. One bug found and fixed: `mdk_3ds/tools/stage_romfs.sh` never staged `MISC/FLIC/`, so the 3DS main menu had no MDK12.FLC video and the slideshow (which starts when the video ends) never ran; it now stages `misc/flic/mdk12.flc`. One scene stays inconclusive (the GUNT_10 bullet-hole shot).
- **Why:** the owner's direction to finish testing in the emulator before the end-of-project hardware session, now that every C# system is ported.
- **Evidence:** `docs/evidence/2026-11-21-mandarine-pass.md`, section "Level-by-level playthrough (Sonnet pass)" (a table per level with the exact commands). The ledge grab ends at the C# golden's feet (-1638.6,239.0,-653.3). The chain runs reach every screen between levels. The menu run after the fix shows the slides over the menu (`/tmp/kilo/mdk-play-menu3.png`). The game's chain order is the C#'s 7, 6, 3, 4, 8, 5.
- **Commit:** this entry's commit.

### Hardware session (2026-11-21): the baseline, E11/E14 on the console, and a MONO=1 harness switch
- **What:** step 1 is the Old-3DS baseline of levels 3/4/8/5/6/7, with a capture of level 3. Step 2 is E11 (baked light) and E14 (workers on core 1), each replayed onto the tip on a throwaway branch and profiled. New: `make -C mdk_3ds AUTOQUIT=90 MONO=1`, which draws one eye whatever the 3D slider says (`03_frame.inc` passes slider 0 to `pica::eyePlan`; the profile line reports `stereo=off(MONO=1, slider ignored)`). It is harness-only, like DRAWDEDUPE=0.
- **Why:** the owner's end-of-project hardware session (full test, profile, the parked A/Bs, then optimisation). Step 2 showed the frame's 13–16 ms `end` span is the per-eye command replay, CPU work that stereo doubles. The appliance's slider is up and nobody is there to move it, so the stereo cost needs a build switch.
- **Evidence:** `docs/evidence/2026-11-21-hardware-session.md` steps 1–2:
  - all runs FRESH;
  - baseline steady means 35.6/16.7/42.5/33.5/22.3/26.9 ms;
  - E11 looks the same on hardware (mean diff 0.31) but moves the frame by −1.4 to +0.9 ms, inside the run-to-run spread, so it stays parked;
  - E14 runs on core 1 (cores 2 and 3 refused on the Old 3DS) with no frame gain at idle.
  - A MONO=1 PROFILE build compiles; all five gates are green.
- **Commit:** this entry's commit.
