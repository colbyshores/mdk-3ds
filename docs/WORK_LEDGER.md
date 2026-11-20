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
