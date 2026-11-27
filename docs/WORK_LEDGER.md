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

### Hardware session (2026-11-21): hw_cycle.sh read a good capture as missing (SIGPIPE under pipefail)
- **What:** `hw_cycle.sh`'s capture check now uses `grep -qF ... <<< "$SESSION"` instead of `printf '%s' "$SESSION" | grep -qF ...`. `hw_cycle_selftest.sh` gains a case, `capbig`: the screenshot line early in a session larger than a pipe buffer. The selftest now runs 19 cases.
- **Why:** step 3's level 4 capture was written (`screenshot: wrote sdmc:/3ds/mdk/hwcap.bmp.log`) but read as CAPTURE MISSING.
  - grep -q exits at its first match. printf, still writing the 48 KB session, dies of SIGPIPE, and under `set -o pipefail` the `!` takes the failed pipeline as "no match".
  - It is the same trap the script already documents for the payload check (line ~77).
  - Step 3 also closed two playthrough items on hardware. The slideshow log lines print (the agent read a 25-line tail). GUNT_7 and DANT_7 are dark in the game itself: the Linux original and enhanced looks match the 3DS.
- **Evidence:**
  - The selftest is 19 PASS / 0 FAIL.
  - The non-vacuity run, with the parent script as HW_CYCLE_UNDER_TEST, fails `capbig` (expected rc 0, got 1). It also failed "a timed-out run kept nothing to read afterwards" once; that case passes on the fixed script and has nothing to do with this change, so it is a timing flake to watch.
  - Captures and the Linux comparisons are in `docs/evidence/2026-11-21-hardware-session.md` step 3.
  - All five gates are green.
- **Commit:** this entry's commit.

### Owner play test (2026-11-21): the scope zooms on X/B; hw_cycle pulls beside its --out
- **What:**
  - `defaultControlBindings` gives zoom_in=X and zoom_out=B.
  - The new `input3dsScopeButtons` runs in main after the poll, only while `player.sniping()`. There, forward/back read only buttons NOT also bound to a zoom key, so X zooms instead of tilting (the circle pad already tilts the scope).
  - `input_3ds_tests` covers the new defaults and the scope rule, including that controls.ini's own zoom buttons keep forward's tilt.
  - `hw_cycle.sh`'s pull file is `$OUT.pull` instead of the fixed `/tmp/mdk-hw-cycle.pull`.
- **Why:**
  - The owner's first play of the release build on their New 3DS: "when in sniper mode X and B should zoom in and out (as the analogue stick can move the camera around)".
  - The pull path: a selftest running at the same time as a console run made the console run read the fake console's log (hardware evidence step 4).
- **Evidence:** the input tests (the digital-map table now expects X and B to also press zoom in/out), the 19-case hw_cycle selftest, and all five gates green.
- **Owner's console:** their `sdmc:/3ds/mdk/controls.ini` was written by an older build and says `zoom_in=none zoom_out=none`, which overrides the new defaults. Edit those two lines to `X` and `B`, or delete the file so the next run writes the defaults.
- **Commit:** this entry's commit.

### PAUSE AND HANDOFF (2026-11-21): what is left
The owner paused the session (weekly usage) to hand the project on. State at this entry's commit: everything above is committed. Local `main` = debef918 (fast-forwarded today), no push.

**1. The arena-entry STUTTER: the top priority, owner-reported.** From the owner's first play test: "really really bad stuttering ... clearly loading textures in and out of RAM ... there should be a cache and thrashing should not be occurring." Not yet measured or fixed. What is known:
- **Three layers can stall or thrash on an arena entry:**
  - (a) `mdk_core/src/game/arena_stream.{h,cpp}`: ONE MTO arena's texture pixels are resident. A switch frees the old arena first, then streams 32 KiB a frame. A drawn arena whose stream isn't done is finished at once, blocking (counted as a stall).
  - (b) GPU-texture LRU eviction (`mdk_3ds/src/render/c3d/04_textures.inc` `evictTextures`). An evicted texture is remade from its source on the next draw.
  - (c) The HD replacements (04_textures.inc "THE HD STREAM"): `kHdBudgetBytes` = 6 MiB (`01_types.inc:122`), largest 341 KiB, so more than ~17 large replacements in view churn.
- **All budgets were sized for the Old 3DS (64 MiB).** The owner plays on a New 3DS (124 MiB for homebrew, main heap capped by the 96 MiB OS_HEAP_AREA). These are linear FCRAM budgets, not VRAM: both models have the same 6 MB VRAM, and the textures here live in linear memory. So the New 3DS can afford far larger, never-thrashing caches; the Old 3DS still needs bounded ones.
- **Owner's ideas:** a cache that never thrashes, async loading, show small mips while the 512x512s load. Owner rules: no main-thread stalls (async I/O, decode off-thread, time-budgeted uploads, show a resident fallback), memory over texture precision, performance over accuracy, both console models must work.
- **Method:** measure FIRST on the appliance with the arena-entry table. The level 7 route over two doorways (09c8ed80): `make -C mdk_3ds AUTOQUIT=90 LEVEL=7 FIXED=1 PROFILE=1 WALK='forward+turbo@60-260;turnright@270-307;forward+turbo@320-552;turnleft@562-599;forward+turbo@612-720;turnleft@730-770;forward+turbo@785-990;turnright@1000-1035;forward+turbo@1050-1300'`, then `hw_cycle.sh --profile`. Run it with HD on and with `TOGGLES=enableHdTextures=0`. Find the layer, then design: e.g. budgets from free memory at boot, keep the corridor pair's arenas resident across a doorway, prefetch the neighbour from the scripts' second-arena/opcode-223 signal, never block on an HD copy.
- **Partial work:** an agent started on this and was stopped by the pause. It left UNCOMMITTED, UNVERIFIED instrumentation edits in `.claude/worktrees/agent-aa9c0555e90e42ef9` (Makefile, main_3ds.cpp, harness_3ds.{h,cpp}, 01_types.inc). Read them or discard them; nothing depends on them.

**2. Ship the zoom fix to the owner.** Build a release `.3dsx` and upload it to the owner's New 3DS (ftpd-pro :5000; its DHCP address moves, it was 192.168.4.90 today; list the SD to confirm) at `sdmc:/3ds/mdk.3dsx`, and fix their controls.ini as above.

**3. Performance (hardware evidence steps 1–4).**
- Old-3DS steady frames at spawn, stereo on: L3 35.6, L4 16.7, L8 42.5, L5 33.5, L6 22.3, L7 26.9 ms.
- On level 8, stereo (−9 ms), lighting (−7.6) and sky shadows (−5) are the costs. With everything off it is still 2 vblanks, CPU-bound: sim ~6.5 ms and draw queueing ~7.4 ms. The `end` span is the per-eye command REPLAY (CPU), not GPU wait.
- **Next:** profile the replay and the draw queueing (the dedupe counters in the profile log); level 5 combat runs the script runtime at ~2.2 ms per step and 4–5 steps per frame. The owner decides which toggles stay on the Old 3DS by default, from step 4's table.
- **E11 (baked light)** `exp/hw-e11-rebased` 81832d85: it looks the same on hardware but saves no frame time. Parked.
- **E14 (workers on core 1)** `exp/hw-e14-rebased` 15a50cf6: no audio starvation (dspdropped 0, restarts 0) and no frame gain at idle. It meets the owner's merge condition; the owner decides.

**4. Still open from the port.**
- The bullet holes on level 8's GUNT_10 turret: inconclusive on every platform (the AIM shortcut's pitch is the suspect; compare a C# viewer frame).
- Owner decisions: noclip "down" button, the sniper round-camera insets on the 3DS, and the boss intros aiming at the origin (needs the original EXE in Ghidra).
- The ends of levels 6 and 7, and doors/lifts/pickups, were only covered by teleports in the emulator playthrough; play them on hardware.
- A compile-only gate for the harness variants (PROFILE, CAPTURE, MONO): `PROFILE=1` bit-rotted unseen for a day.
- The frontend refactor (main_3ds.cpp sprawl) waits until after the performance work.
- CIA packaging and testing come last.

**5. Worktrees and branches from today** (all local, under `.claude/worktrees/`): `hw-session` (detached at dca4a031, used by the console runs), `hw-e11`, `hw-e14`, `agent-a929628fc005c0d8f` (the playthrough, merged), `agent-aa9c0555e90e42ef9` (stopped stutter work). The console runs' raw logs are in the session scratchpad (`scratchpad/hw/`), which a reboot clears. The numbers that matter are copied into `docs/evidence/2026-11-21-hardware-session.md`.

### ARENA-ENTRY STUTTER — root cause and fix (2026-11-22)

**Root cause (measured, not inferred).** The ~1.7 s stutter on arena entry is
`mdk::enhancedLighting()` running on the arena-change frame in
`main_3ds.cpp`. It re-walks the sky panorama — 1804x360 = 649,440 pixels,
three `powf` each (~1.95 M powf) — every time the arena changes. The HD-on
profile (`/tmp/kilo/hw-l7-hdon2.log`) shows `light: arena CDANT_1` /
`light: arena DANT_2` landing immediately before `STALL 1736 ms at frame 224`
and the frame-1137 stall; the arena walks (544–1266 triangles) are negligible
by probe. A host probe put the rebuild at 14 ms on x86 — the powf is what does
not scale to the ARM11.

**Fix (`a98e7c4b`).** `level_light.cpp` now builds the 256 linear palette
colours once per palette (keyed on the palette's own bytes) and the panorama
loop indexes that table directly. Bit-identical by construction (same powf,
same inputs, memoized): the probe's rig bits match to the bit, the
`EnhancedLightingMatchesTheOracle` test passes, and the pinned L3_f1 capture
is byte-identical to the reference golden (`cpp=58615fc8`).

**Also this batch.** `fde6f026` UNLOCK= harness aid (frees locked doors on a
schedule; the corridor route needs it); `42ee7a22` the make/import tick spans
become profile-build-only (review finding: an SVC per texture make does not
belong in the shipped path). Independent adversarial review (deepseek vs qwen
builder) raised the per-pixel-memcmp defect that the first cut had — the
validation is hoisted out of the pixel loop per its finding.

**Pending.** Hardware verification: the profile payload (LEVEL=7 FIXED=1
PROFILE=1 AT=0,380,15,90 UNLOCK=200 HDROMFS=1, 131 MB, chainload verified) is
built and staged in the worktree; the console is with the forsaken agent by
owner instruction 2026-11-22. When it frees: `hw_cycle.sh --profile` and
confirm no `STALL` at frames 223/1137 and the change-frame draw is tens of ms.
The HD-off comparison run is still owed too.

### ORIG PACK UNCOMPRESSED (2026-11-24): the packed-original import was the LZ decode

**What.** `arena_etc1_convert.py` encodes the orig pack with `tex3ds -z none`
(SETTINGS gains `z=none`, so an existing LZ pack is re-encoded, not kept).
PROFILE builds time each packed-original read (`packread`) beside the existing
`texmake` import time.

**Why.** The mid-tunnel 300-400 ms frames were several packed originals made
synchronously on one frame. Measured A/B on the Old 3DS (same route, same ETC
blocks byte for byte): import 690 ms (LZ) vs 37 ms (none) for 39 makes; p99
61.9 -> 55.5 ms; frames > 33.3 ms 157 -> 133. RomFS +16 MiB (.3dsx 161.7 MB).
Evidence: `docs/evidence/2026-11-24-orig-pack-uncompressed.txt`.

**Still open.** The RomFS read (11-27 ms a make) is still on the main thread:
the next increment moves it to the AsyncFileReader (request at door intent,
publish at the frame boundary under a budget, completion after the frustum
cull only as a last resort).

**Known pre-existing defect.** `make -C tests/scripts test` fails on the
committed HEAD `f1083ef3` as well: `c3d_frame` adjudicates only 148 of 157
mutation arms (nine index-expansion / 16-bit-store arms whose mutations no
longer apply or are no longer caught). Not caused by this batch; not yet fixed.

### PACKED ORIGINALS OFF THE FRAME (2026-11-24): async reads, requests ahead, object warm; menu video

**What.** Packed originals are request-then-publish on the AsyncFileReader
(new orig channel, rank-ordered worker, a slot kept for Kurt/menus); imports
at the frame boundary under a budget; a draw-time make only after the frustum
cull, as a last resort. Requests ahead: script-hidden arena batches, a re-ask
of evicted ones for the drawn pair, and an object warm over every model the
level can spawn. Hints never evict and answer from the name index only. The
menu's streamed pictures no longer finish a discarded palette expansion.

**Why.** After the uncompressed pack, every in-play hitch frame was a
synchronous RomFS read (11-27 ms each) on the frame a texture was first
drawn; the profile's paths named them (arena surfaces, then objects).

**Evidence.** `docs/evidence/2026-11-24-async-packed-originals.txt`: Old 3DS
L7 walk, in-play texture reads 30 -> 0 after the level-start frame; frames
549 (148 ms) and 771 (87 ms) no longer hitch; p99 55.5 -> 52.8-53.9. Menu
first frame 554.8 -> 269.0 ms, reopen 392.5 -> 102.1 ms. Independent Opus
review, three rounds, final COMPLETE.

**Still open.** Level-start frame (392 ms: the starting room's reads; finish
them on the load screen); music reads in the simulation (up to 75 ms);
menu background/font atlases' whole-image expansion steps; with an HD pack
installed the object warm answers nothing (name index is skipped).

### EXPANSION FILTER + MIP CHAIN SLICED (2026-11-24): no whole-image step

**What.** The chunked expansion's stage 1 (pica::prepareForFiltering) and
stage 2 (pica::buildMipChain) advance a row of tiles at a time across pump
steps (FilterCursor, kChunkFilterTexels = 16384); the whole-image functions
are the step functions with an unlimited budget (one implementation).

**Why.** Non-packed large textures (sky 1024x360/780x360, font atlases,
menu background, the 64x704 corridor strip) ran each stage over the whole
padded image in one step: 74-211 ms frames after level start and in the menu.

**Evidence.** Bit-identical: a new PicaMatrix test compares the sliced walk
at budgets 1/300/16384/100000 against the whole-image result and an
independent per-texel oracle over 13 sizes x 3 patterns (non-vacuous: three
deliberate breaks each turned it red); HEAD vs new over 33 cases hash-equal.
Old 3DS L7 walk (.git/hw-logs/2026-11-24-slice1.log): frame 17 211 -> <50 ms,
worst of frames 2-30 211 -> 68 ms; p99 52.9. Gates green; c3d_frame 148/157
and main_3ds_flow unchanged from HEAD (pre-existing).

**Still open.** 50-68 ms `begin` frames 18-29 (three 16384-texel steps a
frame; the mip steps cost more per texel than the row steps) -- tunable.

### MUSIC PREFETCH OFF THE FRAME (2026-11-24): reader-thread reads, sliced decode

**What.** SoundBank's prefetch reads go through the AsyncFileReader (new
kChannelMusic, one original-sized step per request; kSlots 5, the streams
test only their own HD+orig slots against kStreamSlots); the decode is sliced
on the main thread (kDecodeStepSamples 32768/frame) straight from the read
bytes (Wav::locate, pcmToSamples). A track wanted before it lands still falls
back to a counted read+decode. The 60-frame release/re-prefetch runs in every
build (it was under log3dsPeriodic()) and keeps the bubble's track.

**Why.** Each prefetch step read on the main thread (11-17 ms a frame through
the corridor crossing) and the last step decoded the whole track (74.9 ms at
frame 799).

**Evidence.** Old 3DS L7 walk (.git/hw-logs/2026-11-24-music1.log, music2.log):
the crossing's music span 11-17 ms + 74.9 ms -> 0.1 ms, worst main-thread step
1.7 ms; TRACK9/TRACK11 arrive in 8 reader reads + 16 slices; p99 52.9 ->
49.9 ms. Host: audio_stream tests (every level 3-8 track sliced == whole;
reader path non-vacuous). Independent Opus review COMPLETE; M1 (stream slot
math), M2 (bubble track kept), L1 (no heapProbe on track-in) fixed.

**Still open.** Crossing frames 59-77 ms: the sliced door build (`sync`
15-20 ms) and the effects warm (17-19 ms); frames 783 (end span 80 ms) and
796 (116 ms, spans small) are not yet attributed.

### MEASURED AND REVERTED (2026-11-24): chunk threshold 32768 -> 4096

Frame 650 of the L7 walk (129 ms, fx 82 ms) is the first explosion's effect
sprites made whole in the effects draw: `texmake 32x960 expand 42.7 ms`, `16x480
expand 11.0 ms` -- the 32x960 strip (30720 texels) is under both the backend's
kChunkMinTexels and the effects warm's 32768 cut. Lowering both to 4096 removed
frame 650's spike but made the corridor crossing worse (frames 793-799 59-77 ->
85-114 ms; >33.3 ms 161 -> 182; p99 49.9 -> 52.2; texture-make total 990 ->
1415 ms): far more textures went through the sliced pump. Reverted
(.git/hw-logs/2026-11-24-fx1.log). Also measured: skipping the content hash for
>1024 textures did not move frame 650 (the hash was not its cost) -- reverted.
Next candidates: effect sprites in the orig pack (the lookup is Lit-only today),
or a warm-only path for mid-size sprites.

Also attributed: the 80-121 ms `end` spans at frames 764/784/804/814 and the
86-93 ms frames at 299/599 are the PROFILE build's census and 300-frame
loggers (they sit on `census`/`drawcounts`/`meshupd` lines), not the game.

### FIRST EXPLOSION (2026-11-24): effect sprites take their packed originals

**What.** A Sprite surface asks the same replacement lookup a Lit draw would
and keeps only a packed-original answer (HD/decoded replacements stay
Lit-only, as the C# MaterialResolver.Replace rule; a Sprite miss marks
nothing). The effects warm asks the pack for every warmed sprite as a hint
(shared requestPackedSurface with the object warm). With no HD pack, the
lookup skips the content hash for a texture over 1024 on either axis -- the
orig pack never holds one, and hashing EXPLODE's 128x3328 strip is ~100 ms.

**Why.** Frame 650 of the L7 walk (the first explosion): `texmake 32x960
expand 42.7 ms` + `16x480 expand 11.0 ms` (SL_MED/SL_SMA, both in the pack)
in the effects draw. Letting Sprites ask the pack first moved the cost into
the objects draw (obj 105.5 ms: the hash of EXPLODE's strip), which the
>1024 rule removes.

**Evidence.** Old 3DS L7 walk (.git/hw-logs/2026-11-24-sprite1.log,
sprite2.log): frame 650 129.3 ms -> no longer a spike; p99 49.9 -> 46.6 ms;
>33.3 ms 161 -> 136; steady 18.43 -> 18.33 ms; texture-make total 990 ->
814 ms. Host: two surface-cache tests and EffectsWarmAsksEverySprite (five
mutations each turned a test red). Gates green; pinned frames unchanged.

### CORRIDOR CROSSING (2026-11-24): the door build's depth step and the effects warm's glow

**Attributed first.** The crossing's 50-68 ms frames (793-799 of the L7 walk)
were the sliced build of the bubble's DANT_2 view (`viewMs` 18-22 ms every
frame) plus the effects warm. A host probe of ArenaSceneBuilder's steps (every
step type ~0.15 ms on x86, 91% of DANT_2's build in DepthLayersBuilder) and
callgrind (a third of it in malloc/free) named the depth layers' overlap test:
four std::vector allocations and a software-divide `%` per corner, per pair of
triangles. A new PROFILE-only `flowrow:` table (rows kept in memory, said at
exit: no SD line on the frames being measured) splits the warm by role and
showed the effects warm's cost was the sprite's glow measurement (ask and start
both ~0 ms): SL_BIG 64x1920 11.3 ms, TRAIL 4.9 ms.

**Kept (measured on the Old 3DS, model 0, 268 MHz, L2 off, stereo; logs in
.git/hw-logs/2026-11-24-cross{A,B,D}.log; baseline sprite2):**

- A. overlap()/clipBy() in fixed storage (two 24-corner buffers, ping-pong; the
  bound is the loop's: a clip at most doubles, three clips) and the wrap as a
  compare, not `%`. Bit-identical: depth layers and full scenes of every arena
  of levels 3-8 hash-equal before/after (host). Sync per step 18-22 -> 3.9-6.8 ms;
  frames 793-798 sum 414 -> 282 ms; p99 46.6 -> 44.3.
- B. Step budgets: 1200 -> 600 triangles a loop step; a collected depth triangle
  counts two units of the 600 (measured ~twice an overlap test). Every sync step
  now 3.3-5.9 ms (the mesh step 8.4); DANT_2 14 steps (was 11), DANT_5 41 (was
  37); the sync guarantee still fired once (level start, as before). p99 42.7;
  steady 18.25; 3+-vblank frames 54 -> 49.
- C+D. effectsGlowOf: a 256-entry linear table (the same powf of the same input,
  once per run, not 768 per sprite) and a texel histogram (four interleaved, so
  a run of one index does not serialise on one counter) instead of per-texel
  float sums. Glow share identical and colour within 7.9e-5 of the old sums over
  all 7692 (level texture, arena palette) pairs of levels 3-8. SL_BIG's warm step
  11.3 -> 8.2 (C) -> 4.1 ms (D); frame 1's effects warm 11.3 -> under the 8 ms row
  threshold.

**Result (crossD vs sprite2):** crossing frames 793-806 without an SD log line
now 30-34 ms (were 50-68); 792 65.4 -> 47.3 (a PROFILE `texmake` line in it);
the 796/797 50-66 ms frames carry the existing PROFILE `objwarm` line (the
X_STRIKB parse itself is 5-6 ms). p99 46.6 -> 43.0; steady 18.33 -> 18.23;
3+-vblank frames 55 -> 47; frame 1 263.4 -> 244.7; door activation 771 52.8 ->
43.6; texture-make total 814 -> 822 ms (unchanged work). The >33.3 count
(136 -> 139) is dominated by frames at 33.3-33.9 ms (two vblanks plus jitter;
sprite1/sprite2 on identical code read 154/136), so it is not the comparison.

**Measured, not needed:** the warm's makes (`start`, startExpansion's
malloc+zero-fill) and its pack asks were ~0 ms in every row, so neither the
zero-fill nor a per-frame warm cap was changed.

**Still open:** the activation frame 791 (draw 44 ms, the second arena's first
draw); the mesh step (8.4 ms at frame 801); the object warm's per-model parse
of a CMI model (X_STRIKB 5-6 ms).

### SCRIPT TICKS ATTRIBUTED (2026-11-24): the slow steps were XG's animation bakes

**What.** A PROFILE-only tick probe in ScriptRuntime (script_runtime.h
`probeClock`/`probeRows`): with a clock set, each 1/30 s tick is split into its
parts (remember, head, triggers, Kurt's side, systems, objects, controllers),
the slowest object is named with its script and motion halves, its opcode count
and its slowest opcode (ScriptVm times each opcode), and spawns are counted with
the slowest one's model lookup. The slowest 40 ticks are kept in fixed storage
and main_3ds.cpp says them as `scripttick:` lines at the level's teardown (no SD
line in the measured frames). Null clock (every other build, every test) costs
one pointer test a tick, an object and a spawn. The Linux frontend reproduced
the same five slow ticks headless (`--fixed-step --level=7 --walk=... --unlock=...`,
x86 ratio ~130-190x); that host wiring was a throwaway and is not in the tree.

**Measured (Old 3DS model 0, 268 MHz, L2 off, stereo; L7 FIXED walk;
.git/hw-logs/2026-11-24-script1.log):** frames 493/621/665/691 were XG's
OBJECT MOTION (13.5/18.6/12.6/56.5 ms of 15.8/21.4/17.2/60.6 ms ticks): the
footing sweep's poseBounds -> pose_parts() baking a whole CMI animation at its
first use (CMI_bf029 22 frames, CMI_b3351 33, CMI_b0a19 18, CMI_e028d 101; 23
parts, 135 vertices). The bake's cost was ~2300 frame-part vectors made, copied
twice and freed for the 101-frame one, not the arithmetic. Frame 649 is three
XF explosions (XF's set_health 0 -> kill -> explode 6.5 ms, the first one
parsing XFD's debris model); frame 433 is the XG spawn's model parse (4.7 ms of a
6.1 ms spawn opcode). Probe overhead: none measurable (script3 p99 39.6 with it,
script4 39.9 without, same code).

### POSE BAKE ONE FRAME AT A TIME (2026-11-24): kept

**What.** ModelAnimation's tracks decode into one buffer per part
(`DecodedTrack`) and resume where they stopped (`beginTracks`/`decodeTo`/
`bakeFrame`); bake() is every bakeFrame in order. scripts/pose_cache.cpp keeps
per (model, animation) the tracks and a frameCount-sized frame table, and makes
a frame (decoding only as far as it) the first time pose_parts() asks for it;
the tracks are dropped once every frame is built. The C# bakes the whole
animation at first use; the frames are the same bytes.

**Attempts (hardware A/B against script1, same probe build):**
- v1, whole decode into flat buffers at first use, frames made on demand
  (script2): 691 60.6 -> 12.1 ms (motion 56.5 -> 8.0, the decode), 493 15.8 ->
  4.5, 621 21.4 -> 5.8, 665 17.2 -> 6.3; p99 46.5 -> 41.8; >33.3 147 -> 139.
  Superseded by v2 (the remaining 8 ms was decoding 101 frames nobody needed).
- v2, the decode resumes per frame (script3): 691 4.7 ms (motion 0.77), 493
  3.25, 621 3.7, 665 5.3; frame-692 spike (91.4 ms) gone; p99 46.5 -> 39.6;
  >33.3 147 -> 135; mean 18.645 -> 18.570.

**Result without the probe (script4 vs sim2, both no-probe builds):** p99 42.7
-> 39.9; >33.3 145 -> 135; mean 18.611 -> 18.559; scriptstep >10 ms lines
493/621/649/665/691 -> 649 only; the 107 ms frame 692 gone. (script1-4 also
carry the coordinator's mdk_render_mark_canvas change; sim2 does not. The clean
A/B is script1 -> script3.)

**Parity.** Old whole bake vs new bake, every arena animation x every arena
model of levels 3-8: 9128 pairs, 470,531 frames, 0 differing (a scratch build of
the pre-change model_animation.cpp beside the new one; three mutations of the new
decoder turned it red: 294 / 721 / 125 pairs). New test
PoseCacheBuildsEachFrameAsTheWholeBake (tests/formats): 721 animated pairs asked
0,1,3,2,last then last..0 against bake(), reference stability, clamps; red under
a shifted-frame mutation (33969 failures), an early track drop (36906) and a
clamp change (683). StreamedArenasBakeExactlyAsTheWholeFile still 721 pairs.
tests/scripts: vm_tests "All VM unit tests match the oracle", tick1 "All cases
match the oracle", level_chain/harness_aids/stream_screen/fall/menu PASS;
c3d_frame 148/157 and main_3ds_flow's 50 FAIL lines identical to the baseline.

**Still open (measured, not changed):** frame 649's explosions (12.4 ms: three
explode() calls, the first parsing XFD); first-spawn model parses (XG 4.7 ms at
433, SW_KEY/SW_CHUTE/SW_H25 0.5-0.7 ms) -- prewarming them would keep every CMI
model resident, which object_view.cpp's warm deliberately does not; findModel
copies the parsed Model into its cache (~10% of a parse on the host) -- a move
would be too small to measure.

### MENU CANVAS PICTURES MADE ONCE (2026-11-24): the splash-to-menu double expansion

**What.** New seam `mdk_render_mark_canvas(texture)`: FontView's atlas and
loadPalettedImage's screen pictures say at creation that they are canvas
textures, so the 3DS backend pins them RGBA8 before their expansion picks a
store. PROFILE builds log each large index texture's creator
(`texcreate WxH frame N caller 0x..`, named with addr2line on mdk.elf).

**Why.** The menu's big font atlas (1024x256) and MDKOPT background (600x360)
were expanded at startup into the 16-bit store, then re-expanded RGBA8 by the
first canvas draw's pin (06_draw.inc) -- on the splash-to-menu frames
(front 556-557, 85/97 ms). Callers named by addr2line: FontView::load,
loadPalettedImage, movie_view's makeImage.

**Evidence.** Old 3DS no-LEVEL menu run (.git/hw-logs/2026-11-24-menu4.log):
the transition no longer re-expands either picture (only the video's
600x360 starts there); the startup expansions finish sooner (frames 25/33/46
vs 35/67/75: no 16-bit pack stage); menu p99 44.8 -> 33.5 ms. Still open: one
96 ms frame at the transition (front 569) -- the video picture's create still
starts a chunked expansion (a ~2.8 MB malloc and zero-fill) that
update_paletted drops a call later, plus the first video frame.

### MENU VIDEO PICTURE CREATED STREAMED (2026-11-24); deferral experiment reverted

