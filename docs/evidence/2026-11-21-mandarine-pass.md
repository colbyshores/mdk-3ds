# Mandarine emulator evidence, 2026-11-20/21

The console was lent to ../forsaken, so 3DS checks ran in Mandarine r1.2.1 (md5 0d2adab6) through mdk_3ds/tools/emurun.sh and the scratchpad's one-emulator lock (emu_one.sh), on DISPLAY=:99 with llvmpipe, in mono. Geometry-shader-lit builds used TOGGLES='enableLighting=0', on the owner's direction. These runs show what works. They are never a timing source.

## 2026-11-20 emulator pass (console lent to Forsaken). Mandarine r1.2.1 md5 0d2adab6, emurun.sh, DISPLAY=:99, llvmpipe, mono
- E11 d29975e: levels 3-8 lit+shadowed, bake loads 39-135 ms, 0 GS splits (evidence file on its branch, uncommitted).
- core-1 6807a5f, level 7, 3000 frames: `cpu: New 3DS; syscore time limit 30% -> 0 (read back 30%)`; reader and DSP audio worker on core 1; music queued=171 restarts=0 dspdropped=0 over 50 s; Kurt pack 148 ms. Probe says cores 2 AND 3 ok -- Mandarine emulates a New 3DS and does not model core reservation, so the Old-3DS refusal and the real core-3 answer are hardware-only.
- gun fc3a869 (+walk), level 8, WALK fire held: gun works in logic (voices 17-19, plays 77->252, debris objects up), the large blue wedge is NOT a bug: an enemy BIGBOLT plasma bolt reaching Kurt; Linux draws the same model at frame 355 (/tmp/kilo/bolt-compare.png). Open hardware question: whether the PICA200 clips at this port.s near plane (Mandarine.s GL clips regardless); queue a near-plane probe for the console.
- tip 627e6bc (gun+walk+E12+items), level 7 E13 route + jumps/fire/itemnext/useitem, 5700 frames, no crash: controls.ini written with the owner defaults; Kurt start set 164/164 frames, 4,841,472 GPU bytes of 5,242,880, 277 ms; route crosses DANT_1 -> CDANT_1 -> (stream switch) -> DANT_2 as on Linux; music dspdropped 0. Unlit black faces in the capture are the GS-lit path (Mandarine has no geometry shader; E11 not merged), not a regression.
- package 1 (22a74e0 = main 9853f81), L3 EVENT=1, lighting off: special_event at frame 60, LevelEnded 459, exit "level ended" 489 -- the same frames as the Linux headless run; no torn-mesh REFUSED line; Kurt's K_TAKEOF/K_FLOATC streamed (8 animations, 2 draws held). Note: the pack stream's staging peaked at 274,267 of its 262,144 bound and dropped 1 read -- check the stream bound on hardware.
- package 4 (ece831a = main c05c24f), lighting off: L8 AT=-209,480,40,90 fire: the XBANG ahead blows up by frame 120 (voices 3 -> 27, objects 23 -> 20, peak draws 68 -> 108); L5 AT=190,199,-570,90 standing: health 100 -> 92 (by f120) -> 89 (by f540), as the agent's golden; L4 snowboarder run clean (plays 2 -> 50 as it moves; its path is pinned by L4_hazards_snow).
- package 2 (8a7fc04), L3 EVENT=1 EVENT_EVERY=1, lighting off: the 3DS chains 3 -> 4 -> 8 -> 5 -> Menu (exit "level ended" at frame 1956), each with its Stream/Statistics/Fall (or SavePrompt) skips logged and the DSP sound reopened per level. Linear free after each teardown is 25,285,504 B every time (run start 25,817,600). Heap "used" after teardown 326,616 -> 327,816 -> 329,928 B: a ~1-2 KB/level residue to find (not a blocker).
- package 5 (7c91a69 = main 9fb8235), L3 sniper walk, lighting off: in sniper mode Kurt's feet stay at (-4.0,0.0,191.8) through back/turn/zoom/fire; turnright turns yaw 96.0 -> 51.2; firing plays 7 -> 18; controls.ini gains zoom_in=none zoom_out=none and logs sniper as gameplay-only (scope view is #7).
- package 3 (main b226bea), L7 AT=0,380,15,90 forward + strafe, lighting off: Kurt stops at the locked X7DOOR at y=478.4 (the C# probe's figure) and slides along it (x 0 -> 13.2) -- no more walking through closed doors.
- package 2/fall/stream merges: no new emulator scene beyond the chain run above (the sims have no screen yet).
- package 8 (e56b7d9), lighting off: L6 COLYM_1 wind slide -- Kurt slides down the tunnel (z -90 -> -234, y -775 -> -507 over 7 s), slower while braking (frames 250-330), turns after; the 3DS loads the 4 slide animations (28 frames). L3 CHMO_6 fan -- Kurt rises z -390 -> -236 in 2 s.
- package 13 (e51ccb4), lighting off: L5 EVENT=61 frames Gunter (capture /tmp/kilo/mdk-cuts.png right); L7 EVENT=93 hides Kurt and holds a fixed shot of the skyline -- the C#'s target is the arena controller at the origin, the suspected oracle bug (needs the original EXE to settle); L5 EVENT=81 ends the run "game finished" at frame 61 (end movie is #20c).
- package 20a (d27edab), lighting off: the 3DS boots to the main menu (MDKOPT background + logo; "New Game | Level: 1 | Saved Game | Options | Quit"); MENU=Accept,Back,Accept goes New Game -> L1_MAP briefing -> (Fall skipped) -> level 7 at frame 330 with the START hint. Capture /tmp/kilo/mdk-menus.png. Owner to judge: the menu text runs close to the top screen's left edge at 0.67 scale.
- package 19 (ef12c72), lighting off: L7 DIE=1 -- "Kurt died at frame 316", LASTGAME loaded, level 7 again at 317 with deaths 1. L3 SAVETEST=1 -- saved 30,148 B (57.6 ms in the emulator, not a hardware figure), loaded, restored hash 77c80424abe4ca48, 13 objects, Kurt back in HMO_1. FAIL (harness, not save): L3 AT=677.5,2125,41,96 starts in HMO_1 with HMO_7 unloaded, Kurt falls forever and no save happens -- sent back to the agent (AT must resolve its arena).
- package 7 (c39934d), L3 sniper walk, lighting off, 960 frames: the 3DS draws the full scope screen -- frame/mask, scene through the scope camera (Kurt hidden), the three round-camera windows (empty, by design on the 3DS), zoom readout 50 + gauge, ammo clip, health 95. Zoom stayed 50 (agent expected 59 with the zoomin walk) -- with zoom_in unbound the WALK zoom may not reach the scope; check when the owner picks zoom buttons. Capture: scratchpad scope-3ds.png.
- package 16 (438773e), L8 GUNT_10 sniper shot at the turret, lighting off: INCONCLUSIVE. None of forward@40-77/78/79 logged a stamp, and the captures at the deadline show the normal view with the turret off screen. The agent's hit window is knife-edge (37/39 hit, 35/41 miss in the C#) at yaw 123.8, where the LUT/newlib rounding (accepted by the owner) can tip it. The goldens prove the stamp path. The 3DS texture rebuild after a stamp still needs seeing: moved to the L8 playthrough (shoot the turret by hand).
- package 18b (acd650b), lighting off: L3 EVENT=1 -- after level 3 the 3DS plays the whole stream (first wall hit at segment 42, Bones at 163, Survived health 1 after 33.7 s), then the flow goes on toward level 4. STREAM=1 LEVEL=7 capture mid-ride: space backdrop, translucent tube (the 3DS colour table), lights, Kurt flying, health box 54 (scratchpad tube-3ds.png).
- package 19 on main (a6234d3), lighting off: L7 DIE=1 MENU=Menu.Accept@2 -- Kurt died at 316, "the menu follows (deaths 1)", the menu offers "Continue | New Game | ...", and Continue plays level 7 again at frame 438 (the C#'s death path). L3 SAVETEST=1 AT=677.5,2125,41,96 ARENA=HMO_7 -- "harness ARENA=HMO_7: Kurt teleported in", saved 4,807 B, loaded, restored, "Kurt at (677.5,2125.0,-0.0) in HMO_7" and still standing at frame 2100 (the restored arena is in Kurt's space). The earlier HMO_7 failure is fixed.
- package 10 (d1a68cb), L4 AT=420,4285,-570,90 ARENA=CMEAT_3 + steering walk, lighting off: Kurt mounts the board and rides down the slope (y 4312 -> 4631, z -585 -> -663 over frames 120-480, yaw swinging with the steering); capture shows him in the K_SURF pose on the board (scratchpad board-3ds.png).
- package 11 (b66434d), L7 ARENA=DANT_5 + the probe's walk, lighting off: INCONCLUSIVE. Kurt is teleported into DANT_5 and moves, but never mounts: no top-down view at frame 1200, the follow camera, health 72. The walk needs exact turns (shoot the comm device, then walk into the XE), tuned in the C#, and the 3DS trig rounding (LUT + newlib, accepted) can shift them. Same class as the bullet-hole shot. For the playthrough pass: add harness shortcuts (start mounted / call the XE) or outcome-tolerant walks, so these set pieces are checked by hand or by robust scripts.
- package 14 on main (41002d2), L5 EVENT=51, lighting off: event 51 fires at frame 60 and the run goes on cleanly to frame 360, past the strike scene (61-300) into 51's cutscene (Kurt at the origin in the red sky, the C#'s own target quirk). The X_STRIKD plane itself isn't captured (the screenshot is at the deadline) -- a mid-scene look goes on the playthrough list.
- package 15 on main (edc63b3), lighting off: L3 EVENT=0 -- special_event at frame 60 (the end-of-level shake), level ended at 459 as before, no crash. L3 AT=-60,1410,-960,0 ARENA=HMO_3 -- "sky: mode -1 (Keep) at frame 1" (opcode 202's consumer on the 3DS: no sky dome, depth-only clear). Shake offsets not captured (pinned by L3_screen_town / L3_screen_hmo3).
- package 21 on main (a94ffa9c), L3 NOCLIP=1 jump then forward, lighting off: "cheats: god off, noclip on"; Kurt rises z 193 -> 253 while jump is held, then flies forward (y 0 -> 278) at constant height through the first room.
- smoke pass at c9b7292e: (1) boot -> INTRO1A splash -> menu -> New Game -> L1_MAP briefing -> fall (skipped) -> loading screen -> level 7, played to autoquit at frame 8575, no crash; (2) LEVEL=3 EVENT=1 -> stream (Bones' rescue, survived health 1) -> statistics intermission (waits for keys) -> autoquit at 9604.

## The set pieces with the harness shortcuts (281b251b)
- **Bomber:** L7 ARENA=DANT_5 RIDE=XE. The harness calls the XE (hitting DANT_5 group 16) and mounts XE_9000 at 9.88 s.
- **XD2:** L7 ARENA=DANT_9 RIDE=XD2. Kurt is mounted on XD2_1002 at 1.02 s; the capture shows the XD2 from behind, with Kurt inside.
- **Air strike:** L5 GIVE=SW_BONES AIM=0,1.25, sniper then fire. The CAPTURE=200 frame shows the X_STRIKB plane mid-flight over the sky alone, which is the strike scene. This was inconclusive before the shortcuts.
- **Save prompt after the L8 stream:** L8 EVENT=1 MENU=Accept,Accept. The flow goes stream → "the save prompt before level 5 (offering "6")" → "saved game 6" → level 5 at frame 2487.
- **Bullet-hole turret:** L8 ARENA=GUNT_10 AIM=123.80164,-26.158365, sniper then fire. INCONCLUSIVE ON BOTH PLATFORMS:
  - The scope reaches the turret on the 3DS, and Linux shows the same view.
  - No hole is visible at the crosshair in either capture (frames 185 and 260 on Linux, 260 on the 3DS). The red circle is the scope's target highlight.
  - The stamp lands on the nearest face (the C#'s approximation), which may be out of view. The goldens pin the stamp; a hand check goes on the hardware list.
- **Slideshow:** SPLASH=0. Not reached: MDK12.FLC was still playing at the 90 s deadline, and the slides start when it ends. Pinned by `movie_tests`. Needs a longer run.

## Level-by-level playthrough (Sonnet pass)

2026-11-21, branch of 281b251b, Mandarine r1.2.1 (md5 0d2adab6), emu_one.sh (one emulator, serial, DISPLAY=:99, llvmpipe, mono), every run `TOGGLES=enableLighting=0` and, for the gameplay runs, `FIXED=1` (guest logs under `/tmp/mdk-emurun.*`, shots `/tmp/kilo/mdk-play-<slug>.png`). Emulator only; nothing here is a hardware result or a timing.

How it was run. A blind walk cannot follow a level's critical path (it stops at the first wall: the routes need turns, and the 3DS trig rounding is accepted), so each level was covered by (a) its start arena with `forward+turbo` and `fire` held, and (b) the arenas that matter, entered with `AT=` and `ARENA=` at an interior point read from the DTI (portals, fans = type 7, turrets and enemies, pickups), with `GOD=1` and the same walk. A run passes when the arena loads, its objects spawn (`objects=` in the frame line), the chain gun's fire plays and the object count falls as enemies die, Kurt stands on a floor (a z that keeps falling means a bad teleport point), and there is no crash and no REFUSED/FAIL line. The game's chain order is the C#'s `GameState.Order = 7, 6, 3, 4, 8, 5`, not 3,4,8,5,6,7.

Classes: PASS works; EMU emulator limit; WALK a blind walk or a teleport point did not do what was wanted (not a bug); BUG.

### Ledge grab (asked for by name)
| Arena / scene | Command | Class | Note |
|---|---|---|---|
| L6 OLYM_3 ledge | `AUTOQUIT=60 LEVEL=6 AT=-1649,239,-669.9,0 ARENA=OLYM_3 WALK='forward@9-59;jump@11-29' FIXED=1` | PASS | Kurt ends at feet (-1638.6,239.0,-653.3), the C# probe golden's value (`L6_ledge_climb` step 60 area: feet64 -104871,15296,-41814 at the end), z up 16.7 (the 16-unit wall), standing on the platform in the shot. |

### Menu slideshow (asked for by name)
| Arena / scene | Command | Class | Note |
|---|---|---|---|
| Main menu video + slides | `SPLASH=0 AUTOQUIT=200` (also 170) | **BUG, fixed** | The menu stayed on the bare MDKOPT image for 200 s: no video, no slideshow, nothing logged. Cause: `mdk_3ds/tools/stage_romfs.sh`'s `stage_dir` is one directory deep, so `MISC/FLIC/` (Mdk12.flc) never reached the RomFS and the main menu never got the video whose end starts the slideshow. Fix: stage `misc/flic/mdk12.flc` (632 KB; pie.flc and shiny.flc are not read by the game, MDKEND.FLC and MDKBZK.MVE are not in the GOG data at all). After the fix, `SPLASH=0 AUTOQUIT=100` at 45 s shows an MDKS gameplay slide with the menu items over it (`mdk-play-menu3.png`), and at 80 s the video's last frame again (`mdk-play-menu4.png`): the cycle runs. Open nit: the "menu: slides loading off the main thread" / "loaded" log lines never print although the slides show, so that commit's ledger check cannot be read off the log. |

### Level 3 (HMO_*)
| Arena / scene | Command | Class | Note |
|---|---|---|---|
| Start HMO_1 | `AUTOQUIT=120 LEVEL=3 WALK='forward+turbo@30-7000;fire@40-7000' FIXED=1 GOD=1` | PASS | Loads (17 arenas); objects 11 -> 28 as HMO_2 streams in; fire kills (objects 29 -> 21, debris in the shot); Kurt walks 400 units to a wall at (-46,399,104). |
| HMO_2 (doors, z 133) | `AT=42,720,140,90 ARENA=HMO_2` + same walk | PASS | Loads, 12 objects, Kurt on the 133 floor. |
| HMO_6 (12 type-8) | `AT=-65,2261,-405,90 ARENA=HMO_6` | PASS | Spawns; objects 13 -> 10 under fire. |
| HMO_8 -> CHMO_6 fan | `AT=378,2419,-250,90 ARENA=HMO_8` | PASS | The arena hands over to CHMO_6 at the portal; Kurt drops to the corridor floor (z -397). |
| HMO_9 (XGS) | `AT=-189,2520,-295,90 ARENA=HMO_9` | PASS | 1 object left after fire. |
| Level end -> stream -> statistics -> save prompt -> fall -> L4 | `LEVEL=3 EVENT=1 EVENT_EVERY=1 FIXED=1 GOD=1 MENU=Menu.Accept@3,...` | PASS | `special_event` at 60, level ended 459, stream (Bones at segment 163, survived health 1, 33.7 s), Statistics Intermission -> Debriefing -> Score -> save prompt "Saved game 4: level 4" (frames 3623-4193) -> Briefing -> Fall (health 100 -> 29, unsteered) -> level 4 at frame 7105. |

### Level 4 (MEAT_*)
| Arena / scene | Command | Class | Note |
|---|---|---|---|
| Start MEAT_1 | `LEVEL=4 WALK=forward+turbo,fire` | PASS | Loads; Kurt falls 374 -> 221 into the teal tube and stops at y 24 (objects 0: the arena has none). |
| MEAT_4 | `AT=275,7378,-1130,90 ARENA=MEAT_4` | PASS | 5 objects, 4 after fire. |
| MEAT_6 | `AT=-132,13650,-2030,90 ARENA=MEAT_6` | PASS | Spawns, fire plays. |
| MEAT_7, first try | `AT=8,14333,-165,90 ARENA=MEAT_7` | WALK | That point is the arena's far portal; Kurt dropped into CMEAT_6 and kept falling (z -6928, GOD on). |
| MEAT_7 | `AT=-7,14674,10,90 ARENA=MEAT_7` | PASS | 18 objects, Kurt on the floor, plays 254 -> 351. |
| MEAT_10 | `AT=-227,23903,-1300,90 ARENA=MEAT_10` | PASS | 25 objects, 21 after fire. |
| Board ride, hazards | not rerun | - | Covered in the 2026-11-20 section (package 10). |
| Level end -> stream -> statistics | chain run | PASS | Level 4 ended at 7564 -> stream (survived, health 1) -> Statistics Intermission. |

### Level 8 (GUNT_*)
| Arena / scene | Command | Class | Note |
|---|---|---|---|
| Start GUNT_1 | `LEVEL=8 WALK=forward+turbo,fire` | PASS | 22 objects, 20 voices, 435 plays; Kurt at y 251. |
| GUNT_3 | `AT=-304,962,85,90 ARENA=GUNT_3` | PASS | Loads, Kurt on the floor, 7 objects. |
| GUNT_6 (12 type-5) | `AT=129,2300,-250,90 ARENA=GUNT_6` | PASS | 18 -> 23 objects, 450 plays. |
| GUNT_7 (XFORKs) | `AT=23,2643,-290,90 ARENA=GUNT_7` | PASS | 27 objects, 216 plays. |
| GUNT_8 | `AT=-324,3022,-290,90 ARENA=GUNT_8` | PASS | Kurt drops to z -377 (the arena's lower level), 11-12 objects. |
| GUNT_10 sniper at the turret | `AT=20,3360,-360,0 ARENA=GUNT_10 AIM=123.80164,-26.158365 WALK='sniper@150-152;fire@190-200'` (3 runs) | INCONCLUSIVE | The scope screen draws, health 100 -> 91 (the turret hits Kurt), but the scope shows a ceiling pattern, no turret is in view and no stamp is logged; the BHOLE stamp stays unseen. Needs the C# viewer's frame at this aim to compare. |
| Level end -> stream (Gunter) -> save prompt -> L5 | `LEVEL=8 EVENT=1 EVENT_EVERY=1 MENU=Menu.Accept@3,@7,...` | PASS | Ended 459 -> stream (Gunter, survived health 63) -> "save prompt before level 5 (offering 6)", closes "saved game 6" -> level 5. |

### Level 5 (MUSE_*)
| Arena / scene | Command | Class | Note |
|---|---|---|---|
| Start MUSE_1 | `LEVEL=5 WALK=forward+turbo,fire` | PASS | 13 objects, 643 plays. |
| MUSE_2 (146 records) | `AT=197,-10,-520,90 ARENA=MUSE_2` | PASS | 9 objects, Kurt on the -580 floor. |
| MUSE_3 | `AT=141,382,-1070,90 ARENA=MUSE_3` | PASS | 4 objects, fire plays. |
| MUSE_5 (4 fans) | `AT=391,203,-2250,90 ARENA=MUSE_5` | PASS | Kurt rises z -2250 -> -2220: the fans lift him, as CHMO_6's did. |
| Level end -> Menu | `LEVEL=5 EVENT=1` (inside the L8 chain) | PASS | Ended -> "the flow shows Menu" -> main menu (INTRO1A splash first, "Level: 6"). |
| Game end | `LEVEL=5 AT=465,-6,-2263,0 EVENT=81 MENU=...` | PASS | Event 81 -> "the end movies (done)" -> main menu -> New Game -> briefing L6_MAP -> fall -> level 5 again (the flow's last index), as the C# order says. |

### Level 6 (OLYM_*)
| Arena / scene | Command | Class | Note |
|---|---|---|---|
| Start OLYM_1 | `LEVEL=6 WALK=forward+turbo,fire` | PASS | 5 objects; Kurt walks 88 units to the start room's wall. |
| OLYM_5 (fans) | `AT=-1648,159,-1650,90 ARENA=OLYM_5` | PASS | 24-28 objects, Kurt on a floor. |
| OLYM_6 (37 type-8) | `AT=-1912,3448,-2100,90 ARENA=OLYM_6` | PASS | 20 objects, 154 draws. |
| OLYM_7 (XT) | `AT=-2191,2445,-2910,90 ARENA=OLYM_7` | PASS | 22 objects, 320 plays. |
| OLYM_8 (XTR) | `AT=-2374,2950,-2230,90 ARENA=OLYM_8` | WALK | No floor under that point (a long bridge arena); Kurt fell to z -15000 under GOD. |
| OLYM_10 (XB2 boss) -> COLYM_8 | `AT=3274,2479,-2350,90 ARENA=OLYM_10` | PASS | The arena hands over to COLYM_8, 7 objects, Kurt standing. |
| OLYM_3 ledge | see the top table | PASS | |
| Level end | not reached | - | Needs the level's own route. |

### Level 7 (DANT_*)
| Arena / scene | Command | Class | Note |
|---|---|---|---|
| Start DANT_1 | `LEVEL=7 WALK=forward+turbo,fire` | PASS | Kurt stops at the locked door, y 478.4 z 15.0 (the 2026-11-20 figure), 20 objects. |
| DANT_3, first try | `AT=208,1442,-100,90 ARENA=DANT_3` | WALK | At the portal edge: Kurt fell through the corridor (z -5800), GOD on. |
| DANT_3 | `AT=280,1000,-60,90 ARENA=DANT_3` | PASS | 33 objects, 24 after fire. |
| DANT_7 (12 type-8) | `AT=381,4513,-85,90 ARENA=DANT_7` | PASS | 16 -> 15 objects. |
| DANT_10, first try (z 15) | `AT=482,5001,15,90 ARENA=DANT_10` | WALK | Below the arena's floor (z 20): fell. |
| DANT_10 boss arena | `AT=489,5010,32,90 ARENA=DANT_10` | PASS | Lands on z 20, 4 objects. |
| Bomber / XD2 / tube | not rerun | - | Covered in the 2026-11-20 section. |

### Not reached
- Level 6's and level 7's ends, and the 7 -> 6 -> 3 links, by playing the levels: blind walks cannot take the routes. The links ride the same `LevelFlow` code as the chain runs that passed.
- A hand-aimed bullet-hole stamp (GUNT_10) and the destroyed-turret texture rebuild.
- Doors, lifts and pickups were not exercised by a walk (teleports skip the trigger geometry); they stay pinned by the goldens (`L7_plat_door`, `L4_plat_trigger`, ...).
- Many surfaces render black or near-black even with lighting off (DANT_7, GUNT_7, the first arena of L4): Mandarine has no geometry shader; judge on hardware.
