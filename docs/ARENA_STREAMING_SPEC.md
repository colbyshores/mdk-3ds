<!-- Owner-authored spec, given in chat to session ses_e1b51c97dffeG5kCaCKPplQYu7 (kilo, 2026-11-16) and recovered verbatim from ~/.local/share/kilo/kilo.db on 2026-11-17 so it lives in the repo. This is the plan branch experimental/arena-streaming works to. Do not edit the text below; record progress in docs/CONTINUATION.md. -->

Create a new branch for this work (for example experimental/arena-streaming) before editing anything. Commit once per phase, and tag the commit at the end of each phase after hardware numbers are recorded. Do not merge to the main branch until every acceptance criterion in section 6 passes on hardware. If a phase makes things worse than the Phase 0 baseline, revert to the previous phase’s tag rather than patching forward.


Spec: Original-style arena streaming for the MDK 3DS port

Read this whole document before editing code. Work in the phases listed at the end, one phase at a time, and report measured numbers after each phase.

1. Goal

Replace the port's "whole level resident up front" loading with the original engine's streaming model, so peak memory on a 3DS is bounded by:

one streamed MTO arena (geometry, textures, palette, models),
one corridor (small, loaded separately),
the audio needed by the current and preloaded arena only.

This also removes the known hardware crash in SoundBank::forLevel, which loads all level SNI archives at once.

Non-goals: do not change script semantics, opcode behavior, Kurt movement, collision, or sprite residency. Do not redesign the renderer.

2. Current port state (facts from the codebase, verify before relying on them)
Mto holds the whole arena file resident. Mto::getArena() parses an arena on demand and caches it for the level's lifetime (mto.h:36-48).
ScriptRuntime::residentArena_ (script_runtime.h:862-864) tracks the last arena made resident through the arena_load opcode. It is what scripts observe. Keep this behavior. It is not a memory mechanism.
SoundBank::forLevel loads all three level SNI archives at once (sound_bank.cpp:15-37). SoundBank::evictOverBudget evicts decoded samples only. The archive bytes stay resident (sound.h:105, docs/CONTINUATION.md "budget-and-evict stays open").
Sprites already have a residency budget on 3DS (kurt_sprite.cpp:171-174, render_types.h:358,368). Out of scope.
The crash is believed to be OOM in SoundBank::forLevel on the 3DS heap. This is unconfirmed until the diag2 free-heap number is in. Do not assume it.
3. How the original engine worked

Confidence tags: [RE] = from published reverse engineering of MDKD3D.EXE with addresses given, recheck in Ghidra. [INF] = inference, not stated by any source. No developer-written source on this was found.