**What.** New seam `mdk_render_create_streamed_texture`: movie_view's
makeImage (the menu video, the splash's two pictures) creates its picture
already streamed, so the 3DS backend never starts the chunked expansion that
update_paletted then dropped (a ~2.8 MB RGBA8 chain allocated and zeroed).
Vulkan and the test stubs forward to create_index_texture.

**Evidence.** Old 3DS no-LEVEL menu (.git/hw-logs/2026-11-24-menu6.log): the
splash-to-menu frame 96.1 -> 78.4 ms; only the atlases and MDKOPT expand.
Tried first and REVERTED (menu5): deferring every large creation's start to
the next frame boundary -- transition 79.0 ms but front frame 1 275 -> 332 ms
(the startup pictures' starts moved out of load into the first frame).

### TWO HALF-DEAD SOURCE GATES REPAIRED (2026-11-24): c3d_frame and main_3ds_flow

**What.** Tests only (`tests/scripts/c3d_frame_test.sh`,
`tests/scripts/main_3ds_flow_test.sh`); no product source changed.

*c3d_frame* failed with "only 148 of 157 arms adjudicated red". Already red
before 2026-11-24: the 2026-11-21 and 2026-11-24 renderer rewrites (the
expansion split into `expandIndexRows`, the chunked expansion and its own
store chooser, the `if (!prepacked)` pack, `mdk_render_mark_canvas`) moved the
text the arms searched for.
- Re-anchored to today's text, each still the defect its review named:
  index straight-through (review-6 F1), RGBA row memcpy (review-6 F1),
  index rows-not-tiles (batch 173), index red/blue swap (review-7 F2), and
  the 16-bit pack after the level-0 upload.
- The two padding-fill arms (review-12 F1) needed a fix to the check as well
  as to the arm. `expandIndices` writes no texel itself now: it zeroes the
  buffer and calls `expandIndexRows`. The bypass scan counted only the helper
  call as a texel write, so a wipe after that call, or inside a loop of row
  calls, was ACCEPTED. Measured: with the arms re-anchored and the check
  unchanged, both arms were green. Two check fixes:
  - a call of a function in the file that fills texels through the helper
    (`helper_writers`) now counts as a texel write;
  - the scan that follows an expansion's delegated callee was a consuming
    regex, so it never followed the callee in `if (expandIndexRows(...))`. It
    is now a lookbehind.
- "The store ignores the texture16 toggle" and "...the canvas pin" were
  accepted because they hit the first occurrence of the chooser's argument
  line. That is now the chunked expansion's chooser
  (`advanceExpansionSteps` stage 3), and the check never read it. The fixes:
  - both arms are pointed back at `makeTextureNow`'s chooser;
  - the check now also reads the chunked chooser: toggle, `keepRgba8`, the
    buffer it then packs, and the pack store being `texture.expandFormat`;
  - four arms put those defects into the chunked path.
- The canvas check also reads `mdk_render_mark_canvas` now (it must write
  `keepRgba8 = true` and `stale = true`). Two new arms drop each write.

*main_3ds_flow* printed about 50 FAILs. Two were real check failures. The
other ~48 were cascades: every arm reported the first already-red check
instead of the one it is cut for.
- `arenas-follow-the-scripts`: its `stream.update` half was **retired**.
  ArenaStream was removed by design in 4fea3380 (2026-11-22, the owner's
  redesign). The check now pins frameDrawn -> `arenaViews.sync(...,
  *frameDrawn, bubbleHints)` -> begin_frame -> `arenaViews.draw(*frameDrawn)`.
  The arm "views synced before the stream is stepped" is **retired** for the
  same reason and replaced by two arms that compile: the sync moved inside the
  frame, and the sync on a second list.
- `music-follows-kurts-arena`: a **stale anchor**. "Prefetch after
  stream.update" became "after arenaViews.sync". The keep list grew a fourth
  entry, the bubble's track (review M2), so `keep[3]`/`releaseMusicExcept(keep,
  3)` became "keep[N] and the release is handed the same N". The arm "the
  loading arena's track is released under Kurt" is re-anchored.
- No real product defect was found in either suite.

*A stale c3d probe was also re-anchored.* "A brace inside a comment does not
truncate the resolved body" (review-8 F3) had stopped changing anything.
Nobody saw it, because the gate exits on its arm count before it runs the
probes. Its comment now sits right before the first texel slot, in
`expandIndexRows`.

**Why.** `make -C tests/scripts test` stopped at c3d_frame, so every suite
after it went unrun. A gate that prints FAIL on correct code teaches people to
skip it. Non-vacuity (each arm turns its own named check red) is what the
gates exist for.

**Evidence.**
- c3d_frame: PASS, 163 arms red, probes green.
- main_3ds_flow: PASS, 22 checks, 64 arms, 24 probes.
- Each repaired or new arm was run on its own and turns its named check red.
- With the writer-call rule switched off, both padding arms are accepted, so
  the rule is load-bearing.
- Every changed or new mutant (15 c3d, 3 flow) and both parents pass a
  `-fsyntax-only` compile with the 3DS flags (devkitARM).
- `make -C tests/scripts test` gets past c3d_frame and main_3ds_flow. It
  then stopped at LinuxBinaryFresh, because `build-linux/mdk` predated
  0ee82043's render.h change. After `cmake --build build-linux`, every later
  suite passed when run on its own, and tick1 passed 518 cases with 0 fails.
- `make -C tests/formats -j4 test`: 200575 checks, 0 failures.
- `bash tools/check.sh`: ALL GATES PASSED.

**Commit:** none. The task said not to commit.

### HD TEXTURES: NO SD FOR A COVERED SURFACE, AND NO POP-IN (2026-11-24/25)
**Owner's words.** "the point was to fix the hitching with the HD texture set";
"these are fucking ETC1 and ETC1A4 and we should NOT be loading the SD textures
AT ALL when HD is turned on"; "yeah I dont want surface pop in"; then (asleep)
"work autonomously using whatever's fable's suggestions come back as".
**What (branch fix/3ds-hd-only, one commit per change):**
- a5c1109f probe: `hdrule:` line per level (hashes, HD answers made from SD,
  SD copies of covered textures, late/blank HD draws).
- c99b8e69 the HD rule: the orig pack's byname.txt rows carry each texture's
  content key in their file names; the HD pack's files carry the same keys, so
  (palette, name) -> HD file is joined by string at level load -- no hash on
  the console, no pack rebuild. A covered pair is made from its two files
  (mdk_render_create_compressed_file's hdPath, C3dTexture::hdOnly): HD on
  reads only the HD file, HD off only the packed original. Sprites take HD too.
- 5604dd1e the reader thread decompresses the HD .t3x (LZ11) into the
  uncompressed layout; the boundary import is a copy (was 1-94 ms each).
- 929ac555 / 786a7783 probes (lateness, margin) and `HDFLIP=` harness aid.
- 5de2dae5 budget follows free linear (superseded by the set, kept).
- aa6ecd04 first view settled on the loading screen (superseded by the set).
- 877c9057 SurfaceCache::createPacked no longer overwrites (leaks) a pair's
  existing texture.
- b767bb75 THE LEVEL'S RESIDENT SET (the Fable review's design): every HD file
  the level's rows name, sized exactly, tier 512 / 256 (levels over 256 dropped
  by the reader's rewrite) by arithmetic against free linear less a 7 MiB play
  reserve; pinned; the loading screen holds (applet loop served) until it is
  resident; the same hold when the toggle comes back on.
- 6df40918 per-file caps (tier 256/128 when 256 does not fit: level 8),
  the old level's copies freed before the next set is chosen, harness gate line.
- f8fb72d4 bullet-hole stamps skipped on HD-covered surfaces while HD is on
  (VISIBLE CHANGE, counted `stampsSkipped`, owner may revisit).
- ed7c09fa no name index -> HD refused loudly (never hashed).
- b79e0b17 tools/hd_name_coverage.py: HD keys no name row reaches.
- 000b9522 toggling HD back on gives the HD-only textures' originals back
  once the set is resident (linear before the next toggle 1.86 -> 3.02 MB) and
  holds for the HUD's copies too; the hold ends after 1 s without progress.
- 35095fcf probe `linkedAfterSd` (HUD pictures made SD at load, then linked).
- 2546e97d with a set resident, linked copies (HUD, Kurt's non-pack frames)
  are not starved by the per-surface budget gate (toggle-on HUD lateness
  2-5 frames -> 0).
**Evidence (Old 3DS old3ds-lan167, model 0, 268 MHz, L2 off, stereo; PROFILE;
romfs HD pack via HDROMFS=1; logs in this worktree's git dir hw-logs/):**

| L7 walk (DANT_1->CDANT_1) | p99 ms | >33.3 | steady mean | late | blank | covered SD | hashes | lin min |
|---|---|---|---|---|---|---|---|---|
| base ea8cd744 | 60.7 | 212 | 19.11 | -- | -- | -- | -- | 29 KB |
| probe a5c1109f | 55.9 | 193 | 19.13 | 31 | 0 | 20 (+38 answered SD) | 46 (1081 ms) | 29 KB |
| HD rule c99b8e69 | 49.9 | 155 | 18.42 | 19 | 279 | 0 | 0 | 2.5 MB |
| + unpack 5604dd1e | 42.8 | 144 | 18.36 | 19 | 436 | 0 | 0 | 2.5 MB |
| + settle aa6ecd04 | 38.6 | 138 | 18.28 | 2 | 0 | 0 | 0 | 2.7 MB |
| + set b767bb75 | 38.6 | 134 | 18.19 | 0 | 0 | 0 | 0 | 2.0 MB |
| tip 2546e97d | 39.7 | 138 | 18.29 | 0 | 0 | 0 | 0 | 2.0 MB |

Tip: `hdrule: gate PASS`, hold 6.1 s for 143 files (4.18 MB at cap 256), no
HD read in play, linkedAfterSd 12 (the HUD's pictures, made SD at load, never
drawn SD). Live toggle (HDFLIP=1500,2100,2700,3300 on the tip): no frame over
50 ms at the off flips; each on flip holds 5.95-5.98 s (157 files) and then
late 0, blank 0, gate PASS; no leak (linear free identical at each off flip,
2.09 MB).

Worst in-play frame: 596 ms (frame 807: the second arena's surfaces hashed in
the view sync) -> gone; the remaining 90-120 ms frames at 764/784/804/814/844
and every x99 are the PROFILE build's census/logger lines (`end` span).
Set runs: L7 tier 256, 143 files 4.18 MB, hold 6.2 s; L8 tier 256/128, 281
files 5.16 MB (103 at 128), hold 13.0 s, gate PASS, p99 52.0 (53.1 streamed),
lin min 1.30 MB; L3 tier 256, 163 files 4.77 MB, hold 5.8 s, gate PASS, p99
59.2, lin min 2.72 MB. HD imports in play: 0 on all three.
**Where SD still loads with HD on (and why):** animated (stacked) textures --
the pack resamples their strips, squeezing frames (0 met on these routes);
name-index misses, expanded never hashed (L7: SL_BIG and the >1024 strips,
which have no orig/HD file either; the coverage tool lists the 10 HD files no
row reaches: EXPLODE, M_COMM x2, O3_BLAST/CORR/FLR1, XCARPIL/XCARTURR/XCA_BELL/
XPIT); the HUD/canvas pictures (~19 a level: made and content-hashed at level
load, cold, never drawn SD because their HD copies settle in the hold); Kurt
(kurt.pak's original frames, owner 2026-11-19); Original-shaded sky/glass (the
PC never replaces them).
**Player build** (no harness, reads sdmc:/3ds/mdk/hd): this worktree's git
dir `hw-logs/play-2546e97d.3dsx` (md5 1c1bed06066e7b9df7318fcd032e832a).
**Suites at the tip (2546e97d):** `cmake --build build-linux`; `tools/check.sh` ALL
GATES PASSED; `make -C tests/formats -j4 test` 200610 checks, 0 failures;
`make -C tests/scripts test` exit 0 (C3dFrame 64 checks, 167 arms; Main3dsFlow
22 checks, 64 arms).
**Unverified:** the owner's SD-card pack (all runs read romfs:/hd; the
appliance's card has no pack and the loop cannot write it) -- the hold time
grows with SD latency, play does not read HD at all; the New 3DS (more linear
heap would raise the tier; heap split not moved); levels 4-6; the toggle hold
in the real pause menu (HDFLIP drives the same code path).
**Not done from the review:** the HUD name-to-key table (needs a host
exporter for the BNI sprites per level palette); extending the exporter to
model archives (the 10 misses); removing the per-surface streaming code (it is
the fallback when a set does not fit and serves the legacy HUD/Kurt copies).

## 2026-11-24 -- cleanup of main, step 1: prune evidence, frames and progress logs

- **What:** removed tracked `.progress-log/`, `docs/frames/` and `docs/evidence/`
  (tracked files 5,360 -> 1,551), keeping the seven evidence files the completion
  ledger cites. Removed `mdk_3ds/tools/hud_panel_compare.py`, whose only inputs
  were pruned PNGs. Retired the one-frame-per-commit rule in
  `gates_batch_integrity.py`.
- **Why:** owner direction, 2026-11-24: an immaculate main; the harness and its
  history live on branch `harness/3ds-profiling` (ea8cd744).
- **Evidence:** `tools/check.sh` ALL GATES PASSED; `make -C tests/scripts test`
  exit 0; gate self-test PASS.

## 2026-11-25 -- the immaculate main on the HD-only tree (branch cleanup/immaculate-hd)

- **Owner's words:** "I want master to look absolutely immaculate without a bunch
  of ifdefs"; the harness to its own branch; split the ~4,500-line main_3ds.cpp;
  "make sure that you dont disturb the work that we have already done" (a new
  branch, from fix/3ds-hd-only 76826339, the owner-tested build plus the HD set).
- **What (one commit per step):**
  - 42590c8e the evidence/frames/progress-log prune (8357fcc6, cherry-picked).
  - 4b5fee8c every harness `#if` in mdk_3ds/src resolved as a release build
    sees it (4916462f's script on the new tree): 136 conditionals, 2,025 lines;
    the release ELF and every object byte-identical to 76826339's.
  - 930508e1 the harness leaves: harness_3ds.{h,cpp} and its calls, the
    Makefile's harness section, HDROMFS staging, hw_cycle.sh + self-test,
    compare_linux.sh, kDrawDedupe, kDspAudio (the statistics screen keeps its
    software sink -- it was never a DSP-failure fallback), log3dsPeriodic and
    every line it gated, the census, the 3DS BMP capture, the HD probes and
    the hdrule lines (render_c3d_stats.h is now render_c3d_hd_set.h, the set's
    load summary). Four warnings fixed. Tests updated (c3d_frame dedupe check,
    ndsp_sink's main-ledger part, main_3ds_flow's respawn check); AGENTS.md P10
    says where the harness lives; ledger row 3ds.harness-hooks cites a note.
  - a497ae0a dead HD streaming: the HD pack's content-hash lookup (unreachable:
    the HD pack is refused without a name index). The per-surface stream stays
    (HUD pictures, Kurt's HD frames, a set that does not fit); spec section 4
    marked superseded for the set's textures.
  - efedb848, 4b8669ae main_3ds.cpp split, pure moves: 4,268 -> 2,904 lines;
    config_3ds, packs_3ds, level_load_3ds, platform/clock_3ds.h,
    platform/heap_probe_3ds. The frame loop stays inline (40-odd shared locals).
- **Evidence:**
  - Release build (clean, `make -C mdk_3ds`): zero compiler warnings. Player
    build: `.git/worktrees/cleanup-hd/hw-logs/play-4b8669ae.3dsx` (md5
    e74c4b6074c23da6c0e6ae4fb76fb4bb; ELF 043c5cb8dfc57c8f20162005776c91a8),
    no harness literal in the ELF.
  - At 4b8669ae: tools/check.sh ALL GATES PASSED (gates_frames ran: PASS);
    tests/formats 200610 checks, 0 failures; all 19 tests/scripts suites pass
    (c3d_frame 64 checks / 167 arms / 29 probes).
  - Hardware (old3ds-lan167, CFGU model 0, 268 MHz, L2 off, stereo on; branch
    throwaway/cleanup-hd-hw 7d86f347 = 4b8669ae + fix/3ds-hd-only's harness;
    `AUTOQUIT=90 PROFILE=1 FIXED=1 HDROMFS=1 TOGGLES=enableHdTextures=1`; logs in
    `.git/worktrees/cleanup-hd/hw-logs/`):

    | run | frames | mean ms | p50 | p95 | p99 | >33.3 | HD gate |
    |---|---|---|---|---|---|---|---|
    | L7 walk, fix/3ds-hd-only final | 4858 | 18.52 | 16.8 | 29.1 | 39.7 | 138 | PASS |
    | L7 walk, this branch | 4998 | 18.00 | 16.8 | 25.6 | 33.8 | 102 | PASS |
    | L8 walk, fix/3ds-hd-only final | 2955 | 30.45 | 28.6 | 42.6 | 52.0 | 266 | PASS |
    | L8 walk, this branch | 3084 | 29.18 | 28.6 | 32.7 | 41.8 | 93 | PASS |

    L7: set tier 256, 143/143 resident, 4.18 MB, hold 6.05 s. L8: tier
    256/128, 281/281 resident, 5.16 MB, hold 12.1 s. The throwaway's PROFILE
    build has no per-frame span ticks and no arena-entry table, so its frames
    carry less instrumentation than the reference's.
- **Not verified:** the player build itself on a console or in Mandarine (the
  hardware runs are its code plus the harness); the frame-loop split (not done).
- **Commit:** see `git log cleanup/immaculate-hd`.

## 2026-11-25 -- 3DS render/CPU optimisations, one commit each (branch perf/3ds-render-opts)

- **Owner's words:** level 1's wide open arenas, many distant enemies barely
  visible, "the frame rate tanks in spots; facing a wall still chugs". Target the
  Old 3DS; one feature per commit so each can be dropped and profiled. Approved
  list: a shared pixel-size helper, the script interpreter's copies, a frustum cull
  for debris/effects, debris and enemies of 2 px or less not drawn, mono in the
  scope, the scope frame's hidden fill. NOT built (owner): occlusion/BSP culling,
  tick-rate LOD, closed-form debris catch-up.
- **Base:** cleanup/immaculate-hd be83d1dc. Measurements on throwaway branches
  (never on the perf branch): `throwaway/render-opts-hw` (7d86f347's harness +
  spans, GPU time `C3D_GetDrawingTime`, draw sub-spans, scope-frame histograms, a
  level-1 census, AT=/ARENA=), `-s` / `-s1` (+ the script commits), `-final`
  (+ every perf commit). Prior-solution lookup: Obsidian searched (the 2026-11-19
  perf-pass note: decide on steady mean + vblank spread); Cognee `recall` timed out
  (300 s, recorded); pi-knowledge not in this session.

### The commits (each droppable; effect measured on the console)

| commit | what | measured effect (Old 3DS) | verdict |
|---|---|---|---|
| 00297b61 | `projectedPixelSize` / `sphereNearDepth` (camera_builder): px = size * max(P00*W/2, P11*H/2) / zNear, FLT_MAX at the near plane | helper only; layer_tests pin formula, axis, resolution, zoom, near guard | keep (the culls use it) |
| 61638733 | the VM reads each opcode by pointer from the decoder's cache (flat 8192-slot table by pc, then the std::map) instead of deep-copying the Instruction | scripts per 1/60 step **1.43 -> 0.82 ms** (DANT_5, s1-wall5 vs b0-wall5); sim p95 5.1 -> 3.5 ms | **keep -- the biggest CPU win** |
| 38912530 | runTick's object snapshot and partBounds' vectors reused | 0.82 -> 0.76 ms/step on top (s-wall5) | within noise of one run; drop if you want fewer lines |
| 26ee9e50 | debris pieces and sprite effects frustum-culled before they are built (3DS only; Linux untouched) | walk+fire route: effects build 0.66 (b0) vs 0.71 ms (with the cull, debris px off): **no measurable gain** -- the pieces were on screen | within noise; keep only as the base the next commit edits (or drop both) |
| aab20290 | debris of <= 2 px not drawn (TINY DEBRIS, default 2) | same route: effects build **0.71 -> 0.50 ms**, cpu busy p95 16.1 -> 15.5 ms; GPU unchanged (12.47 / 12.44) | small; keep (default 2 px) |
| 583a5ef0 | enemies of <= 2 px not drawn (TINY ENEMIES, default 2), hysteresis 2 / 2.5, current zoom, one decision for both eyes, draw only; never projectiles (XG_MISS, XG_BOMB1), never bosses (XB* family -- XB1 is DANT_10's boss, max health 9000 -- XGUNTA, the health bar's object) | **no enemy was ever <= 2.5 px in any measured spot** (census: smallest 3.0 px XF on the start route, 9.5 px XGEN / 19.6 px XG in DANT_5). Its cost after moving the test behind the frustum cull: cpu 10.22 (on) vs 10.48 ms (off), noise. A first version that measured every enemy before the frustum cull cost ~0.4-0.8 ms and was rewritten | gain unproven here; harmless at 0 cost -- drop unless you see dot-sized enemies |
| 3700feb5 | the scope in 2D (SCOPE IN 2D, default **off**: changes the look) -- gfxSet3D(false) while scoped, renderer plans eyes only while gfxIs3D() | scope frames: **GPU 21.75 -> 10.51 ms**, cpu busy 13.31 -> 10.03 ms; 3-vblank frames 62 -> 12 per 90 s | the biggest scope win; your call on the look |
| 41006e69 | the scene scissored to the scope's 600x360 window (SCOPE CLIP, default **on**): new seam `mdk_render_set_scene_window`, Vulkan no-op | scope frames: GPU **21.75 -> 20.62 ms** (-5%); 3-vblank frames 62 -> 34 | small but free; keep. Same picture: identical by construction (the frame is opaque outside the window; scissor grown 2 px) and checked side by side in Mandarine (`cap-clip1/0`, the only 37 differing px are a moving marker) |

### Before/after on the console (old3ds-lan167, CFGU model 0, 268 MHz, L2 off, stereo on, HD set on 143/143)

Config every run: `TOGGLES=enableHdTextures=1;texture16=1;kurtCompressed=1;cap30Fps=1;enableHaze=1;enableFiltering=1;enableMipmaps=1;enableSkyShadows=1`
over the console's config (lights, mirrors, glow, lighting, edge smoothing on).
`AUTOQUIT=90 PROFILE=1 LEVEL=7 HDROMFS=1 FIXED=1` (one 1/60 step a frame, so the
same frame is the same scene in both builds). DANT_5 spot: `AT=100,2320,-60,90
ARENA=DANT_5` (yaw 270 = "wall"). Logs: `.git/worktrees/render-opts/hw-logs/2026-11-25-*.log`.

| scenario | build | steady mean ms | 3 / 4+ vblank frames | cpu busy mean (p95) ms | scripts ms/step | GPU mean (p95) ms |
|---|---|---|---|---|---|---|
| DANT_5 open, standing | b0 (base) | 33.71 | 6 / 16 | 11.62 (14.5) | 1.44 | 15.42 (17.7) |
| | scripts | 33.72 | 8 / 16 | 10.28 (11.9) | 0.73 | 15.09 (17.4) |
| | final, enemies off | 33.69 | 6 / 15 | 10.48 (12.2) | 0.74 | 15.19 (17.4) |
| | final (all defaults) | 33.70 | 8 / 12 | 10.22 (12.2) | 0.73 | 15.00 (17.4) |
| DANT_5 facing yaw 270 | b0 | 33.62 | 5 / 9 | 7.53 (10.4) | 1.43 | 13.54 (15.8) |
| | decoder only | 33.59 | 5 / 9 | 7.03 (9.0) | 0.82 | 13.63 (15.7) |
| | scripts | 33.62 | 6 / 8 | 6.67 (8.1) | 0.76 | 13.49 (15.7) |
| DANT_5 scope (scope frames only) | b0 | 34.57 | 71 / 15 | 14.13 (16.7) | 1.62 | 21.81 (24.0) |
| | final, clip off | 34.41 | 62 / 13 | 13.31 (14.9) | 0.91 | 21.75 (23.9) |
| | final (clip on) | 33.97 | 34 / 10 | 13.32 (14.9) | 0.92 | 20.62 (22.7) |
| | final + scope in 2D | 33.70 | 12 / 11 | 10.03 (11.4) | 0.90 | 10.51 (11.7) |
| start route walk + fire (debris) | b0 | 33.52 | 10 / 3 | 10.18 (17.0) | 0.91 | 12.46 (14.6) |
| | final, debris px off | 33.50 | 9 / 2 | 9.80 (16.1) | 0.70 | 12.47 (14.6) |
| | final (debris 2 px) | 33.54 | 8 / 5 | 9.55 (15.5) | 0.70 | 12.44 (14.5) |
| start route walk | b0 | 33.54 | 8 / 5 | 8.63 (12.9) | 0.63 | 12.29 (14.3) |
| | final | 33.52 | 7 / 4 | 8.37 (12.1) | 0.42 | 12.29 (14.3) |

(~2,600 frames each; "cpu busy" = sim + draw build + C3D_FrameEnd + audio per
frame, the GPU's own `C3D_GetDrawingTime`. One run per cell: differences under
~0.3 ms CPU / ~0.3 ms GPU are noise.)

### Findings

- **At these spots the Old 3DS already holds the 30 fps cap**: CPU busy 7-14 ms
  and GPU 12-22 ms against a 33.3 ms frame; 98-99% of frames are 2 vblanks. So no
  optimisation moves the frame-period percentiles here; they buy headroom (CPU
  -1.3 ms in the open arena, -0.9 ms on the wall; GPU -11 ms in the scope with
  2D). The spot where the owner's frame rate tanks was not found by these walks:
  the next step is the owner's own route with the free-play PROFILE build.
- **Facing a wall** (DANT_5 yaw 270): GPU 13.5 ms vs 15.4 ms looking across the
  arena, and 13.1 ms in the start room. The GPU's time is mostly per-pixel cost
  of the full screen (stereo, 2x1 supersample, lighting, haze, shadows), not hidden
  geometry. The CPU part at the wall was the scripts (1.43 ms per step, two steps
  a frame in real time) -- that is what the decoder commit cut.
- **Census (every 30th frame):** enemies by full bounding-sphere diameter at
  400x240: none <= 2.5 px anywhere measured; debris on the walk+fire route: of
  ~30-55 in-frustum pieces, 10-35 at <= 2 px per sample.
- **Occlusion/BSP culling -- design note, not built.** Today: per-eye batch
  frustum cull (06_draw.inc) and the object/effects culls; the original drew BSP
  painter order. Expected gain: bounded by the geometry behind walls, which the
  numbers above put at <= ~2 ms GPU (open view 15.4 ms vs wall 13.5 ms includes
  everything that is not hidden too) and ~0.1-0.2 ms CPU (the arena draw build is
  0.13-0.17 ms a frame). A portal/arena-graph cull (draw only arenas reachable
  through visible connector openings, the DTI connections the runtime already
  has) would be the cheapest form: CPU ~0.1 ms a frame, no new data. Not worth
  building while the frame is capped at 30 with 11+ ms of GPU slack; revisit only
  if a measured spot is GPU-bound above 33 ms.
- **Defaults chosen:** TINY DEBRIS 2 px, TINY ENEMIES 2 px, SCOPE IN 2D off,
  SCOPE CLIP on. Existing config.ini files keep working (a missing key keeps its
  default; tests pin it). Linux/Vulkan: no behaviour change (the culls are set
  only by the 3DS frontend; the scene-window seam is a no-op there).
- **Not changed:** gameplay, RNG streams (all culls are draw-side; script goldens
  identical), CPU culls are not gated by console model (they cost nothing on the
  New 3DS either); resolution enters only through projectedPixelSize.

### Evidence

- Emulator (Mandarine r1.2.1 md5 0d2adab6..., `DISPLAY=:99`, one instance,
  `mdk_3ds/tools/emurun.sh --seed-sdmc` with the HD pack on the SD and
  `enableLighting=0`): the cleaned player build `play-4b8669ae.3dsx` and the final
  `play-41006e69.3dsx` both boot splash -> menu -> briefing -> fall -> level 1
  (A presses by xdotool on :99), HD set 155/155 resident, 0 given up, the level
  drawn (`hw-logs/2026-11-25-emu-smoke-play-*.log`). The emulator's heap is 100 MB
  (not the console's 64 MB) and it has no geometry shader: it checks boot and
  loading, not memory or speed. emurun's frame counter reads 0 on a player build
  (no frame lines): expected.
- tools/check.sh ALL GATES PASSED; tests/formats 200652 checks, 0 failures;
  tests/scripts all suites (c3d_frame included) exit 0; clean release build
  `make -C mdk_3ds` zero compiler warnings, no harness strings in the ELF.
- Player build: `.git/worktrees/render-opts/hw-logs/play-41006e69.3dsx` (md5
  56b65f34953f62022e39326daa9f2bac, ELF 2de8c9a4b0654702ec101a0eac328f5b).
- **Not verified:** the player build itself on the console (the hardware runs are
  its code plus the harness; the boss exclusion list was added to the enemy cull
  after the hardware runs, so XB*/XGUNTA are never hidden -- none were in the
  measured spots); scope in 2D on a real 3D panel by eye (the frame times are
  measured, the look is not); the owner's own slow spot.
- **Commit:** see `git log perf/3ds-render-opts`.

## 2026-11-25 -- the dead HD attach step is removed (branch cleanup/dead-hd-attach)

- **What:** `SurfaceCache::surfaceId` no longer attaches an HD copy to a
  palette-expanded texture (`mdk_render_replace_compressed` when the lookup
  answered a `compressedPath` without `compressedOriginal`). Gone with it: the
  `SourceStats::hdAnsweredSd` counter (read only by tests; the `hdrule:` line
  that printed it left main with the harness) and the `compressedOriginal` flag,
  which meant nothing once the branch was gone (every `compressedPath` is a
  packed original). Tests: `surfaceCacheHandsACompressedReplacementToItsSeam` and
  the HD-upscale Sprite block of `surfaceCacheKeepsHdAndDecodedReplacementsLitOnly`
  removed (they covered only the removed step); the `hdAnsweredSd` checks
  dropped; the requestPacked "other answer makes nothing" block and the
  named-Sprite-miss block now use a decoded image (the live non-packed answer)
  with the same checks against `mdk_render_replace_colour`.
- **Kept:** `mdk_render_replace_compressed` itself (Kurt's HD frames), the rgba
  colour replacements (PC/Linux), the packed-original and HD-pair creation, the
  per-surface HD streaming, toggle give-back, the door bubble and arena loading.
- **Why:** owner, 2026-11-25: "Remove the dead attach step, its counter and the
  tests that only cover it. This shouldn't change anything you see." The only
  `compressedPath` producers (`mdk_3ds/src/packs_3ds.cpp`, name index and orig
  pack by content) both set `compressedOriginal`; `mdk_linux` sets only rgba.
- **Evidence:** tools/check.sh ALL GATES PASSED; tests/formats 200630 checks,
  0 failures (22 fewer: the removed checks); tests/scripts all suites exit 0
  (c3d_frame included); Linux build compiles; clean release `make -C mdk_3ds`
  with zero compiler warnings. Mandarine r1.2.1 (md5 0d2adab6), `DISPLAY=:99`,
  `emurun.sh --seed-sdmc` (HD pack, enableLighting=0), A presses by xdotool:
  menu -> briefing -> fall -> level 7, HD set 155/155 resident, 0 given up, main
  loop entered (`hw-logs/2026-11-25-emu-smoke-dead-attach.log`). Not run on the
  console (the owner tests it).
- **Commit:** this one.

## 2026-11-25 -- the levels' music as DSP-ADPCM, committed (branch perf/3ds-memory)

- **Owner's words:** stream the music as DSP-ADPCM from the vini1264 high-quality
  soundtrack mod (`../mdk_soundtrack`, drop-in SNI archives); the .dsp files are
  COMMITTED (copyright risk raised and accepted), in their own commit, with the
  conversion script; 32 kHz mono, a good resampler, loudness matched to the
  originals; the build needs no download and no `../mdk_soundtrack`.
- **What:** `mdk_3ds/music/levelN/<name>.dsp` (48 tracks, 67,996,303 bytes) and
  `mdk_3ds/music/CONVERT_REPORT.txt` (per track: source format, loudness, gain,
  peak, size, sha256 prefix); `mdk_3ds/tools/music_dsp_convert.py` (run with
  `python3 -I`: ffmpeg soxr resample at 28 bits to 32 kHz, mono = the mean of the
  channels, EBU R128 loudness matched to the GOG entry, a lookahead limiter
  (alimiter, 5/50 ms, delay-compensated) where gain alone would clip, then the
  vendored encoder); `mdk_3ds/tools/dspadpcm/` (gc-dspadpcm-encode, MIT, from
  ../t2k's vendor copy, unchanged).
- **Which tracks:** the music-flagged entries (SNI flag 2) the level's CMI names,
  plus CORRIDOR (LEVELnS.SNI): exactly what LevelMusic can play. Never converted:
  level 4's SONG_ACTIVE/MEDIUM/LIGHT, AMB1-3 and GOLD (music-flagged, named by no
  CMI, never played). The soundtrack's three extra tracks are ASKED FOR by the
  game today -- AMB_DRIP and H9WATER by LEVEL3.CMI's table, TRACK12 by
  LEVEL7.CMI's -- and are silent with the GOG archives (the bank misses them); they
  are converted and will play. MISC/OPTIONS.BNI in the mod (13.97 MB vs 1.16 MB)
  is the front-end archive; see the music-stream entry for what the 3DS plays.
- **Loudness:** gains -2.9 to +8.0 dB; 19 replaced tracks needed the limiter to
  reach the original's loudness (the 8-bit originals are squashed flat: MUSIC1
  -14.8 LUFS vs the mod's -22.5); the extra tracks take the median gain (+3.4 dB).
  Unchanged originals get no gain (L3 H3 and L6 CORRIDOR clip 2 / 3,284 samples
  the resampler pushed past full scale, as a 16-bit output clips them).
- **Commit:** this one.

## 2026-11-25 -- the 3DS streams its music as DSP-ADPCM (branch perf/3ds-memory)

- **Owner's words:** stream the music as DSP-ADPCM; behaviour indistinguishable
  from now ("do it exactly how the game does it"): LevelMusic untouched, the same
  fades, swap and loop, a new track from its beginning with no delay, the same
  pause/menu behaviour, relative volume matched, Linux unaffected; adapt T2K's
  player (small ndsp buffers, worker thread, two streams for the crossfade); no
  dropout on SD stalls or arena loads; R1, stack limits, clean teardown,
  HOME/sleep safe; remove the whole-track decode path that becomes unused.
- **What:**
  - `mdk_core/src/audio/dsp_adpcm.{h,cpp}`: the .dsp header and the predictor
    history carried across wave buffers (T2K's openDsp/adpcmAdvance, Forsaken's
    "broken record" fix), host-tested.
  - `SoundBank::addStreamedMusic` / `getMusic`; `SoundMixer::launch` asks
    getMusic for the MUSIC bus only. A registered track answers with the file's
    geometry and a sample-less stand-in: never read out of LEVELnO.SNI, never
    decoded, never prefetched. The archives' volume/loop flags are kept; an extra
    soundtrack track plays looping at volume 0 like every retail music entry. A
    frontend that registers nothing (Linux) is unchanged. LevelMusic unchanged.
  - `audio_dsp_3ds.cpp`: the music channels (ndsp 23 and 22, the crossfade pair)
    stream ADPCM from `romfs:/music/levelN/<name>.dsp`: 6 wave buffers x 768
    frames (0.336 s) per channel = two seconds queued (72 KiB linear, was 128 KiB
    of PCM16 for 0.74 s); the worker alone opens/reads/closes the files and reads
    OUTSIDE the lock (the main thread posts start/stop/mix requests and signals);
    loop = reopen at the end (Forsaken's romfs EOF latch), whole-track loop as the
    game does (flag 1; the WAV cue chunks were never read). Two PRIMES: the loading
    arena's and the door bubble's nearest track are opened and their first two
    seconds read ahead by the worker (`audioDspPrimeMusic`, from the frame's old
    prefetch hints), so a start copies the prime and reads nothing. The drained-
    channel restart (Forsaken's trap) stays and is counted and logged as an
    UNDERRUN. R1 at the worker entry; 16 KiB stack, no recursion, small locals;
    aptHook parks ndsp calls while suspended; close joins the worker, then closes
    the files. Music lines: `music: <name> on chN: primed|NOT primed (ms)`,
    `music: <new> for <arena> (was, fading)`, `music: prime <name> for <arena>`.
  - Removed (now unused on the 3DS): the bank's music prefetch/step/decode use in
    main_3ds.cpp, `releaseMusicExcept` use, the lent music reader
    (`musicRead*`, render.h's `mdk_render_*_music_*` seam on all backends, the
    file thread's `kChannelMusic`; its reserved slot stays a spare so the texture
    streams' share is unchanged). `audioDspOpen()` no longer takes a bank. The
    bank's decoded budget 6 -> 4 MiB (effects only). The core prefetch itself
    stays: the Linux frontend uses it.
  - `stage_romfs.sh` links `mdk_3ds/music` into `romfs:/music`.
- **Not converted / not played:** MISC/OPTIONS.BNI's MAINSONG (the menu song; the
  mod's is 13.6 MB of 44.1 kHz stereo) is not played by this port on any target
  (main_menu.cpp: "MAINSONG/OPTSONG are not ported"), so it is not converted.
- **RomFS:** the music adds 64 MB; the GOG LEVELnO.SNI archives stay staged (the
  bank still answers script-visible metadata and corridor geometry from them;
  a script may play an O-archive entry as an effect), so the original music
  samples ship twice in the .3dsx: about 23 MB. Not stripped.
- **Evidence:** see the hardware entry of this branch.
- **Commit:** this one.

## 2026-11-25 -- no mip chain for textures only drawn Original-shaded (branch perf/3ds-memory)

- **Owner's words:** "No mip chains for textures never drawn with mipmaps (sky
  slices, canvas fonts/pictures): pixel-identical output required."
- **Why it is pixel-identical:** 06_draw.inc lets the sampler descend a chain only
  for a draw whose shading is not Original (`mipped`), and writes `maxLevel = 0`
  otherwise; the sky's slices and every canvas draw (fonts, pictures, HUD) are
  Original-shaded. Level 0 is the same bytes with or without a chain
  (pica::buildMipChain writes below level 0 only; the 16-bit store is chosen from
  level 0's alpha), so the sampled texels are the same.
- **What:** `C3dTexture::noMips`; makeTexture and the chunked expansion build no
  chain for it. Set by a new seam `mdk_render_mark_unmipped` (render.h; no-op on
  Linux and in the test stubs), called for each sky slice in buildSky3ds; by
  `mdk_render_mark_canvas` (font atlases, screen pictures: said at creation, so
  their first make has none); and by the first canvas draw's RGBA8 pin (from the
  texture's next make on; no extra re-make is forced for it). Compressed
  (.t3x) and streamed pictures are untouched.
- **Saving:** a third of each such texture's level-0 bytes (the sky's two
  slices on level 3, the menu/HUD atlases and pictures); measured in the
  hardware entry of this branch.
- **Evidence:** c3d_frame 64 checks / 167 arms / 29 probes; hardware in the
  branch's hardware entry.
- **Commit:** this one.

## 2026-11-25 -- the linear heap 24 -> 28 MiB (branch perf/3ds-memory)

- **Owner's words:** "I dont want to drop from 256x256 for OG 3DS. We shouldnt have
  to." Move freed main memory to the linear heap on the Old 3DS with an explicit
  margin above the measured in-play peak; keep the New 3DS split correct.
- **Measured first (base 4d9264f3 walk, hw-logs/base-walk-report.md):** main cap
  40.02 MB; highest main use in play over levels 3-8 30.63 MB (level 8), at load
  29.48 MB (level 4); level 8's HD set at 256 is 8,538,976 B against 5,178,112 B
  of room above the 7 MiB play reserve (12,518,144 B free at the decision): short
  3.36 MB, so 103 files went to 128.
- **What:** `kLinearHeapBytes` 24 -> 28 MiB. Old 3DS (65,200,128 B after the
  image): linear 29,360,128, main 35,840,000 -- 5.2 MB above the highest in-play
  main use measured (and that figure still had music decoded in main, which the
  DSP-ADPCM stream removed). New 3DS (124 MiB, dump 41's region): main 95.04 MiB
  (inside the 96 MiB area; the cap no longer binds there), linear 28 MiB (was
  27.04). Not more on the New 3DS: a larger linear heap puts levels 3/6/7 on the
  512 tier (a visual and GPU-cost change needing an owner A/B). heap_split
  tests updated (the Old 3DS boot-log figures, the boundaries at 28 MiB).
- **Evidence:** see the branch's hardware entry.
- **Commit:** this one.

## 2026-11-25 -- the music primes are kept across starts (branch perf/3ds-memory)

- **Why:** the first hardware walk of the DSP-ADPCM stream (level 8,
  `hw-logs/2026-11-25-fw-8.log`) showed a primed track used up by its start: going
  back through the same door (GUNT_2's DUNE_M2) found no prime and read the track's
  start on the worker first (105 ms late), because the door bubble hints a track
  once per change of its arena.
- **What:** a prime closes its file once read and keeps its bytes and the place
  they end (header, sample, predictor history); a start copies the bytes and opens
  its own file at that place. So a hinted track stays primed until two other
  wishes replace it. The music log names each start's channel and whether it was
  primed (the start's own serial, not the newest request's) and each underrun's
  channel and track.
- **Commit:** this one.

## 2026-11-25 -- Kurt's level animations in kurt.pak; no HD lookup for Kurt (branch perf/3ds-memory)

- **Owner's words:** "yes do all 3" (an Opus review's three items, relayed). This
  is item 1: the base walk's only gate FAILs (hw-logs/base-walk-report.md: late
  25 on level 6, late 8 on level 4) were Kurt's level animations -- K_SLIP,
  K_SLIDE, K_FSLIDE (level 6), K_SURF (level 4) -- made as 16-bit index textures
  with a 128 px HD copy streamed per frame, drawn SD until it landed.
- **Review claims verified:** the level animations were added after
  loadKurtFrames3ds (so no kurt.pak copy could be attached); the exporter read
  TRAVSPRT.BNI only; the lookup prefers a level copy (`pack->find(name, level)`).
- **What:**
  - `kurt_frames_export.cpp` also exports KurtSprite's kLevelAnimations from the
    level's SNI archives (TRAVERSE.SNI, LEVELnS.SNI, LEVELnO.SNI, the later one
    winning, as SoundBank::animation does), through the level's palette, so the
    converter packs them (original resolution, >256 fitted to 256, as every
    frame). kurt.pak: 408 -> 455 frames, 1,196,849 -> 1,295,281 bytes (+98 KB
    of romfs); they stream and rotate in the existing sprite budget, not pinned.
  - main_3ds.cpp: loadKurtFrames3ds runs after the level-animation loop;
    `kurtSprite.setMods(&hdPack)` is gone (the 3DS asks the HD pack for no Kurt
    frame; the owner's SD pack keeps its K_* files, unused -- no re-upload).
  - KurtSprite::prefetchNext reads ahead the level animations that are no state
    (K_SURF, K_SURFJ) after the states' (Linux has no pack: no change there).
  - HD spec 3.1/3.2/4 say so.
- **Not in this commit:** the toggle-on re-request of "Kurt's non-pack frames"
  (04_textures.inc) also serves the HUD's pictures; it goes with item 3.
- **Evidence:** vm_tests, main_3ds_flow pass; hardware in the branch's walk.
- **Commit:** this one.

## 2026-11-25 -- HD back on frees the originals before the set is read (branch perf/3ds-memory)

- **Owner's words:** "yes do all 3" (item 2): the HD off->on dip, 269,952 bytes
  of linear free on level 8 right after the hold (base walk).
- **Cause (verified):** the HD-off stretch makes the HD-only textures' packed
  originals; on toggle-on the boundary pass freed each only once its pinned copy
  was resident (04_textures.inc), and during the menu's hold no frame begins, so
  `gpuPending` stayed set and none went until the whole set had been read on top
  of them.
- **What:** in `mdk_render_set_enhanced_toggles`, HD going back on between frames
  with a set pinned: wait for the GPU (`gxCmdQueueWait(nullptr, -1)`: libctru
  2.7.0's implementation ignores the queue argument and waits on the running GX
  queue -- the wait C3D_FrameBegin does -- read off libctru.a's gxqueue.o), then
  free every HD-only texture's original whose pinned copy is not given up
  (`freeHdOnlyOriginals`, shared with the boundary pass). A copy that fails later
  leaves its texture to the packed original, read again like any evicted one.
  The toggle-on peak is not counted into the tier decision.
- **Evidence:** c3d_frame passes; hardware: the walk's toggle-on lines.
- **Commit:** this one.

## 2026-11-25 -- the HUD's and the bomber's HD pictures are their only copy with HD on (branch perf/3ds-memory)

- **Owner's words:** "yes do all 3" (item 3). Rule: with HD on, covered pictures
  load ONLY the HD file -- no standard read or expansion, no pop-in (resident
  before the loading screen ends); HD off and the live toggle still work; Linux
  unchanged.
- **Review claims verified:** hud_view.cpp's loadImage and PICKUPS frames, and
  bomber_overlay(_mods).cpp's BOMBTARG/CROSS, created the index texture (made at
  once by createIndexTexture: makeTexture or the chunked expansion) and THEN linked
  the HD file (`mdk_render_replace_image_compressed`), so the standard copy was
  expanded and kept beside the HD one for the level. No other caller links a
  canvas picture to the HD pack (menu screens, fonts, the sniper screen have
  none).
- **What:**
  - New seam `mdk_render_create_hd_image(w, h, indices, palette, hdPath)`
    (render.h): on the 3DS an index texture linked to its HD copy and HD-only --
    nothing is made at creation; with HD on the copy is the picture (06_draw.inc's
    HD-only path, the indices never expanded); with HD off the first draw makes it
    from the indices kept. Linux and the test stubs: the index texture (no
    compressed pack there; Linux's rgba replacement path is unchanged).
  - `mdk_render_pin_hd_set` pins every such picture's copy with the level's set,
    so the loading screen holds for it and the toggle brings it back; the
    toggle-on re-request of "other linked copies" (HUD, Kurt's non-pack frames)
    is gone -- the pictures are pinned and Kurt has no HD copy (item 1).
  - hud_view.cpp: SC_STAT, SKULL, SNIP_TXT and the PICKUPS frames resolve their
    HD file first and use the seam; bomber_overlay: `BomberOverlay::hdPaths`
    (in the mods file, so the goldens' binary still links the overlay without
    ModImages) resolves BOMBTARG_0/CROSS_0 and `load` takes them.
  - HD spec 3.1/3.2/4 updated.
- **Evidence:** host suites; hardware: the walk's covered-standard census.
- **Commit:** this one.

## 2026-11-25 -- the HD replace seams that nothing reaches are removed (branch perf/3ds-memory)

- **Why:** after items 1 and 3 no caller can reach `mdk_render_replace_compressed`
  (its only caller, KurtSprite's compressed-mods branch, needs `setMods` with the
  3DS's compressed pack, which main_3ds.cpp no longer does; the desktop's mods are
  never compressed) or `mdk_render_replace_image_compressed` (no caller left).
- **What:** both seams out of render.h, the 3DS backend, the Vulkan stubs and the
  test stubs; KurtSprite keeps its decoded-mod path (Linux) and its `hdSkipped`
  count; mods_tests' "never attached to an HD copy" checks counted calls to the
  removed seam and go with it (the same tests still check no index texture and
  one pending file id).
- **Per-surface HD streaming is still reachable**, so it stays: a level whose set
  does not fit (`DOES NOT FIT: streamed per surface`), and the HUD/bomber pictures
  in that case (pinned only with a set). Nothing else of it is provably dead.
- **Commit:** this one.

## 2026-11-25 -- the music reads a track's short last frame (branch perf/3ds-memory)

- **Found on the console** (`hw-logs/2026-11-25-fw-8.log`, the walk of the DSP-ADPCM
  stream): `music: UNDERRUN on ch23 (CORRIDOR): ... every buffer back, at sample
  86016 of 92308`. Every committed .dsp is 1-7 bytes shorter than whole frames:
  gc-dspadpcm-encode writes the last frame only as far as its samples need. The
  reader asked for whole frames, the last read came back short and was treated as
  a failure, and the track could neither finish nor loop: the channel drained,
  restarted, read nothing again, and went silent for the rest of the level.
  (The earlier commit's underrun line, which now says why and where, is what
  showed it.)
- **What:** the stream knows the file's data size (`ftell`, as T2K does) and its
  byte position; the last read takes what the file holds and zero-fills the rest
  of that frame (never played: the buffer's sample count stops at the track's
  length); loop, seek and prime adoption keep the byte position.
- **Commit:** this one.

## 2026-11-25 -- eight seconds of music queued per stream (branch perf/3ds-memory)

- **Owner's words:** "we shouldnt be so stingy with memory that we arent holding
  an adequate buffer for the music and sounds."
- **What:** 24 ADPCM wave buffers per music channel (8.06 s at 32 kHz, was 6 /
  2.0 s): 294,912 bytes of linear memory for both streams (was 73,728; +221 KB).
  A start still queues two seconds at once (6 primed buffers, or read on the
  worker) and the worker tops the queue up six buffers a pass; the primes stay
  two seconds (73,728 bytes of main memory for both).
- **Why that size:** measured on the Old 3DS walks, the slowest music buffer read
  148 ms, an unprimed start 105-145 ms; the longest stretch the game itself is not
  drawing is the HD set's toggle-on hold, 12.1 s on level 8, during which the
  worker kept reading. Eight seconds is fifty times the slowest read.
- **The underrun the walk showed was not a buffer size**: it was the short last
  frame (the commit before this one). The hardware entry gives the counts.
- **Commit:** this one.

## 2026-11-25 -- perf/3ds-memory on the Old 3DS: the walk, before and after

- **Base walk (4d9264f3) and final walk (4bf1744c)**, every level 3-8 through door
  crossings, one level transition (7 -> 6), the HD off/on toggle, the scope:
  `.git/worktrees/memory/hw-logs/base-walk-report.md` (both tables), logs
  `2026-11-25-bw-*` and `2026-11-25-fw-*`. Console old3ds-lan167 (CFGU model 0,
  268 MHz, L2 off, stereo on), owner config, HD on, harness on throwaway branches
  only (removed afterwards; logs kept).
- **Results:** level 8's whole HD set resident at 256 (was 103 files at 128);
  levels 4 and 5 now on the 512 tier (fits); every level gate PASS (the base's
  level 4/6 late HD draws are gone), 0 standard copies beside an HD copy; linear
  after HD back on 2.40 MB on level 8 (was 0.27); main cap 35.82 MB against an
  in-play peak of 30.41 MB (level 4); music 0 underruns, 0 missing, effects 0
  evictions/misses; frame means within +-0.3 ms of the base.
- **Found on the console and fixed on the way:** the encoder's short last frame
  (music went silent at a track's end), primes used up by one start.
- **Host and builds:** tools/check.sh ALL GATES PASSED (gates_frames PASS);
  tests/formats 200624 checks, 0 failures; tests/scripts all suites pass; clean
  `make -C mdk_3ds` zero compiler warnings, no harness strings in the ELF.
  Mandarine r1.2.1 (md5 0d2adab6) on DISPLAY=:99, one instance, `emurun.sh
  --seed-sdmc` (HD pack, enableLighting=0), A by xdotool: menu -> briefing -> fall
  -> level 7, HD set 169/169 resident, music started, main loop
  (`hw-logs/2026-11-25-emu-smoke-play-4bf1744c.log`).
- **Player build:** `.git/worktrees/memory/hw-logs/play-<hash>.3dsx` / `.elf`.
- **Commit:** this one (ledger only).

## 2026-11-25 -- the object pass skips `dead`, not isRemoved's scan (branch perf/3ds-removed-scan)

- **What:** `main_3ds.cpp`'s object pass (and `mdk_linux/src/main.cpp`'s twin)
  tests `obj.dead` instead of `runtime->isRemoved(&obj)`. isRemoved walks
  `removed_` (every object removed so far this level) once per visible slot per
  presentation, so the cost grows with play. One token per frontend, no new
  state; the gate `main_3ds_flow_test.sh` check 14 accepts either skip.
- **Why it is the same skip:** remove() is the only writer of `dead = true` in a
  running level and sets it as it appends to `removed_` (spawns and box spawns
  start `dead = false`; the deque only grows, so a removed pointer never aliases
  a live slot). The other writer is a save's `"Dead"` field, on a fresh runtime;
  neither the port (script_snapshot.cpp, `!dead` filter) nor the C#
  (ScriptRuntime.Snapshot.cs:45) saves a dead object, so it is false in every
  real save -- and a hand-made `"Dead": true` would draw nothing either way,
  because ObjectView::addObject returns on `obj.dead`. Gameplay callers of
  isRemoved are unchanged (not measured here).
- **Cost measured on the host** (Linux build with temporary counters, removed
  before the commit; `--headless --god --fixed-step`, walk
  `forward+fire` 2 min, 7200 frames): removed objects and pointer comparisons
  per presentation at the end -- L3 217 / 51k, L5 749 / 575k, L7 66 / 5k,
  L8 400 / 167k. `dead == isRemoved` held on all 4.4M visible-slot queries.
  The 3DS build's isRemoved loop is 6 instructions a pointer (ldr, add, cmp,
  beq, cmp, bne; disassembled from this branch's mdk.elf); at about 6 cycles
  each pointer L5's 575k is ~3.5M cycles, ~13 ms at 268 MHz, and L8's 167k
  ~4 ms (estimates from instruction counts, not a console measurement). Payoff bar: over 0.3 ms (1% of a 30 Hz frame) -- crossed after
  about a minute of play on L3, L5 and L8.
- **Test:** vm_tests `FrontendDeadSkipMatchesIsRemoved` (an attached chute
  removed first, a repeated removal; every slot's dead == isRemoved; a dead
  object not in `removed_` draws nothing, a live one draws).
- **Evidence:** `make -C tests/scripts test` all suites pass (the new vm test
  included; main_3ds_flow 22 checks / 64 arms red); `tools/check.sh` ALL GATES
  PASSED, gates_frames PASS on this branch's Linux build (pinned frames
  unchanged); clean `make -C mdk_3ds`, no compiler warnings, no harness strings.
  Player build: `.git/worktrees/perf-removed-scan/hw-logs/play-removed-scan.3dsx`
  (md5 8b8acd5ddb1e607c94876660fdc31d9c) / `.elf` (88a288c7b8c668357e8cebab3773f185).
- **Not verified:** a console timing of the gain; Mandarine was not run (the
  emulator slot was held by other projects' instances all session).
- **Lines:** +4/-1 main_3ds.cpp (3 comment), +2/-1 Linux main.cpp, +50 vm test,
  +6/-3 gate. No new state.
## 2026-11-25 -- the New 3DS asks for 804 MHz + L2 (branch perf/3ds-n3ds-speedup)

- **What:** `main_3ds.cpp`, after the log opens and before gfxInitDefault:
  `APT_CheckNew3DS`, and on a New 3DS `osSetSpeedupEnable(true)`; one boot line,
  `main: New 3DS, 804 MHz + L2 requested` or `main: Old 3DS, 268 MHz (no speedup
  to request)`. 14 lines (6 of them comment), one local bool, no new state. The Old 3DS makes no
  ptm:sysm call.
- **Why:** nothing in the game asked for the fast mode. A .3dsx launch carries a
  descriptor with the 804 MHz / L2 bits clear, and `mdk_3ds/cia/mdk.rsf` asks
  `CpuSpeed: 268MHz`, `EnableL2Cache: false`, so the owner's New 3DS has been
  running MDK at the Old clock. Sibling ports on the same console do the same
  call (`../forsaken` main_3ds.cpp, `../t2k` applyNew3dsSpeedup).
- **libctru 2.7.0, read from the linked `libctru.a`:** `osSetSpeedupEnable`
  stores `__ctru_speedup` and calls `PTMSYSM_ConfigureNew3DSCPU(3)` through a
  transient ptm:sysm session (nothing if the session cannot open);
  `aptWaitForWakeUp` calls `__ctru_speedup_config` (three call sites), so the
  request is re-applied after HOME / applet returns without a hook of ours.
  `mdk.rsf` already lists `ptm:sysm`; it is left at 268 MHz / L2 off: the run-time
  request covers both packages, and changing the exheader would be a second,
  separately tested variant.
- **Timing code:** the frame clock (`clock_3ds.h`), the stream clock, the
  texture-upload budget and the audio timers all divide `svcGetSystemTick` by
  `SYSCLOCK_ARM11`; the tick keeps its 268 MHz rate at 804 MHz (libctru's own
  time base, and t2k's game clock does the same with the speedup on). No
  cycle-counted busy loop exists in `mdk_3ds/src`; the waits are
  `svcSleepThread`. Worker priorities and cores are untouched.
- **Owner check (New 3DS, cannot be done here):** start MDK, get into a level,
  open Rosalina (L + D-pad Down + Select) -> **New 3DS menu** while MDK runs. It
  names the action each entry would take: "Set clock rate to 268MHz" and
  "Disable L2 cache" mean the game is at 804 MHz with L2 on; "...to 804MHz" /
  "Enable L2 cache" mean it is not. Close Rosalina without choosing. Repeat after
  a HOME-menu round trip and after closing/opening the lid. `sdmc:/3ds/mdk/mdk.log`
  should say `main: New 3DS, 804 MHz + L2 requested`. The game must run at the
  same speed as before (a 3x-fast game would mean the tick rate moved).
- **Not verified:** the actual clock (no console here; the appliance is the Old
  3DS and is in use elsewhere); the lid-sleep path (libctru re-applies on
  wake-up from HOME/applets; sleep is the owner's check above); the frame-time
  gain.
- **Evidence:** clean `make -C mdk_3ds` (devkitARM, libctru 2.7.0) with no
  compiler warnings; the ELF links `osSetSpeedupEnable`, `__ctru_speedup_config`,
  `APT_CheckNew3DS`; no harness strings. Player build:
  `.git/worktrees/perf-n3ds-speedup/hw-logs/play-n3ds-speedup.3dsx`
  (md5 162a3f3723fa5eab986a77484e6d7c61) / `.elf` (477add5cd2373d6a4b62cfdd474dbf0e).
  `make -C tests/scripts test` all suites pass; `tools/check.sh` ALL GATES PASSED
  (gates_frames PASS, smdh SKIP: no .cia). Mandarine NOT run: the machine's
  emulator slot was held by other projects' instances for the whole session (the
  one-instance rule), so there is no emulator boot of this build.
## 2026-11-25 -- a debris piece's culling radius is measured once, not every frame (branch perf/3ds-effects-caches)

- **Owner's direction:** two caches from the outside review's ticket D
  (`.git/mdk-review/mdk_3ds_stereo_performance_spec.md`): the debris culling radius and
  the final halo tint, each kept only if it is expected to save about 0.3 ms per
  presented frame on the Old 3DS in a debris storm ("I didnt want to implement a bunch
  of shit that adds complexity without payoff"). Nothing else from ticket D.
- **What:** `Debris::Piece::reach` (debris.h), -1 until measured, put back to -1 by
  `Piece::reset` (every `take()`, so a reused pool slot starts unmeasured).
  `EffectsView::build` measures it at the piece's first culled build with the same loop
  (corner order, `mathMax`, the squared length, `sqrtf`) and reads it after that. The
  corners are written only where a piece is made (shatter, break-up, spark, all after
  `take()`), so the radius cannot go stale. A NaN radius stays NaN and stays cached
  (`NaN < 0` is false); 0 is a valid radius. The culls' margin, near-plane rule, zoom and
  thresholds are untouched, as are the float flags. 18 lines of game code (+13 -8);
  +4 bytes a piece on the 3DS (`sizeof(Piece)` 140 -> 144, measured with devkitARM),
  2,400 bytes over the 600-piece pool; effects_view.o text +12 bytes.
- **Why, measured first:** the storm (L8_hazards_debris: up to 361 pieces, 5,064 corners)
  spent, before this change, 2.0 ns a corner (27 ns a piece) on the radius on the host
  (Ryzen, -O2, `MDK_FX_BENCH`). On the ARM11 the loop (devkitARM 15.2 -O2 disassembly) is
  three loads, a dependent vmul/vmla/vmla chain and two vcmp+vmrs+branch pairs (mathMax's
  NaN test and compare) per corner, with nothing to overlap between corners: an
  ESTIMATE of 35-55 cycles a corner, 0.66-1.04 ms per presented frame at the storm's
  peak at 268 MHz (the host time times the ledger's 50-60x integer ratio gives 0.51-0.61
  ms, its 130-190x whole-tick ratio 1.3-1.9 ms). It is at or above 0.3 ms for 116-140 of
  the run's 400 steps (about two seconds of storm); ~0 ms with no debris. What is left a
  piece is one load, compare and branch. Host build before/after (all six cameras,
  mean over the run, three interleaved runs on a loaded machine): 31.8-39.4 -> 30.0-30.6 us.
  Estimates, not a console measurement: the hardware appliance was with another agent.
- **The halo tint cache was measured and NOT built:** three `powf` a glowing sprite cost
  4 ns a call on the host; devkitARM's `powf` (libm armv6k/fpu, the optimized-routines
  double-precision one) is ~80 instructions with a ~110-cycle chain of dependent f64
  VFP operations, ~150 cycles a call: at the storm's 96 glowing sprites 0.16 ms per
  presented frame (0.11 ms at the backend's 64), 0.06-0.22 ms by the host ratios. Under
  the 0.3 ms bar, so dropped.
- **Evidence:** `tick1 dump effects` / golden `tests/scripts/golden/L8_effects_storm.txt`
  (new, in the `tick1` suite): the storm with six cameras every step (the 3DS culls at
  the default and widest stereo margin, a 6-degree zoom, an eye inside the cloud through
  the near plane, a frustum edge through the cloud, no cull), every vertex, draw and halo
  hashed as bits. The golden was written by the build BEFORE the cache and matches after
  it byte for byte; the only added line is `reach checked=48813 differ=0` (every measured
  radius against the old loop, as bits, every step). It covers 17 pool slots reused with
  another shape, zero-size and NaN-size sparks, halos measured late (pixels away at the
  entry's making), a cleared view, and the other arenas' palettes (identical on level 8).
  Non-vacuity: without the reset in `Piece::reset` it fails at s=100.
## 2026-11-25 -- the main menu's songs as DSP-ADPCM (branch perf/3ds-memory)

- **Owner's words (relayed):** the title screen needs its music -- MAINSONG on the main
  menu, OPTSONG under the options -- behaving exactly as the game does; the soundtrack
  mod where it replaces them, 32 kHz mono .dsp, committed, same converter and loudness.
- **Verified first:** the C++ port never played either song (4d9264f3's main_menu.h
  and menu_items.h list them as not ported; no frontend plays a menu song). The owner's
  memory of title music is the C# build's, not this port's.
- **What:** `music_dsp_convert.py` also converts the front end's two songs into
  `mdk_3ds/music/menu/`: MAINSONG from MISC/OPTIONS.BNI (the soundtrack's 77.5 s
  44.1 kHz stereo replaces the GOG 39 s 22 kHz 8-bit; loudness matched to the GOG
  song, +6.9 dB through the limiter, 1,416,613 bytes) and OPTSONG from MISC/MDKSOUND.SNI
  (the soundtrack has none: the original's 3 s loop, no gain, 55,099 bytes). The
  levels' median gain ignores the menu, so every level track is byte-identical
  (re-run checked: no level .dsp changed); CONVERT_REPORT's rows now name the folder.
- **Commit:** this one.

## 2026-11-25 -- the 3DS main menu plays MAINSONG, and OPTSONG under its options (branch perf/3ds-memory)

- **Owner (relayed):** no music at the title screen on play-08f80f9c; play the two
  songs exactly as the game does.
- **Behaviour, from the C# (9708e8d6^: Menu/MainMenu.cs, Menu/MenuItems.cs,
  Flow/Game.cs):** MAINSONG starts when the main menu opens (under the INTRO1A
  splash too), loops forever on the music bus at full gain; the options stop it and
  start OPTSONG once (it plays on across the options' own pages); going back stops
  OPTSONG and starts MAINSONG again from its beginning; any screen change stops
  everything (Game.Show's StopAll), so the briefing, the fall and the level start
  silent of it, and the menu reached again (from a level, a death, the statistics)
  starts MAINSONG afresh. Volumes are the settings' master and music bus gains,
  applied every frame so the Audio page is heard at once.
- **What:** mdk_core: `MainMenu::song()` / `songStarts()` and MenuItems'
  `optionsSong()` (the C#'s two _songVoice fields as state; menu_tests pin the
  sequence: open, options, an options page and back, back to main, the quit page,
  opening on the options). 3DS: menu_screens_3ds.cpp opens the DSP path with the menu
  screen -- a bank that knows only romfs:/music/menu's two streamed tracks, a mixer,
  the binder's music channels -- plays the song the menu names, and closes it (worker
  joined, buffers freed) when the flow leaves the menu, before the next screen opens
  its own audio. `audioDspMusicRegisterFolder`; the folder is forgotten at close.
  OPEN_ITEMS updated.
- **Not done:** the Linux frontend's menu stays silent (it has no streamed-music path;
  the core state is there for it); OPTSONG under the in-level pause menu's options (the
  C# PauseMenu shares MenuItems, so the original's pause options played it too);
  SND_PUSH and OPTBUTT (out of scope); the movies' sound (not wired on either target).
- **Evidence:** menu_tests; tests/formats, tests/scripts pass. Hardware
  (old3ds-lan167, throwaway harness pressing keys, `hw-logs/2026-11-25-mm.log.partial`):
  menu -> `music: MAINSONG on ch23` -> options -> `OPTSONG` -> an options page and back
  (no restart) -> back -> `MAINSONG` again -> New Game -> briefing, fall (menu audio
  closed) -> level 7 `TRACK11 on ch23` -> MAIN MENU -> `MAINSONG` again.
- **Commit:** this one.

## 2026-11-25 -- the door-crossing hitch: SD log lines off the main thread; music and file workers off its core (branch perf/3ds-memory)

- **Owner (relayed):** "very slight hitching again between areas" on 08f80f9c (New 3DS,
  HD on), not on 4d9264f3; "I want that kind of stuff on the second core".
- **Root cause, measured** (old3ds-lan167, 268 MHz, L2 off, stereo on; level 8, a
  teleport-and-walk route through 12 arena changes, 6 of them walked corridor doors;
  throwaway harness timing each slow frame's spans and its SD log lines):
  - bisect 4d9264f3..08f80f9c: the extra time arrives with 71e4f946 (music streaming);
    the 840 corridor entry is 140-155 ms on every build from it on (5 runs) against
    94-127 ms on 4d9264f3 (3 runs);
  - every log line is `fflush`ed to SD by its caller and costs it 10-20 ms; a door
    crossing writes 3-5 lines: 21-46 ms of the entry frame on 4d9264f3, 44-93 ms on
    08f80f9c, which adds `music: X for Y` (inside the simulation step: +10 ms sim on
    every crossing) and the prime line, while its worker's reads share the card;
  - not the cause: the 512 tier / 28 MiB linear (LINEAR24 run unchanged), the music
    worker's CPU (MUSICOFF, MUSICTRICKLE, a lower-priority worker, deferring primes
    250 ms: none removed it), MIPMAPS (4d9264f3 with MIPMAPS off still 90-144 ms).
- **What:** `log3dsPrintf` queues the line (16 KiB) for a writer thread that writes and
  flushes at once (a full queue is written by the caller; close joins and drains).
  `side_core_3ds.h` picks a worker's core: 2 on a New 3DS, the one system-core slot
  (APT_SetAppCpuTimeLimit) on an Old 3DS for the music worker, else today's default
  core; every threadCreate retries on core -2 if refused. The music worker takes the
  system core (Old) / core 2 (New); the file reader and the log writer take core 2 on
  a New 3DS and stay on core 0 below main on an Old one (one app thread fits on core 1).
- **Evidence** (corridor entry frames, ms; 33 = no frame over 45 ms):
  4d9264f3 94/90/60 at 840/1130/1430 (MIPMAPS on), 144/136 (off); 08f80f9c 151/90/57,
  180/124 (off); this branch 33 at all six crossings in 6 runs: MIPMAPS on x2, off x2,
  music worker back on core 0 (log thread only), system-core share 10%. 0 music
  underruns in every run. Frames over 45 ms per 90 s run: 91 (4d9264f3, spike lines
  only) -> 29-33.
- **No measured gain from the core moves on the Old 3DS walk:** the log writer alone
  gives the same frames as all three changes (both 33 at every crossing; mean CPU busy
  7.29 vs 7.33 ms). They are separate commits so they can be dropped.
  System-core share: 10% and 30% measured the same, so 10%.
- **Not verified:** a New 3DS on hardware (core 2: Mandarine, an emulated New 3DS,
  ran all three threads there, menu and MAINSONG fine, `hw-logs/emu-smoke-a2aed4ff.txt`);
  the CIA (cia/mdk.rsf AffinityMask 1, CanAccessCore2 true: the fallback covers a
  refusal); HOME and lid sleep with a system-core thread (no remote HOME on the
  appliance).
- **Doorway texture work off main (assessed, not done):** with the log moved, no
  corridor entry reached 45 ms with MIPMAPS on or off on this walk, so moving the
  original expansion and mip-chain build off main would buy nothing measurable on the
  Old 3DS; it would need the expansion state (source, palette, chain buffer) handed to
  a worker, cancelled on eviction, and a wait when a draw needs it now. On an Old 3DS
  the only free slot (core 1, 10-30%) is too small for it.
- **Commits:** c93b2ede (log writer), 411bdc95 (music worker core), a2aed4ff (file
  reader core), this one (10% share, ledger).
## 2026-11-25 -- cleanup/split-large-files: the oversized sources split along their seams

- **Why (all entries below):** the owner asked for the files over 1500 lines to be
  split so they can be read at a glance (AGENTS.md section 10, plain flat code). Pure
  code movement: no behaviour change, no renames, no new abstractions. Where the
  pieces share file-static state (anonymous-namespace helpers, `static` functions)
  they become `.inc` fragments of the SAME translation unit, in the original order
  -- the renderer_c3d.cpp / renderer_vk.cpp precedent -- so the object code can be
  checked byte for byte instead of argued.

### c3d/06_draw.inc (1758 lines) split into five fragments
- **What:** `06_draw.inc` (561: the combiner stages, haze, prepareDraw),
  `06_draw_submit.inc` (540: drawRecorded, the diagnostic triangle), `06_glow.inc`
  (336: the glow bloom pass), `06_queue.inc` (221: coalescing, prepareQueue,
  replayQueue), `06_glow_billboards.inc` (116). renderer_c3d.cpp includes them in
  the original order. Comments that named a moved function by file now name its
  new file.
- **Evidence:** `renderer_c3d.o` built with the 3DS release flags is byte-identical
  to main's (md5 7eed41db, whole object and disassembly); tests/scripts
  c3d_frame_test.sh passes (it expands `c3d/*.inc` like the compiler).
- **Commit:** this one.

### c3d/03_frame.inc (2184 lines) split into six fragments
- **What:** `03_frame_state.inc` (516: the render debug switches, the deferred-texture
  settle, the forward declarations, the GPU state setters and their dedupe,
  bindProgram), `03_eye.inc` (285: installEye, edge smoothing),
  `03_shadow_probe.inc` (314: the GPU read-back and the sun shadow's ramp/native
  tests), `03_frame.inc` (423: the dedupe's log line, mdk_render_begin_frame),
  `03_shadows.inc` (539: the frame's light, the shadow map and caster pass),
  `03_toggles.inc` (127: haze, the HD-only originals, the toggles' setter, the 30 fps
  cap). Same include order as the original file; function-specific comment
  references follow their function.
- **Evidence:** `renderer_c3d.o` (3DS release flags) byte-identical to main's (md5
  7eed41db); c3d_frame_test.sh passes.
- **Commit:** this one.

### c3d/04_textures.inc (1888 lines) split into five fragments
- **What:** `04_textures.inc` (421: eviction, slots, the expansion helpers, the
  compressed and streamed imports), `04_expansion.inc` (266: the chunked
  expansion), `04_make.inc` (396: the orig pack's reads, the HD rule,
  makeTextureNow), `04_hd_stream.inc` (532: the HD stream), `04_streams.inc` (289:
  the orig pack's pump, the loading screen's settle, the resident HD set).
- **Evidence:** `renderer_c3d.o` byte-identical to main's (md5 7eed41db);
  c3d_frame_test.sh passes. renderer_c3d.cpp is still one translation unit, so
  nothing about inlining, static state or initialisation order can have moved.
- **Commit:** this one.

### mdk_linux vk_context.cpp (1826 lines) split into a .cpp and three fragments
- **What:** `vk_context.cpp` (510: the helpers, instance, surface, device,
  teardown, swapchain re-creation) and `vk_context/01_swapchain.inc` (607, pasted
  inside the anonymous namespace as before), `02_eye_targets.inc` (221),
  `03_frame.inc` (510: passes, beginFrame, endFrame, submitFrame).
  tests/scripts/linux_refusal_report_test.sh now expands `vk_context/*.inc` the way
  it already expanded `c3d/` and `vulkan/`, so it reads the same text as before.
- **Evidence:** `vk_context.cpp.o` (CMake RelWithDebInfo flags) identical to
  main's after stripping debug info; linux_refusal_report_test.sh PASS (23
  checks, 78 arms red, 7 probes green). Negative control: with the old include
  regex the test FAILS ("endFrame is not in the file", mutation arms inert), so
  the regex change is what keeps its coverage, not a loosening.
- **Commit:** this one.

### game/player.cpp (1794 lines) split into a .cpp and four fragments
- **What:** `player.cpp` (628: constants, helpers, update, turning, items, firing,
  muzzle) and `player_state.inc` (449), `player_vertical.inc` (257),
  `player_platforms.inc` (267), `player_slide.inc` (225), included at the bottom
  in the original order. The @vfp-exempt R6 pins (carry, slide heading x2) moved
  with their functions. player_ledge.cpp and player_sniper.cpp untouched.
- **Evidence:** player.o byte-identical on the 3DS (md5 fab37454); debug-stripped
  objects identical for the Linux build and tests/scripts.
- **Commit:** this one.

### scripts/script_runtime.cpp (5858 lines) split into a .cpp and nine fragments
- **What:** `script_runtime.cpp` (787: helpers, construction, update, doorway geometry)
  plus `script_runtime_{tick,arenas,motion,world,ai,objects,events,squads,combat}.inc`
  (359-889 lines each), listed in the .cpp's header and included at its bottom
  in the original order. Every helper stays file-local; the 17 @vfp-exempt pins
  moved with their code.
- **Evidence:** script_runtime.o byte-identical on the 3DS (md5 15f70338);
  debug-stripped objects identical for Linux and tests/scripts. Negative control:
  swapping two of the includes gives a different object (9e59d7c4), so the
  comparison does see a reorder.
- **Commit:** this one.

### formats/glb.cpp (1732 lines) split into a .cpp and two fragments
- **What:** `glb.cpp` (587: constants, matrices, pack, Glb::read, Glb::write),
  `glb_json.inc` (574: the JSON value, parser and writer) and `glb_reader.inc` (589: the
  glTF Reader class), both pasted inside the anonymous namespace where they stood.
- **Evidence:** glb.o byte-identical on the 3DS (md5 24f31e48); debug-stripped
  objects identical for Linux, tests/scripts and tests/formats.
- **Commit:** this one.

### Not split: main_3ds.cpp, mdk_linux main.cpp, scripts/script_vm.cpp
- **Why:** each is big because of ONE function, and the brief allows extracting a
  function only where that is provably behaviour-preserving. `main()` in
  main_3ds.cpp is lines 175-2898 of 2898 (the level loop alone 1427-2798, with
  ~100 locals and [&] lambdas it shares); `main()` in mdk_linux/src/main.cpp is
  371-3161 of 3161; `ScriptVm::execute` is 567-2490 of 2493 (one opcode switch,
  261 case labels). Moving the few hundred lines around them would not bring
  any of the three near the target, and cutting a function body across files
  with mid-function #includes would be provably identical but harder to read.
  Left for a deliberate refactor (a state struct for the frontends, opcode
  groups for the VM), which is not code movement.
- **Commit:** this one (ledger only, with the last split).

### cleanup/split-large-files rebased onto main f2490d62 (the hitch fix and title music)
- **What:** the seven split commits replayed onto f2490d62; main had not touched
  any split file, so only this ledger conflicted (both sides kept). The pre-rebase
  branch is kept as `cleanup/split-large-files-on-08f80f9c`. No new split: no file
  main changed crosses 1500 lines (main_3ds.cpp 2916, Linux main.cpp 3162 -- both
  left whole as before; audio_dsp_3ds.cpp 1288, menu_screens_3ds.cpp 660).
- **Evidence:** clean 3DS release builds, same devkitARM, of main (detached
  worktree) and of this branch: mdk.elf md5 20748e39 for both, equal to the
  coordinator's reference play-main-f2490d62.elf; all 177 objects byte-identical,
  no path-dependent difference. Clean Linux build and 3DS build 0 compiler
  warnings; tests/scripts `make test` passes (C3dFrame, LinuxRefusalReport and
  the rest; the frame gate's oracle-pixel fidelity arms SKIP, no reference on
  disk); tests/formats 200624 checks, 0 failures; tools/check.sh ALL GATES PASSED
  (gates_frames PASS on the Linux build, gates_smdh SKIP: no .cia).
- **Commit:** this one (ledger only).

## 2026-11-25

### A packed animated strip draws one frame, not the whole strip (spec ticket B, first part)
- **What:** `compressedUv` (06_draw.inc), used by the prepare and by the
  pending-at-submit path (06_draw_submit.inc), now takes the draw and returns its
  frame's slice of the texture's rectangle through the new pure helper
  `pica::compressedFrameUv` (scaleY = (bottom - top) / frames, offsetY = top +
  frame * scaleY). A one-frame texture (Kurt's packed frames, every still packed
  texture) keeps the rectangle bit for bit, whatever frame a draw names. HD copies,
  indexed textures, the filter (an animated draw stays NEAREST) and the mip rule
  (an animated draw stays at level 0) are untouched. No new state; +28 lines in
  pica_matrix.{h,cpp}, +4 net in the two .inc files, +100 test lines.
- **Why:** owner-approved fix. Before, every frame of a packed strip drew the
  whole strip squashed into one quad: SL_MED (32x960 in 32x1024) sent
  (scaleY, offsetY) = (-0.9375, 1) for frames 0, 1 and 29; now (-0.03125; 1,
  0.96875, 0.09375). Affected: the effect sprites that are in the orig pack --
  TRAIL (the debris smoke, 64x64x11), SL_MED, SL_SMA, BUBB, BUBB_POP, PULSE,
  SB_MED, SB_SMA, M_COMM, BONEFLC -- and any arena surface with frameCount > 1,
  which the arena draws at frame 0 as the C# does. The over-1024 strips
  (EXPLODE, SL_BIG, FIRE) are not packed and are not touched here (later item).
- **Frames cannot alias:** the effects/object submissions' consecutive-draw
  shortcut caches only the texture id (the frame is set per draw); the backend's
  coalescing compares the uv words, which now differ per frame (a pending record
  too: its prepare-time uv is the frame's slice of the default rectangle); both
  eyes replay the one prepared record, and the pending path recomputes from the
  same draw->frame in each eye.
- **Evidence:**
  - Host: `testTheCompressedStripFrame` (tests/scripts pica_matrix_tests) samples
    numbered synthetic strips (SL_MED and TRAIL geometry) under NEAREST through
    the helper: every pixel of a 1:1 quad reads its own frame and row (orientation),
    edge samples and minified/magnified quads stay in the frame. With the helper
    forced to the old whole-rectangle answer it fails 68 checks; with the fix 0.
  - Hardware, old3ds-lan167 (Old 3DS), throwaway/anim-frame-hw (main + harness +
    SPRITES=1, a stream of TRAIL and slime drops in front of Kurt, LEVEL=3,
    AUTOQUIT=90), atmctl stereo screenshots mid-run, both cycles FRESH: before,
    TRAIL reads as stacked horizontal yellow/grey bars and the drop as a striped
    blob ("lines through the smoke"); after, round puffs and a solid drop.
    Captures in .git/worktrees/anim-frame/hw-logs/hw-ab.
  - tests/formats 200624 checks 0 failures; tests/scripts make test; tools/check.sh
    ALL GATES PASSED (smdh, frames SKIP); player .3dsx 0 compiler warnings.
- **Not verified:** the ETC1A4 colour of frames whose height is not a multiple of
  4 (BUBB 31, BUBB_POP 30, M_COMM 129) can mix two frames inside one 4x4 block --
  a codec property of the existing pack, not sampling; no Mandarine run (the
  machine's one emulator slot was taken).
- **Commit:** this one.

### The owner's "lines through the smoke": this bug, not screen-door
- **Finding:** the debris smoke is TRAIL (64x64 x 11 frames, 11 ticks, spawned
  by one debris piece in four). Its strip (64x704 -> 64x1024) is in the orig pack
  as ETC1A4, so it drew all 11 frames squashed into one quad: ~11 thin horizontal
  slivers with the background between them. The art itself is solid (0.17%
  isolated texels, no dither). The C# drew sprites at Pass.DoubleSided, which is
  not blended (Renderer.cs blends only Blended/Overlay): an index-0 cut-out with
  depth write, the original's colour key -- so the hard edges are authentic, and
  there is no screen-door transparency anywhere. Blending is unchanged (not asked).
- **Evidence:** decoded frames and the simulated squash in
  .git/worktrees/anim-frame/hw-logs (TRAIL_frames.png, TRAIL_correct_vs_bug.png);
  the hardware before/after above.
- **Commit:** this one (the fix above).

## 2026-11-26

### Linux enhanced mode: an animated texture's colour layers are one frame each (EXPLODE segfault)
- **What:** `getColourView` (mdk_linux/src/render/vulkan/09_colour.inc) built the
  enhanced colour texture's array layers from the index texture's FULL stacked
  height: `colourChain(indices, w, h, frames)` treats `h` as one frame, so it read
  `frames` times past the indices and every layer was the strip from frame f down.
  It now uses `height / frames` (the C#'s `SizeOf`, Renderer.Colours.cs, removed in
  9708e8d6) for the chain and the image, and refuses (the original pipeline draws
  it) a texture whose rows are not whole frames, the C#'s `CanExpand`. +11 lines
  net, no new state.
- **Why:** the census agent's segfault. Reproduced here: `MDK_DATA_DIR=MDK mdk
  --headless --level=5 --at=250,121,-1622,0 --give=SW_BONES --aim=0,1.25
  --walk='sniper@20-22;fire@80-82' --enhanced --fixed-step --seed 1 --frames 900
  --capture-frame=600` -- the parent build (fix-arena-entry-stutter, same 09_colour)
  dies with SIGSEGV after frame 390, gdb: `colourExpand (count=425984)` <-
  `colourChain (width=128, frames=26)` <- `getColourView (frames=26)` <-
  `drawEnhanced` (EXPLODE 128x3328). The fixed build runs to frame 600 and writes
  its capture. Every other animated texture (TRAIL, SL_MED, ...) was drawn from
  wrong layers in --enhanced before this (the shader samples layer = frame with uv
  over one frame, enhanced.frag:161); they read past the indices by less and did
  not fault.
- **Evidence:** the two runs above; tools/check.sh gates_frames PASS on the built
  Linux binary; tests/formats 200624 checks 0 failures; tests/scripts make test.
- **Commit:** this one.

### The animated strips over 1024 drawn on the 3DS as pages (spec ticket B, second part)
- **What:** EXPLODE (128x128 x 26 = 128x3328), FIRE (64x73 x 36 = 64x2628) and
  SL_BIG (64x64 x 30 = 64x1920) -- the only strips over the PICA's 1024 in levels
  3-8 (exporter census: 104 + 48 + 104 arena rows, ONE content key each, so one
  file each for every palette) -- were refused by the orig-pack build and by the
  backend's size guard, so the explosion, the fire and the wound slime were never
  drawn. Now:
  - `arena_etc1_convert.py` writes such a strip as PAGES of whole frames,
    `<name>@<key>.p<k>.t3x`, 1024 / frame height frames a page (EXPLODE 8+8+8+2,
    FIRE 14+14+8, SL_BIG 16+14), ETC1A4, NO mip chain (an animated draw is held at
    level 0). The manifest/byname rows are the strip's own, so the name index finds
    it as before.
  - `mdk_render_create_compressed_file` takes the frame count (render.h; the
    Linux and test stubs follow). In the 3DS backend a height over 1024 makes a
    STRIP id that only lists its page ids (createStrip, 05_resources.inc); each
    page is an ordinary packed original, read on the reader thread and imported at
    a frame boundary, asked for when the warm asks for the strip. A page already
    made for another palette is the same file and is SHARED (one copy a level), and
    pages live in the level scope.
  - `mdk_render_draw_mesh` sends each draw of a strip to its frame's page before
    it is queued (pica::stripPage: page, frame in page, frames in page), so the
    prepare, both eyes' submits, the coalescing and the pending path all see an
    ordinary packed strip; a826ee43's compressedFrameUv picks the frame.
  - New state: `C3dTexture::pages` + `framesPerPage`. Lines added: 152 C++ (05_resources
    74, pica_matrix 34, 06_queue 19, 01_types 8; the rest signature/comments; 18
    removed), 51 test, Python +78/-27.
- **Not changed:** every texture at or under 1024, the HD rule (an animated
  texture still takes no HD copy, so the owner's SD pack is not involved and nothing
  has to be re-uploaded: the orig pack is in the RomFS with the binary), filter/mip
  policy, blending (sprites stay DoubleSided cut-outs, the C#'s).
- **Evidence (host):**
  - `testTheStripPages` (tests/scripts pica_matrix_tests): every frame of the three
    geometries lands on its page/frame, and every pixel centre of a 1:1 quad drawn
    through that page's rectangle under NEAREST reads exactly that strip frame and
    row (page boundaries 7/8, 15/16, 23/24, 13/14, 27/28 included); out-of-range
    frames clamp.
  - `arena_etc1_convert.py --check`: every page's ETC1A4 alpha is the colour key
    texel for texel at the top-left. hw-logs/strip_frames_ref_vs_pages.png: the
    indexed expansion beside the page's encoded frame for EXPLODE 0,1,7,8,15,16,23,
    24,25, SL_BIG 0,1,15,16,29, FIRE 0,1,13,14,27,28,35 -- same order, orientation
    and alpha silhouette; mean RGB error 2-10/255 (the existing ETC look).
  - tests/formats 200624 checks 0 failures; tests/scripts make test PASS;
    tools/check.sh all gates PASS (smdh SKIP, no .cia; frames PASS on the Linux
    build). A read-only review agent over the diff found no serious issue.
- **Evidence (hardware, old3ds-lan167, Old 3DS 268 MHz L2 off, stereo on,
  throwaway/restore-strips-hw{,-before}, LEVEL=8 (in-game level 5) PROFILE=1
  AUTOQUIT=90, BLAST=20: an explosion every 20 steps in front of Kurt -- left,
  centre, right; near and far -- plus a wound on the nearest object; HD on with
  the level's HD set from romfs:/hd (HDROMFS=1, the owner's config; tier 256, 295
  copies resident), every cycle FRESH):**

  | | before (a826ee43) | after | delta |
  |---|---|---|---|
  | linear free, min in play (sampled /15 steps) | 3246 KiB | 2702 KiB | -544 KiB (= EXPLODE 416 + SL_BIG 128) |
  | linear free at loop entry | 6942080 | 6516096 | -416 KiB (EXPLODE warmed at load) |
  | main free (cap - used), min | 10770 KiB | 10756 KiB | -14 KiB |
  | strip draws sent to a page | 0 | 9249 | |
  | textures evicted / main-thread pack reads / failures | 0 / 0 / none | 0 / 0 / none | |
  | steady frame mean | 33.93 ms | 34.54 ms | +0.61 ms |
  | p95 / p99 | 35.5 / 44.6 ms | 41.5 / 46.2 ms | |
  | frames of 3 vblanks / 4+ | 67 / 0 | 118 / 0 | |

  Without the HD set (SD has none on the appliance): linear min 10771 -> 10227 KiB
  (-544), steady 33.85 -> 34.55 ms, p95 34.9 -> 41.4 ms, 23 -> 23 main-thread pack
  reads (all at level start, the same in both: not the strips'), 0 evictions.
  The frame cost is the restored explosions' fill (2-3 128x128 sprites of up to
  ~1/3 screen, overlapping, both eyes), the stress scene's: a game explosion is one
  at a time.
- **Cost by level** (exact page bytes, one shared copy, resident once warmed):
  levels 6-8 EXPLODE + SL_BIG = 544 KiB; levels 3-5 add FIRE = 736 KiB. On the
  tightest level measured (LEVEL8 = in-game 5) the route's linear minimum goes
  3.17 -> 2.64 MB; the earlier "about 2.2 MB" minimum on other LEVEL8 routes would
  be about 1.7 MB. Nothing else was dropped (0 evictions, same HD set, 0 failures).
- **Not verified:** the in-play look on hardware (the stereo screenshots of the
  restored explosion failed: ATM did not commit the sets); other LEVEL8 routes'
  minima; FIRE on hardware; no Mandarine run.
- **Commit:** this one.

### Restore-strips hardware follow-up: the appliance stopped answering after the screenshot run
- **What happened:** after the profile A/B above, a non-profile run of the same
  payload (LEVEL=8, BLAST=20, HD set) was pushed (transfers.log bytes_out
  248731656 verified) and five `atmctl screenshot` sets were asked for mid-run; ATM
  answered each "not committed within 30s", and from then on :8080 and :17491 never
  answered (:17492 open, ping and ARP fine). Trap 8 run in full from this
  workstation: directed discovers, then `cmd reboot` twice with 5 minutes of
  discover+poll after each -- no body, no reboot taken. The device log of that run
  could not be pulled, so whether the game hung or ATM wedged is NOT KNOWN. The
  same code had just completed four profile cycles FRESH (two without the HD set,
  two with it). Needs a physical power cycle; the operator was not asked.
- **Consequently not done:** the stereo screenshots of the restored explosion and
  wound (frame order/orientation in play, both eyes, page boundaries), the level 3
  A/B (FIRE + EXPLODE + SL_BIG = 736 KiB; payloads were built), other LEVEL8
  routes' minima. No Mandarine run either: two Mandarine instances of another
  project were running.
- **Kept:** the logs, payloads (md5 in the report) and the throwaway branches as
  patches (hw-logs/throwaway-patches: the harness, the BLAST hook + counters, the
  HDROMFS root); throwaway/restore-strips-hw{,-before} deleted.
- **Player builds:** perf/3ds-restore-strips 22d11f12 mdk.3dsx md5
  c9445927e8712b98dfbb0a5b25d036a9; test/3ds-restore-strips-fast 7da914a7
  (22d11f12 + 0fbced92 FAST 3DS) md5 8b1623f16b37f122c09cc76c35dbf4e8; both 0
  compiler warnings, in .git/worktrees/restore-strips/hw-logs.
- **Commit:** this one (ledger only).

### Restore-strips: the console back -- the look on hardware, the level 3 A/B, and an HD pairing the strips must not take
- **The look (hardware, old3ds-lan167, LEVEL8, HD set, BLAST=20):** a both-eye
  readback at frame 600 (`hw_cycle --capture`, cycle FRESH; the throwaway branch
  carries fix/3ds-hd-only's writeScreenshotBmp, which main's harness lacks):
  hw-logs/after-cap600-stereo.png, -zoom.png. One whole EXPLODE frame -- a round
  yellow-orange fireball, upright, its colour-key edge clean, no bands of a
  neighbouring frame and no squash -- the SAME frame in both eyes at the stereo
  offset, a second explosion half behind the wall corner. Before the change that
  route drew no explosion at all (the strip was refused).
- **Level 3 A/B (FIRE's levels; HD set from romfs:/hd, tier 256, BLAST=20,
  PROFILE, both FRESH):** linear free min 7270 -> 6684 KiB (-586), main min
  13203 -> 13189 KiB, steady 33.45 -> 33.72 ms, p95 35.0 -> 36.3 ms, 3-vblank
  frames 11 -> 25, 0 evictions, 0 main-thread pack reads, 0 failures, 10766 strip
  draws. FIRE's 192 KiB is resident only once something draws or warms it (the
  route did not show a FIRE object); EXPLODE + SL_BIG account for the 544.
- **Found in that run and fixed here:** byname now names EXPLODE, and the HD
  pack holds a squeezed LEVEL3 EXPLODE (256x6656 -> 256x512), so the level's
  pinned HD set took one more copy (178 vs 177, +43648 bytes at tier 256) that no
  draw can use -- an animated texture never draws an HD copy. loadOrigByName
  (packs_3ds.cpp) no longer pairs a strip over 1024 with an HD file. +7/-2 lines.
- **Evidence:** tests/scripts make test, tests/formats 200624 / 0, tools/check.sh
  ALL GATES PASSED (smdh SKIP), player .3dsx 0 warnings. The fix itself is not
  yet re-measured on the console (the L3 set count should read 177 again).
- **Commit:** this one.
- **Addendum, second readback (frame 1213, FRESH):** hw-logs/after-cap1213-stereo.png
  -- a farther, smaller explosion, again one whole upright frame with a clean edge,
  identical frame in both eyes. No SL_BIG wound and no FIRE were in either picture
  (not seen on hardware). The throwaway branches are deleted again; their patches,
  the capture wiring included, are in hw-logs/throwaway-patches.

## 2026-11-26 -- adjacent halos with the same tint are one draw per eye (branch perf/3ds-halo-batch): KEPT

- **Owner's direction:** the outside review's ticket H halo batch
  (`.git/mdk-review/mdk_3ds_stereo_performance_spec.md`), kept only if it saves about
  0.3 ms of the Old 3DS frame in a real heavy-explosion scene; count first, drop it if
  runs are rare.
- **What:** `drawGlowBillboards` (06_glow_billboards.inc) draws each maximal run of
  adjacent halos whose three colour floats are bit-equal (`memcmp`) as ONE
  `C3D_DrawArrays(first * 6, run * 6)`, `GPU_GEOMETRY_PRIM` under the pass-through
  program and `GPU_TRIANGLES` otherwise, as before. Same tint uniform (equal colour
  bits times the same strength are the same bits), same vertex buffer, same triangles in
  the same order, same program/blend/depth/texture state: the same pixels by
  construction. `billboardsDrawn` still counts logical quads. +12 -4 lines, no new state.
- **Counted first (host):** `tick1 dump effects` (the L8 debris storm, six cameras, 400
  steps), the backend's first 64 halos per frame: 52,460 quads in 2,114 runs (24.8 per
  run); the 64-quad peak is ONE run. A sprite's halo colour is per texture entry
  (EffectsView::queueSprite), so an explosion's sprites share it and are queued together.
- **Measured (old3ds-lan167, CFGU model 0, 268 MHz, L2 off, stereo on, cap 30, HD on
  from RomFS):** in-run A/B on throwaway/halo-shadow-hw (frames alternate batched /
  one-each in pairs; the halo loop timed with svcGetSystemTick, both eyes).
  Heavy-explosion route (repeatable, FIXED=1 so the run is deterministic: identical
  counts in three runs): `make -C mdk_3ds AUTOQUIT=90 PROFILE=1 LEVEL=7 HDROMFS=1 FIXED=1
  AT=100,2320,-60,90 ARENA=DANT_5 RIDE=XE TOGGLES='enableHdTextures=1;texture16=1;
  kurtCompressed=1;cap30Fps=1;enableHaze=1;enableFiltering=1;enableMipmaps=1;
  enableSkyShadows=1' WALK='forward@820-2600;turnright@1200-1260;turnleft@1800-1860;
  fire@800-802;fire@860-862;...;fire@2480-2482'` (the bomber, a bomb every 60 frames; 29
  fire segments; the harness needs the AT/ARENA/RIDE wiring of the throwaway branch).
  Halos per frame: 14.4 quads both eyes in 2.2 runs (85% of draws removed), peak 41
  quads/eye in 2 runs. Halo CPU per frame, one-each -> batched (two runs):

  | halos/eye | frames | one-each | batched | saved |
  |---|---|---|---|---|
  | 24-47 (mean 35) | 15+15 | 0.839 / 0.802 ms | 0.107 / 0.114 ms | **0.69-0.73 ms** |
  | 8-23 (mean 12) | 27+27 | 0.309 / 0.302 ms | 0.073 / 0.073 ms | 0.23 ms |
  | 1-7 (mean 3) | 134+135 | 0.108 / 0.110 ms | 0.064 / 0.066 ms | 0.04 ms |
  | all halo frames | 176+177 | 0.201 / 0.198 ms | 0.069 / 0.071 ms | 0.13 ms |

  About 10.5 us of CPU per draw saved. GPU time (C3D_GetDrawingTime) on halo frames
  without a shadow recast: 13.79 -> 13.68 ms and 13.80 -> 13.67 ms (n164 / n88, within
  noise). Frame period unchanged: the Old 3DS holds the 30 fps cap here (98-99% of
  frames 2 vblanks), so the saving is CPU headroom in explosion frames, not a faster
  frame. Logs: `.git/worktrees/halo-shadow-hw/hw-logs/2026-11-26-bomber{-1,3-1,3-2}*`.
- **Verdict:** kept -- 0.7 ms in the heavy-explosion frames for 12 lines and no state.
- **Not verified:** a pixel comparison on the console or in Mandarine (the machine's
  emulator slot was held by other projects' instances; the harness here has no capture
  readback wired). Identity rests on the construction above. Not measured on a New 3DS.
- **Evidence:** tests/scripts `make test` passes, tests/formats 200624 checks 0 failures,
  tools/check.sh ALL GATES PASSED (gates_frames PASS on the Linux build, gates_smdh SKIP:
  no .cia); player .3dsx (devkitARM 15.2.0) 0 compiler warnings.

## 2026-11-26 -- SHADOW REFRESH AUTO is about 15 Hz at any frame rate (branch perf/3ds-shadow-cadence): KEPT

- **Owner's direction (verbatim "lets do C as well"):** with SHADOW REFRESH on AUTO,
  redraw the sun map every 4th presentation at 60 fps and every 2nd at 30 (about 15 Hz
  both), knowing moving shadows update half as often at 60. Manual 1-4 and the
  immediate recasts unchanged.
- **"Running at 60", defined by elapsed time, not by a mode:** AUTO recasts once two
  presentations AND 3.5 vblanks (`kShadowAutoTicks` = 15,684,000 ticks of the 268 MHz
  system tick, 58.5 ms) have passed since the last cast (`pica::shadowRefreshDue`). At 60
  that is every 4th presentation (3 presentations = 50 ms < 58.5 <= 66.8), at 30 every
  2nd (66.8 ms), slower than 30 still every 2nd (the old AUTO). Why time: the cap30Fps
  setting says what the frame rate is allowed to be, not what it is (a New 3DS with FAST
  3DS off, or a busy scene, runs uncapped at 30-45); a measured-rate mode would need
  hysteresis for a rate wandering across the boundary. The time rule needs neither: a
  wandering rate gets about 15 Hz, never fewer than one cast per 4 presentations nor more
  than one per 2, and 3.5 vblanks leaves ~8 ms of clock jitter either side.
- **What:** `pica::shadowRefreshDue(setting, framesSince, ticksSince)` (pica_matrix.{h,cpp},
  host-compiled), called by `prepareShadows` in place of `frame - castFrame >= every`; one
  new field, `shadowCastTick`. The moved/sunTurned/rigChanged/fresh/native recasts are
  untouched. +31 -8 lines of game code (about 10 of them code, the rest comments), +57
  lines of test.
- **Host test (tests/scripts pica_matrix_tests, in vm_tests):** 120 simulated
  presentations: at 60 fps every 4th (30 casts, also with the clock read 2 ms early/late),
  at 30 and at 20 every 2nd, a rate wandering 1/2 vblanks gives 40 casts never more than 3
  apart, 60 with a 6-vblank hitch never more than 4 apart, manual 1-4 every N at 60 and 30,
  and the 3/4-vblank edges. Non-vacuity: with the old rule (`framesSince >= 2`) five checks
  fail (the 60-fps, wobble, hitch and edge cases).
- **What it is worth (old3ds-lan167, CFGU model 0, 268 MHz, L2 off, stereo on, cap 30, HD
  on from RomFS):** the Old 3DS holds 30, where AUTO does exactly what it did, so the
  60-fps branch cannot be exercised there; the cost of a cast was measured instead, on
  throwaway/halo-shadow-hw: each frame's GPU time (C3D_GetDrawingTime) charged to that
  frame's cast/reuse class, prepareShadows+castShadows timed, and the cadence forced to
  every 2nd / every 4th in alternating 256-frame blocks.

  | scene | casters/frame | GPU, cast frame vs reuse frame | CPU prepare+cast, cast vs reuse | forced every-2 vs every-4 blocks, GPU mean |
  |---|---|---|---|---|
  | L8 start (sunlit, live 100%), `LEVEL=8 FIXED=1 WALK='turnright@300-420;turnleft@1200-1320;forward@1800-1900'` | 3.9 | 15.68 / 15.57 vs 14.82 / 14.80 ms: **+0.8 ms** | 0.41 vs 0.02 ms: **+0.39 ms** | 15.22 vs 15.00 ms (**-0.21 ms**) |
  | L7 DANT_5 (bomber route of the halo entry), three runs | 3.1-3.3 | 14.0-14.2 vs 13.4-13.5 ms: +0.6-0.7 ms | 0.34-0.36 vs 0.02 ms | within the scene's own variation (+0.07 / -0.14 ms) |

  So at 60 fps AUTO's change (every 2nd -> every 4th) removes one cast per 4 presentations:
  about **0.2 ms GPU + 0.1 ms CPU per presented frame on average** at Old-3DS speeds in these
  scenes, more with more casters. It does NOT make a cast frame cheaper: a frame that
  misses 16.7 ms only because it casts now misses half as often, not never.
  Logs: `.git/worktrees/halo-shadow-hw/hw-logs/2026-11-26-{l8-1,bomber2-1,bomber3-*}*`.
- **What only the owner's New 3DS can show:** that AUTO takes the 60-fps branch in play
  (cap off, FAST 3DS on), the saving at 60 in a GPU-bound stereo frame, and whether
  moving casters (enemies, Kurt, doors) updating at 15 Hz instead of 30 are acceptable to
  the eye. The New 3DS's GPU runs at the Old's clock, so the per-cast GPU cost above
  should carry over; its CPU cost is about a third.
- **The real AUTO path on the console (2026-11-26 05:4x, after the appliance came back):**
  schedule unforced, cap off (`cap30Fps=0`), L8 start route above, same throwaway
  instrumentation (`HWAB_SHADOW=0`, 9ffe711d). The Old 3DS ran mixed: 1,332 one-vblank and
  1,709 two-vblank frames of 3,071 (mean 29.0 ms). AUTO cast 1,040 times (one per 2.95
  presentations; 11.7 casts a second, immediate recasts included), where the old AUTO's
  every-2nd rule would have cast ~1,535. A cast frame cost +0.84 ms GPU (15.67 vs 14.83 ms)
  and +0.40 ms CPU (0.415 vs 0.018 ms), so here AUTO saved about 0.14 ms GPU + 0.06 ms
  CPU per presented frame. Note the rate: with frames of 1 and 2 vblanks mixed, a cast
  waits for 4-6 vblanks (8.6-15 Hz), so a wandering rate lands BELOW 15 Hz, not at it;
  pure 60 is 15 Hz, pure 30 is 15 Hz. Log: `.git/worktrees/halo-shadow-hw/hw-logs/
  2026-11-26-l8auto-3*` (FRESH, push verified, 247,980,288 bytes).
- **Evidence:** tests/scripts `make test` passes (PicaMatrix PASS), tests/formats 200624
  checks 0 failures, tools/check.sh ALL GATES PASSED (gates_frames PASS on the Linux build,
  gates_smdh SKIP: no .cia); player .3dsx (devkitARM 15.2.0) 0 compiler warnings.
- **Commit:** this one.

## 2026-11-26 -- the sky as de-dithered ETC1 with HD on (branch perf/3ds-sky-etc1)

### The sky's slices from build-time ETC1 copies, the 8-bit dither removed
- **What:**
  - `arena_tex_export` (host) also writes each level's sky: the wrap through the
    DTI palette (index 0 remapped as the dome remaps it) and every slice exactly
    as `buildSkyDome` cuts it (the same code, linked into the tool).
  - `arena_etc1_convert.py convert_skies`: the WHOLE wrap is de-dithered (a
    gaussian, sigma 1.0 texel, wrapped across the 360-degree seam, clamped at the
    top and bottom rows), each slice cut by the dome's rule (checked equal to the
    exported slice before the blur), padded to 1024x512 by repeating its last row
    and column, and encoded `tex3ds -f etc1 -q high -z none` with no mip chain
    (the sky is drawn Original-shaded, level 0 only). Output
    `build-arena-etc1/sky/level<L>_<k>.t3x` + `sky/manifest.txt` (incremental);
    `stage_romfs.sh` links them as `romfs:/orig/sky/`.
  - `buildSky3ds(..., level, hd, ...)`: with the HD toggle on a slice is
    `mdk_render_create_compressed_texture` of its .t3x (read whole at level load)
    and its indices are never expanded; with HD off, or no file, the 16-bit
    expansion exactly as before. The pause menu's HD toggle remakes the sky
    (`releaseSky3ds` + `buildSky3ds`), so the two are never both resident.
- **Why:** the owner saw the 8-bit palette dither in the sky with HD on ("the
  sky isnt part of the HD recompression to ETC1 ... We definitely need to fix
  that"). The sky is not in the HD pack (`TextureExport.cs`: "Not exported: the
  sky"), so the fix is the original art de-dithered, not an HD source. Merely
  re-encoding the dithered art keeps the dither and smears it into 4x4 blocks
  (measured: ETC1 of the raw art rmse 5.6 against the art, of the de-dithered
  art 2.1 against its source).
- **Memory:** levels 3, 4, 7, 8 have two slices (1024 + 780 stored columns),
  levels 5, 6 one (902): 2 MiB / 1 MiB of 16-bit store become 512 KiB / 256 KiB
  of ETC1 (1.5 MiB / 0.75 MiB of linear saved). The main heap keeps the .t3x
  bytes (256 KiB a slice) instead of the index source (360 KiB a full slice).
  RomFS +2.5 MiB.
- **Evidence (host):** every level's sky compared at screen scale (~1:1; the
  3DS shows ~0.75 px per sky texel) and 3x, current 16-bit look vs the built
  .t3x decoded: `.git/worktrees/sky-etc1/hw-logs/sky-compare/level<L>_compare.png`,
  seam close-ups `level<L>_seams.png`, full panoramas `level<L>_{original,now565,etc1}.png`,
  numbers `report.txt`. The dither is gone on all six; no new banding; the step
  across the slice boundary and the 360-degree wrap is the image's typical
  column step (e.g. level 3: 1.21 / 1.38 vs 1.13); the edge rows meeting the
  flat caps keep their colour (mean within 2 levels). CPU readers of the sky
  (`level_light.cpp`, `sky_modes.h`) read the DTI, untouched. Linux untouched.
  3DS build 0 compiler warnings; tests/scripts `make test` rc 0; tests/formats
  200624 checks, 0 failures; tools/check.sh ALL GATES PASSED (gates_smdh SKIP).
- **Not verified here:** Mandarine (another project's instances were running);
  hardware: see the follow-up entry.
- **Commit:** this one.

### The ETC1 sky on the Old 3DS appliance (hardware follow-up)
- **What:** level-3 runs on `old3ds-lan167` (CFGU model 0, 268 MHz, L2 off,
  stereo on), this branch vs its base 72cfb30b, each with the harness
  cherry-picked (565b08b3) on a throwaway branch (`throwaway/sky-etc1-hw`; the
  base as a detached worktree), `AUTOQUIT=90 LEVEL=3`, HD toggle on (default),
  pushed by `hw_cycle.sh` (FRESH both). The harness on this base has no
  framebuffer capture (`CAPTURE` is wired in the render files it leaves out), so
  the pictures are ATM screenshots taken ~45 s into each run.
- **Evidence:** `.git/worktrees/sky-etc1/hw-logs/`:
  - `hw_l3_base_vs_new.png`: the same level-3 start view, base vs new, with a 3x
    sky zoom: the base's sky shows the dither speckle, the new one is smooth, same
    colours and cloud shapes; no seam in view.
  - `hw-new-l3.session.log`: `sky: ... 2 ETC1 slices (hd=1)`; linear consumed
    between `mem[level-data]` and `mem[arena-view]` 712,832 B against the base's
    188,544 B: the two ETC1 slices are 524,288 B, made at load. The base's two
    16-bit slices are made at their first draw (after `mem[loop]`, so no probe
    sees them): 2,097,152 B by the store's arithmetic. Frame 30's resident
    texture bytes: 8,882,048 (new) vs 10,652,096 (base). This harness build
    logs no in-play linear-free figure (the profile's linear-free minimum reads
    0: its first-use counters are not on this base), so the 1.5 MiB saving is
    the arithmetic, consistent with these readings, not a measured minimum.
  - `hw-{new,base}-prof.cycle.txt` (`PROFILE=1`, OG): steady mean 25.10 ms vs
    25.42 ms, p95 27.8 both; frame 1 484 ms vs 1075 ms (the base's first frame
    includes the two sky expansions among other first-use work, not isolated).
- **Not tested on hardware:** toggling HD in the pause menu (the remake path;
  the harness's HDFLIP does not go through the menu), levels other than 3.
- **Commit:** this one (ledger only).

## 2026-11-26 -- the README and the build, for an end user with their own MDK (branch build/docs)

- **What:**
  - `README.md` rewritten for the C++ port (it still described the C# port): what the port is,
    the prerequisites in one place, the 3DS and Linux build steps, installing, the default
    controls, the music, today's HD pack steps (marked as pending the C++ texture tool, which
    is being written on `build/hd-pack-native`; `build_hd_pack.sh` was not touched), tests,
    layout. Upstream credit (nautilusbits/mdk-sdl, mdk-godot), contributors and licence kept.
    The Legal section now says that `mdk_3ds/music/` is the exception to "no game data here".
  - `tools/check_prereqs.sh 3ds|linux`: one line per missing item, exit 1. `make -C mdk_3ds`
    runs it (order-only `prereqs`) before compiling, so a missing package or missing game data
    fails in the first second, not after the compile.
  - `stage_romfs.sh`: no game data, a failed Kurt conversion or a failed arena conversion now
    stop the build (before: a warning and a degraded `.3dsx`). `MDK_ARENA_ETC1=0` /
    `MDK_KURT_ETC1=0` still skip a conversion on purpose. Without `mdk_3ds/music` the music is
    converted from the user's own data (`music_dsp_convert.py <data> <data> build-music/`,
    host compiler via `HOST_CC`, since the 3DS Makefile exports `CC=arm-none-eabi-gcc`).
  - `MDK` was committed as a symlink to `/mnt/nas/Development/mdk-sdl/MDK` (0fec7245): untracked
    (the owner's data folder untouched); `.gitignore` `/MDK/` -> `/MDK`, which also matches a
    symlink.
  - `mdk_linux/third_party/{vulkan,volk}` were also committed as absolute symlinks into the
    owner's checkout, so a fresh clone could not configure: the pinned copies (VK_HEADER_VERSION
    360, volk 360, with their licences; 2 MB) are now committed, as the third_party README
    always said. The stale `.gitignore` lines (a `fetch_third_party.sh` that does not exist) went.
- **Why:** owner, 2026-11-26: "update the documentation and build process, update the readme
  accordingly"; the goal was a fresh clone that builds end to end from the README.
- **Evidence (fresh clone of this branch into the scratchpad, /tmp, a different filesystem from
  the data, so the RomFS is copied, not hard-linked; data given only as `MDK_DATA_DIR`):**
  - Without data: `make -C mdk_3ds` stops at `prereqs` with `MISSING: game data: no <clone>/MDK`.
  - `make -C mdk_3ds -j16`: 887 s wall at nice 19 on a machine at load average 20-140 (other
    agents), 0 compiler warnings; `mdk.3dsx` 234,688,932 B; 1,030 arena images + 10 sky slices,
    455 Kurt frames, 0 failed; 50 music tracks.
  - `cmake -S mdk_linux -B build-linux && cmake --build build-linux -j16`: 2.3 s + 81.7 s,
    0 warnings. `build-linux/mdk` on `DISPLAY=:99` (Xvfb, lavapipe) reached the main menu and
    ran 660 frames until the 25 s timeout.
  - Music absent (`mdk_3ds/music` moved aside): rebuild 46.5 s, 47 tracks (23 MB), `.3dsx`
    188,206,836 B. Every generated track is byte-identical to a standalone run; the 11 tracks
    the committed set took from the original data are byte-identical to the committed files;
    the other 36 differ (the committed ones are the fan soundtrack), and the soundtrack's 3
    extra names (level 3 AMB_DRIP, H9WATER; level 7 TRACK12) are absent.
  - Failure paths: `TEX3DS=/nonexistent` with an empty work dir stops at Kurt (and with
    `MDK_KURT_ETC1=0`, at the arenas) with `stage_romfs: ERROR: ...` and make exit 2; both
    opt-outs together stage 117 files and exit 0; a missing host C compiler with no
    `mdk_3ds/music` stops at `prereqs`.
  - In the fresh clone: `make -C tests/formats` 200,624 checks, 0 failures;
    `make -C tests/scripts test` exit 0, 996 PASS, 0 FAIL, 4 SKIP (the frame gate's oracle
    arms: no oracle frames in /tmp/mdk-frame-reference), 2,913 s under load.
    In the worktree: `tools/check.sh` ALL GATES PASSED (smdh and frames SKIP: no .cia, no
    Linux binary there).
- **Not verified:** the `.3dsx` was not run (no console and no emulator in this task), neither
  with the committed music nor with the generated music; the `.cia` path; the HD pack steps
  (documented from the script, not run); `dkp-pacman -S 3ds-dev 3ds-zlib` on a clean machine
  (the package list is from `pacman -Q` here); SDL3 from a distribution package.
- **What it would take to drop the committed music** (owner's decision; nothing deleted, no
  history rewritten): the build already generates the original-data set when the folder is
  absent, so it is `git rm -r mdk_3ds/music` plus the README sentence. The cost to players: the
  fan soundtrack (36 replaced tracks, 3 extra) is gone unless they supply it themselves --
  `music_dsp_convert.py <soundtrack> <data> <out>` already takes it as its first argument, and a
  `MDK_SOUNDTRACK_DIR` hook in `stage_romfs.sh` would be one line. ffmpeg with libsoxr becomes a
  build prerequisite. The files stay in the public fork's history either way; removing them
  from history is a separate, destructive decision.
- **Commit:** this branch.

## 2026-11-26 -- the HD texture pack without .NET (branch build/hd-pack-native)

### build_hd_pack.sh: a C++ export and a Python mod step replace the C# `--upscale-textures`
- **What:**
  - `mdk_3ds/tools/hd_export/hd_tex_export.cpp` (+ Makefile, host build like
    arena_etc1/ and kurt_etc1/): reads every texture and 2D image the HD mod
    replaces out of the game data through mdk_core's loaders, in the C#
    generator's order (TextureExport.Collect, Kurt, CanvasExport.Level, Menus;
    fonts skipped), keyed by `hdKeyOf`; writes `index.tsv` and one `<key>.rgba`.
  - `mdk_3ds/tools/build_hd_mod.py`: everything after the export, as
    HdGenerator/UpscaleImages do it: the pinned Real-ESRGAN v0.2.5.0 release
    downloaded once into its own empty folder, SHA-256 checked, unpacked into
    another empty folder (only the upscaler binary is run); manifest (format=1,
    the C#'s layout, so a C#-made mod is picked up as it is), mod.txt, per-level
    prune, incremental skip, first-level-wins for shared keys; bleed (4-neighbour
    rings) + 8-texel wrap; `-n realesrgan-x4plus -s 4`; margin crop, 2x2 box,
    hard/soft bilinear alpha; RGB or RGBA PNG.
  - `build_hd_pack.sh` runs export -> mod -> `hd_pack_convert.py`; header, the
    converter's header and `docs/HD_TEXTURES_3DS_SPEC.md` say what it needs now:
    host C++ compiler + zlib, Python 3 with numpy and Pillow, a Vulkan GPU,
    network once, tex3ds. No .NET. `src/` is unchanged (still the reference).
  - **mdk_core fix:** `Texture::parseAnimated`'s delta form (only M_COMM, levels
    4 and 7) applied change k to frame k in place; the C# builds frame k+1 from
    frame k. C++ frame 0 was altered and frames 1-5 mostly empty. Fixed; new
    synthetic test `AnimatedTextureDeltasBuildEachNextFrame` (19 failures on the
    old code, 0 on the fix).
- **Why:** owner: "that old C# program should probably be in C++ if that is
  possible"; then the owner's design: C++ only for reading the game data, Python
  for the rest, no new requirements. Parity with the C# was the requirement.
- **Evidence** (scratchpad; .NET 10.0.404 SDK fetched there only to run the
  reference, sha512 checked):
  - With a deterministic stand-in upscaler (nearest 4x + a position pattern, so
    the box filter's rounding is exercised) in both the C# and the native run,
    levels 3-8: all 1417 pre-upscale frames pixel-identical; all 1382 mod images
    identical in pixels and colour mode; manifest.txt and mod.txt byte-identical;
    per-level size lines identical. Before the delta fix, M_COMM's 12 frames were
    the only difference (other keys).
  - Incremental: two deleted images re-made, a stale level-3 entry pruned with
    its file, a no-change run makes 0 -- identical trees/manifests on both sides.
  - Real run, `build_hd_pack.sh` end to end on the idle RX 6600 (0% busy before):
    download + SHA-256 + unpack, 1417 frames upscaled in 492 s, 33 min in all
    under a loaded host; the 3DS pack's 1382 `.t3x` are **byte-identical** to the
    existing `build-hd-pack/` (made by the C# path 2026-11-17/18): same set,
    names, formats, sizes. The manifest's `src_sha256_16` (a hash of the mod
    PNG's bytes) differs on every line: Pillow's zlib stream is not .NET's (the
    pixels are the same), so converting a new mod into an old C#-made pack
    re-encodes everything once, to the same bytes.
  - `make -C tests/formats` 0 failures; `make -C tests/scripts test` pass (Linux
    built first); `tools/check.sh` all gates pass (smdh skipped, no .cia).
- **Upscaler non-determinism:** the stand-in upscaler isolates our pre/post
  processing from the GPU (bit-exact). On this GPU and driver Real-ESRGAN was
  also repeatable: the 2026-11-18 C# pack and today's native pack are identical.
  Another GPU or driver may give different upscaler pixels; that is not ours.
- **Not done / differs:** the C#'s `--hd-model=anime` / `--hd-scale=4` and the
  settings.cfg upscaler source are not carried (the build only used the
  defaults; MDK_REALESRGAN_URL/SHA256 replace the source); the old
  `textures-hd/` migration does not apply (no user folder). The 3DS shows the
  delta fix only after its orig ETC1 set (romfs orig/, arena_etc1_convert.py) is
  rebuilt: that set still holds M_COMM from the old decode under the old key,
  and its by-name index serves it. Not run on a console or in Mandarine.
- **Commit:** this one.

## 2026-11-26 -- second README pass: the native HD generator (branch build/docs)

- **What:** merged `build/hd-pack-native` (bd9e3f66: no .NET; `hd_export/hd_tex_export` +
  `build_hd_mod.py`; the `Texture::parseAnimated` delta fix) into `build/docs`, this ledger
  unioned. README's HD section rewritten for that flow (no .NET anywhere in the README, the
  "being replaced" note gone); `tools/check_prereqs.sh hd` (Python modules, host C++ and zlib,
  tex3ds, `libvulkan.so.1`; 8 lines) and the README names it.
- **Why:** owner's plan: "we will update it again once the C++ texture conversion tool is
  finished".
- **Evidence:**
  - Stale arena cache after the decode fix: the first fresh clone's `build-arena-etc1/` (made
    before the fix) was brought to this branch and rebuilt incrementally (`make -C mdk_3ds`,
    32.8 s): `1030 images, 2 encoded now, 1028 kept`. The two re-encoded files are M_COMM's two
    palette views; their keys moved (`75e6a954...`, `b1861aaf...` -> `33b2ce90...`,
    `2abcfc49...`), `byname.txt` points at the new files and the old `.t3x` were deleted. No
    fix needed: the key is the hash of the decoded indices, and the exporter is rebuilt from
    `texture.cpp` by its dependency files, so a fixed decode makes a new key.
  - The pinned Real-ESRGAN URL downloads (46,931,474 B) with the pinned SHA-256.
  - HD steps from a second fresh clone, README order: `tools/check_prereqs.sh hd` passed;
    `build_hd_pack.sh --levels 3` exit 0 in 399 s at nice 19 under load: export 561 level-3
    textures + 27 menu images, mod 578 textures, pack 578 (400 ETC1A4, 178 ETC1, 0 failed),
    `build-hd-pack/manifest.txt` present. **The upscaler was a stand-in** (nearest 4x, same CLI,
    via `MDK_REALESRGAN_URL=file://...` and its SHA-256), because two other projects' emulators
    were using the GPU; the real Real-ESRGAN run and the pack on a console are not verified
    here. The owner's `build-hd-pack/` was not touched.
  - Clean 3DS build of the merged branch in the second fresh clone: `make -C mdk_3ds -j16`
    exit 0, 725 s at nice 19 under load, 0 compiler warnings (only make's pre-existing
    "jobserver unavailable" notice from the exporter sub-make), 1030 arena images, 455 Kurt
    frames, 50 music tracks.
  - After the merge (first clone): `make -C tests/formats` 200,650 checks, 0 failures; Linux
    rebuild 0 warnings; `make -C tests/scripts test` exit 0, 996 PASS, 0 FAIL, 4 SKIP (the
    frame gate's oracle arms); worktree `tools/check.sh` ALL GATES PASSED.
- **Commit:** this branch.

## 2026-11-26 -- HD through the bubble: the HD window, an uncompressed pack, 30 MiB linear (branch perf/3ds-hd-bubble)

- **What:**
  - The level-wide pinned HD set (`pinHdSet3ds`, `holdHdSet3ds`, `mdk_render_pin_hd_set`) is
    replaced by **the HD window** (`mdk_3ds/src/hd_window_3ds.{h,cpp}`): Kurt's arena, its doorway
    neighbours and the arenas its trigger boxes and scripts name (opcodes 100/112/173/223, DTI
    records 1/3); the drawn arenas are always in it and their files are asked for first. At
    level load every arena's files (its name-index rows) are sized at 512/256/128 and every
    window is checked against the free linear heap less the 7 MiB play reserve; an arena whose
    window does not fit steps down, never the level (`buildHdWindow3ds`). In play the frontend
    re-asks the window when Kurt's arena or the drawn list changes (`mdk_render_set_hd_window`,
    the queue rebuilt in the window's order) and, if a drawn arena's files are still to come,
    the frame waits for them (`holdHdWindow3ds`, the clock rebased) -- logged, counted, and
    expected to be rare. `drawsHdOnly` no longer falls back to the original for a copy outside
    the set. A `hd: level end` line counts waits, their milliseconds, and draws of an HD-only
    texture without its copy (`hdDrawnMissing`, must be 0).
  - **The HD pack ships uncompressed**: `hd_pack_convert.py` encodes with `tex3ds -z none`, and
    `--from-pack` rewrites an older pack without its mod (LZ10/LZ11/Huffman/RLE unpacked in
    Python; `--check-unpack` proves it byte for byte against tex3ds' own `-z none` for every
    compression). README, `build_hd_pack.sh` and the HD spec say so.
  - **Linear heap 28 -> 30 MiB** (`heap_split_policy.h`, its tests).
  - The per-texture cache clean on import (fix/3ds-mip-chain 68e52d7e's two hunks) is part of
    the import path; `kHdImportsPerFrame` 4 -> 2 from the import's measured cost.
  - docs/3DS_ARCHITECTURE.md 3-5: the window rule, the heap split, the uncompressed packs.
- **Why:** owner 2026-11-26, approving the design report: "streaming the HD textures through
  the bubble ... that should allow every level have 512x512 textures"; no pop-in, no doorway
  hitches, one design for both models, plain code; the window as one hop plus named arenas, the
  short-connector second hop only if a doorway is measured to wait.
- **Evidence (host and appliance bench; the hardware A/B of docs/3DS_ARCHITECTURE.md 8 follows):**
  - Appliance bench (old3ds-lan167, 268 MHz, L2 off; throwaway/hd-bubble-bench-hw 9bc4aa62, 100
    level-8 HD files, 7.09 MB LZ -> 11.78 MB raw): core 1 at 30% reads 3.5 MB/s but reads and
    unpacks only 0.77 MB/s of file (unpack 1.66 MB/s, 390 ms for one file); core 0 1.78 MB/s;
    core 1 at 80% reads slower (1.46 MB/s); 32 KiB freads halve the read rate; an import is 3.1
    ms (4.1 max) and its C3D_TexFlush 0.24 ms (0.77).
  - From the packs and the level data (worked in the design report): the window at 512 is at
    most L3 5.60, L4 4.54, L5 4.97, L6 5.47, L7 6.20, L8 8.79 MiB, against 10.92 MiB of room
    with 30 MiB linear; the thinnest doorway margin with the raw pack is L4's CMEAT_5 connector,
    +0.1 s at turbo (to be measured on the hardware).
  - The existing pack rewritten raw: 1382 textures, 0 failed, 84.2 MB (`--from-pack`, 60 s).
  - `make -C tests/scripts test` exit 0 (HdWindow, HdWindowLevels, HeapSplit PASS; the window
    tests fail 8 checks with the script scan or the tier rule broken), `make -C tests/formats`
    exit 0, `tools/check.sh` all gates pass, 3DS build 0 errors.
- **Review** (independent agent, 2026-11-26): two P3 defects fixed in 17889b9d -- a copy that
  left the window was freed while the last frame could still be on the GPU, and the in-play wait
  evicted with nothing protected (it now waits for the GX queue first); an empty window kept the
  HUD's pictures unpinned; a corridor row skipped as a name collision lost its file. Not changed:
  the fit counts each arena's own window, not the drawn arenas a teleport's frame may add (the
  2.1 MiB margin covers one), and a wait gives up after 1 s without progress (logged, counted).
- **Emulator** (Mandarine r1.2.1 md5 0d2adab6, `DISPLAY=:99`, emurun.sh, harness payloads of
  throwaway/hd-bubble-change-hw with ROUTE -- waypoint door walks, noclip, god):
  - L7: `hd: level 7 window: 19 arenas, 144 files, 1 script/trigger neighbours; largest window
    5959360 bytes, room 10383360`; loading-screen wait 84 files; DANT_1 -> CDANT_1 -> DANT_2 ->
    CDANT_2, each window's new files resident 0.4 s after it was asked for; 0 waits, 0 draws
    without their copy.
  - L8: `largest window 8400064 bytes, room 10398208`, 8 script/trigger neighbours; GUNT_1 ->
    CGUNT_1 -> GUNT_2 -> CGUNT_2 -> GUNT_3, windows resident 0.7-1.2 s after asked; 0 waits.
  - The emulator's heap and timing are not the Old 3DS's: these show the logic, not the margins.
- **Hardware:** the A/B (docs/3DS_ARCHITECTURE.md 8) is built for all six levels (base:
  c95310c9 + harness, LZ pack; change: this branch + harness, raw pack; same routes) and was
  queued, but old3ds-lan167 left the network at ~15:30 (ping and ARP dead, broadcast discovery
  empty) while another agent held the appliance lock; it needs physical power. Not measured yet.
- **Player build:** `mdk_3ds/mdk.3dsx` of 17889b9d (no harness), kept as
  `.git/worktrees/hd-bubble/player/mdk-hd-bubble-17889b9d.3dsx`; it needs the raw pack on the SD
  card (`hd_pack_convert.py --from-pack`).
- **Commit:** this one.

## 2026-11-26

### LEVEL6 (in-game level 2): the OLYM_4 cannon never fired with Kurt on it
- **What:** opcode 209 (`if_player_on_me_low`) in `mdk_core/src/scripts/script_vm.cpp`
  is now "Kurt's platform is this object and he fires". It also required his feet to be
  at or below the object's model top (`kurtPosition().z <= worldBounds(obj).max.z`). That
  test was dropped.
- **Why:** the owner's report (2026-11-26, photo in `.git/mdk-review/owner-reports/`):
  on level 2's row of cannons, Kurt stands on the middle one and fires, and nothing
  happens. The cannon should fire on each shot, and the third shot should send it through
  the window behind him.
  - The level is LEVEL6 (`kOrder = {7, 6, 3, 4, 8, 5}`). OLYM_4 spawns six XBGUN. The
    middle one at x -1732 runs script 23159: `if_player_on_me` (114) raises it. Then a loop
    on 209 fires XBG_B1 and counts shots in var 2.0. Once that count is past 2 (the third
    shot), the cannon follows path 35601. At path frame 4 it hides and shatters group 1
    (the window), shows COLYM_4 and preloads OLYM_5. When the path ends it dies and drops
    Kurt.
  - Root cause: once the cannon has risen, Kurt's feet are 0.02 above its model's top,
    so 209 was always false. On the host the script sat at 23193 for the whole run while
    Kurt stood on it in the Shot state.
  - The C++ copied the C# in `src/`, which is v1.1.1 and has the same bug. Upstream fixed
    it in `3b072d2a` "Fire LEVEL6's cannon with Kurt on it" (v1.2.1, origin/main). The
    original (0x458a59) compares his feet with his floor on the platform (0x573a1c), and
    standing on the platform already means that test passes. The fix ports upstream's
    change.
  - Every other opcode in the sequence was checked against the C# and is implemented. That
    covers the XBGUN script, XBG_B1's script, and OLYM_4's arena script (114, 68, 3, 154, 12,
    1, 66, 89, 61, 67, 64, 92, 116, 2, 33, 98, 138, 100, 102, 41, 240, 223, 16, 210, 53,
    106, 218, 17, 50, 88, 107, 158, 238, 172, 135, 71, 247, 119, 86, 161, 230, 252). The
    run to OLYM_5 also put no opcode in the VM's unimplemented bucket.
  - 209 occurs once in the whole game (all six CMIs disassembled), so no other level
    depends on it.
- **Related, not fixed here:** upstream `d14ed0b5` "Run objects newest first, per arena"
  (after v1.2.1) fixes LEVEL7 DANT_6's cannon XD6GUN, which blew up as soon as Kurt came
  near. Newly spawned XG had not yet counted themselves in var 1.0 when the cannon checked
  that count. The port still appends new objects and runs one mixed list (`objects_.push_back`
  in `script_runtime_arenas.inc`), so in-game level 1's window cannon probably has the same
  bug. This is not verified in the port. The fix changes the object order for every level,
  and many goldens pin that order, so it needs its own increment.
- **Evidence:**
  - Host: the new `tick1` case `L6_cannon` drops Kurt on the middle XBGUN and holds fire
    from step 60. It checks, in order: he stands on it, it fires (23250), it takes the path
    (23266), the window shatters (23303), it carries him in COLYM_4, and at step 700 he is
    in OLYM_5.
  - The case fails on the unfixed VM (`never reached "/r23250"`) and passes with the fix.
  - The case checks markers, not a golden, because the C# oracle cannot run here (no
    dotnet) and its v1.1.1 copy has the bug.
  - Linux build, `make -C tests/scripts test`, `make -C tests/formats` (200,624 checks, 0
    failures) and `tools/check.sh` all pass with zero warnings.
  - Hardware: see the follow-up entry.
- **Commit:** 419613a6.

### The OLYM_4 cannon on the Old 3DS appliance (hardware follow-up)
- **What:** one harness run on `old3ds-lan167` (Old 3DS, 268 MHz, L2 off, stereo per
  config), `hw_cycle.sh --once`, FRESH, push verified (`bytes_out=234691660`). The build:
  - Branch `throwaway/level2-cannons-hw`, which is 3f59a93a plus the harness cherry-pick
    3fe09fde plus the 209 fix.
  - That harness has no `AT=`/`ARENA=` wiring in `main_3ds.cpp`, so 49e3c824's two
    blocks were copied in (throwaway only).
  - Flags: `AUTOQUIT=90 LEVEL=6 FIXED=1 AT=-1732,92,-1260,90 ARENA=OLYM_4
    WALK='fire@60-200;...;fire@600-900'`.
  - `--capture` refused the payload: this harness base has no framebuffer capture. There is
    no picture.
- **Evidence:** `.git/worktrees/level2-cannons/hw-logs/`:
  - `L6-cannon.session.log`: the walk's feet and arena at frames 60, 200, 240, 300, 400
    and 600 are identical to the host dump's at the same steps
    (`host-L6-cannon-platforms-dump.txt`). In OLYM_4 he stands on the cannon at 60 and is
    still on it at 200. In COLYM_4 at 240 he is at (-1733.0, 157.5, -1249.2), and at 300
    and 400 he is still flying. At 600 he is in OLYM_5 at z -1665.0.
  - The music follows the arenas: ROCK_FS, then CORRIDOR for COLYM_4, then DUNE_M2 for
    OLYM_5. The run exits clean after 3662 frames.
  - `mdk-player-419613a6.3dsx`: the player build of the fix (no harness).
- **Not seen on hardware:** the picture of the window shattering and of the flight (no
  capture on this harness). Mounting by walking from the level's start was not run either:
  the run teleports Kurt over the cannon.
- **Commit:** this one (ledger only).

### Upstream's C# gameplay fixes (1.1.1 to 1.2.6, and its C++ 2.x), ported to our C++
- **Why:** the owner's request (2026-11-26): "lets apply all of the upstream fixes but in
  C++". Upstream is `origin` (nautilusbits/mdk-sdl, read only). Our port started from its C#
  1.1.1 (d5bb66ec). This branch, `fix/upstream-gameplay`, takes the gameplay fixes one commit
  each, in upstream's order. The upstream C# commit is the specification; upstream's own C++
  (d8724627..367cc6fc) is the second reference.
- **How the goldens move:** a golden changes only when the C# oracle at the fix's upstream
  commit prints the new bytes and our C++ prints the same bytes. The oracle is the golden's own
  probe (`tests/scripts/*_probe`) built against `git archive <commit> src` with .NET SDK
  10.0.404 (scratch). The probe at d5bb66ec reproduces the old golden byte for byte
  (checked on `L4_ledge_short`), so the setup is sound.
- **Per fix:**
  - **c81dafc1, ease the camera's push through doors.** `followCameraClear` caps the wall
    push and the solid-object shove at 40 u/s (`kMaxPushSpeed * delta`); the rest is pushed
    on the next steps. `followCameraClear` and `buildFollowCameraCleared` take the step's
    `delta`. Three goldens moved; each equals the oracle at c81dafc1: `L4_ledge_short` (the
    wall push at step 37), `L7_ride_xd2`, `L4_board_end`.
  - **3f9315c5, keep Kurt on steep slopes running downhill.** The walk's downhill glue
    follows the slope's whole drop (`downhill / n.z`, capped at 90 u/s), and a running or
    standing Kurt starts the fall animation only after 0.15 s off the floor (`_airFor`).
    Measured on LEVEL6's OLYM_8 slope with the ledge probe (`tp:1:OLYM_8:2307:2469:-1164:180,
    fwd:30-120`): falls into K_FALL while running drop from 10 to 3, the same in the oracle
    and the C++. Eleven goldens moved. Nine equal the oracle at 3f9315c5 byte for byte:
    `L3_kurt_steps`, `L7_plat_door`, `L4_board_lost`, `L4_board_end`, `L4_hazards_snow`,
    `L6_wind_tunnel`, `L6_wind_arena`, `L6_wind_fan`, `L6_wind_wall`. Two take the C++'s
    output: `L7_corridor_middle` (one line, a feet z of 4 against 5) and `L4_board_off` (5
    lines, vz -8.3972 against -8.4009). Both walk at a yaw off the axes, where the glue now
    binds and divides by n.z, so the trig LUT in `Player::facing` (accepted drift, owner
    2026-11-20) shows. Evidence: with `facing` switched to libm cosf/sinf in a scratch build,
    both equal the oracle byte for byte; the shipped LUT was put back.
  - **Oracle tooling:** `tests/scripts/kurt_probe` is new. The driver that made
    `L3_kurt_steps`/`L3_kurt_lethal` (`/tmp/kilo/oracle/Driver`) was not kept; this probe
    replays `dumpKurt`'s schedule and reproduces both goldens byte for byte at d5bb66ec.
  - **93743b33, home the fall's missiles once per tick.** `FallMissile::home` blends with
    keep = 0.8^(dt × 30) instead of 0.8 per frame. The frame's dt is the frontend's elapsed
    time (60 fps and up on Linux, 30 or less on the 3DS), so the steering now matches the
    original's per-tick rate at any frame rate. Six goldens moved, all equal to the oracle at
    93743b33 byte for byte (the 1/30 s cases too: `(1 - keep) × 250` rounds differently from
    the old folded 50): `L6_skydive_chase`, `L8_skydive_hard`, `L3_skydive_slow`,
    `L7_skydive_skip`, `L6_fallview_chase`, `L8_fallview_hard`.
  - **3005da70, fire projectiles from the posed part.** `MdkObject::partCenter` takes the
    box of the part's vertices in the current pose through `transform()`, as the part is
    drawn (the rest-pose box only when the part has no vertices). LEVEL7's boss XB1 had his
    GUN at his feet at rest, so every BIGBOLT started in the floor. Twenty-one goldens
    moved, all equal to the oracle at 3005da70 byte for byte: `L8_holes_guntam`,
    `L6_holes_xb2`, `L7_bomber_mount`, `L7_bomber_bombs`, `L7_bomber_walk`, `L7_bomber_end`,
    `L6_hazards_bomb`, and LEVEL5's tick tables `L5_t590` to `L5_t30000` and
    `L5_t10000_script`.
  - **Oracle tooling:** `tests/scripts/table_probe` is new. It rebuilds the lost driver of
    the tick tables (`golden/L*_t*.txt`, `L*_t*_script.txt`) and reproduces 15 of them at
    d5bb66ec byte for byte (5 tables and 3 scripts named in its header, plus 7 spot
    checks). `tick1 dump table|script <level> <ticks>` prints the C++ side.
  - **94bf1d0b, hang chains from a one-point leader's hook.** The chain update hangs a
    link from its leader's second reference point, or from its only one when the leader's
    model has fewer than two (LEVEL7's ship XE: its hook). One golden moved,
    `L7_plat_tank` (an XE chain in DANT_3 next to the tank), equal to the oracle at
    94bf1d0b byte for byte.
  - **a48e64c7, hang chain links by their animated points.** `MdkObject::reference_point`
    takes the point where the current animation frame has it (frame clamped to the
    track), as the C# does; the rest-pose point only without an animation. It is every
    reference point's reader (chains, attachments, wounds, shots from a point), so 46
    goldens moved, all equal to the oracle at a48e64c7 byte for byte: LEVEL5's tick tables
    and script states from t720, `L8_fire`, `L8_fire_run`, four `L8_items_*`,
    `L8_rounds_ship`, `L3_screen_hmo3`, `L8_sniper_hud_ship`, `L6_plat_gun`, `L7_ride_xd2`,
    `L4_board_end`, five `L7_bomber_*`, `L8_end_level`, and four `*_hazards_*`.
  - **a91c0870, follow the jump's arc in its frames.** `Player::animateJump`: the first 10
    frames of the jump (7 of the running jump) play one a tick, then each frame waits for
    the vertical speed to drop to its threshold (0x492030), and past the last comes the
    fall. Reaching the top in those frames sets `_pastJumpTop`, which lets held jump open
    the chute at once (it waited ~0.4 s for -16 u/s before, issue #17 upstream). A jump no
    longer falls through the state machine's speed test, only through its frames. Nine
    goldens moved, all equal to the oracle at a91c0870 byte for byte: `L3_kurt_steps`,
    `L8_items_air`, `L7_ride_lost`, `L3_wind_shaft`, `L8_wind_fan`, `L6_ledge_climb`,
    `L4_ledge_climb`, `L4_ledge_short`, `L6_ledge_letgo`.
  - **224d1144** (upstream's next gameplay commit) only moves `tests/kurt_moves_test.sh`'s
    ledge check to 2 s for this change. That script tests the C# app in `src/` (1.1.1),
    not our C++, so there is nothing to port; the C++ ledge goldens above already follow
    the chute-at-the-top arc.
  - **8a3740ac, keep Kurt's gun in his right hand strafing left.** `KurtSprite::draw`
    mirrors only the left roll (the 1996 demo's K_ROLLL); K_SIDE is no longer mirrored on
    a left strafe, as the original (damp_animate 0x4646a4, damp_sprite_draw) never does.
    No golden covers the sprite's quad; the build and both host suites pass. The 3DS and
    Linux draw Kurt through this one function, so both change together.
  - **Follow-ups from the full suite after 8a3740ac:**
    - `tests/scripts/fall_frontend_test.sh`: the Linux frontend's chased LEVEL7 fall now
      lands with health 84, not 67. That is 93743b33: the fall probe run the frontend's way
      (`7 1 1 2400 0.0166666667 chase:0-2400`) carries health 67 at 3f9315c5 and 84 at
      93743b33, and the Linux binary logs 84. The pickups and inventory are unchanged.
    - `audit_arm11_math` flagged `partCenter`'s rest-pose fallback cosf/sinf once the posed
      branch was added above it (3005da70); it carries the same R6 pin as
      `reference_point` now.
  - **The camera's clearance skips a dead Kurt (C# FollowCamera.cs:132; coordinator's
    request from the death-flow check).** `followCameraClear` returns at once while Kurt's
    state is Dead, as the C# (every version) and upstream's C++ do; before, the wall push
    and the object shove could slide his corpse through the ~4 s death beat. No golden
    moved. tick1's new `L7_dead_clearance` puts Kurt with a wall behind him in LEVEL7's
    start room: alive, the camera pushes him 5.33 away; dead, 0. With the check removed
    the dead run moves 5.33 too, so the case fails. `docs/OPEN_ITEMS.md`'s entry is
    closed.
  - **dce8d904, keep Kurt's chute shut in a wind zone.** The wind zone's air arm calls
    `Player::blow` (the pull down plus `_blown`), and `updateAir` neither opens the chute
    nor keeps it open while `_blown`; `updateVertical` clears it after the fall. No golden
    moved: none drops through a wind box in the air. Upstream's own case (COLYM_3 under
    OLYM_3's hatch) did not reach the box in the wind probe either (Kurt starts in COLYM_2
    there and the zone's arena script does not run), so this is checked by build and suites
    only.
  - **32b1e1b0, hold the slide brake's last frame.** K_BSLIDE is `Once` in
    `KurtSprite`'s state table (it looped 3 frames 10 times a second, so the sprite
    flickered while braking). Draw-side only; no golden covers the sprite pick.
  - **e96fb32e, stop Kurt on an object face he touches.** `solids::entry` counts a start
    exactly on a grown face (t = 0) as an entry on that axis, as the original (0x45f588)
    does. No golden moved. Upstream's case (LEVEL4 MEAT_7, a jump over the board through
    the closed X4DOOR) was tried in the platform probe at five jump steps; the old and new
    oracle and our C++ end in the same place in each, so the exact on-the-face contact was
    not reproduced there. Checked by build and suites only.
  - **8fceff8c, fly Gunter head first down the stream.** The Gunter tube's `other_` starts
    with yaw 0 (the tube's frame unturned, 0x435a34/0x4366f4) instead of the 90 Kurt
    uses, so he no longer lies sideways across the tube. Two goldens moved, both equal to
    the stream probe at 8fceff8c byte for byte: `stream_L8_planet`, `stream_L8_death`.
    (The probe links the stream sources by `../../../src`; the oracle script places it
    under `tests/scripts/` for that, and it reproduces `stream_L8_planet` at d5bb66ec.)
  - **47395c03, limit the mouse look to -30..60 degrees.** `FollowCamera` clamps the look
    offset so the look and the automatic pitch (arena, fall, climb, ride tilt) together
    stay within -30..60, the automatic part alone may go past (`clampLook`); before, the
    range was -60..90. The view pitch is now `automatic + look`, the C#'s order. This is
    the 3DS's look too: the circle pad/C-stick look feeds `Input::mouseY`, so on the 3DS
    the look now stops at -30/60 (a behaviour change there, as upstream intends: past 60
    Kurt's sprite lay flat behind the floor). No golden moved (none looks with the
    mouse). New vm_tests case `MouseLookStopsAtTheLookLimits`.
  - **3bb211b6, lift Kurt out of a floor he sank into** (a deliberate deviation from the
    original, upstream's issue #30). `ArenaSpace::sunk` measures how far the falling box's
    lowest corner lies under the floor plane a fall move stopped on; `Player::climbOut`
    (before `land`) moves him up onto it when that is more than 0.5 and at most 2.5 and the
    move up is free, never on a ride. LEVEL8's GUNT_7 walkway (no side wall) let Kurt walk
    2 units inside it. **It also moves LEVEL3's start:** Kurt starts 1.81 units into the
    start pad and now stands on it from the first step (feet z 190.0 -> 191.8), in the C#
    1.2.4 oracle too. That shifts every level-3 Kurt golden. Twelve goldens moved; eleven
    equal the oracle at 3bb211b6 byte for byte (`L3_kurt_steps`, `L4_ledge_climb`,
    `L3_sniper`, `cheats_L3_god`, `L3_end_level`, `L3_strike_teeth`, `L3_kurt_invuln`,
    `L3_kurt_knockdown`, `L3_kurt_flashes`, `L3_kurt_push`, `L3_kurt_camroll`), and
    `L7_corridor_middle` takes the C++'s output, 3 lines off the oracle through the trig LUT
    (a libm `Player::facing` scratch build matches the oracle byte for byte). The pinned
    frame (`gates_frames`, L3_f1) still passes.
  - **Oracle tooling:** `tests/scripts/hurt_probe` is new; it rebuilds the lost driver's
    `hurt` mode and reproduces all eight `L3_kurt_{hurt,hurtland,knock,invuln,knockdown,
    flashes,push,camroll}` goldens at d5bb66ec. `sniper_probe` also writes the wheel's
    notches to `Input.ZoomWheel` when the C# has it (upstream 1618b8c3 zooms through it
    while sniping); it still reproduces `L3_sniper`/`L7_sniper` at d5bb66ec and at 47395c03.
  - **d14ed0b5, run objects newest first, per arena.** Spawns go to the front of the
    object list (`push_front`, the original's 0x45fd4c), and the tick runs Kurt's arena's
    objects, then the active second arena's (as it is by then), each once
    (`runArenaObjects`, `MdkObject::ran_this_tick`). LEVEL7 DANT_6's cannon XD6GUN blew up
    as soon as Kurt came near because it checked its XG count before they counted
    themselves. Knock-on fixes: `touchDamage`'s snapshot of the list now indexes the old
    entries behind any it spawns; a save's restore appends its boxes (`spawnBox(...,
    atBack)`) so the list keeps the saved order, as the C#'s `Objects.AddRange`.
    **Goldens: 367 moved** (the list order is in every table): 365 equal the oracle at
    d14ed0b5 byte for byte, among them all `L*_t*` tables and scripts, the
    pose/transform/blend/layout/draw/bound/runner dumps, rounds, holes, items, bomber,
    hazards, platforms, ride, board, strike, cutscene, screen and the five saves. The five
    saves were checked against d14ed0b5 with 6eb0963b's `town_total` save field taken
    out of the C#: that field is the timer ring's (presentation, 1.2.3), not part of this
    port. Two take the C++'s output: `L8_fire` (17 lines: one debris piece bounces one
    step apart; a libm `Player::facing` scratch build equals the oracle byte for byte, so
    it is the trig LUT, now visible under the new random order) and `L8_effects_storm`
    (the C++ effects view over `L8_hazards_debris`, which equals the oracle). tick1's
    hand-written `footing`/`walk` checks and vm_tests' synthetic spawn-order cases read the
    list in its new order (the same objects and values). Also fixed in vm_tests:
    47395c03's case compared -30 + 15.000001 with == -15.
  - **Oracle tooling:** `table_probe` gained the pose, transform, blend, layout, draw,
    bound and runner modes (all 25 such goldens reproduced at d5bb66ec); `rounds_probe`
    calls `SniperRounds.Camera(i, 1f)` where the C# has upstream a73a5cd2's alpha.
  - **Full suite after d14ed0b5:** Linux build 0 warnings; `make -C tests/scripts test` 999
    PASS, 0 FAIL; `make -C tests/formats` passes; `tools/check.sh` failed only the R11
    screen, which is file-level: `animateJump`'s `while` (a91c0870) made
    `player_state.inc` "a file with a loop", so `wrapAnimationFrame`'s `%` became a
    candidate. `animateJump` moved to `player_vertical.inc` (the jump's file, no `%`);
    `tools/check.sh` then passes all gates.
  - **e1d13fdf, spawn LEVEL4's red bones and EWJ pickups.** Opcode 161 goes through
    `ScriptRuntime::spawnFlagged`: SW_EWJ and BONEFLC have no model, so they become sprite
    boxes (6x6x6 and 9x9x6) flagged 0x2008a6 instead of being dropped; anything else
    spawns as before. LEVEL4's board runs had lost 20 bones and 11 EWJ, LEVEL8 2 EWJ. Six
    goldens moved; five equal the oracle at e1d13fdf byte for byte (`L4_board_ride`,
    `L4_board_lost`, `L4_board_dead`, `L4_board_end`, `L4_hazards_snow`) and
    `L4_board_off` takes the C++'s output (the same 5 trig-LUT lines as at 3f9315c5; a libm
    `Player::facing` scratch build equals the oracle). Upstream's companion 84ad71e4
    (stretch a spawn_box sprite over its box, so a bone draws 9x6, not 9x5) is a drawing
    change and is left to the presentation work.
  - **e1fde558, stand Kurt on platforms' triangles, not boxes.** `solids::platform` takes
    the part's posed triangles under the feet (`MdkObject::floorAt`: the segment in model
    space against the part's triangles, either side, Moller-Trumbore as the C# writes it;
    damp_platform_floor 0x41d2c4 casts at them, 0x414668); a box only selects the
    candidates, and `Solid` carries its part. On LEVEL3's hover ship XGS Kurt stood level
    with the fins, 2.6 above the deck. The same commit nudges a snowboard stuck where the
    path time cannot advance towards the path ahead (`Snowboard::nudge`). Eight goldens
    moved; seven equal the oracle at e1fde558 byte for byte (`L3_plat_keeper`,
    `L7_plat_tank`, `L6_plat_gun`, `L4_board_off` -- which matches the oracle again --,
    `L4_board_lost`, `L4_board_dead`, `L4_board_end`). `L4_board_ride` takes the C++'s
    output: one line, the follow camera's z one 1/64 apart at a yaw of 159.1 (the view's
    trig LUT; the feet and every other column equal the oracle).
  - **7f0dfca8, keep a kept Kurt over his platform in the air.** `carryKurt`: a platform
    that keeps Kurt (0x800000) also carries his XY while he is in the air, at the
    platform's height and without turning him (0x46634e). A jump on LEVEL6's flying
    XBGUN left him up to ~10 units behind it. No golden moved. A jump on the XBGUN at
    rest (platform probe, `tp:1:OLYM_4:-1732:92:-1260:0,fire:60-210,jump:250-251`) is the
    same in the oracle before and after this commit and in our C++; the flying-gun case of
    upstream's unit test was not reproduced in a probe.
  - **6ccc0cf4 (upstream's C++ 2.0 only), the second snowboard run no longer ends in
    MEAT_4.** `Snowboard::nudge` moves a stuck Kurt through `Player::shove`, which collides,
    instead of writing the step into his feet: since e1fde558 the board sticks against
    MEAT_4's bank (y 9360) and the raw nudge put Kurt inside it, over no floor. Upstream
    notes the C# 1.2.6 has the same bug, so the oracle is the C# 1.2.6 (1094e273) with
    exactly this change made in `Snowboard.Nudge` (`_kurt.Shove(...)`) in scratch.
    `L4_board_end` equals that oracle byte for byte and still ends in MEAT_5's box;
    `L4_board_ride` takes the C++'s output (one camera z 1/64 apart, the view's trig LUT,
    as at e1fde558).
  - **Not ported, and why:**
    - **1618b8c3** (wheel and zoom bindings in the scope): not applicable. Our Linux
      frontend has fixed bindings (`window_sdl.cpp`; the settings' `bind.*` lines are kept
      but never applied) and the 3DS has no wheel. With the default bindings the upstream
      change only stops a scoped wheel notch from also pressing the item keys, and our
      `player_sniper.cpp` already skips the item keys on a step the wheel moved. If
      rebinding is ever wired up, port this with it.
    - **224d1144**: test-only (`tests/kurt_moves_test.sh`, which runs the C# app in `src/`).
    - **03ee9031** (pull the board over floors it sank into): a fix to upstream's
      `FloorPull` (a27ca2f4, "draw the ridden board over the floor under it"), a drawing
      feature our port does not have. Presentation; listed for that work with a27ca2f4.
    - **84ad71e4** (stretch spawn_box sprites over the box): drawing; presentation.
    - **c881c9bc** (keep the between-frames view upright looking down): part of upstream's
      in-between-frame interpolation; presentation. Our steps already clamp at 89 degrees
      and 47395c03 stops the look at 60.
    - **6eb0963b** (a save's `town_total` for the timer ring): belongs with the timer ring
      (235b1792, presentation). Our saves do not write it; the C# 1.2.3+ loads such saves.
  - **Upstream's later C++ (d8724627..367cc6fc):** reviewed commit by commit; the only
    behaviour fix is 6ccc0cf4 (ported above). 1acbcf17 is tests and parity tooling (it
    marks the second snowboard run's test should_fail, which 6ccc0cf4 then fixes);
    7750e87b speeds up PNG deflate and SHA-256 (not gameplay); the rest are the port
    itself, Android, CI, docs and naming. The merge commits between 1.1.1 and 1.2.6 carry
    no source of their own except c5d11e7c's mod-image conflict resolution (mods).
  - **Elsewhere, for the owner:** presentation (the other agent's list) -- 5f9ea5f2,
    683a53ef, 04c25852, c881c9bc, 8bc42528, ae746b5d, 7740deb1, a73a5cd2, bf9b3a53,
    75ec50e2, a27ca2f4, 03ee9031, 84ad71e4, 235b1792, f0fa4c17, 5b338e44, 6eb0963b,
    bb39a854, 70c27b14, 7e61858f, 8c6c24cd, bbd2fddb, e1eb36bb, 09f2e511. Renderer
    (D3D12/SDL GPU): c14fa040, 9fad9b1e, 674a983f, a9cc6ddf. New features, not fixes:
    47325ca3 (FPS overlay), 0f04fe59 (zip mods), 83379440 (mdk.log, console from the
    menu), 8f7dd7b2 (quick save/load), bebe9c9a/25a11c52 (version in a corner), the HD/PNG
    work (3f24f303, b30b8c40, aad0f065, 0551812a, 2e072015, 1b1c21ea), and the Android /
    touch / NativeAOT commits (7e43522b, 98ebc06d, dbe8ef93, 1a90639f, 4a0c5c28, c571901e,
    f21cb391, f989576b, 4e0015bb, 9410097c, ea4b340d, b8a84ce8, 1bf504b4, 86c6bea0,
    ee11e624, e1fd2f86, eb6454e5, e2d839ca).
  - **Deliberate differences checked:** none of the fixes depends on the console model,
    adds a thread or file I/O, or touches the 30 fps cap: the per-tick fixes use the step's
    `delta` (c81dafc1, 93743b33), which is right at any rate. 47395c03 changes the 3DS's
    stick look: it now stops at -30/60 degrees. 3bb211b6 starts LEVEL3 1.81 higher (on
    the pad instead of in it). d14ed0b5 reverses the object list (newest first), which
    also changes which object a script's "first match" finds -- the C#'s order now.
  - **The trig LUT, where it shows:** five goldens take the C++'s output instead of the
    oracle's (`L7_corridor_middle`, `L4_board_off` until e1fde558, `L8_fire`,
    `L4_board_ride`, and the C++-only `L8_effects_storm`); each was checked by a scratch
    build with libm in `Player::facing` (or is a camera-z 1/64 step in the view's LUT),
    which is the accepted drift (owner, 2026-11-20). Every other moved golden equals the
    oracle at its fix's upstream commit byte for byte.
  - **Final host run (a5e477b2):** Linux build 0 warnings; `make -C tests/scripts test` 999
    PASS, 0 FAIL; `make -C tests/formats` passes; `tools/check.sh` ALL GATES PASSED
    (gates_smdh skipped: no .cia).
  - **3DS:** player build (`make -C mdk_3ds`, no harness) of a5e477b2: 0 compiler warnings
    (make's own "jobserver unavailable" notice only); saved as
    `.git/worktrees/upstream-gameplay/hw-logs/mdk-player-a5e477b2.3dsx`. **Not run on
    hardware or in Mandarine** by this work: everything above is host-verified against
    the C# oracle; no 3DS-specific code changed.
- **Commit:** branch `fix/upstream-gameplay`, ab4bbed0..a5e477b2 and this ledger commit.

## 2026-11-26 -- upstream presentation fixes ported to C++ (branch fix/upstream-presentation)

The owner asked for "all of the upstream fixes but in C++". Upstream (`origin`, read only)
fixed the C# after 1.1.1 (d5bb66ec, which this port is made from) up to 1.2.6. Each entry
names the upstream commit it ports; the C# commit is the specification. The gameplay
fixes are another agent's.

### 5f9ea5f2 -- the sky's directions from the camera's axes (Linux)
- **What:** the Vulkan sky's clip-to-direction matrix for the centre camera is built from
  the camera's axes and projection (`skyDirections`), as upstream's `ViewCamera.SkyDirections`;
  a stereo eye keeps the inverse (`skyInverse`, `RendererVk::eyeSide`).
- **Why:** inverting rotation x projection in float put the screen triangle's corner
  directions off one plane, by an error that changed with the camera's angle, so the sky
  shook against the world while turning.
- **The 3DS:** not applicable. Its sky is a dome mesh (`render/sky_dome.cpp`, ETC1 on main)
  drawn through the world camera; there is no clip-to-direction inverse to correct.
- **Golden:** the pinned frame L3_f1 changed (the sky). The oracle was re-captured from
  upstream at 5f9ea5f2 (`tests/frames/capture_oracle.sh` gained `MDK_ORACLE_ROOT`, a C# tree
  to build the oracle from; .NET 10.0.404 in the scratchpad, display :99 in the script's
  private namespace). Measured at tolerance 0: upstream 5f9ea5f2 vs its parent 2.411% of
  pixels; this C++ vs the 5f9ea5f2 oracle 0.051% (max 247, mean 0.0444), down from the
  1.240% ceiling recorded against 1.1.1. The manifest was regenerated with
  `regenerate_reference.sh` against that oracle.
- **Evidence:** Linux build 0 warnings; `make -C tests/scripts test` all PASS except the
  frame gate before the manifest was regenerated (it was the changed frame, confirmed by
  rebuilding with the old path: CONTRACT CLEAN); after: FrameGate PASS, tick1 "All cases
  match the oracle", `tools/check.sh` ALL GATES PASSED.

### 683a53ef -- the sounds the arenas carry
- **What:** `SoundBank::forLevel` adds a fourth archive: the sounds embedded in LEVELnO.MTO's
  arenas whose names none of the three SNI archives has (the first arena's when two share a
  name), read at the level's load (only their own bytes: the directory, three small reads an
  arena, then each WAV) and played once at full volume, as upstream's `SoundBank.ArenaSounds`.
  `arenaSoundBytes()` reports what it holds.
- **Why:** 68 names across the six levels played nothing, e.g. LEVEL7's barricade soldiers'
  SING and TAUNT1 (DANT_2), LEVEL4's FLY, LEVEL5's SMASH, LEVEL8's XFORK.
- **Cost:** resident bytes per level, measured on the retail data: L3 566,552, L4 299,732,
  L5 304,238, L6 249,016, L7 185,680, L8 411,666. Decoded on first play like the other
  archives' effects. The reads are at the level's load, on the thread that already reads
  TRAVERSE.SNI and LEVELnS.SNI there; none in play. The load-time cost on the console is not
  yet measured. **Flagged for the 3DS architecture (owner rule: loading belongs to the core-1
  streaming worker):** these reads should move with the bank's other load reads when that
  worker lands.
- **Not like the original:** the original registers an arena's sounds when its stream
  completes and drops them with it (0x41983c -> 0x4310cc, at most 16). Upstream (and this)
  hold all of a level's at once.
- **Goldens:** 42 tick1 cases changed (scripts now hear these sounds; some scripts wait on a
  voice, so object counts and timings move too). The oracle was the C# 1.1.1 tree with only
  683a53ef's SoundBank.cs applied, the probes passing `levelData.Arenas` as upstream's Viewer
  does (.NET 10.0.404, scratchpad `up-tools/cases.sh`). 41 goldens are byte-identical to that
  oracle; the 42nd, L8_effects_storm, is the C++'s own record over L8_hazards_debris's storm
  (which matches the oracle) and was rewritten from the C++. Against upstream at 683a53ef
  itself, 12 differ, from upstream's other changes in between.
- **Evidence:** vm_tests' new case (level 7 plays SING and TAUNT1 at 0x7FFF, once; every level
  has the fourth archive); tick1 "All cases match the oracle"; Linux build 0 warnings.

### 04c25852 -- the fall's missile trails as the original
- **What:** each missile keeps its velocity at each trail point (`FallMissile::trailHeadings`);
  the ribbon runs between the tips of the MISSILE model's tail fins (its leftmost and rightmost
  vertices) as the missile was turned at that point, swells just behind it (x1.25) and tapers
  to nothing over 32 points; white at 215/255, the last 8 segments yellower and more opaque.
  Both ways of drawing it: the desktop's mesh (per-vertex colour) and the 3DS's lean canvas
  (one quad a segment, in its colour).
- **Why:** the trails were a flat grey band (an approximation in the C#), fixed upstream from
  the original (0x439454, 0x43911c, 0x4392a4, 0x491d64, 0x410874).
- **Cost:** per frame, one `orient` (a normalize and two crosses) per trail point drawn: at
  most 24 missiles x 32 points. Not measured on the console yet.
- **Evidence:** Linux build 0 warnings; tick1 all cases match (no golden draws the trails; the
  fall dumps hold the trail points, unchanged); a level 7 fall captured at frame 1500, desktop
  and lean: the ribbons show white with yellow tips at the missiles.

### c881c9bc -- the view upright looking straight down
- **What:** `FollowCamera::blend` (the frames between steps) clamps the pitch to the steps'
  +-89 degrees (`clampViewPitch`), as upstream's `ViewPitch`.
- **Why:** looking all the way down, a frame between steps looked exactly 90 degrees down; the
  view turned over and Kurt jumped from the middle of the screen to the top.
- **Evidence:** Linux build 0 warnings; tick1 all cases match.

### 8bc42528 -- the fall's explosions face the camera
- **What:** the EXPLODE model is pitched 90 degrees (`explosionTurn`, CreateRotationY(-pi/2))
  before its random yaw, and goes off at Kurt's position, not where the missile met his box.
- **Why:** a missile hitting Kurt showed a thin yellow sliver beside him: the model is a flat
  fireball thin along x, seen edge-on unpitched (original +0x13c; 0x411ee8 copies Kurt's
  position).
- **Goldens:** L3_skydive_slow, L6_skydive_chase, L8_skydive_hard (the explosions' positions)
  rewritten from the C++ and byte-identical to the C# 1.1.1 oracle with 683a53ef, 04c25852 and
  8bc42528 applied (fall_probe, scratchpad `up-tools`).
- **Evidence:** Linux build 0 warnings; tick1 all cases match.

### ae746b5d -- animations drawn between their frames
- **What:** `MdkObject::remember` keeps the animation and its time; `MdkObject::frames_at`
  (the C#'s FramesAt) names the two baked frames a frame between ticks draws between and the
  share of the second (a loop goes on to its first frame; another animation, a restart or a
  script-set frame snaps). `ObjectView::addObject` takes the tick's alpha and, below 1, lerps
  the two frames vertex by vertex into one scratch pose it keeps (the stream consumes it at
  once, so one scratch serves every object, where the C# keeps one per model). Both frontends
  pass the tick's alpha for every object (the driven and carried ones too, as `PoseOf`).
- **Why:** monsters, items and doors animated at 30 Hz on screens faster than the ticks.
- **Not like upstream:** the vertex lerp is the plain `a + (b - a) * t` (drawing only; .NET's
  fused form would be an `fmaf` call per coordinate on VFPv2); the C#'s `% count`s are
  bounded subtractions (R11), and a time outside two counts is drawn as it is.
- **Cost on the 3DS:** one lerp per vertex of each animated object drawn between ticks, and
  the next frame baked (once) sooner. To be measured on the appliance against the commit
  before it (see the interpolation measurement entry below).
- **Evidence:** Linux and 3DS builds 0 warnings; tick1 all cases match; tests/formats 200,650
  checks, 0 failures; `tools/check.sh` ALL GATES PASSED (the R11 screen first flagged the
  two `% count`s).

### 7740deb1 -- flying pieces and effects drawn between ticks
- **What:** each debris piece keeps its centre and turn, and each sprite effect its place,
  at the start of the tick (`Debris::update`, `Effects::update`); `EffectsView::build` takes
  the tick's alpha and draws them between that and now (culling at the drawn place). New ones,
  and wounds whose object jumped, are drawn where they are; the sprites' frames stay at the
  tick rate. Both frontends pass the tick's alpha (Linux: 1 under `--fixed-step`).
- **Why:** sparks, shattered pieces, slime drops, wounds and bubbles moved at 30 Hz on faster
  screens.
- **Not like upstream:** a piece's turn is the normalised lerp (short way round) instead of
  `Quaternion.Slerp`: the turn between two ticks is small, and the slerp's acos and sines per
  piece per frame are libm calls on the ARM11 (AGENTS.md R6). Drawing only.
- **Cost on the 3DS:** a lerp per piece and effect, and four multiply-adds and a square root
  per piece; measured with the interpolation entry below.
- **Evidence:** Linux and 3DS builds 0 warnings; tick1 all cases match (L8_effects_storm builds
  at alpha 1, unchanged); `tools/check.sh` ALL GATES PASSED.

### a73a5cd2 -- the sniper rounds and their cameras drawn between ticks (the tick half)
- **What:** each round keeps its place, camera, yaw, pitch and roll at the start of the tick
  (`SniperRounds::update`; a new round is drawn where it is); `SniperRounds::visualTransform`
  and `camera(index, alpha)` give the flying model and the round camera between that and now,
  and both frontends pass the tick's alpha (`SniperView::draw`/`hudOf` take it).
- **Why:** the rounds in flight and the views at the top of the scope moved at 30 Hz.
- **Not done, the step half (the scope's zoom, tilt and clip timer between steps):** this port
  draws Kurt and the camera at the last step -- upstream's step blending (FrameBlend's Kurt
  pose, `FollowCamera.Blend`) is not ported and neither frontend calls `FollowCamera::blend`
  (frame_blend.h) -- so blending the scope alone would draw it out of step with the view it
  is in. It belongs with that subsystem if the owner wants frames above the step rate. The
  same holds for c881c9bc, whose clamp is in `FollowCamera::blend` and has no caller yet.
- **Evidence:** Linux and 3DS builds 0 warnings; tick1 all cases match (the HUD and rounds
  goldens use alpha 1); `tools/check.sh` ALL GATES PASSED.

### 75ec50e2 -- the pause menu in the fall
- **What:** Esc (Linux) or START (3DS) in the fall opens the pause menu (`menu::PauseMenu`:
  Resume, Options, Main menu, Quit) and the fall waits under it, drawn as it stopped with the
  menu over it; Esc/B or Resume goes on. The frontends no longer pass the press as the sim's
  skip (`FallSim::Input::skip` stays, for the skydive probes' `skip:` schedules).
- **Why:** the skip was the port's testing shortcut; the original opens its abort prompt
  there (0x403cf8).
- **3DS note:** the fall shows the C# pause menu, not the owner's level START menu (its toggles
  and cheats are the level's); START or A picks the item, B resumes.
- **Evidence:** Linux and 3DS builds 0 warnings; `fall_frontend_test.sh` case 2 now opens the
  menu at 3 s and resumes at 3.5 s in the intro, with the same fall time at both and no
  "Fall ended": PASS. Not run on the console.

### a27ca2f4 (and 6eb0963b's FloorPull guard) -- the ridden board over the floor under it
- **What:** `KurtSprite::boardPull` (C# FloorPull.Of): the eye's rays to the board model's eight
  box corners; a floor they cross inside the box (or up to a unit under it) pulls the board
  towards the eye by `inFront`, the same on screen; corners on the eye's plane are skipped
  (6eb0963b). Both frontends skip the ridden board in the object loop and draw it after the
  objects with the pulled transform (Viewer.DrawBoard).
- **Why:** on LEVEL4's board runs the board sank into the slope (uphill Kurt's feet lag a step
  under the floor, banked turns sink an edge); the original draws an object after its leaf's
  floor with no depth buffer, so the board always shows whole.
- **Cost:** while on the board only: one 4x4 inverse and up to eight floor crossings a frame.
- **Evidence:** Linux and 3DS builds 0 warnings; tick1 all cases match; `tools/check.sh` ALL
  GATES PASSED. LEVEL4 CMEAT_3 ride (`--ride=XSNOWB --at=420,4285,-570 --walk=forward@60-700`,
  640x480) A/B with the pull switched off by a temporary hack: at frame 480 the board is
  hidden under the floor without the pull and whole with it (2,684 pixels differ, all on the
  board). Not run on the console.

### 235b1792, f0fa4c17, 5b338e44, 6eb0963b -- the HUD's timer ring and colours
(Written on `fix/upstream-hud` by a helper agent in its own worktree, cherry-picked here in
upstream order after a27ca2f4: 3ebc6a0b, 5e42aeca, 2e3468b7, 6a0e9aee.)
- **What:**
  - 235b1792: the mission timer ring around the health empties as the town's time runs out
    (SC_BSTAT stamped over SC_STAT's indices once per degree; `ScriptRuntime` keeps the town's
    total and `townTimeLeft()`; both frontends fill `HudState.timeLeft`).
  - f0fa4c17: the HUD's sprites take Kurt's arena's colours 64-175 (`HudView::enter`, a
    pointer compare a frame; on an arena change only the images whose indices reach 64-175
    are remade -- one 38x48 icon on the retail data), so the dog's icon is not magenta.
  - 5b338e44: a mod's SC_STAT picture (Linux enhanced look) is re-stamped with the ring.
  - 6eb0963b (its timer and HUD parts; its FloorPull guard is in a27ca2f4's entry): saves keep
    the town's total (`"town_total"`), older saves take the difficulty's, the share is clamped.
- **Not like upstream:** a clamp instead of the float wrap loop (T2); the ring's cotangent
  through the trig table (R6); no per-pixel divide in the mod restamp (R11).
- **Known gaps:** with the 3DS HD set covering SC_STAT the stamp is skipped (the HD rule), so
  the ring stays full there; the sniper and bomber overlays keep the level's palette.
- **Goldens:** the seven `L*_save_*.txt` gained the `"town_total"` key (sizes grown by its
  length, hashes over it; script-checked against the 1.1.1 goldens); tick1 also loads each
  save without the key (the older format).
- **Evidence:** on the helper's branch and again on this stack: Linux and 3DS builds 0
  warnings; tick1 all cases match; tests/formats 200,650 / 0; `tools/check.sh` ALL GATES
  PASSED. Not checked by eye; not run on the console.

### 09f2e511 -- crafted PNG and GLB files refused
- **What:** PNGs over 8192 pixels a side are refused before the row arithmetic (int32
  `width * bpp`, `(stride + 1) * height`) can overflow; a GLB accessor with a negative count,
  byteOffset or stride, or an offset past the int's end, is refused before it sizes a buffer
  or reads before the binary chunk. The chunk-length checks were already 64-bit here.
- **Found on the way, fixed here:** the GLB reader's own JSON integer read (`glb_json.inc`
  `asInt`) multiplied the sign in as a digit: "-4" read as 26. glTF has no legitimate negative
  integers, so this only mattered for crafted files, which it let through.
- **Why:** mods are downloaded files; upstream capped and bounded the decoders after crafted
  sizes crashed them.
- **Evidence:** tests/formats' new `craftedGlbAndPngAreRefused` (a negative count, a negative
  byteOffset, a 2^31-1 byteOffset, a 65536-wide PNG; the negative byteOffset was accepted
  before the `asInt` fix): 200,662 checks, 0 failures. Linux and 3DS builds 0 warnings; tick1
  all cases match; `tools/check.sh` ALL GATES PASSED.

### 03ee9031 -- the board pulled over floors it sank deeper into (not on the list; a27ca2f4's follow-up)
- **What:** `KurtSprite::boardPull` probes 18 points of the board's box (a 3 x 3 grid at its
  bottom and top) instead of its 8 corners, and a floor counts where a ray crosses it within
  the board's length of the point, instead of inside the box (the 4x4 inverse goes).
- **Why:** upstream found the board still vanished under the floor in 1.2.5 (MEAT_1 on the
  flat, out of CMEAT_3's first bend, in the tubes): it is 0.4 thick and sinks up to 2, so
  crossings inside the box missed it, and a tube's floor or a bump under its middle crossed
  none of the corner rays. It finishes a27ca2f4, so it is ported with it.
- **Cost:** while on the board, up to 18 floor crossings a frame (from 8).
- **Evidence:** the same CMEAT_3 ride, 640x480: at frame 480 the board was half hidden with
  the 8-corner pull and is whole now; Linux and 3DS builds 0 warnings; tick1 all cases match;
  `tools/check.sh` ALL GATES PASSED. Not run on the console.

### The rest of the presentation list: already present, not applicable, not done
- **bf9b3a53 (an intro splash too short for its palettes): already present.**
  `IntroSplash` already refuses an INTRO1A entry shorter than the two 768-byte palettes
  (`intro_splash.cpp`, `entry->size < kPaletteSize * 2`), as the splash that is missing.
- **8c6c24cd (fill the loading bar as the level loads):** the 3DS is `fix/3ds-load-bar`'s
  (e31d6f50, a11edd8b: the screen kept through the load, drawn at its steps and through the HD
  hold, the steps' fractions from a measured level 4 load; upstream redraws at most every
  100 ms from per-part fractions measured on a desktop). Not duplicated here. **Linux: not
  done** -- a whole `--level=3/8 --frames 1` run, Vulkan start included, takes 0.8 s wall on
  this machine, so a moving bar has nothing to show.
- **bbd2fddb (the insets in both eyes): not applicable as written.** Upstream cleared the inset
  list after the first eye. Linux clears it at the frame's start, but a stereo frame there
  draws no insets at all (a stated refusal in `renderInsets`: a pre-existing gap, not this
  bug). The 3DS's round-camera insets are `fix/3ds-bullet-cam`'s (f7e03bfb, once per eye).
- **bb39a854 (2D screens whole on non-4:3 displays; the vertical field of view widened below
  4:3) and 7e61858f (the fall's ground and haze on a near-square screen): not applicable.**
  The 3DS's top screen is 5:3 (wider than 4:3: unchanged upstream) and the Linux window is
  4:3 (1280x960; only `--true-aspect` captures change it). The safe-area half is Android's.
- **e1eb36bb (the camera's arena over the other one): not done, for the owner.** Upstream
  splits the depth range into two bands (the camera's arena, its objects and effects, Kurt and
  the board in front; the other drawn arena behind), set per draw as the viewport's depth
  range, and maps the enhanced look's screen passes' depth back. Here that means a band on
  every queued draw in both backends, the 3DS queue not folding draws across bands, its depth
  map (or MVP) per band, the Linux enhanced passes (occlusion, haze, shadows) remapped, and
  half the depth precision for every draw on the 3DS. The case it fixes is LEVEL4's snowboard
  ride end (MEAT_3's shell over CMEAT_1's floor). Owner decision: worth that, or a narrower fix
  for that spot.
- **25a11c52 (the version in the main menu's corner): not done -- a feature.** This port has
  no version string; it needs build plumbing (a version from git into both builds) first.
- **a73a5cd2's step half and c881c9bc:** see their entries (the port does not blend Kurt or
  the follow camera between steps).

### The interpolation's cost on the Old 3DS: NOT MEASURED (the appliance was held)
- **Built, not run:** three level 8 profile payloads (`AUTOQUIT=90 LEVEL=8 PROFILE=1`, the
  harness from throwaway/sky-etc1-hw 3fe09fde merged in on `throwaway/upstream-presentation-hw*`):
  A = f20161a7 (before the interpolation), B = 306d7cf8 (ae746b5d + 7740deb1 + a73a5cd2's tick
  half), C = 3028722f (this branch's tip). Kept, with the locked runner, in
  `.git/worktrees/upstream-presentation/hw-logs/` (`hwrun_up.sh A_base_L8prof B_interp_L8prof
  C_final_L8prof`, `hw_cycle.sh --profile` each under `.git/mdk-review/appliance.lock`).
- **Why not run:** the appliance lock was held by the mip-root campaign from 14:04 to past
  15:57; the runner waited and was stopped rather than left to take the lock unattended.
- **Emulator (Mandarine r1.2.1, md5 0d2adab6, DISPLAY=:99, `emurun.sh`):** A and C both boot
  level 8, draw the HUD (C: canary and ink in place) and then log no frame past 5 in 60-240 s
  -- the same with the commit before the interpolation, so not this branch's; not timing
  evidence either way.
- **What to decide on the numbers:** each of the three is its own commit. If B costs the Old
  3DS a refresh, the 3DS frontend can pass alpha 1 to `addObject`'s pose and
  `EffectsView::build` (the transforms already blend) without touching Linux.
- **Also checked:** Linux, level 3 at 150 s (`--god`, frame 9000): the timer ring shows an
  emptied wedge from the right, counter-clockwise (235b1792).

### 2026-11-26 -- 3DS: coplanar details get the oracle's depth pull (ceiling lights stop flickering)
- **What:** `drawRecorded` (06_draw_submit.inc) composes `buildDepthPullMatrix`
  (the C#'s `DepthPull`: scale towards the camera by layer/256 of the distance,
  lines 1/128) into the MVP of any draw with `material.depth_layer != 0` (or a
  lines draw), Overlay exempt, as the Vulkan backend's `12_draw.inc` already
  does. `coalesciblePair` (06_queue.inc) no longer folds two draws of different
  layers. `docs/OPEN_ITEMS.md`'s "DepthPull on the 3DS" item is closed.
- **Why (root cause):** the owner's flickering light decals. Core already gives
  every arena triangle that overlaps a bigger triangle of its plane a depth layer
  (`arena_scene.cpp`, the C#'s `Layers.cs`, merged as `origin/zfight` e13d91d0)
  and splits batches by it, but the Citro3D backend never read `depth_layer`
  (OPEN_ITEMS said so: it lacked the eye position, which `mdk_render_set_camera`
  has stored in `cameraPos` since). So a light and its ceiling sat at the same
  depth under `GPU_LEQUAL`: the winner per pixel was rounding, changing as the
  camera moved, and because batches sort by material before layer, some details
  are even drawn BEFORE their surface. Not the depth format, the transforms or
  the second-pass programs: every program (unlit, lit GS, shadow-lit GS, pass-GS)
  takes the same `mvp` clip position; the lit `world` uniform stays unpulled, so
  normals and shadow lookups use the real surface.
- **Which surfaces:** a host census (`computeArenaDepthLayers` over every
  arena; scratch tool, not committed) finds 800 layered triangles: L3 10, L4 35,
  L5 256, L6 369, L7 73, L8 57. E.g. DANT_7's ceiling tube lights `D7_TLITE`
  (6) and their rims (12), DANT_6's windows `D6_WIN1` (38), OLYM_5's hub rims
  (52), OLYM_7's mirror/floor tiles, MUSE_3/4's flat-colour panels. Objects,
  sprites and the glow halos carry no layer (the C# does not either; the halo
  has its own half-size nudge, effects_view.cpp).
- **Choice:** the oracle's own mechanism, already in core and on Linux, not a
  new one. `C3D_DepthMap` offset per draw was the alternative: a constant offset
  in stored depth is a huge world distance at range (decals through walls far
  away) and tiny up close, while the pull is a share of the distance. Margin of
  layer 1 in the 24-bit buffer with this projection (near 0.5, far 20000):
  329 LSB at 100 units, 66 at 500, 33 at 1000, 6.6 at 5000. Cost: only layered
  draws pay -- one pull matrix, one or two 4x4 multiplies and one uniform write
  per draw per eye, plus the frame matrix re-sent on the next unlayered draw.
- **Other backends:** Linux/Vulkan already applied the pull (12_draw.inc, and
  its casters in 11_shadow_post.inc); not affected.
- **Evidence (host):** Linux build 0 warnings; 3DS build 0 compiler warnings;
  tests/scripts `make test` rc 0; tests/formats 200624 checks, 0 failures;
  tools/check.sh ALL GATES PASSED. Independent read-only review (Sonnet): no
  defects. Linux reference view of the scene (DANT_7, lights on the ceiling):
  `--level=7 --arena=DANT_7 --at=520,4420,-110,90`.
- **Not verified here:** Mandarine (other projects' instances were running; one
  instance max). Hardware: see the follow-up entry.
- **Commit:** this one.

### The decal depth pull on the Old 3DS appliance (hardware follow-up, INCOMPLETE)
- **What:** A/B of ad997c4f against its base 3f59a93a on `old3ds-lan167` (CFGU
  model 0, 268 MHz, L2 off, stereo on), harness 3fe09fde cherry-picked on
  `throwaway/decal-zfight-hw` plus the AT/ARENA/GOD/NOCLIP start aids ported
  from `harness/3ds-profiling` (the main-based harness lacked them), scene
  `LEVEL=7 ARENA=DANT_7 AT=520,4420,-110,90 GOD=1 NOCLIP=1 AUTOQUIT=90`
  (the room under DANT_7's ceiling tube lights; Linux view of the same spot:
  `--level=7 --arena=DANT_7 --at=520,4420,-110,90`). This harness has no
  framebuffer capture, so the pictures were to be ATM screenshots.
- **What happened:**
  - First pass: AT/ARENA were not wired on this harness, so Kurt started in
    DANT_1 (`.git/worktrees/decal-zfight/hw-logs/first-try-no-AT/`).
  - Second pass: Kurt was teleported, but without noclip he fell through the
    unloaded floor. Both builds ran 90 s cleanly (FRESH, 5294 frames each), but
    the screenshots show an empty void (`second-try-fell/`). The profiles of that
    pass (steady 16.884 ms vs 16.890 ms, both vsync-bound) therefore measure no
    layered draws. They are NOT the cost of the fix.
  - Third pass, with noclip: the base build ran FRESH (Kurt held at the spot,
    `hw-before-shot.session.log`). During the fix build's run (pushed 13:22,
    verified `bytes_out=234688444`) the console stopped answering HTTP, and by
    13:54 it was ARP-dead (`hw-after-shot.cycle.txt`: 48 failed pulls). The next
    lock holder (bullet-cam) found it ARP-dead too and did not push. Trap 8 was
    followed once: directed discovers for 2 min, no answer, ping dead. It needs
    physical power. It is NOT established whether the fix build caused this.
    Its run log is on the card (`sdmc:/3ds/mdk/mdk.log` or `mdk.prev.log`), and
    so are the six screenshot sets (`shot_0000000d`-`0f` before, the next three
    after, per `atm.log`). Pull those before anything else touches the console.
  - The ATM screenshot quota was exhausted by stale sets at the start. They were
    retrieved to `.git/mdk-review/atm-retrieved-2026-11-26/` and purged; the
    purge deleted 32 sets, about 18 of them beyond the truncated listing and not
    retrieved (README there).
- **Still owed:** the before/after look in DANT_7 on hardware, the frame cost
  with the decals in view, and an answer on the hang. Next step: power the
  console, pull `mdk.log`/`mdk.prev.log` and the shot sets, and rerun
  `after-shot.3dsx` and `after-profile.3dsx` (kept in hw-logs).
  Mandarine: not run, because other projects' instances held the
  one-instance limit all session.
- **Commit:** this one (ledger only).

### The sniper's round cameras on the 3DS (2026-11-26, fix/3ds-bullet-cam)
- **What:** the three ovals at the top of the scope (the C#'s round cameras: `SniperView.DrawRoundCamera`, `Renderer.Insets.cs RenderInset`, `SniperRounds.Camera`) now show the round's chase view on the 3DS.
  - Root cause: the shared core already matched the C# (`SniperRounds::camera`, the watch after a hit, the miss/hit/kill fills, `SniperView::drawRoundCamera`; pinned by `golden/L8_sniper_hud_ship.txt`/`_kill.txt`), and Linux drew the insets. The 3DS frontend passed `roundInsets=false` and the citro3d backend refused Scene insets (package #7's owner-gated deferral), so a watching window was filled as an empty one.
  - `mdk_3ds/src/render/c3d`: `mdk_render_begin_inset` records up to three Scene insets (rectangle, view, projection, eye); `drawSceneInsets` (06_queue.inc), called by `replayQueue` once per eye at the glow's slot (after the scene and the glow, before the canvas, as the C#'s UnderCanvas), sets the inset's viewport and scissor, resets its depth with a far-plane quad (no rectangle clear on this target; depth state `reset`: ALWAYS, depth written, colour kept, as the C#'s LOAD/CLEAR), puts the inset camera's matrix, frustum and (for the lit programs' face turn) eye in place of the eye's, re-submits every non-canvas prepared record through `drawRecorded`, and restores the eye's. The quad's corners sit in the scratch at `kInsetQuadOffset`, written once at create.
  - `main_3ds.cpp`: `roundInsets=true`; while a round camera watches, the object and effects culls (frustum and tiny-size) are off, since the round's 90-degree view is not the scope's.
  - `tests/scripts/c3d_frame_test.sh`: `drawSceneInsets` joins the closed emission and bind sites, and a new check pins that it is called only from `replayQueue`.
- **Why:** owner report 2026-11-26: the bullet chase cams in sniper mode do not work.
- **Known differences from the C#:** the inset's sky is the 3DS's queued dome (only under Backdrop::Sky, centred on the scope's eye), not a panorama centred on the inset; the sun shadow map stays centred on Kurt.
- **Evidence:** Linux build clean; 3DS build 0 compiler warnings; `make -C tests/scripts test` rc 0 (`PASS C3dFrame`, 65 checks, 167 arms); `tests/formats` 200624 checks, 0 failures; `tools/check.sh` ALL GATES PASSED (gates_smdh SKIP). Linux headless L3 walk `sniper@120-124;zoomin@200-240;fire@260-300` frame 275 shows the first oval watching (as before; Linux unchanged). Hardware: the follow-up entry. An independent review (read-only agent) found no cache desync or geometry-stage hazard; its tiny-cull, lit-camera and scissor findings are fixed in this commit.
- **Commit:** this one.

### The sniper's round cameras on the Old 3DS appliance, and the ROUND CAMERAS toggle (2026-11-26)
- **What:** measured the round cameras on `old3ds-lan167` (CFGU model 0, 268 MHz, L2 off, stereo on) and added the pause menu's ROUND CAMERAS toggle (`EnhancedToggles::sniperRoundCams`, config `sniperRoundCams`, row 24; on by default, the C#'s look). Off, a watching window is the empty fill and the scope's culls stay on (the pre-fix behaviour). One path on Old and New 3DS; no file I/O added.
- **How:** throwaway branch `throwaway/bullet-cam-hw` (3f59a93a + the harness 3fe09fde + this fix + a throwaway frame-period split: frames sniping with a round camera watching vs sniping without). `AUTOQUIT=90 FIXED=1 LEVEL=3 GOD=1`, the owner's toggles (HD, texture16, kurtCompressed, cap30Fps, haze, filtering, mipmaps, sky shadows), walk: sniper at frame 120, zoom in 200-240, fire every 60 frames 260-1706. `BCOFF=1` = the same build with the cameras off. Pushed by `hw_cycle.sh`, FRESH each.
- **Evidence** (`.git/worktrees/bullet-cam/hw-logs/`):
  - `hw-on.log` (`--profile`, cameras on): 875 frames with a camera watching, mean 44.4 ms, max 55.5, every one over 33.3 ms; sniping without one: 33.4 ms. Steady mean 37.7 ms (vblanks 2: 1773, 3: 583). Longest command list 34,302 words (buffer 128K words).
  - `hw-off.log` (`--profile`, `BCOFF=1`): the same 875 frames 33.45 ms (2 over 33.3); steady mean 33.6 ms; longest command list 17,286 words.
  - `hw-shot.log` (`--once`, cameras on, not profiled): the same split, 43.7 vs 33.4 ms; clean exit, no crash.
  - So the faithful cameras cost ~11 ms a frame in stereo while one watches: 30 fps is not held; off holds it. SCOPE IN 2D (one eye in the scope) should roughly halve the cost; its run (`onmono.3dsx`) never ran (below).
- **Not verified:** no picture of the 3DS insets: the harness on this base has no `CAPTURE` (refused before the push), ATM `screenshot_auto` captured nothing, and the console dropped off the network (ARP incomplete, directed and broadcast discovery silent) between my last FRESH run (13:13) and the screenshot run (13:46), during another agent's appliance session; trap 8's discovery did not bring it back, so it needs power. Mandarine was not available (another project's two instances). The look on hardware, both eyes at the slider, SCOPE IN 2D's cost and the New 3DS are unmeasured.
- **Upstream (origin/main, 1.2.x):** the round cameras' logic is unchanged since 1.1.1 except a73a5cd2 (cameras and rounds drawn between ticks: `SniperRounds.Camera(index, alpha)`, `TransformOf`), a presentation change the fix/upstream-presentation branch ports; it touches `SniperRounds::camera` and `SniperView::draw`'s callers, which this branch leaves alone. bbd2fddb (insets in both eyes) is the C#'s stereo fix; the 3DS draws its insets in each eye's replay already.

### The loading bar moves (3DS)
- **What (owner: "the loading screen bar doesn't show the loading progress"):**
  the 3DS drew the loading screen ONCE, bar empty, and presented nothing else
  until the level's first frame, so the bar sat at 0 through a ~12.6 s load.
  That was a faithful port of the C# Play (`LoadingScreen.Show(_ui, level, 0f)`
  once, Game.cs:480) -- the C# never moves the bar either; its load is short.
  Now the 3DS keeps the screen through the load (made inside the level's scope,
  freed with `LoadingScreen::release` before play) and draws it again at the
  load's steps (level data 23%, start view 42%, assets 57%... 72%) and, while
  the HD set's hold waits, from 72% to full as the set lands (at most a frame
  every 0.25 s). Layout and colours are the C#'s, unchanged. Linux keeps the
  C#'s one frame.
- **The renderer side:** a loading frame (`MdkFrameParams::loading`) skips the
  3DS backend's texture pumps and is not counted. Without that, the first
  hardware run showed the step frames making the start arena's packed
  originals BEFORE the HD set is sized: linear free at the set's choice fell
  16,177,920 -> 15,424,256 B, 2.1 MB less after the hold, and frame 30's log
  fired during the load. With it, all three are byte-identical to the base.
- **Evidence** (`.git/worktrees/load-bar/hw-logs/`; `old3ds-lan167`, Old 3DS
  268 MHz, L2 off, stereo per the slider, `AUTOQUIT=90 LEVEL=4 HDROMFS=1`, HD on,
  harness on `throwaway/load-bar-hw` / base on `throwaway/load-bar-base`,
  all cycles FRESH):
  - `L4base.session.log` (3f59a93a + harness) vs `L4final.session.log` (this
    fix): HD hold 3525 vs 3528 ms, 115 files both; linear free at the set
    16,177,920 both, after it 8,432,512 both; frame 30's texture line identical.
    The hold drew 13 loading frames in 181 ms. Step timestamps (`loading: bar`
    lines) 2.9/5.4/7.1/8.3/8.6/9.0/9.1 s of 12.66 s match their fractions.
    The ~8 step frames are ~12-14 ms each (by the hold's measured frames):
    ~0.1 s of a 12.7 s load, not separately measured against the base.
  - `L4-final-cap14.png`: the 14th loading frame read back on the console,
    bar at ~84% mid-hold. (A capture's SD write stalls ~7 s inside the hold,
    so capture runs are not timing runs; the slow holds of `L4-cap12*` are
    that, not the bar.)
  - Host: Linux build clean; 3DS player build 0 compiler warnings
    (`player-load-bar.3dsx`); tests/scripts `make test` rc 0; tests/formats
    200624 checks, 0 failures; tools/check.sh ALL GATES PASSED (gates_smdh SKIP).
- **Not verified:** other levels on hardware (the fractions are level 4's; a
  level's own split differs a little), HD off (no hold: the bar goes 72% -> full
  at the end), the menu's HD-toggle hold (unchanged, draws no bar), Mandarine.
- **Commit:** this one.

## 2026-11-26 -- the combined test build: HD window + upstream fixes + decals + bullet cams + load bar (branch test/3ds-combined-11-26)

- **What:** one branch the owner can test as one build, in worktree `.claude/worktrees/combined`,
  from `perf/3ds-hd-bubble` e7dcd1a9 (which holds `arch/3ds-platform` and
  `fix/3ds-second-core-streaming`'s code commit c95310c9; that branch's later ledger-only commit
  00172ca1 is not in it). Each branch is its own `--no-ff` merge, in this order:
  1. 49db95d9 `fix/upstream-gameplay` (with `fix/level2-cannons`)
  2. c5221a5b `fix/upstream-presentation`
  3. 2315ec29 `fix/3ds-decal-zfight`
  4. d2ba986f `fix/3ds-bullet-cam`
  5. 59150ee1 `fix/3ds-load-bar`, then 7802a292 (its HD step reworked onto the HD window)
  6. b1bf4428 review fixes (below)
  No throwaway, harness or FAST-toggle branch is merged.
- **Why:** owner request: a single build of the 11-26 work to test.
- **Conflicts and how they were resolved:**
  - `docs/WORK_LEDGER.md`, every merge: both sides appended entries; both kept, in merge order.
  - `tests/scripts/fall_frontend_test.sh` (merge 2): the PASS line takes both intents -- health 84
    (upstream 93743b33, gameplay) and Esc pausing the fall (upstream 75ec50e2, presentation).
  - **46 tick1 goldens** (merge 2), moved by both upstream branches (arena sounds 683a53ef and the
    fall's explosions here; newest-first objects, chains, platforms, missiles there). Each was
    rewritten from the merged C++ (scratch `bless`: the C++'s output written over the golden it
    failed; the five save goldens from `tick1 dump save`, which the bless writes three times) and
    compared byte for byte with the C# oracle, the branches' own probes (`tests/scripts/*_probe`)
    built with .NET 10.0.404 against `git archive` of upstream `src`:
    - 42 equal upstream **7f0dfca8** (it holds every fix either branch ported except 6ccc0cf4, a
      LEVEL4 board fix none of these touch) with the probes passing `levelData.Arenas` as
      upstream's Viewer does: the L5 tick tables, the hazards, bomber, plat_keeper, ride_xd2,
      rounds_mortar goldens, and the five saves (`town_total` is native at 7f0dfca8);
    - the three skydive goldens equal C# 1.1.1 + 93743b33, 683a53ef, 04c25852, 8bc42528 (the
      fall probe does not build against 7f0dfca8's later FallSim API);
    - `L8_effects_storm` has no oracle twin (the C++'s effects view over `L8_hazards_debris`'s
      storm, which equals the oracle), as on both branches.
    - Two goldens that merged as text but not in behaviour: `L7_ride_lost` and `L7_ride_test`
      (jump arc a91c0870 on one side, arena sounds on the other); the merged C++ printed new
      bytes, rewritten, both equal the 7f0dfca8 oracle.
  - `mdk_3ds/src/main_3ds.cpp` (merge 4, the bullet cams): the effects build keeps both -- the
    tiny-debris cull off while a round camera watches, and the build with the tick blend
    (7740deb1). The `sniperView.draw` call merged with both new arguments; the insets read the
    blended round cameras (a73a5cd2's `hudOf(..., tickAlpha)`), so the two compose. Not clean as
    the agents expected, but one hunk.
  - `packs_3ds.{h,cpp}`, `main_3ds.cpp` (merge 5, the load bar): the branch's `holdHdSet3ds`
    over the removed pinned set was dropped for the HD window's `holdHdWindow3ds`; the bar steps,
    the kept screen and the loading frames that skip the pumps kept; the `level: loaded in` timer
    starts once the screen is up.
- **The load bar on the HD window (7802a292):** `holdHdWindow3ds` takes the loading screen, the
  bar's start (0.72) and the frame index. At level start it reads `mdk_render_hd_pending` over the
  window against the level-start window's size (the files still to come when the wait starts) and
  draws `from + (1 - from) * landed`, a frame only when the count moved and at most every 0.25 s.
  The "toggled on" and in-play waits pass no screen and are unchanged.
- **Independent review** (a separate read-only agent over the merges and the rework), fixed in
  b1bf4428:
  - a real semantic conflict: the decal depth pull read `r->cameraPos`, which the bullet cams'
    scene insets never change, so inside a round-camera window every layered or line draw was
    pulled towards Kurt's camera (off its place, still z-fighting). The pull now reads
    `r->pullEye`, set with the frame camera and to the inset camera during `drawSceneInsets`,
    restored after (the Vulkan backend's `activeCamera` already did this);
  - the bar's share is clamped and never goes back (the whole-window count can rise when the
    pumps queue a linked copy); two stale comments.
  - Not changed, recorded: the loading screen's picture is now resident while the start window
    is sized, so `room` is smaller by its size than in hd-bubble's measurements (the
    `hd: level N window` line logs room and any step-down); on a full heap the hold's pumps
    could evict it (the next bar frame would draw without it).
- **Checks** (all at `nice -n 19 ionice -c2 -n7`, on b1bf4428):
  - Linux build (RelWithDebInfo, the default): 0 warnings. 3DS build (`make -C mdk_3ds`,
    devkitARM): 0 warnings.
  - `make -C tests/scripts test` (Linux built first; LinuxBinaryFresh PASS): exit 0, 1001 PASS,
    0 FAIL, 4 SKIP (the frame gate's oracle arms); tick1 "All cases match the oracle".
  - `make -C tests/formats`: 200,670 checks, 0 failures.
  - `tools/check.sh`: ALL GATES PASSED (gates_smdh SKIP: no .cia built).
  - Merge 2's commit carries `tests/frames/{capture_oracle.sh,reference.sha256}`, protected
    files changed by fix/upstream-presentation's own 887339fc and identical to that branch; the
    pre-commit gate refuses them without `MDK_OWNER_APPROVED=1`, which was set for that commit
    only, for that reason. Nothing else protected changed.
  - Emulator (Mandarine r1.2.1, md5 0d2adab6 verified, `DISPLAY=:99`, `mdk_3ds/tools/emurun.sh`,
    the only MDK instance; a perfect_dark instance was also running): the
    `throwaway/combined-hw` payload `AUTOQUIT=90 LEVEL=7 HDROMFS=1 TOGGLES=enableLighting=0`
    (the raw pack from `.claude/worktrees/hd-bubble/build-hd-pack/`). The bar logged 23, 42, 57,
    66, 68, 71, 72, 100 %; `hd: level 7 window: 19 arenas, 144 files ... largest window 5959360
    bytes, room 10383360`; `hd: wait loading screen: 84 files asked, 0 left, 100 made in
    1662 ms`; `level: loaded in 9300 ms`; `exiting after 5367 frames ... harness autoquit`,
    `--- session end ---`; `hd: level end: 0 waits, 0 draws without their HD copy`. emurun
    exits 5 on these runs because this tree logs frame lines 1-5 only (its frame counter reads
    5); the guest log is the evidence. Capture `hw-logs/emu-L7.png`: DANT_1, Kurt, the HUD. The
    emulator's heap and timing are not the Old 3DS's.
- **Builds kept** (`.git/worktrees/combined/hw-logs/`):
  - Player (no harness, plain `make -C mdk_3ds`) of b1bf4428: `mdk-combined-b1bf4428.3dsx`,
    md5 `a4446585c5ea67eec05aa3a97c9abb2e` (234,561,092 bytes). It reads HD from the SD card and
    needs the uncompressed pack (`hd_pack_convert.py`, `-z none`): already converted at
    `.claude/worktrees/hd-bubble/build-hd-pack/` (not regenerated).
  - Harness payloads for the appliance, on `throwaway/combined-hw` 33b97b0a (this branch plus
    the harness and route commits of `throwaway/hd-bubble-change-hw`, cherry-picked; never on
    this branch): `mdk-combined-hw-once-33b97b0a.3dsx` (`AUTOQUIT=90`, md5
    `2353cdb53e2235c0232388239f845648`) and `mdk-combined-hw-profile-33b97b0a.3dsx`
    (`AUTOQUIT=90 PROFILE=1`, md5 `49c159a506a8cf366c23dff8f66162be`); build logs and the
    emulator logs beside them.
- **Not measured / not verified:**
  - Nothing on hardware. The Old 3DS appliance is hard-hung (other agents' campaigns hold its
    lock); the owner's New 3DS was not used. Every branch's own hardware A/B is still open, and
    the combination adds its own: the decal pull's cost inside the round-camera insets, the HD
    window's margins with the loading screen resident, and the load bar's step fractions, which
    were measured with the pinned set (level 4, 12.6 s) and not on the window's shorter wait.
  - The C# oracle covers the host goldens only; the 3DS frontends' behaviour (bar, insets, pull)
    is not oracle-checked.
- **Commit:** the merges and fixes above, and this ledger commit.

### The sniper's round cameras in one eye (2026-11-26, owner: "we dont need stereoscopy there")
- **What:** the round cameras' insets are drawn in the first eye only, at zero parallax (no eye offset), and the second eye gets the same pixels by a copy after its replay (`copyInsetsToSecondEye`, 06_queue.inc): one `C3D_FrameSplit` and one `GX_TextureCopy` per inset rectangle, left eye's colour buffer to the right's, in the 8x8-tiled layout (memory row = panel x, column = 239 - panel y, the rotation writeScreenshotBmp measured; the glow pass's tile-row copy is the same mechanism, verified on hardware). The rectangle is grown to whole tiles (at most 7 target pixels, under the frame around the windows). Chosen over drawing the inset in both eyes at zero parallax because that keeps the cost; chosen over a render-to-texture inset because the copy needs no new VRAM target. ROUND CAMERAS stays on by default (owner). One path on Old and New 3DS. With the slider closed or SCOPE IN 2D there is one eye and no copy.
- **Evidence:** Linux build clean; 3DS 0 compiler warnings; `make -C tests/scripts test` rc 0 (`PASS C3dFrame`); `tests/formats` 200632 checks, 0 failures; `tools/check.sh` ALL GATES PASSED (gates_smdh SKIP). An independent read-only review found no arithmetic, ordering or GX-budget defect (1 split + at most 3 copies per frame).
  - Old 3DS appliance (`old3ds-lan167`, model 0, stereo on), the same walk, toggles and harness as the earlier entry, `--profile`, FRESH (`.git/worktrees/bullet-cam/hw-logs/hw-on1eye.log`): the 875 frames with a camera watching, mean **39.5 ms** (max 53.8; 857 over 33.3 ms), against 44.4 ms drawn in both eyes and 33.45 ms with the cameras off. Steady mean 35.7 ms (vblanks 2: 2295, 3: 190), against 37.7 (both eyes) and 33.6 (off). Longest command list 27,844 words (34,302 both eyes).
- **Not verified:** the picture. No ATM screenshots (owner: suspected cause of the hard hangs), the harness on this base has no capture, Mandarine was not used. Whether the right eye's windows show the copied cam (the memory-row orientation is taken from the screenshot readback and the glow, not seen through the barrier) is open for the owner's look on the console.
- **Commit:** this one.

### Script opcode watch list for the playthrough (2026-11-26, branch docs/opcode-watchlist)
- **What:** `docs/OPCODE_WATCHLIST.md`, a read-only coverage comparison of our VM
  (test/3ds-combined-11-26 f517348b) against upstream's C++ 2.0 (origin/main 367cc6fc). The C#
  (1.1.1 and 1.2.6) is the authority. Rows are sorted in level order, with the arenas and objects
  where each opcode is used.
- **Why:** the owner's full playthrough. These are the places where our handling is thinner than
  upstream's.
- **Findings:** both VMs dispatch the same 238 opcodes. 234 are equal. Four are partial in ours,
  where the state is written but nothing consumes it:
  - 133: the M_COMM screen does not animate (LEVEL4 MEAT_3, LEVEL7 DANT_5).
  - 159: the box sprites are not stretched over their box (upstream 84ad71e4).
  - 226: the LEVEL6 OLYM_6 pendulum does not swing, which is a gameplay difference.
  - 242: the dog's tether lines are not drawn (LEVEL5 MUSE_5, LEVEL8 GUNT_10).

  None is weaker upstream. Opcode 5 (LEVEL7 DANT_2 XBANG) is unhandled everywhere and is counted
  by `noteUnimplemented`. The demo's five opcodes (300-361) have no case in ours, and ours
  cannot load the demo.
- **Evidence:** the case-label set difference is empty. A scratch census walks every script from
  its entry point through this tree's decoder (236 opcodes used, 107,856 reachable sites).
  `tick1 probe 18000` reports nothing unimplemented on LEVEL3-8.
- **Not done:** no code changed, and nothing was run on hardware or in the emulator.
- **Commit:** this one.

## 2026-11-26 -- script opcode parity with the C# (branch fix/opcodes-parity)

Owner decision: the four opcodes `docs/OPCODE_WATCHLIST.md` lists as partial in our C++ are made
to match the C# like for like, one commit each, in priority order. The C# is the authority (our
`src/` 1.1.1 plus the upstream fixes this tree has taken); upstream's C++ only pointed at what
was missing. Oracle: `tests/scripts/hazards_probe` built with .NET SDK 10.0.404 (scratch) against
`git archive` of upstream's C# at v1.2.6 (it has every upstream fix this tree has taken) and
against 1.1.1 + 683a53ef (the arena sounds, which our bank has); the probe's new tokens are
`rope:TYPE` (place, pitch, yaw, swing speed and plane, flags, rope colour and mask, and
`RopeView.LinesOf`'s ends at 1/10000, plus an FNV-1a over the bits of every float) and, with
133, `frames`.

### 226 jump_to -- the LEVEL6 OLYM_6 pendulum swings
- **What was wrong:** the VM set the pivot, speed, gain, plane and `FlagSwinging`, but no tick
  code read them: XSWINGB hung still ~1985 units under its pivot, with no rope, and hit Kurt
  only when he walked into it.
- **What:** `ScriptRuntime::motionSwing` is `ObjectMotion.Swing` (0x43cfe8) operation for
  operation, called where `ObjectMotion.Update` calls it (after the command, before gravity;
  not for an object its command killed): w -= sin(pitch) * gain, pitch += w * gain, the object
  at pivot + (h.x sin, h.y sin, -cos) * length, the plane turned towards the scripts' target by
  at most 15 degrees (folded to +-90) at each turning point and the yaw moved there at 10 degrees
  a second, rope colour 1, mask 1, points object and pivot. libm sinf/cosf (bit-identical to
  .NET's MathF here, the fromAngle probe note), .NET's left-associative degrees-to-radians.
  `MdkObject::rope_lines` is `RopeView.AddLines` (pair mode; FrameBlend.Carried for the end on
  the object, blending off at alpha 1). `game/rope_view.{h,cpp}` draws them after the objects,
  where Viewer.cs:705 does, on both frontends: Lines with a flat palette colour where the
  backend has a line primitive (Vulkan; new capability `line_primitive`), and on the PICA200
  (no line primitive, no vertex colour) a ~1.5-pixel camera-facing quad per line, textured with
  an 8x8 RGBA texture of the colour (made the first time a colour is seen, freed with the level
  scope; no file read, no per-frame import).
- **Oracle:** new golden `L6_hazards_swing` (Kurt in the pendulum's path in OLYM_6, 1200 steps):
  the C++ equals hazards_probe at v1.2.6 **byte for byte**, including the PENDHIT knock-down at
  s=320. Against 1.1.1 + 683a53ef the `rope=` column (place, pitch, yaw, swing state, rope ends,
  bit hash) is byte-identical on every line; the other columns part from s=130 (another
  object's state: an upstream gameplay fix 1.1.1 lacks). 1.2.6 does not differ from 1.1.1 for
  this opcode: `Swing`, the jump_to case and `RopeView`'s lines are unchanged, 1.2.x only puts
  the rope draw in its arena's depth band. A 300-step run from Kurt's landing spot in OLYM_6
  equals 1.2.6 byte for byte too. No existing golden moved: tick1 522 of 522 pass with the new one.
- **Frames (Linux, headless, --fixed-step, 640x480, DISPLAY=:99, lavapipe):**
  `pendulum_linux.png` in the session scratchpad (`opframes/`, frames 240, 330, 450 from
  `--level=6 --arena=OLYM_6 --at=-1895,2857,-2048,85`): the white rope from the pivot ~2000 units
  up down to the bob, at a different swing angle each frame.
- **Cost on the 3DS:** one object a tick runs the swing (two sinf, one cosf, a few multiplies);
  the rope is one draw of 6 vertices a frame while XSWINGB is drawn, from one 4096-vertex
  dynamic mesh made the first frame a rope exists, plus one 8x8 RGBA8 texture per rope colour.
  Nothing per frame touches a file.
- **Not verified:** no hardware run (owner away; no appliance use) and no Mandarine run for this
  commit. The 3DS quad's width is a judgement (C# lines are one pixel on the PC); it has not
  been looked at on a console.
- **Commit:** see `git log` (this entry's commit, "opcodes: 226 jump_to ...").
- **Checks:** Linux build (RelWithDebInfo) 0 warnings; `make -C tests/scripts test` exit 0
  (tick1 522/522); `make -C tests/formats` 200670 checks, 0 failures; `tools/check.sh` all
  gates pass (smdh skipped: no .cia) after the swing's libm trig got its `@vfp-exempt R6` pin
  (gameplay: the LUT would move a damaging object); `make -C mdk_3ds` 0 compiler warnings.

### 133 arena_texture_frame -- the comm device's screen (M_COMM) animates
- **What was wrong:** the VM kept the scripts' frames in `runtime.animatedTextures`
  (`AnimatedTextures.Set`, mode 1: frames per second), but the level draw always passed frame 0,
  so the comm device's screen on LEVEL4 MEAT_3 and LEVEL7 DANT_5 never moved.
- **What:** `LevelView::draw` takes the scripts' frames and gives an animated batch
  (frameCount > 1) `frameOf(arena, texture name)`, the C#'s
  `frames.FrameOf(view.Arena.Name, batch.Animated)` (LevelView.cs:139); `ArenaViews` carries the
  pointer, which both frontends set each frame before `arenaViews.draw` (a runtime replaced at a
  level change is never read through a stale pointer). A level whose scripts never set a frame
  skips the lookup on the map's emptiness.
- **3DS texture paths:** the frame only moves the uv. M_COMM is one stacked strip of 6 frames,
  128 x 774 (6 x 129), under the PICA's 1024: the arena ETC1 set has it as one 128 x 1024 texture
  per palette (`build-arena-etc1` manifest), whose draw takes its frame's slice through
  `pica::compressedFrameUv`; an expanded original takes `offsetY = frame * frameHeight`. A
  stacked texture never gets an HD copy (`hdPackLookup` gives no `hdPath` for frameCount > 1,
  06_draw.inc never binds one for frame_count > 1), so the HD window holds no M_COMM and a frame
  swap neither imports a texture nor reads a file on core 0. The `Texture::parseAnimated`
  delta-decode fix (build/hd-pack-native, M_COMM only) is in this tree, so the frames differ as
  the C#'s do (frame 1: 78 texels from frame 0, frame 2: 2656). The known ETC1 limit stays: a
  frame height of 129 puts a frame boundary inside a 4x4 block, so the screen's edge row can
  bleed (ledger, the compressed strip frame entry).
- **Oracle:** the hazards probe's new `frames` token prints `AnimatedTextures.Frames` and
  `FrameOf` per step. New goldens, both the C# oracle's bytes: `L4_hazards_comm` (MEAT_3, 400
  steps at 6 frames/s; equal to v1.2.6 and to 1.1.1 + 683a53ef) and `L7_hazards_comm` (DANT_5,
  Kurt turns and fires: the chain gun hits the device and the screen goes to 12 frames/s; 600
  steps equal to v1.2.6 byte for byte; 1.1.1 + 683a53ef parts from s=20, Kurt's landing, an
  upstream gameplay fix 1.1.1 lacks, which shifts the hit by a few ticks). 1.2.6's 133 path is
  1.1.1's (its LevelView only adds depth bands). tick1 524/524.
- **Frames (Linux, headless, --fixed-step, DISPLAY=:99, lavapipe):** `comm_linux_frames.png`
  in the session scratchpad (`opframes/`): the DANT_5 device through the scope
  (`--level=7 --arena=DANT_5 --at=80,2255,-60,90 --sniper=0.6,2`), capture frames 100, 110 and
  120, top row this build (the screen's red indicator changes), bottom row the same build with
  the frames pointer forced to null (frame 0 every time, the old picture). Pixels differ between
  the two at 5 of 6 captures (up to 2098 pixels), only on the screen.
- **Cost on the 3DS:** per frame, one `std::map` lookup per animated batch in a drawn arena of a
  level whose scripts set a frame (MEAT_3, DANT_5: one batch); nothing in other levels.
- **Not verified:** not run on hardware or in Mandarine; the ETC1 strip's edge-row bleed was not
  looked at on a console.
- **Checks:** Linux build 0 warnings; tick1 524/524 and vm_tests pass; `make -C tests/formats`
  200670 checks, 0 failures (its link-graph guard: `AnimatedTextures::frameOf` is inline now, so
  the level draw links no scripts translation unit); `tools/check.sh` all gates pass (smdh
  skipped); `main_3ds_flow_test.sh` pass; `make -C mdk_3ds` 0 compiler warnings. The slow
  shell half of `make -C tests/scripts test` was run on the 226 commit and the final tree.

### 242 set_2d0_block -- the dog's tether lines are drawn
- **What was wrong:** the VM set `rope_mask` 0xFF and the four points, save/load kept them, but
  nothing drew them: the dog XBN (LEVEL5 MUSE_5, LEVEL8 GUNT_10) looked free.
- **What:** `MdkObject::rope_lines` gains `RopeView.AddLines`' 0xFF branch: for each rope point
  that is not exactly zero (`!= Vector3.Zero`, so a zeroed point is a dropped line), a line from
  reference point i+1 (`reference_point`, carried with the object between ticks) to the point.
  The draw is the 226 commit's `RopeView` (Lines on Vulkan, quads on the PICA200), so nothing
  in the frontends changed. The VM's own 0xFF is now `MdkObject::kRopeAllLines`. The XBN model
  has no reference points, so every line starts at the dog's origin (the C#'s ReferencePoint
  returns Position past the list, as ours does).
- **Oracle:** new goldens `L5_hazards_dog` (MUSE_5) and `L8_hazards_dog` (GUNT_10), 300 steps
  each with `rope:XBN`, both the C# oracle's bytes at v1.2.6; 600-step runs equal v1.2.6 byte
  for byte, and their rope column equals 1.1.1 + 683a53ef's on every line (the other columns
  part on other objects' upstream fixes). 1.2.6's 242 and RopeView lines are 1.1.1's.
- **Frames (Linux, headless, --fixed-step, 640x480, DISPLAY=:99, lavapipe):** `dog_linux.png`
  in the session scratchpad (`opframes/`): left, MUSE_5 (`--level=5 --arena=MUSE_5
  --at=465,-6,-2263,112`, frame 120), the dog high on its perch with white tether lines down to
  the platforms; right, GUNT_10 (`--at=-43,3250,-380,90`), the lines at the top of the view.
- **Cost on the 3DS:** up to 4 quads (24 vertices) a frame while the dog is drawn, in the rope
  view's mesh; four `reference_point` calls (each a cosf/sinf for the yaw) a frame.
- **Not verified:** the scripts' line drops (4, 3, 2, then 0 lines before the howl) were not
  reached in a probe run: standing at the arena's start and firing at the dog (it is
  indestructible, 65000 hp) leaves all four lines up in both arenas for 2400-3000 steps. The
  drop path is the same `rope_lines` loop over zeroed points, but no golden shows it. Not run on
  hardware or in Mandarine.
- **Checks:** Linux build 0 warnings; tick1 526/526, vm_tests pass; `make -C tests/formats`
  200670 checks, 0 failures; `tools/check.sh` all gates pass (smdh skipped); `make -C mdk_3ds`
  0 compiler warnings.

### 159 spawn_box -- the box sprites are stretched over the box (a port of upstream 84ad71e4)
- **What was wrong:** our EffectsView drew a spawn_box sprite at its image's own aspect, scaled by
  `max(sizeX, sizeZ) / width`, which is C# 1.1.1. Upstream 84ad71e4 (C# 1.2.0) corrected it to
  the original's (0x43b2b0, box sizes at 0x316 and 0x31e): the image stretched over the box's
  width and height. LEVEL4's bones (BONEFLC, 64 x 36 in a 9 x 9 x 6 box) drew 9 x 5, not 9 x 6;
  the FIRE/PULSE hazards and the EWJ worms take the same path.
- **What:** `EffectsView::build`'s box loop takes `texelX = size.X / max(Width, 1)` and
  `texelZ = size.Z / max(Height, 1)`; `queueSprite` and `spriteCulled` take the two texels
  (the effects pass the one texel twice). The 3DS halo (a 3DS-only glow billboard) is stretched
  with its sprite, and its nudge and the cull's radius take the larger side; for an effect
  (equal texels) `max(t*w, t*h)` is bit-identical to the old `t * max(w, h)`, so nothing else
  moved (tick1's `L8_effects_storm` passes unchanged).
- **Oracle:** the hazards probe's new `sprites` token runs the C#'s own `EffectsView.cs` and
  `MaterialResolver.cs`, compiled into the probe against a CPU-only Renderer (as table_probe
  does), under a view-projection with exact 0/1 entries (right +X, forward +Y), and prints the
  frame's draws and an FNV-1a over every vertex's position and uv bits; tick1 builds our
  EffectsView under the same matrix. New golden `L3_hazards_boxsprites` (L3_hazards_boxes'
  schedule + `sprites`, 300 steps): the C# oracle's at v1.2.6, which has 84ad71e4, byte for
  byte. Non-vacuity: the same v1.2.6 tree with 84ad71e4 reverse-applied differs from it on the
  `spr=` column of 60 lines and on nothing else. The probe calls either C# `Draw` signature
  (1.1.1's, 1.2.x's with tick and camera arena; tick 1 = blending off) and shims the internal
  `ScriptMath.Size` with the same expression. (A 1.1.1 tree with 84ad71e4's two lines
  hand-applied parts from our C++ on other columns from s=6 -- upstream gameplay fixes 1.1.1
  lacks -- so v1.2.6 is the oracle here.)
- **Frames (Linux, headless, --fixed-step, 640x480, DISPLAY=:99, lavapipe):** `bones_crop.png`
  in the session scratchpad (`opframes/`): LEVEL4 CMEAT_3's snowboard run
  (`--level=4 --arena=CMEAT_3 --at=420,4285,-570,91`), a red bone at capture frames 150 and 250,
  this build then the build before (the 242 commit) side by side: the new bone is a row taller
  (9 x 6 against 9 x 5); `bones_linux.png` is the whole frame 250 pair. 32 and 78 pixels differ,
  only on the bone.
- **Cost on the 3DS:** one more float divide per drawn box a frame (a handful of boxes); the
  halo and cull take a max of two products instead of one product.
- **Not verified:** not run on hardware or in Mandarine.
- **Checks:** Linux build 0 warnings; tick1 527/527; `make -C tests/formats` 200670 checks, 0
  failures; `tools/check.sh` all gates pass (smdh skipped); `make -C mdk_3ds` 0 compiler
  warnings. The changed hazards probe still reproduces `L3_hazards_boxes` and
  `L6_hazards_swing` byte for byte at v1.2.6 without the new token.

### Review of the four opcode commits, and the fixes it led to
- **Review:** an independent read-only agent read 1e1324b7..fd0c005a against the C# (`src/` and
  v1.2.6), AGENTS.md and the 3DS backend. It found no parity bug in the four ports (Swing,
  RopeView/Carried, AllLines, the 84ad71e4 stretch, operation for operation) and confirmed the
  3DS side (no per-frame texture work for a frame swap, the rope textures' lifetime inside the
  level scope, the frames pointer set before the only draw).
- **Fixed from it:**
  - `L6_hazards_swing` kept Kurt on the pivot's line, so the plane never turned (yaw 0 on every
    line): new golden `L6_hazards_swing_turn` (Kurt 100 units off the line; the yaw runs 0 to
    -15 and back through the clamp and MoveToward), equal to v1.2.6 byte for byte and its rope
    column to 1.1.1 + 683a53ef's. The old case's description no longer claims a turn.
  - Nothing tested the 133 draw path itself (the goldens pin the scripts' frames): new
    tests/formats case `LevelViewDrawsTheScriptsTextureFrame` loads LEVEL7 DANT_5's LevelView
    and checks the M_COMM batch's queued frame: 0 without frames, before a frame is set and for
    another arena's key, 3 after mode 0 sets 4.5. With the draw's lookup disabled it fails
    (`got 0, want 3`). tests/formats now links `scripts/animated_textures.cpp` (it depends on
    nothing; the Makefile says why, and the link-graph guard still passes).
  - `RopeView` retried a refused mesh every frame and a refused colour texture for every rope:
    both are latched now.
  - `carriedPoint` (the rope's end on the object between ticks; render only, never at a tick)
    takes the trig LUT, as presentation trig must (AGENTS.md R6, `audit_arm11_math`).
  - Stale comments: main_3ds.cpp's header and object-pass note said the ropes were not ported;
    effects_view.h's `spriteCulled` described one texel.
- **Kept, with the reason:** the `@vfp-exempt R6` pin on `motionSwing` stays: the math audit
  flags libm trig on cold code too, and the swing is gameplay (it places a damaging object), so
  the LUT is not acceptable there. The 3DS rope quads take no depth pull (Vulkan's Lines do):
  a rope hangs in the air, with no surface to fight.
- **Not fixed (before this branch, outside the four opcodes):** when `updateCommand` kills an
  object, the C# Update returns at once; our `motionVelocity` still runs gravity and the
  velocity step on it (the new swing call is guarded). No retail case was checked.
- **Checks (this commit):** Linux build 0 warnings; tick1 528/528, vm_tests pass;
  `make -C tests/formats` 200681 checks, 0 failures; `tools/check.sh` all gates pass (smdh
  skipped: no .cia); `make -C mdk_3ds` 0 compiler warnings. The full `make -C tests/scripts
  test` passed on fd0c005a and again on this tree (below).
- **Watch list:** `docs/OPCODE_WATCHLIST.md` names each fix's commit.
- **Not done:** no hardware (owner away; no appliance use); no Mandarine run (optional; the
  machine's load was ~98 through this work).

## 2026-11-27 -- LEVEL4's ride end: the camera's arena over the other one, at full depth precision (branch fix/l4-ride-end-depth)
- **The bug (upstream e1eb36bb), and that this port showed it.** At the end of LEVEL4's
  snowboard tube (CMEAT_1, by the doorway to MEAT_3) both arenas are drawn, and MEAT_3's flat
  shell lies above CMEAT_1's floor. Depth-tested, the shell won: a flat dark grey plane covered
  the tube's floor and walls and Kurt below the knees. Linux headless
  (`--level=4 --arena=CMEAT_1 --at=62.35,2582.81,-392.90,90`, lavapipe, `DISPLAY=:99`, f517348b):
  50,499 of 76,800 pixels differ from the fixed frame at 320x240 on EVERY frame from the
  scripts' first tick (frame 30, 60, 120, 240, 480; frame 1 draws one arena and is identical);
  walking back up the tube (`--walk=back@30-600`) 36,614-49,398 pixels at frames 60-480. It is
  a place, not a moment: it lasts as long as Kurt and the camera are in that end of the tube
  with MEAT_3 drawn (the ride itself was not reproduced; walking/standing there it never let
  up). The 3DS shows it too: Mandarine r1.2.1 (md5 0d2adab6 checked, `emurun.sh`, `DISPLAY=:99`,
  the only MDK instance) on `throwaway/l4-ride-end-hw-base` (throwaway/combined-hw 25180768,
  `AUTOQUIT=60 FIXED=1 LEVEL=4 AT=62.35,2582.81,-392.90,90 ARENA=CMEAT_1
  TOGGLES=enableLighting=0`): the same grey plane, Kurt cut at the knees.
- **Not like upstream.** Upstream drew every draw in one of two halves of the depth range
  (viewport min/max depth). Owner decision: no -- it halves the depth precision of every draw
  on the 3DS. Instead, full precision everywhere, and the original's own rule (0x41df35: the
  other arena first, the camera's arena over it) only where it can change a pixel:
  - `ScriptRuntime::cameraArena` (upstream CameraArena, 0x490db4): Kurt's arena, or the second
    one when the line from 3 units above his feet to the camera leaves through a doorway into it.
  - `ArenaViews::draw` (both frontends): with two arenas drawn, the camera arena's box grown to
    take in the eye (the stereo separation, at least 1 unit, around it). If no batch of the
    other arena cuts it, nothing is marked and the frame is drawn exactly as before. If one
    does, the other arena's batches (and, through the new `mdk_render_set_back`, its objects)
    are queued `back`, and the camera arena's batches whose box-with-the-eye cuts the box around
    those are `cover` (`render/arena_scene.h` gives the argument: a point that hides another
    lies on the segment from the eye to it).
  - `DrawQueue` keeps `back` draws in their own buckets. Vulkan: the back buckets, then each
    opaque cover draw's depth at the far plane (viewport depth range [1,1]; no colour), then
    every bucket as before; the enhanced look's camera-depth prepass does the same and the
    sun's map reads both sets. Citro3D (D24S8): the opaque cover draws first, marking the
    stencil; the back buckets only where it is unmarked; then the rest -- NO draw is added on
    the 3DS, only order and a cached stencil state (set every eye; the round-camera insets'
    depth reset clears the stencil too). One picture, two ways (`render/draw_queue.h`).
- **Not done like upstream, recorded:** the other arena's effects, debris and ropes are not
  marked; Kurt and the camera arena's objects stay depth-tested against the other arena
  (upstream drew them over it); a round camera's inset replays the frame camera's marks.
- **Independent review** (read-only agent over the first version): found that the other
  arena's glass, drawn first with only its overlapping batches, was painted over by its own
  walls drawn later (fixed: the whole other arena and its objects go first); that the Vulkan
  cover depth came from another vertex program than the enhanced draw it must let through
  (fixed: far plane); the stereo margin (fixed: from the separation); test gaps (fixed:
  `LevelViewMarksTheLayers`, a camera-beyond-the-doorway case). The first version also added a
  depth-only re-draw of the camera arena on the 3DS too; measured on Linux at doorways it is
  +2..+45 draws and up to +58 % triangles in most two-arena frames (the boxes cut in 272 of 336
  doorway teleports), so the 3DS was moved to the stencil, which adds none.
- **How often it changes a frame:** 84 corridor doorways x 4 yaws (every level), teleported
  onto the doorway, frame 40, 160x120, base vs fixed: 33 of 336 frames differ, every one with
  two arenas drawn; pixel changes are the camera's arena drawn over the other where they
  overlap. Unchanged: level 3 frame 1 (the pinned frame gate), levels 3/5/6/7/8 walking from
  their starts (frames 1/300/900), LEVEL4's MEAT_7 doorway walk.
- **Cost:** one arena-box test and a pass over the two views' batch boxes per frame with two
  arenas drawn; nothing with one. 3DS: no extra vertices; one stencil-state change per phase
  change, per eye. Linux: the cover draws' depth again (+0..45 draws). NOT MEASURED on the Old
  3DS (no appliance).
- **Tests:** `vm_tests` `ArenaLayering` (box arithmetic, the queue) and `CameraArenaLevel4`
  (DepthBandTests' two data facts plus the camera out through CMEAT_1's doorway; a
  `return currentArena_` mutation fails it); `tests/formats` `LevelViewMarksTheLayers`;
  `tests/scripts/arena_cover_test.sh` (upstream's rides_test check: Kurt's dark legs at the
  ride end, both looks: 0 / 0 on f517348b, 2937 / 2961 fixed, needs >= 1000);
  `main_3ds_flow_test.sh`'s arena-draw pattern follows the new call.
- **Checks** (`nice -n 19 ionice -c2 -n7`): Linux build 0 warnings; 3DS build (`make -C
  mdk_3ds`) 0 warnings; `make -C tests/scripts test` exit 0, 1004 PASS, 0 FAIL, 4 SKIP (the
  frame gate's oracle arms); `make -C tests/formats` 200,681 checks, 0 failures;
  `tools/check.sh` ALL GATES PASSED (gates_smdh SKIP). (This worktree needed the usual
  untracked `logs/ralph/disabled` park marker for gates_batch_integrity.)
- **Emulator (the stencil path):** the same harness build with this branch merged
  (`throwaway/l4-ride-end-hw`, merge left uncommitted there), Mandarine r1.2.1 md5 0d2adab6,
  `DISPLAY=:99`, `emurun.sh --secs 40 --shot`: base shows the grey plane and Kurt cut at the
  knees; fixed shows the tube's snow and walls, Kurt whole with the board at his feet. Both runs
  logged the harness teleport into CMEAT_1 and reached frame 5 in the log (the tree logs frames
  1-5 only); the emulator's timing is not the Old 3DS's. During the fixed run another
  emulator (`/tmp/kilo/heap-growth/before.3dsx`, owner unknown, possibly MDK) was also running.
- **Evidence kept** (`.git/worktrees/l4-ride-end/evidence/`, not committed): the Linux A/B at
  the ride end (original f120, enhanced f60), the Mandarine pair (`emu_3ds_{base,fix,ab}.png`,
  their emurun logs), the doorway sweep's table.
- **Not verified:** nothing on hardware (no appliance); the 3DS cost is argued (no added
  draws), not measured; no C# oracle frame of e1eb36bb (no dotnet host on this machine), so
  "same pixels as upstream" is argued, not measured; the ride itself (from its start) was not
  driven.
- **Commits:** eaef0d79 (the fix, tests, this entry) and this ledger update.

## 2026-11-27 -- fix-arena-entry-stutter recorded as merged; its content was already here (branch test/3ds-combined-11-26)

- **What:** 20e2521f merges fix-arena-entry-stutter with `-s ours`: the tree is unchanged.
- **Why:** each of its six commits is an older copy of a commit this branch (and main) already
  has: c657187e = d0a80228 (the sky's per-palette linear table, the arena-entry stutter fix),
  1d5b1382 = 4f92fe25 (scope zoom X/B, pause handoff), 8ec5e1fb = 6c10b4c6 (harness UNLOCK),
  16b9f53b = 95da0680 and 85aaf3be = cbfe36d3 (instruments), 49e3c824 = dcb61d21 (ledger) --
  same subjects, the newer copies in main's history. A content merge conflicted in 11 files
  and would only have re-added the harness (harness_3ds.*, hw_cycle.sh, render_c3d_stats.h,
  the profile instruments) that 930508e1 moved off this line to harness/3ds-profiling.
- **Evidence:** `git merge-base --is-ancestor d0a80228` holds for main, this branch,
  perf/3ds-hd-bubble and both sides of the 11-26 HD A/B, so the doorway worst frames measured
  there (L7 CDANT_3->DANT_4 86-102 ms, L8 CGUNT_1->GUNT_2 173-207 ms) were taken WITH the sky
  table; they come from elsewhere (L4's measured case: a synchronous view build, ~250 ms; see
  perf/3ds-hd-bubble's ledger, 2026-11-27). No code changed, so no build or test was rerun.
- **Commit:** 20e2521f (merge) and this one (ledger).

### The fall's pause menu -> MAIN MENU sent the next fall back to the menu (2026-11-26, fix/fall-pause-main-menu)
- **Owner report (combined 11-26 build, 3DS):** a level's fall, START, MAIN MENU, another level:
  its briefing shows, then the game is back at the main menu instead of the fall.
- **Root cause (both frontends):** the fall's pause menu (`menu::PauseMenu fallPause`,
  mdk_3ds/src/menu_screens_3ds.cpp:291, mdk_linux/src/menu_screens.cpp:246, added with upstream
  75ec50e2) is a local of the screens loop and outlives the fall. Its "Main menu" item
  (`PauseMenu::act`, mdk_core/src/game/pause_menu.cpp:75) sets the answer `Menu` and leaves the
  menu open, as the C# does -- but the C# makes a new PauseMenu with each FallScreen. Here the
  next fall's first frame found `fallPause.open()` still true, took the `update` path
  (menu_screens_3ds.cpp:454) and got the stale `Menu` back: straight to the main menu.
  `PauseMenu::show` (which clears the answer) was never reached, because the menu was "open".
- **Fix:** `openFall` (the C#'s new FallScreen) starts a new `fallPause` before `fall->open`, in
  both frontends. Nothing is configured on that menu elsewhere, so the fresh one is the same as
  the first.
- **Test:** `fall_frontend_test.sh` case 2b (Linux, headless): new game, the fall, Esc, Main
  menu, new game again -- the second fall must be the run's last flow line, with two falls and
  two main menus. Without the fix: FAIL (3 main menus, ends at the menu); with it: PASS. Case 0
  reads both frontends' `openFall` for the reset before `fall->open` (the 3DS's cannot run on a
  host); a mutant without the 3DS line fails it.
- **Evidence:** Linux build 0 warnings; `make -C tests/scripts test` rc 0 (`PASS FallFrontend`,
  `PASS MenuFrontend`, `PASS Main3dsFlow (22 checks, 64 arms, 24 probes)`, tick1 all cases
  match); `tests/formats` 200670 checks, 0 failures; `tools/check.sh` ALL GATES PASSED
  (gates_smdh SKIP, no .cia); 3DS build rc 0, 0 compiler warnings. An independent read-only
  review agreed on the cause and found no defect in the fix. Not run in Mandarine or on a
  console: the defect is in both frontends' shared shape and is reproduced and pinned on the
  Linux frontend.
- **Other ways to the main menu, checked for the same stale state:** the 3DS level's START menu
  (`EnhancedMenu`, closed with `menu.setOpen(false)` on Menu, statistics and the next fall;
  `pauseOptions` and the cheats page cannot be open when MAIN MENU is chosen); the Linux level's
  pause menu (reset at the level's end, main.cpp:3091); death and game-finished (the 3DS does
  not close the START menu there, but the simulation is paused while it is open, so neither can
  happen with it open); the fall's own game over (the pause menu is closed then). None has the
  defect.
- **Found, not fixed:** the fall pause menu's OPTIONS page is never given the settings
  (nor the host, the fonts or the print: `items()` is set up for the level's pause menus only),
  so on its pages the setting rows have blank labels and change nothing, on both frontends.
  Pre-existing since 75ec50e2; fixed in the next entry, its own commit.
- **Commit:** this one (branch fix/fall-pause-main-menu, from test/3ds-combined-11-26).

### The fall's pause menu's OPTIONS pages were blank and inert (2026-11-27, fix/fall-pause-main-menu)
- **What:** the fall's pause menu (both frontends) now gives its items the main menu's fonts,
  `Host::Console`, the front's settings and the print line before it shows (the level pause
  menus' wiring: mdk_linux/src/main.cpp ~2114, mdk_3ds/src/main_3ds.cpp ~1673). A change on its
  options pages goes through `fallApplied`: saved as the main menu's are (`applied`: Linux
  `saveSettings`, 3DS `front->save` = `saveConfigNow`) and applied at once to the fall's own
  mixer (`applyAudio`), as the level's are to the level's.
- **Why:** before, `fallPause.items()` was never set up: no settings, so every setting row's label
  was empty and a step did nothing; no host, so the pages were the desktop oracle's; no print,
  so nothing reached the log. The C# PauseMenu (75ec50e2) is made over the Ui, with its
  settings and fonts.
- **Test:** `fall_frontend_test.sh` case 2c (Linux, headless): in the fall, Esc, Options, Audio,
  Left -- the log has "Menu: Resume | Options | Main menu | Quit", "Menu: Graphics | Audio |
  Game | Back" and the labelled volumes, and the run's own settings.cfg has master_volume=70.
  Against the binary without this change: FAIL (4 checks: no lines, the file kept 80); with it:
  PASS. Case 0's second check reads both frontends for `fallPause.items()` and the four setters
  in the 12 lines before the fall's `fallPause.show();`; a mutant without any one of them (3DS)
  or without the setSettings (Linux) fails it.
- **Not like the C#:** the options page's song (`_ui.Play(_song, Bus.Music)`) is not played in
  the fall, as it is not in the level's pause menu on this port.
- **Evidence:** Linux build 0 warnings; `make -C tests/scripts test` rc 0 (`PASS FallFrontend`
  with 2c, `PASS MenuFrontend`, `PASS Main3dsFlow (22 checks, 64 arms, 24 probes)`, tick1 all
  cases match); `tests/formats` 200670 checks, 0 failures; `tools/check.sh` ALL GATES PASSED
  (gates_smdh SKIP, no .cia); 3DS build rc 0 (menu_screens_3ds.cpp recompiled), 0 compiler
  warnings. An independent read-only review found no defect (the fall's mixer lives whenever
  `fallApplied` can run; the 3DS `front->save` is `saveConfigNow`, the level's saver). Not run
  in Mandarine or on a console: the 3DS side is pinned by the source check only.
- **Commit:** this one.
### The Old 3DS "menu-exit hang" of 2026-11-26: not a menu exit; one run end for every frame loop
- **Asked:** root-cause the hard hang of `old3ds-lan167` (no ping, no ARP, UDP `cmd reboot`
  not consumed, physical power needed), on the hypothesis that quitting from the MAIN MENU
  tears down with the menu's slide reads or music worker live.
- **The session named as the hang is not one.** `sd-pull-1851/mdk.prev2.log` line 606 (the
  only session on the card without `--- session end ---`) is the `L5a` cycle of
  `throwaway/overnight-smoke-hw` at 09446445 (pushed ~08:17; built after 09446445 08:09 and
  before ca63e48e's GOD=1 at 08:34; `.git/worktrees/overnight-smoke-hw/hw-logs/L5a-cap.out`).
  Kurt died at frame 2279, the menu came up, its slides loaded at frame 2698 -- and that
  harness never asks its deadline outside the level loop (`harnessAutoquitDue` is called only
  in main_3ds.cpp's level loop on every 11-26 throwaway carrying the 7d86f347 port:
  overnight-smoke, decal-zfight, combined, hd-bubble-base, mip-root, second-core-hw2;
  `harness/3ds-profiling` and `fix/3ds-hd-only` do ask it in the menus and the stream). The
  game sat in the menu: hw_cycle `TIMEOUT ... consecutive failed pulls=0`, "HTTP answers but no
  new session end", and the next push (08:36) was received by the stub. The console was alive.
- **A quit from the main menu was run on the appliance and was clean.** `throwaway/death-menu-hw`
  14bd3a01 (which added the menus' autoquit), cycle A 12:15-12:28
  (`.git/worktrees/death-menu-hw/hw-logs/A.session.log`): three deaths, the menu each time, the
  last visit `menu: slides loaded at frame 4756 (8 loaded)` with MAINSONG on the worker, then
  `exiting after 14258 frames ... harness autoquit`, `--- session end ---`, FRESH, and the next
  lanes' cycles (12:32-13:21) ran normally.
- **The real hard hang** began during `decal-zfight`'s `after-shot.3dsx` cycle (pushed 13:22,
  push returned after 367 s, `hw-after-shot.cycle.txt`: 48 consecutive failed pulls from the
  start of the poll; ARP-dead by 13:46, `bullet-cam/hw-logs/hw-shot2.cycle.txt`). That run
  quit from INSIDE level 7, not from a menu, and its log on the card (sd-pull-1851 `mdk.log`,
  5th session) is complete: `exiting after 4360 frames (5381 steps): harness autoquit`,
  `--- session end ---`. HTTP had stopped answering while the game was still running; the ATM
  `screenshot` requests sent mid-run came back "not committed within 30s" then "artifacts.txt
  timed out" (`shots-after-{1,2,3}.json`). The overnight `restore-strips` wedge (~02:46,
  WORK_LEDGER "the appliance stopped answering after the screenshot run") has the same shape:
  mid-run ATM screenshot sets "not committed", then :8080 and :17491 dead. Ranked candidates:
  (1) ATM/Rosalina's live capture path (`RosalinaMenu_CaptureScreenshotSet`, netload_stub
  `sysmodules/rosalina/source/menus.c`) under a running MDK -- firmware, outside this repo;
  (2) something in the run itself that kills the network sysmodules mid-run. Not proven:
  no ATM log or crash dump from 13:30 has been read (both are on the card).
- **Teardown audit (this tree and 09446445), no defect found:** every thread is joined before
  exit (the streaming worker, or on 09446445 the log writer, the HD reader and the audio worker),
  `ndspExit` runs on every open, the render targets' deletes and `C3D_Fini` wait for the GPU
  queue (citro3d's `C3D_RenderTargetDelete` disassembled: `gxCmdQueueWait(-1)`), romfs is
  released after its readers. An independent read-only review agent reached the same verdict.
- **What was fixed** (the defect class behind the stranded session): the run's own end was a
  field per loop (`FrontScreens3ds::frameBudget`, `Stream3dsFrontend::frameBudget`) and a
  harness's deadline one call in the level loop. `platform/run_end_3ds.{h,cpp}`'s
  `runEnded3ds(frame, &reason)` is now the one check the level loop, the front screens and the
  stream all make; a harness adds its deadline there (`throwaway/menu-exit-hw` d4c41824 does).
  `--frames` behaves as before (same position, reason, teardown).
  `tests/scripts/run_end_test.sh` fails any `aptMainLoop()` frame loop in `mdk_3ds/src` without
  the call and any per-loop `frameBudget` copy, and proves both on seeded copies of the tree
  (against the unfixed tree it reports 8 failures).
- **Evidence:**
  - Linux build (RelWithDebInfo): 0 warnings. 3DS player build (`make -C mdk_3ds`): 0 warnings.
  - `make -C tests/scripts test`: exit 0, 1004 PASS, 0 FAIL, 4 SKIP (frame-gate oracle arms);
    RunEnd 3/3. `make -C tests/formats`: 200,670 checks, 0 failures. `tools/check.sh` (with
    `MDK_DATA_DIR`): ALL GATES PASSED (gates_smdh SKIP, no .cia).
  - Mandarine r1.2.1 (md5 0d2adab6), `DISPLAY=:99`, `emurun.sh --secs 150`, the only MDK
    instance (a perfect_dark instance was up). Menu-only boot, `AUTOQUIT=60`:
    - fixed (`fixed-menu60.3dsx`, md5 44e7d073): splash, MAINSONG, slides loaded at frame 724,
      `audio: dsp close`, `exiting after 3477 frames (0 steps): harness autoquit`,
      `reader: at stop`, `--- session end ---`;
    - unfixed (`throwaway/combined-hw` 33b97b0a, `unfixed-menu60.3dsx`, md5 efb1221d): slides
      loaded at frame 723, then 14 ten-second worker summaries in the menu and no exit -- the
      L5a stranding reproduced. The emulator does not hang on either.
- **Hardware: NOT RUN** (owner away; nobody can power-cycle the appliance). Payloads ready in
  `.git/worktrees/menu-exit/hw-logs/`: `fixed-menu60.3dsx` (autoquit from the main menu after
  the slides load) and `fixed-L5-death-menu120.3dsx` (the L5a scene: LEVEL=5, HDROMFS, the
  L5a toggles, no god; Kurt's death, the menu, autoquit there at 120 s). `unfixed-menu60.3dsx`
  is kept for the emulator only: on the console it would sit in the menu until a UDP reboot.
  Before any of them: pull `/luma/atm/atm.log` and `/luma/dumps/arm11/` for 13:2x-13:4x.
- **Commit:** 1fde4fbd (the fix and its test); this entry.

## 2026-11-27 -- test/3ds-combined-11-26 (ff13fd11) merged into fix/opcodes-parity
- **What:** the combined line's ride-end ordering (fix/l4-ride-end-depth), the fall's pause menu
  and the run-end fix merged into the opcode branch. Conflicts resolved keeping both intents:
  `LevelView::draw(ArenaLayer, box, eye, margin, frames)` -- the ride end's back/cover marks
  and opcode 133's per-batch frame are independent fields of the same MeshDraw; `ArenaViews::draw`
  passes the frames on all three layers; both frontends set the frames pointer, then make the
  ride end's `arenaViews.draw(*frameDrawn, cameraArena, eye, separation)` call; both host tests
  (`LevelViewMarksTheLayers`, `LevelViewDrawsTheScriptsTextureFrame`) kept, the seam stub
  records frames and back/cover counts; the ledger is the union of both sides.
- **Ropes in the ride-end ordering:** `RopeView::draw` takes the frame's `layeredBack` and queues
  the rope of an object in that arena `back` (`MeshDraw::back`; the same buckets
  `mdk_render_set_back` routes to), so the pendulum's rope and the dog's tethers go under the
  camera arena's cover with their arena. Ropes are never `cover` and never make a frame layered
  on their own.
- **Found by the resolution's review (a gap on the combined line, not made by the merge):** the
  objects of the `back` arena are not actually queued `back`. The frontends switch
  `mdk_render_set_back` on around `ObjectView::addObject`, but that only fills the object
  stream; `ObjectDraw::draw` submits it later, after the flag is off, and sets no `back`. Fixed
  in the next commit on this branch (kept separate from the merge so it can be judged alone).
- **Checks:** Linux build 0 warnings; `make -C tests/scripts test` exit 0 (1014 PASS, 0 FAIL,
  ArenaCover pass, the seven opcode goldens pass); `make -C tests/formats` 200692 checks, 0
  failures; `tools/check.sh` all gates pass (smdh skipped); `make -C mdk_3ds` 0 compiler
  warnings. Linux frames of the pendulum (OLYM_6 f330), the dog (MUSE_5 f120) and the comm
  screen (DANT_5 scope f110) are pixel-identical to the pre-merge captures (those spots draw
  one arena or nothing layered). An independent read-only review of the resolution found no
  lost or duplicated code and no caller of an old signature.
- **Not verified:** no layered frame with a rope in the other arena was looked at (no retail
  rope sits at a two-arena doorway that was found); no hardware, no Mandarine.


### The ride end's `back` arena: its objects are now actually queued `back` (fix/opcodes-parity)
- **What was wrong (on the combined line since eaef0d79):** both frontends switched
  `mdk_render_set_back` on around `ObjectView::addObject` for the objects of the arena
  `ArenaViews::draw` drew `back`. `addObject` only fills the object stream; `ObjectDraw::draw`
  submits it after the loop, when the flag is off again, and it set no `MeshDraw::back`. So the
  other arena's objects were drawn in the ordinary buckets, not under its glass with it as the
  ride-end entry says, and the ropes made `back` by the merge would have been out of step with
  their objects.
- **What:** `ObjectStream::back` (the frontend sets it per object, where it called
  `mdk_render_set_back`; cleared at reset and after the loop), copied into each
  `ObjectStreamDraw::back` by `addObject`, and `ObjectDraw::draw` sets `MeshDraw::back` from it.
  The ridden board, the strike's subject and the sniper rounds are added after the loop, unmarked,
  as before. `mdk_render_set_back` stays in the render API (unused by the frontends now).
- **Evidence:** a scratch build counting the `back` draws the Vulkan backend receives (not
  committed): at the ride end (`--level=4 --arena=CMEAT_1 --at=62.35,2582.81,-392.90,90`,
  60 frames) 2706 non-identity-world (object) draws arrive `back` with this fix, and by
  construction none did before (the only path was the renderer flag, off when the stream is
  submitted). The ride end's frames 60 and 120 at 320x240 are pixel-identical before and after
  (no MEAT_3 object is in view there), and `ArenaCover` still passes; no frame where the change
  shows was found or looked at.
- **Checks:** Linux build 0 warnings; `make -C tests/scripts test` exit 0 (1014 PASS);
  `make -C tests/formats` 200692 checks, 0 failures; `tools/check.sh` all gates pass (smdh
  skipped); `make -C mdk_3ds` 0 compiler warnings.
- **Not verified:** no hardware, no Mandarine; no host test pins the stream's `back` (ObjectDraw
  is not linked by either host suite).

## 2026-11-27 -- every sound effect placed in the DSP arena at level load; core 0 decodes and copies none in play (branch perf/3ds-sfx-core1)

- **What:** docs/3DS_ARCHITECTURE.md 2 gave core 1 "feeding sound-effect samples to their
  buffer", and the second-core inventory (c95310c9) left `SoundBank::get`'s first-play decode
  and the DSP arena copy (`arenaGet`) on core 0. Option A of the brief: the effects are
  level-resident. `audioDspPlaceEffects` (after `audioDspOpen`, behind the loading screen; also
  when the fall and the stream screen open) decodes every `SoundBank::effectNames` sound,
  copies it into the 2 MiB arena (PCM8 where the low bytes are zero, else PCM16, as before),
  marks the entry resident (never evicted; matched by name, or by its Sound for the fall's and
  the stream's unnamed `#<address>` voices) and drops the bank's int16 copy
  (`SoundBank::dropSamples`: the Entry stays, the name is pinned so the budget cannot evict it).
  The first-play copy is kept only as a counted fallback ("IN PLAY" in `audio: dsp close`).
  No worker change, no new thread, no per-model code; the DSP still mixes ndsp 0-7.
- **Why A, from measurement** (`tick1 sfxset <level>`, retail data; arena = the bytes
  audio_dsp_3ds stores, 32-byte aligned; decoded = the int16 store):

  | level | effects | arena bytes | % of 2 MiB | int16 decoded |
  |---|---|---|---|---|
  | 3 | 78 | 1,435,360 | 68% | 2,757,206 |
  | 4 | 81 | 1,280,096 | 61% | 2,406,222 |
  | 5 | 80 | 1,188,512 | 57% | 2,263,492 |
  | 6 | 80 | 1,214,624 | 58% | 2,315,572 |
  | 7 | 79 | 1,137,696 | 54% | 2,161,762 |
  | 8 | 74 | 1,206,272 | 58% | 2,288,136 |
  | fall | 23 | 1,063,680 | 51% | 1,317,574 |

  Every set fits with >= 650 KB to spare, none comes from the streamed LEVELnO.SNI (0 demand
  reads), and the load cost measured 0.24 s (below), so B (a worker job class with a ready flag
  and a late-play rule) buys nothing A does not. The music-flagged entries `effectNames` leaves
  out: no retail CMI spells one outside its arena music table (sfxset's second line, all six
  levels), so none is expected on the effects bus; the level-end line would count one.
- **Hardware** (old3ds-lan167, CFGU model 0, 268 MHz, L2 none, stereo on, 30 fps cap, the
  owner's toggles with HD on, no HD RomFS; `hw_cycle.sh --profile`, both FRESH, push verified).
  Route: level 7, `AT=100,2320,-60,90 ARENA=DANT_5 GOD=1 FIXED=1 AUTOQUIT=150`, walk forward
  820-2600 with two turns and a shot every 60 frames from 800 to 2480 (29 shots). `RIDE=XE` was
  in the build line but the `throwaway/combined-hw` harness does not wire it, so Kurt walked
  and fired rather than rode the bomber: 23 distinct effects first-played. Baseline =
  `throwaway/sfx-core1-base-hw` 80618d01 (combined-hw + the instrumentation commit only);
  change = `throwaway/sfx-core1-hw` 0d0c16e2 (combined-hw + this branch's code). One run each.

  | measure | baseline | change |
  |---|---|---|
  | effects decoded on core 0 during play | 23 (241,581 samples) | **0** |
  | arena copies on core 0 during play | 23, 21.0 ms (slowest 2.31 ms) | **0** |
  | decode time on core 0 during play | ~21 ms (est.: 88.8 ns/sample, from the change's load decode) | 0 |
  | effect samples held in the main heap during play | 483 KB and growing with play | 0 B |
  | placement at load | -- | 79 of 79, 1,137,696 B; decode 96.0 ms + copy 73.3 ms, 237.5 ms in all |
  | level load (`level: loaded in`) | 10,264 ms | 10,425 ms (+161 ms) |
  | frame period mean / p95 / p99 | 34.174 / <=34.9 / <=64.1 ms | 34.190 / <=34.9 / <=63.8 ms |
  | frames over 33.3 ms (of ~4,388) | 2,571 | 2,572 |
  | worker busy, steady 10 s windows | 6-8% (music 5.7-7.3%) | 6-8% (music 5.7-6.6%) |
  | music underruns / DSP dropped frames | 0 / 0 | 0 / 0 |
  | main heap at the loop (used / free) | 23,295,744 / 1,816,800 | 23,312,328 / 1,923,096 |

  Logs: `.git/worktrees/sfx-core1/hw-logs/` (`base-L7.log`, `new-L7.log` -- the latter holds
  both sessions; `s-base.txt` / `s-new.txt` are the two cut apart).
- **A run that did not measure:** `FALL=3` (to time the fall's and level 3's placement) is not
  wired on this harness either: the build booted to the main menu, never autoquit, and the
  cycle timed out after 900 s. The console was left running MDK's menu, so a UDP `cmd reboot`
  was sent at 01:57:56 (boot_count 788 -> 789, confirmed) to give it back to netload for the
  next lock holder. No screenshots were requested.
- **Checks:** Linux build 0 warnings; `make -C tests/scripts test` rc 0 (L3_audio_meta and
  L3_audio_mix PASS: no sample byte moved); `tests/formats` 200,670 checks, 0 failures;
  `tools/check.sh` ALL GATES PASSED; 3DS release build (clean) 0 warnings. Independent
  read-only review: a dropped sound could be evicted by the budget and its arena entry then
  matched by a reused pointer (fixed: `dropSamples` pins, pointer matching only for `#` names);
  the stream screen also played effects (now placed too); log arithmetic nits fixed.
- **Not verified:** the fall's and the stream screen's placement on hardware (host sizes only);
  level 3's placement time (the largest set, 1.44 MB: ~0.3 s by the level-7 rate); the other
  five levels on hardware; a heavier fight (the bomber ride); the owner's New 3DS; Mandarine
  (not run); HOME/sleep (no lifecycle change).
- **Commits:** fe00876a (instrumentation, `tick1 sfxset`), 358597f0 (the placement), this entry.

## 2026-11-27 -- test/3ds-combined-11-26 (6c5b3bcb, perf/3ds-sfx-core1) merged into fix/opcodes-parity
- **What:** the combined line's new tip (sound effects placed in the DSP arena at level load)
  merged after the ff13fd11 merge and the object `back` fix, so the coordinator's merge of this
  branch is clean. Only the ledger conflicted (union); `main_3ds.cpp` and `tick1.cpp` merged
  without conflict (their sfx-set dump and audio calls beside this branch's rope draw and
  hazards tokens).
- **Checks:** Linux build 0 warnings; `make -C tests/scripts test` exit 0 (1014 PASS, ArenaCover
  and the eight opcode goldens pass); `make -C tests/formats` 200692 checks, 0 failures;
  `tools/check.sh` all gates pass (smdh skipped); `make -C mdk_3ds` 0 compiler warnings.
## 2026-11-27 -- the heap after a death's reload (branch fix/3ds-heap-growth)

### What grew per reload: nothing in use; the heap's top, from fragmentation
- **The report:** on the Old 3DS, in the death-menu flow (die, main menu, Continue or a save, the level again), `mem[level-teardown]` did not come back to the previous level's figure: `used` 619,800 -> 632,432 -> 640,176 bytes (+12.6 KB, +7.7 KB) and the in-play `arena` 26.74 -> 27.59 -> 30.23 MiB (`throwaway/death-menu-hw` 14bd3a01 on `old3ds-lan167`, 2026-11-26, three teardowns: L7, L7 again with the first death in the scope, then L3 from a save).
- **Measured, in-use bytes -- not a leak:**
  - Mandarine, LEVEL3 death loop (`throwaway/heap-growth-hw`: `throwaway/combined-hw` + the death-menu walk, `KILLAT=240`, Continue at every menu visit, `TOGGLES=enableLighting=0`, no HD pack in the lane), 15 deaths with the scope entered in every level: `used` after each teardown 618,560 (x5) then 618,672 (x10). With the scope only in the second level: 589,904, then 618,560 -- the +28.6 KB is the scope's first use, once. Two runs, the same one-time +112 bytes at the sixth teardown. 0 bytes a reload after that.
  - The two hardware steps were each a first (the first sniper death; the first LEVEL3 after LEVEL7), not a rate.
  - Linux host (shared core), `--die --die-every-level --sniper`, under an LD_PRELOAD allocation census keyed by call stack (frame pointers, scratchpad tool, not in the tree): every stack in the game's code flat over 27 reloads; the only stacks that move are lavapipe's (+1.2-1.7 KB, then flat).
- **Measured, the heap's top -- fragmentation, a one-time 5.5 MiB step:** the in-play `arena` (newlib's sbrk top, the figure the 34.3 MiB Old 3DS cap applies to) was 25.18 MiB on the first load and 30.69 MiB on every reload after it (Mandarine, 15 deaths), with `used` the same. A heap map (`HEAPMAP=1` on the throwaway: newlib's `_malloc_r` family and `operator new` wrapped, the live blocks by address and caller) after a teardown: 1,544 blocks, ~605 KB, of which 258 blocks (~280 KB) sat between 4 and 24 MiB, i.e. among the level's own data:
  - ~250 from `FontView::load` -- the START menu's two `static mdk::FontView`s (main_3ds.cpp); each level's load copies a Font into them (`font_ = font`, a vector per glyph), and the first level's load put those vectors wherever the heap had room, in the middle of the level. They outlived the level; every later load had to place its large blocks around them.
  - 8 renderer-lifetime vectors that first grew during the level: the texture slot table (512 x 224 bytes = 114,688 at +22.6 MiB), a draw-queue bucket (10,240), the canvas vertices (12,288), the prepared draws (8,400), the creation log (4,096) and three small ones.
- **Fix:**
  - main_3ds.cpp: the two FontViews are the level's (heap, `unique_ptr` in the level block), freed with it; the static options page's MenuItems font pointers are cleared at the teardown (`setFonts(nullptr, nullptr)`; its `textWidth` already answers 0 for none).
  - 02_lifecycle.inc: the renderer's run-long tables reserved at creation, at about twice LEVEL3's measured capacities (textures 1024, creation log 1024, palettes 16, prepared 256, canvas quads 256 / vertices 1024, inset draws 16, pack queue 64, draw queue 256 solid / 64 each other pass; `DrawQueue::reserve`, new). `hdQueue` is not reserved: 04_streams.inc swaps a new vector in.
- **After (Mandarine, the same loop, 15 deaths each):**
  - Fonts only: `used` after teardown 464,848, then 465,000 / 465,112 flat; 8 blocks above 4 MiB; in-play `arena` 25.15 MiB on the first load, 28.99 MiB on every reload.
  - Fonts + tables: `used` after teardown 681,552, 681,712, then 681,824 flat; 1 block (132 bytes) above 4 MiB; the top at a teardown 7.72 MiB (was 22.68); in-play `arena` 25.58 MiB on the first load, **25.58-25.71 MiB on every reload** (was 30.69).
  - Per reload: in-use 0 bytes before and after; the heap's top in play **-4.98 MiB** on every reload after the first. Cost: +63 KB held for the run (681.8 vs 618.7 KB) and +0.40 MiB on the first load's top.
- **Test:** `tests/scripts/death_reload_test.sh` (in `make -C tests/scripts test`): the Linux frontend headless, LEVEL3, `--die --die-every-level --sniper` (`--die-every-level` is new, a test aid like `--event-every-level`), an Accept every 0.5 s so each menu visit takes Continue, 5400 frames (15 deaths). It fails when the floor of the heap in use after the teardowns rises more than 16 KB from teardowns 3-6 to the last four (floors, because a teardown can catch a worker's in-flight read: 1-3 MB spikes that are gone by the next), or the fds or the renderer log move. Clean runs: +4.4, +5.4, +6.1 KB alone, +1.0 KB inside the full suite. Non-vacuity: a 4 KB-a-reload leak injected into the Linux frontend rose 41 KB over 9 reloads and failed. It guards in-use bytes only; the fragmentation above is the console allocator's layout and is measured in Mandarine / on the console.
- **Evidence:** Linux build 0 warnings; `make -C tests/scripts test` rc 0; `make -C tests/formats` 200,670 checks, 0 failures; `tools/check.sh` ALL GATES PASSED (gates_smdh SKIP); 3DS build of this branch 0 warnings. Emulator: Mandarine r1.2.1 md5 0d2adab6, `DISPLAY=:99`, `mdk_3ds/tools/emurun.sh`, mono, one MDK instance (a perfect_dark instance was also running); guest logs kept in `/tmp/kilo/heap-growth/emu-{before,census,heapmap,after,after2}-mdk.log`. An independent read-only review found no lifetime defect in the font change; its findings (the wasted `hdQueue` reserve, the press schedule's clock, the test's scope note, the INT/TERM trap) were fixed.
- **Not verified:**
  - Nothing on the console for the fix: the appliance lock was held by other agents' campaigns all night. The emulator's heap is bigger (96 MiB cap) but the allocator is the same newlib in the same binary; the console's figures are not measured. Payloads for that A/B are built (`/tmp/kilo/heap-growth/hw-payload{,-after}.3dsx`: `AUTOQUIT=480 LEVEL=3 KILLAT=240 KILLSNIPE=-1`, Continue at every visit, the owner's SD config).
  - Only LEVEL3 was measured. A level that needs more than the reserved table sizes grows a table mid-level once (a pin, as before, but once).
  - With HD on (the owner's config), the HD copies table (`hdCopies`, `hdByPath`: one entry per HD file ever seen, never pruned) grows mid-level per new file: bounded by the pack's file count, and a fragmentation source of the same kind; not measured (the emulator lane has no HD pack).
- **Commit:** this one.
## 2026-11-27 -- the "L7 regression" on the combined tip: mostly a longer route; the real cost is the upstream object order and the pose blend (branch perf/3ds-l7-regression)

- **What:** the combined tip had 481 frames over 33.3 ms on the L7 door walk against 211 for the
  11-26 HD-window build (perf/3ds-hd-bubble's ledger, 2026-11-27). Bisected over the first-parent
  merges on the appliance with one route and one instrument for every build: each payload is that
  commit + throwaway/combined-hw 25180768's harness + a 5-second window of the frame's spans
  (sim, with the runtime and Kurt inside it; GPU wait at frame begin; object pass; effects; frame
  end; whole CPU frame). Branches throwaway/l7-{base,mid,bisect,tip}; logs
  `.git/mdk-review/l7-view-logs/{base,mid,pres,tip}-L7.log`.
- **Conditions:** old3ds-lan167, CFGU model 0, 268 MHz, L2 off, stereo on, HD on, PROFILE OG,
  `AUTOQUIT=244 LEVEL=7 HDROMFS=1 GOD=1 NOCLIP=1 PROFILE=1 ROUTE_START=8 HDIMPORTS=2` and the
  hd-bubble campaign's L7 waypoint route (DANT_1 ... DANT_4 ... DANT_1, turbo out, walk back).
  One run per build, every run FRESH.
- **The 211 was a shorter run, not a faster build.** The 11-26 runs were built with a route
  10 s shorter (autoquit 234 s against 244 s; the waypoint times are identical), and the extra
  10 s are spent idle in a scene that runs at the 60/30 Hz edge (CPU 17.2 ms a frame, 5-9 frames
  over 33.3 ms in every 5 s). On the same route the parent of the merges measures 438, not 211.
  The L8 "improvement" (1,745 against 2,243) is the same artefact the other way: the two L8
  routes differ in length (11,938 against 14,040 steps).
- **Same route, whole run** (frames over 33.3 ms / mean ms / 2-vblank frames):
  - e7dcd1a9 (perf/3ds-hd-bubble, the merges' parent): 438 / 18.19 / 946
  - 49db95d9 (+ fix/upstream-gameplay): 471 / 18.26 / 993
  - c5221a5b (+ fix/upstream-presentation): 504 / 18.32 / 1,024
  - 92011514 (the tip: + decals, bullet cams, load bar, review fixes, ride-end depth): 478 / 18.36 / 1,046
  - Run-to-run noise: the steady 150 s tail alone varies 172-205 over-33 frames between builds
    whose spans there are equal, so differences under ~30 frames are not a measurement.
- **Where the real cost is** (per-frame ms, same windows):
  - **fix/upstream-gameplay, 615be6d4 (upstream d14ed0b5, objects run newest first, per arena):**
    the 5 s Kurt spends in CDANT_3 by the DANT_3 door: script runtime 4.3 ms a frame on the
    parent, 6.1 (gameplay) / 6.5 (presentation) / sim 8.4 (tip); frames over 33.3 ms in that
    window 16 -> 43 / 48 / 52; the DANT_3->CDANT_3 crossing 8 -> 30 / 31 / 37. Host route probe
    (the same waypoints through ScriptRuntime::update, callgrind): 3eb94e4c -> 615be6d4 raises
    the runtime there from 18.5 to 30-34 us a tick, and the scripts call touchDamage 780 times
    in the first 76 s against 152 (worldBounds/poseBounds of every same-arena object per call,
    mdk_core/src/scripts/hazards.cpp:103-111). The objects now run in the original's order and
    their scripts take the original's paths -- the fix's purpose (LEVEL7 DANT_6's cannon); the
    cost is the game's, not a defect of the port.
  - **fix/upstream-presentation, 07297dd3 (upstream ae746b5d, poses between their frames):**
    the object pass in the steady DANT_4 scene 3.33 -> 3.66 ms a frame (ObjectView::addObject's
    per-vertex lerp, mdk_core/src/game/object_view.cpp:465-483). It is absorbed by the vsync
    wait there: frames over 33.3 ms in the tail 184 (parent) / 205 / 172 (tip), within noise.
  - Decals, bullet cams, load bar, ride-end depth: nothing measurable (GPU command words per
    frame unchanged, 7,224 vs 7,225; object pass and frame end equal within 0.1 ms).
- **Not fixed (no measured gain):** passing alpha 1 to the 3DS object poses would give back the
  0.32 ms of the object pass but no frames on this route; the d14ed0b5 cost is gameplay
  fidelity. Candidate for a separate, measured branch: a per-tick cache of the objects' world
  bounds in touchDamage (exact, so it keeps the oracle's hit set) -- not attempted under the
  gameplay gate tonight.
- **Recommendation:** no blocker for the merge to main from this measure; re-base the
  door-walk A/Bs on one route file so two campaigns cannot differ in length again.
- **Commit:** this one (ledger only; no code changed).
