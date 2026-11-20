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
