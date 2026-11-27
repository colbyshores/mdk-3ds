# 3DS platform architecture

Owner decision, 2026-11-26: one architecture for the Old and the New 3DS, built
around two cores. Core 0 runs the game; core 1 loads and streams its assets.
Every 3DS change follows this document. A change that needs to break it asks the
owner first.

## 1. One design for both models

- The same threads, on the same cores, with the same budgets, on the Old and the
  New 3DS. No code path is chosen by console model.
- The New 3DS's extra cores (2 and 3) are not used.
- The one allowed difference is the New 3DS speed-up (804 MHz and the L2 cache,
  `osSetSpeedupEnable`), one call at boot. It changes how fast the same code
  runs, not what it does.
- The temporary FAST 3DS menu row (profiling only) never reaches main.

## 2. Two cores, two jobs

| Core | Thread | Owns |
|---|---|---|
| 0 (application core) | main | input, simulation, scripts, the camera, building and submitting frames, every Citro3D call, handing finished assets to the renderer at a frame boundary |
| 1 (system core) | the streaming worker | every file read, every decompress or decode that is not the GPU's or the DSP's, music streaming, releasing what an arena no longer needs, log writes |

- **One worker thread on core 1.** The kernel allows one application thread
  there. It serves a queue with fixed priority: music first, then what the
  player is about to see, then hints, then log writes.
- **Work is chunked.** No item holds the worker long enough to starve music:
  large files are read in chunks, and music keeps a generous buffer
  (owner: "buffers are cheap").
- **Core 1's time share** (`APT_SetAppCpuTimeLimit`) is set from measurement,
  as high as streaming needs while the system stays responsive (HOME, sleep).
- **Core 0 does no file I/O and no asset decoding during play.** The only asset
  work it keeps is what must happen on the thread that owns the GPU state: a
  short copy or pointer hand-off at a frame boundary, under a per-frame budget,
  then the cache flush that makes the texture visible to the GPU (see 6).
- **Hand-off is explicit.** The worker never touches Citro3D state; the main
  thread never blocks on the worker during play. Shared data has one owner at a
  time (AGENTS.md P4); every worker thread sets FZ/DN at entry (R1).
- **Audio is native.** The DSP decodes the music (DSP-ADPCM) and plays and
  mixes the sound effects on its hardware channels; neither core mixes audio.
- **The sound effects are level-resident, so nothing feeds them in play.** At
  level load (and when the fall or the stream screen opens), behind the loading
  screen, every effect the level can play is decoded and copied into the 2 MiB
  DSP sample buffer, pinned there, and the bank's own copy is freed
  (`audioDspPlaceEffects`). Measured on the retail data (`tick1 sfxset`): 1.14-1.44
  MB a level (74-81 effects), 1.06 MB for the fall. Core 0 decodes and copies no
  effect during play; the level-end and `audio: dsp close` lines count any that
  do (must be 0).

## 3. Memory

| Class | What | Lifetime |
|---|---|---|
| Level-resident | level data and geometry, scripts, HUD, fonts, heads, scope, Kurt's start set, the 2 MiB sound-effect buffer with every effect of the level in it, music buffers | loaded behind the loading screen, freed at level end |
| Arena-streamed | each arena's textures: HD copies where they exist (512 target), else the packed originals; effect and object textures | requested through the door bubble, released when the arena leaves it |
| Streamed on demand | Kurt's other animations, music | by their streams, bounded caches |
| VRAM (later phase) | static, level-resident textures that are read heavily | placed at level load |

- The arena-streamed set has one budget, sized so the worst case along the
  level's real arena graph fits on the Old 3DS: every arena's HD window
  (section 4) is checked against the free linear heap less the 7 MiB play
  reserve at level load. Nothing per surface is streamed in play.
- When an arena's 512 set cannot fit, that arena steps down a tier, never the
  whole level. Measured 2026-11-26: the largest window is 8.79 MiB at 512
  (level 8, CGUNT_9); every level fits at 512.
- **The heap split** (`heap_split_policy.h`): 30 MiB linear, the rest main
  (owner 2026-11-26; 28 before). On the Old 3DS that leaves 33.74 MB of main
  against a level-8 peak of 29.11 MB; a level-8 walk confirms the main heap's
  minimum before the change merges.

## 4. Streaming: the door bubble

- The bubble decides residency for every arena-streamed asset, HD included: the
  arenas through doorways near the camera are requested ahead (speed-scaled
  lead), nearest first; arenas leaving it are released by the worker.
- **The HD window** (owner 2026-11-26, `mdk_3ds/src/hd_window_3ds.h`): Kurt's
  arena, its doorway neighbours, and the arenas its trigger boxes and scripts
  name (teleport, arena_show, preload); the drawn arenas are always in it, and
  their files are read first. An arena's files are its rows of the name index:
  everything drawn through its palette. One hop is the whole rule: with the
  uncompressed pack the next arena's set arrives while Kurt crosses the current
  one (worked from the data: the thinnest margin is level 4's 31-unit connector
  CMEAT_5, +0.1 s at turbo). A second hop through short connectors is added
  only if a doorway is measured to wait on the hardware.
