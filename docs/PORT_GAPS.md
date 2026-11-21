# The gameplay still to port from C# — a gap inventory (2026-11-20)

**What this is.** A read-only audit of the C# oracle in `src/` (Mdk.Game, Mdk.Engine, Mdk.Formats) against `mdk_core/src` and both frontends, done after the owner made gameplay porting the priority on 2026-11-20 ([docs/WORK_LEDGER.md](WORK_LEDGER.md)). Each package below is one agent's job of a few hours. They're ranked so the ones that stop a level being finished come first. Update the Status column as packages land.

**The headline (before package 1 landed): no level could be finished in the C++.** Every retail level ends through opcode 131 (`special_event`, value ≤ 50 → `EndTheLevel` → `EndLevel.cs`), and 131 has no handler. Neither frontend can load a following level either.

## How the opcodes were counted

A scanner decoded every reachable script in LEVEL3–8 using the C++ decoder and counted which opcodes appear. These counts replace the claim in `OPCODE_COVERAGE.md` that "none of the 11 fire". That claim held only because the reachability probe's Kurt stands still.

| Opcode | Name | L3 | L4 | L5 | L6 | L7 | L8 |
|---|---|---|---|---|---|---|---|
| 131 | special_event | 1 | 1 | 7 (51–55, 61, 81) | 1 | 3 (0, 92, 93) | 3 (0, 91, 92) |
| 158 | touch_damage | 7 | 14 | 21 | 4 | 13 | 9 |
| 114 | if_player_on_me | 8 | 7 | – | 5 | 14 | 4 |
| 167 | move_to_bomb | 21 | 11 | 6 | 13 | 16 | 17 |
| 172/178/184 | explosion / explosion_damage / explode | 8/–/5 | 4/36/5 | – | 2/–/– | 2/–/24 | 14/–/4 |
| 159 | spawn_box | 11 | 2 | 2 | – | – | – |
| 142 | fan_create (Kurt's updraft landed with package 8) | 2 | 5 | 4 | 6 | 2 | 4 |

`if_hit_weapon 0` (a sniper bullet) appears in every level. Weapon types 2, 3, 4 and -6 appear in L5 and L7.

## Packages

| # | Package | C# covered | What the C++ lacks | Impact / levels | Depends on | Status |
|---|---|---|---|---|---|---|
| 1 | End of level | EndLevel.cs; ScriptRuntime EndTheLevel, SpecialEvent ≤ 50; Kurt Frozen/Rising; FollowCamera rising | 131 falls to `noteUnimplemented` (script_vm.cpp:2294); no Frozen; Rising not ported | No level can be finished (L3–L8) | – | **landed 2026-11-20**: 131's ≤ 50 arm, EndLevel.cs, Frozen/Rising/Pose, the camera's rising, the torn arena and its pieces on both frontends, `levelEnded()` (goldens `L*_end_*.txt`); the frontends exit "level ended" at the hook #2 replaces |
| 2 | Next-level transition | Flow/Game.cs AfterLevel/Play/AfterStream; Viewer `_ending` | Both frontends exit on any screen other than Level; no teardown/reload | The game stops instead of loading the next level | 1 | **landed 2026-11-20**: `GameFlow::skipScreen`/`skipUnported` (the stream, statistics, fall and save prompt skipped the C#'s way, `golden/flow_next_level.txt` vs `tests/scripts/flow_probe`); a level loop with teardown and reload in both frontends (the 3DS backend's `mdk_render_mark/release`), `tests/scripts/level_chain_test.sh` (3 -> 4 -> 8 -> 5, byte-identical reloads, flat heap); 3DS unrun in the emulator |
| 3 | Solid objects, platforms, lifts (+ Keeper) | Collision/Solids.cs; ArenaSpace.Move with solids; Kurt Nearby/FindPlatform/Platform/Contact/Carry; SolidsWithin; ClipView; Kurt.Keeper.cs | arena_space move has no solids; getKurtPlatform returns null; opcode 114 missing; the default-case GosubElse divergence (script_vm.cpp:2293 vs ScriptVm.cs:1799) | Kurt walks through doors and object walls and falls through lifts; lifts never move; floor triggers dead (L3, L4, L6, L7, L8) | – | **landed** 2026-11-20 (goldens `L*_plat_*`, `L7_corridor_locked`); the ride parts landed with #9 |
| 4 | Combat and hazard opcodes | ScriptVm 158/159/167/172/178/184; TouchDamage; SpawnBox; UpdatePathSpeed | All fall to the default case; their dependencies already exist | No contact damage; objects that should explode keep running; no explosion damage; aliens ignore the bomb (all levels) | – | done 2026-11-20: ported with nine C# goldens (`golden/L*_hazards_*.txt`), plus the path push the snowboarders ride and the box sprites; see WORK_LEDGER |
| 5 | Sniper A: scope and mode | Kurt.Sniper.cs, Scope.cs, Inventory SelectAmmo/UseAmmo, FollowCamera ScopeView | player.cpp:989 | Sniper can't be entered | – | **done 2026-11-20**: game/player_sniper.cpp, game/scope.{h,cpp}, Kurt ammo, follow_camera buildSniperCamera; kKeyZoomIn/Out; golden/L3_sniper, L7_sniper byte-exact. Seams: Player::setSniperHooks (#6's FireSniper), Scope::zoomLimit/toScreen/locks/lockLimit (#6's UpdateSniperTarget), buildSniperCamera + Scope state (#7) |
| 6 | Sniper B: rounds | Scripts/SniperRounds.cs; UpdateSniperTarget/FireSniper; opcode 28 | none | Sniper targets, homing and mortar rounds; L7 mortar puzzle | 5 | **done 2026-11-20**: scripts/sniper_rounds.{h,cpp} (the rounds, plus ScriptRuntime's UpdateSniperTarget/FireSniper/Sweep), opcode 28, hooks bound by the runtime; golden/L8_rounds_ship, L8_rounds_kill, L7_rounds_mortar byte-exact. Seams: the strike arm (#14), the round models' draw and round cameras (#7), the wall's bullet hole (#16) |
| 7 | Sniper frontends and HUD | SniperOverlay, SniperView; zoom keys | sniper bound but not implemented | Sniper unusable on device | 5, 6 | **done 2026-11-20**: game/sniper_overlay.{h,cpp} (HudView's), game/sniper_view{,_draw}.cpp, the scope camera/listener/eye and Kurt hidden in both frontends, the rounds in flight drawn, the render seam's insets (Vulkan: Scene + Own; 3DS: Own only, the clip), the Linux wheel; golden/L3_sniper_iris, L8_sniper_hud_ship, L8_sniper_hud_kill byte-exact; Linux frames vs the oracle's --sniper. Open: 3DS round cameras (owner-gated), the strike (#14) |
| 8 | Updrafts, slide, wind | Kurt.Updraft.cs, KurtUpdraft/FanSpark; Kurt.Slide.cs, WindZone, opcode 224 | player.cpp:854-880, 940 | Fans don't lift Kurt (all levels); L6 wind tunnels | – | **landed 2026-11-20**: Kurt.Updraft.cs and Kurt.Slide.cs in `Player` (with FallMove's Sliding branch, CameraRoll.Slide, EnterArena), KurtUpdraft / FanSpark / WindZone and opcode 224 (`scripts/updraft_wind.cpp`); goldens `L*_wind_*.txt`, and package 4's fan-dependent hazards goldens re-pinned (`L4_hazards_fan`, `L6_hazards_balls`). There is no crouch-slide in the C#: the slide is the wind tunnels' |
| 9 | Rides core + XD2 | Scripts/Rides.cs, Kurt.Ride.cs | ridesRidden_/ridesBoard_ always null | L7 DANT_9; foundation for 10 and 11 | 3 | **landed 2026-11-20**: `scripts/rides.{h,cpp}` (Rides.cs: Update, TryMount, MountWalker, UpdateWalker, Follow, Dismount, Lost, RideWalker, TakesHits), Kurt.Ride.cs into `Player` (Walk.Riding, Visible, RideKeys, RideWalker, JumpOff, GetOffBoard, Glide, UpdateGun, the Ride delegate as `Player::RideHook` and its branch in Update and Land); opcode 171 true for the XD2 Kurt rides; hits go to it; KurtSprite hides him; the frontends draw the ridden XD2 with Kurt (Driven). Goldens `L7_ride_xd2`, `L7_ride_lost`, `L7_ride_test` vs `tests/scripts/ride_probe`. Seams for #10/#11: `Rides::board_`/`bomber_` (TryMount finds XSNOWB/XE and leaves Kurt off them), `Player::RideHook`, `glide`, `getOffBoard` |
| 10 | Snowboard | Scripts/Snowboard.cs | the board drop is not applied | L4's snowboard run | 9 | **landed 2026-11-20**: `scripts/snowboard.{h,cpp}` (Snowboard.cs whole: steering, speed, slope, pitch, jump, sounds, path follow, heading, Place, Ram, the K_SURF/K_SURFJ pose and pivot, GetOff's thrown board), `Rides::mountBoard` and the board arms of Update/Dismount/Lost/TakesHits, the solid-arena drop on the board, `CameraRoll::follow`, the K_SURF muzzle. Goldens `L4_board_ride`, `_off`, `_lost`, `_dead`, `_end` (the run to MEAT_5's end box) vs `tests/scripts/board_probe`, byte for byte |
| 11 | XE bomber | Scripts/Bomber.cs, BomberOverlay, LookDown | none | L7 DANT_5 bomber ride | 9 | |
| 12 | Ledges | Kurt.Ledge.cs; ArenaSpace Free/Edge | player.cpp:650 | No ledge grab or climb | – | **landed 2026-11-20**: `game/player_ledge.cpp` (GrabLedge, HangFrom, UpdateClimb), `ArenaSpace::edge`/`free`, `Bsp::edge`; four C# goldens (`golden/L{4,6}_ledge_*.txt`) from `tests/scripts/ledge_probe`; no ledge in any start arena, so no 3DS harness walk yet (see WORK_LEDGER) |
| 13 | Cutscenes and special events 51–93 | SpecialEvent arms, Start/End/CutsceneTick, CutsceneCamera.cs, visibility | no Cutscene branch in runTick; nothing writes `visible` | Boss intros, Gunter, end of game | 1 | **landed 2026-11-20**: the arms 52–93, RunTick's Cutscene branch, the shots (to the bit), objects' and Kurt's Visible, `CutsceneCamera` on both frontends, `gameFinished()` (the frontends exit "game finished"); 51's cutscene half, its strike scene left to 14 (goldens `L*_cut_*.txt`) |
| 14 | Air strike and strike scene | AirStrike.cs, StrikeScene.cs | none | L5 final fight | 5, 6, 13 | **landed 2026-11-20**: scripts/air_strike.{h,cpp} (AirStrike, BezierPath), scripts/strike_scene.{h,cpp} (StrikeScene, `playStrikeScene`), `fireSniper`'s strike arm, 51's strike half (131 is whole), game/strike_view.{h,cpp} and both frontends (the game waits, sky + model, Esc/START skips; StrikeUsed kept across a death); goldens `L5_strike_gunter`, `L3_strike_teeth`, `L5_strike_event51` byte-exact. Seam: #7's overlay `SniperView::aim` calls `airStrike().findTarget` once #7 merges |
| 15 | Town timer, shake, sky modes, group texture | TownTicks; FollowCamera UpdateShake; SkyModes; opcodes 140, 202 | no consumers | Shake on every level; minecrawler town messages | Messages | **done 2026-11-20**: the town timer (TownTicksByDifficulty, the countdown, FlattenTown: shake 5, OOT_L<n>[A] zoomed in front, the flattened flags; no GameOver, as the C#), the shake (ShakeRaised -> `FollowCamera::raiseShake`, UpdateShake, Turn's shake half in update and blend; the C#'s time-seeded Random is the oracle's seeded generator with an injected seed), `game/sky_modes.h` (BackdropOf/ClearOf/SkyColour) in both frontends' frame (`MdkFrameParams::backdrop`: the 3DS skips its dome and, for Keep, its colour clear; Vulkan skips the sky, Keep drawn as Clear), and 140's consumer (`retexturedArenas` -> `ArenaViews::refresh` -> `LevelView::refresh`). Goldens `golden/L*_screen_*.txt` vs `tests/scripts/screen_probe` (the probe seeds the C# camera's Random by reflection, so the shake is exact); see WORK_LEDGER |
| 16 | Bullet holes (opcode 130) | Scripts/BulletHoles.cs | none | Cosmetic (L5, L6, L8) | 6 | **done 2026-11-20**: scripts/bullet_holes.{h,cpp} (Nearest, Weights, Stamp; ScriptRuntime's StampBulletHole/ArenaTexture), opcode 130, stamped textures pinned against the arena stream, both frontends re-upload them (`SurfaceCache::refresh`; the 3DS re-expands one a frame and drops the HD copy); golden/L8_holes_guntam, L6_holes_xb2 byte-exact. No wall holes: the C#'s rounds stamp none |
| 17 | Fall (pre-level skydive): sim, then screen | Fall/*.cs | 17a (the simulation) is ported: `fall/fall_sim.{h,cpp}`, `fall/fall_missile.{h,cpp}`, golden/L*_skydive_*.txt. 17b lacks the screen and view (FallScreen, FallView, FallModels, FallGround, FallPalette, FallHud) and the flow does not enter it yet | Skydive, and the pickups carried into the level | 2 | 17a done (2026-11-20); 17b open |
| 18 | Stream (post-level tube): sim, then screen | Stream/*.cs | 18a sim done: `mdk_core/src/stream/` (Flight, StreamTube, TubeBasis, WatcomRandom), byte-exact against `tests/scripts/stream_probe` (golden/stream_*.txt). 18b done: `stream/stream_screen`, `stream/stream_view`, both frontends (Linux `--stream=N`, 3DS `STREAM=1`), the flow's seed and end health; the report matches the C# app byte for byte (golden/stream_screen_L*.txt), the models golden/stream_view_models.txt | Tube ride; the Gunter route | 2 | 18a done 2026-11-20; 18b done 2026-11-20 (see WORK_LEDGER) |
| 19 | Save/load | SaveGames, Snapshot, Restore, SavePrompt | owner cut 2026-11-05 | No saves; a death reloads the level | 2 | **done 2026-11-20**: util/json, game/save_games, save_prompt, viewer_saves, scripts/script_snapshot; the C#'s files byte for byte (golden/L*_save_*.txt vs tests/scripts/save_probe, both ways cross-loaded; golden/save_flow.txt, json_floats.txt); death -> LASTGAME -> the main menu (#20a), whose Continue reloads the level, and its Saved Game page lists and loads the saves (golden/menu_saves.txt vs menu_probe); Linux F2/F9 + `--save-dir/--load/--save/--die`, 3DS pause-menu X/Y + `DIE=1`/`SAVETEST=1`; a load's arenas enter Kurt's space (the one-ArenaSpace hole, for loads). The after-LEVEL8 and statistics prompts' drawing is #20a's (`SavePrompt::layout`); 3DS unrun in the emulator |
| 20 | Menus and screens | MainMenu, PauseMenu, Stats, EndMovie, Settings | Menu/ cut | Briefings, statistics, difficulty | 2 | **un-cut by the owner 2026-11-20**. **20a landed 2026-11-20**: MainMenu, MenuItems (options pages per host), PauseMenu (Linux as the C#'s; on the 3DS its Options and Main menu are rows of the START menu), Settings (the C#'s settings.cfg on Linux, config.ini's game lines on the 3DS; difficulty, gore and the volumes applied as a level starts), the Briefing (StatsScreen's Briefing page, Typesetter); the menu is both frontends' entry screen (`--level`/`LEVEL=` still start in a level). Goldens `golden/menu_*.txt` vs `tests/scripts/menu_probe`; Linux menu and briefing frames byte-identical to the C#'s. Open: 20b (Statistics, Slideshow, Heads -- StatsScreen's last page can reuse `Briefing`), 20c (EndMovie, VideoPlayer/MDK12.FLC behind the menu, IntroSplash, LoadingScreen), the menus' sounds, the pointer, #19's save hooks (`menu::MenuSaves`) |
| 21 | Cheats | Flow/Cheats.cs, Kurt.Cheats.cs | player.cpp:911 | Gore toggle, god mode | – | **done 2026-11-20**: game/cheats (TOOSCARYFORME, SEETHEWHOLEGAME, Viewer.TypeCheats' filter), Kurt's Mortality and Clipping (god: no damage, no fall-out death; noclip: UpdateNoclip), MainMenu's typed keys (DebugKey, Really Quit?'s Y/N); opcode 13 stays the C#'s explicit false. Linux: typed in play, `--god`/`--noclip`, `--type=TEXT@S`; 3DS: the START menu's CHEATS page (gore, god, noclip), `GOD=1 NOCLIP=1`. Goldens `cheats_typed`, `cheats_menu`, `cheats_L3_noclip`, `cheats_L3_god`, `cheats_L7_hurt` vs `tests/scripts/cheats_probe`. Open: SEETHEWHOLEGAME on the 3DS (no keyboard), the 3DS `down` button (unbound) |

## Files the packages share

These packages edit the same C++ files, so they're ported in worktrees and merged one at a time, in order:

| File | Packages |
|---|---|
| `script_runtime.{h,cpp}` | 1, 3, 4, 6, 8–11, 13–16 |
| `script_vm.cpp` | 1, 3, 4, 6, 8, 13, 16 |
| `player.{h,cpp}` | 1, 3, 5, 8–13, 21 |
| `follow_camera` | 1, 3, 5, 11, 12, 15 |
| `arena_space` | 3, 12 |
| the frontends | 2, 7, 11, 13, 14, 17–20 |

Merge order: 1 before 2; 3 before 9–11; 5 before 6, and 6 before 7, 14 and 16.

## Ledger entries now stale (fix them when the package they belong to lands)

- `OPCODE_COVERAGE.md`'s partial table — FIXED 2026-11-20 with package 4, each re-verified in the code:
  - 165/166: `itemsBomb` has a writer (items.cpp) — moved to complete.
  - 81/195: written by the chain gun, the blast, the mortar, Twister (81), the cow (195) and now touch_damage; they stay partial for the sniper rounds' writer alone (package 6).
  - 215: the white flash is drawn (`hud_view.cpp`) — moved to complete.
  - 16/56: `kill` runs the explode effects (`ScriptRuntime::explode`) — moved to complete.

## Oracle gaps — don't chase these

- Arena-embedded sounds that the C# never plays.
- The C# never raises GameOver.
- Opcodes the C# leaves stubbed: 13, 77, 239, 199, 210 and 250.
- Opcodes absent from both the C# and the retail scripts: 90, 141, 144–148, 155, 182, 196, 225 and 227.
- Behaviour that can't be pinned to the oracle, because the C# itself isn't deterministic there:
  - the camera shake's time-seeded random (package #15 pins it anyway: its probe swaps the
    camera's `new Random()` for `new Random(seed)` by reflection, and the C++ seeds its port of
    that generator the same);
  - the Stream's TickCount seeds.
- The 1996-demo code.