3.1 Level structure [RE]
A level is a chain of arenas joined by corridors. Arena names look like HMO_1, MEAT_3. Corridors use the same name with a C prefix (CHMO_1).
A connection is a doorway box with a direction. In LEVELn.DTI these are type 6 records, 36 bytes each (u32 type, s32 id, f32 angle, f32 x,y,z, char[12] name). They come in pairs sharing an id (1000+), one in the arena and one in the corridor. angle is the direction Kurt crosses to leave (0 -x, 1 +x, 2 -y, 3 +y, 6 -z, 7 +z).
Kurt changes arena only by crossing a connection or by teleport.
3.2 Data layout [RE]
LEVELnO.MTO: directory is u32 count, then per arena char[8] name, u32 offset. Each arena block starts with u32 size, then offsets for the models section, palette (112 x RGB), world section, and an embedded texture archive (HMO_n.MAT). So an arena's textures live inside its own block and stream and free with it.
World section: material names, BSP nodes (44 bytes each), triangles (36 bytes each), vertices, BSP leaf data.
The models section has 24-byte sound entries per arena (likely per-arena sound references, [INF], verify).
Corridor geometry is stored in the SNI sound archive in the arena world layout. Sources disagree on the file: LEVELnO.SNI (formats docs) versus LEVELnS.SNI (engine doc). Resolve this by inspecting the actual files.
Original source file names from executable strings: memblock.c, mdkfopen.c, chunks.c, and under ..\mdksrc\share\: readbin.c, loadmats.c, setupob.c, gifread.c, allocenm.c.
3.3 Streaming state machine [RE]
Only one MTO arena is resident at a time. _g_current_arena names it.
Corridors (arena flag +0x44 bit 1 clear) load and free separately, at 0x431914 and 0x431ad8.
arena_load (0x419d00) loads an arena without activating it. Corridors go to arena_activate (0x4194e4). MTO arenas go to arena_switch (0x41970c).
arena_switch sets the streaming flag (0x573b00) and frees the previous MTO arena's buffers (arena struct +0x24 and +0x40) before streaming the new one.
arena_load_step (0x41983c) reads one 32 KiB chunk and clears the flag when the last chunk arrives. game_frame (0x41d4d8) advances one chunk per frame.
arena_activate parses the streamed world (arena_parse_world), initializes triangle groups, registers textures, recreates object models.
While the streaming flag is set, the arena being streamed is not drawn and has no effects. Collision and effects are gated on it.
Music and sound load one step per frame (0x419ac0, 0x4193a4). arena_load also starts music preload (0x4192c4).
3.4 Active versus preloaded [RE]
Active: Kurt's arena (0x573a0c) and the second arena when flag 0x573a6c = 1. Only these update, draw, are solid, and run effects and scripts.
Preloaded (0x573a6c = 0): loaded and music-ready, but frozen, undrawn, not solid.
All other arenas are unloaded. At most two are active, during a crossing.
3.5 Triggers [RE]
game_frame checks type 6 connections every frame (via 0x41c550). Crossing one sets the second arena to the one Kurt left and loads the new one.
Type 1 trigger records (0x41bf1c) call BSPShow on their arena. Type 3 records call arena_load asynchronously and clear 0x573a6c when the arena changes.
Script opcode 100 calls BSPShow. Script opcode 223 calls arena_set_neighbour (0x41a2d0), which starts a background load of the second arena and clears 0x573a6c (preloaded, not active).
Doors (0x43cc68) call BSPShow when they start opening, and BSPShow(NONE) when they finish closing.
BSPShow (0x41a11c) blocks until streaming, sounds and music are loaded, then sets 0x573a6c = 1 and spawns the arena's DTI objects on first show.
Teleport (0x41bce4): into an MTO arena, the arena is shown directly. Into an unloaded corridor, the last-indexed connected arena is preloaded and other corridors are freed.
Level start (0x41ba68) and load game (0x42fb18) call BSPShow on Kurt's arena and the second one.
Object moves into an arena (0x43ca00) load it only if it is Kurt's or the active second arena.
arena_clear (0x419f78) clears the second arena. Deactivation (0x419cb0) frees effects and stops or frees objects of an arena that is no longer active.
3.6 Why tunnels hide the load [INF]

The corridor is small and resident. The next MTO arena streams one chunk per frame while Kurt is inside it, and the far door cannot finish opening until BSPShow sees the stream complete. The exact moment the preload starts (opcode 223, type 3 trigger, or the door) is not pinned down by any source. Determine it from the scripts and Ghidra, do not guess.

3.7 Behavior visible to players (speedrun wiki)

Approaching a door opens it and makes the room beyond passive. Walking through swaps active and passive. Enemy drops are fixed when the enemy spawns, often at room load. Loading a save can load rooms early. These confirm the active/passive model and that spawn happens at first show.

4. Open questions to resolve before or during Phase 1
Which SNI file holds corridors, and how are corridor entries distinguished from WAV entries in the port's Sni parser?
What exactly are the 24-byte sound entries in the MTO models section, and do they map to per-arena sample sets?
Is the 32 KiB chunk a read size from disk only, or also a parse unit? (Check arena_load_step in Ghidra.)
What are the buffers at arena +0x24 and +0x40?
Does the original read from the CD or hard disk in these chunk reads (cddata, hddata, hduse in MDK.CFG)? For the port this only matters for naming, since 3DS reads come from SD or RomFS.

Write answers into docs/CONTINUATION.md as you get them, with the Ghidra address or file evidence.

5. Implementation plan

Do one phase at a time. After each, build, run on emulator, then on hardware, and report numbers.

Phase 0: measure. Add free-heap and peak-heap logging at level load, arena switch, and after SoundBank::forLevel. Record baseline numbers for every level on hardware. Confirm or refute that forLevel is the OOM point.

Phase 1: chunked MTO reader.

Change Mto to keep only the directory (count, names, offsets) resident and hold a file handle, not the whole file.
Add an ArenaStream object: opens the arena block by offset, reads size, then reads 32 KiB chunks into a preallocated buffer, one per step() call, with done() and progress().
On completion, run the existing getArena() parse path on the buffer. Remove the lifetime cache. getArena() must return a handle that is freed on eviction.