- **No pop-in.** An arena does not become visible until its set is resident. If
  a doorway, teleport, ride or scripted reveal reaches an arena whose set is not
  complete, the transition waits for it, as the original engine's `BSPShow`
  did. The bubble's lead makes that wait rare; it is measured, not assumed.
- Level start loads the level-resident set and the starting arena's bubble
  behind the loading bar; the bar tracks that work.

## 5. Assets are prepared at build time

- Everything level-related that the GPU samples is pre-compressed at build time
  (ETC1, or ETC1A4 where it has alpha) with its mip chain: arena textures, sky,
  Kurt, effect strips, HD copies. The console reads and hands off; it does not
  expand palettes.
- **The packs are stored uncompressed** (`tex3ds -z none`; owner 2026-11-26).
  Measured on the Old 3DS: core 1 at its 30% share reads 3.5 MB/s, but reads and
  unpacks tex3ds' default compression at only 0.77 MB/s of file, and one file's
  unpack held the worker 390 ms. The HD pack is ~85 MB raw against 55 MB.
- **A non-square ETC1 texture is sampled down to the level whose short side is
  16 texels, never to its 8 level**, on both consoles
  (`mdk::pica::mipLevelsSampled`). The PICA200 draws part of the 16 level from
  the 8 level's tiles when it blends the two; measured on the Old 3DS,
  `docs/MIP_GLITCH.md`. The packs keep their whole chains.
- Small 2D art made once per level (HUD, fonts, heads, scope, bomber, screens)
  may stay uncompressed, made at level load, never at first show. The font
  atlases' sliced expansion is finished behind the loading screen
  (`mdk_render_finish_expansions`), without a mip chain.
- **Measured, step 3 (2026-11-27):** the texture census (`textures: census`,
  `texmake:` and `pack: miss` log lines, render_c3d_hd_set.h) finds no in-level
  arena, object or effect texture left to palette-expand on levels 3-8 with HD on:
  every Lit and Sprite surface the levels drew resolves to a pack. What core 0
  still makes is the level's 2D art at load (63-68 textures, 2.65 MB, ~0.66 s
  on the Old 3DS, 1.57 MB of it the two font atlases) and 1x1 flat colours in
  play (256 bytes each). No new build-time pack was warranted.
- The movie stays a streamed texture.

## 6. GPU visibility

- Every texture written by the CPU is flushed from the CPU cache before the GPU
  can sample it, per texture, not only by the end-of-frame flush. This is P3
  hygiene. It is NOT the fix for the New 3DS mip glitch: the owner's console
  shows the glitch with the flush in place (`docs/MIP_GLITCH.md`).
- Imports happen at a frame boundary, never inside a draw, and nothing in flight
  on the GPU is freed or overwritten before the frame that used it completed
  (AGENTS.md P3).

## 7. Work order

The streaming work is one line of branches, each built on the last:

1. **Thread layout** (`fix/3ds-second-core-streaming`): section 2. Remove the
   per-model placement (`side_core_3ds`).
2. **HD through the bubble** (`perf/3ds-hd-bubble`): sections 3 and 4, replacing
   the level-wide pinned HD set; the loading bar (`fix/3ds-load-bar`) follows
   what level start then loads; the per-texture flush (section 6) is part of
   the import path.
3. **Remaining runtime expansion** to build-time packs (section 5;
   `perf/3ds-etc1-leftovers`: measured, nothing in-level left to convert; core-0
   pack reads in play 0 on the L5-L8 door walks).
4. **VRAM placement** (section 3, last row).

Renderer and gameplay fixes (bullet cams, decals, upstream fixes, death flow)
are separate branches and add no per-model code.

## 8. Every step is profiled and tested on the hardware appliance

A step is not done, and the next step does not build on it, until it has been
measured on the Old 3DS appliance (`old3ds-lan167`, AGENTS.md P10) against a
baseline build of its parent, and the numbers are in `docs/WORK_LEDGER.md`.

- **Conditions:** Old 3DS profile (268 MHz, L2 off), stereo on, HD on, the
  owner's toggles; record console, clock, cache and stereo mode for each run.
- **Routes:** the same scripted routes for baseline and change, on all six
  levels: level start, a door walk at walking speed and at turbo speed through
  several arenas and back, and one heavy combat scene. Runs that can kill Kurt
  use god mode.
- **Measured each run:**
  - frame time: mean, p95, max, and the frames over 33 ms, overall and at each
    doorway crossing;
  - core 1: share of its time slice used, queue depth, music underruns (must
    be 0);
  - streaming: files and bytes per second, time from an arena's first request
    to its set being resident, transitions that had to wait and for how long,
    surfaces drawn without their texture (must be 0);
  - core 0: file reads and decodes during play (must be 0), hand-off time per
    frame;
  - memory: linear free minimum and main heap free minimum, evictions, failed
    allocations (must be 0);
  - level load time;
  - a capture per level, looked at.
- **Lifecycle:** HOME and back, lid sleep and wake, and a clean exit, on the
  build that changes threads.
- **The owner's New 3DS** confirms the same build behaves the same; the
  appliance cannot stand in for it, and it is not claimed.

A step that makes any measure worse than its baseline is reported with the
numbers before it is kept.