Phase 2: second-arena state machine. Mirror section 3.3 to 3.5 using the port's names:

Fields: current arena, second arena, secondActive, streaming.
Functions with the original roles: arenaLoad, arenaSwitch, arenaLoadStep, arenaActivate, arenaSetNeighbour, bspShow, arenaClear.
arenaSwitch frees the previous MTO arena's buffers before allocating the new one. Assert that at most one MTO arena is resident at any time.
Call arenaLoadStep once per frame from the game frame update.
Hook type 6 crossing, type 1 and 3 triggers, opcodes 100 and 223, doors, teleports, level start, and load game exactly as in 3.5.
Keep ScriptRuntime::residentArena_ semantics unchanged.

Phase 3: door gating. bspShow must not make an arena active until streaming, sound and music are ready. The original blocked. On 3DS prefer keeping the door in its opening state and not letting the arena become active until ready, with a time-budgeted pump of extra chunks. If a stall is unavoidable, a short blocking wait is acceptable, as in the original. Log every stall duration.

Phase 4: corridors. Load corridor geometry from the SNI archive separately and free it separately. Do not keep corridor data resident across arenas. Implement the teleport rule: into an unloaded corridor, preload the last-indexed connected arena and free other corridors.

Phase 5: per-arena audio.

Stop SoundBank::forLevel loading all three SNI archives. Load only the entries needed by the current and preloaded arena, one step per frame as in 3.3.
Free archive bytes and decoded samples for an arena when it is deactivated. This closes the open item in docs/CONTINUATION.md.
Music for an arena preloads when the arena preloads and starts when shown.

Phase 6: verify. Run every level end to end on hardware with heap logging. Compare against the Phase 0 baseline.

6. Acceptance criteria
Peak heap during every level on hardware is below the 3DS budget with a stated margin, and lower than the Phase 0 baseline. Report both numbers.
Assertion holds: never more than one MTO arena resident.
No script behavior change: existing script tests and a full playthrough of each level still pass.
Walking each tunnel shows no visible hitch beyond what Phase 3 logging reports. Report the worst stall in ms per level.
Teleport and load-game into mid-level positions work.
SoundBank::forLevel no longer loads all archives, and no crash on hardware.
7. Rules
Do not claim done without measured hardware numbers. Emulator heaps are larger and hide this bug.
Do not guess unknowns from section 4. Resolve them from files or Ghidra and record the evidence.
Treat addresses above as pointers to recheck, since they come from a secondhand summary of the reverse engineering docs. If the repo has tools/ghidra, use it.
Make small commits per phase. Keep desktop behavior working (unbounded budgets remain valid on desktop).
If a phase needs a design choice not covered here, pick the simplest option that keeps original behavior, write the choice in docs/CONTINUATION.md, and continue.
8. Branching and reasoning effort

Branch. Before editing anything, create a new branch for this work (for example experimental/arena-streaming). Commit once per phase, and tag the commit at the end of each phase after hardware numbers are recorded. Do not merge to the main branch until every acceptance criterion in section 6 passes on hardware. If a phase makes things worse than the Phase 0 baseline, revert to the previous phase's tag instead of patching forward.

Reasoning effort. The reasoning level is set by the operator in the harness or API, not by this prompt. This work runs at max effort throughout. Do not shorten your reasoning or skip verification to save tokens, and do not pause to ask for a different effort level. Pass any reasoning content back unchanged across tool-call turns.

9. Sources
Godot port engine internals (arena streaming, addresses): https://github.com/nautilusbits/mdk-godot/blob/main/docs/engine.md
Godot port docs index: https://github.com/nautilusbits/mdk-godot/blob/main/docs/README.md
mdk-sdl original engine notes (32 KiB chunks, active/passive): https://github.com/nemo22/mdk-sdl/blob/main/docs/original-engine.md
mdk-sdl formats (MTO, DTI, SNI layouts): https://github.com/nemo22/mdk-sdl/blob/main/docs/formats.md
mdk-sdl architecture: https://github.com/nemo22/mdk-sdl/blob/main/docs/architecture.md
Speedrun wiki, room model and glitches: https://kb.speeddemosarchive.com/MDK
Executable strings, cheats, memory allocator strings: https://new.tcrf.net/MDK_(DOS,_Windows)
Install layout and MDK.CFG: https://dosdays.co.uk/topics/Games/game_mdk.php
Wikipedia (PC versus PlayStation data handling, Mick West via IGN): https://en.wikipedia.org/wiki/MDK