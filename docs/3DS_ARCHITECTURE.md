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
| 1 (system core) | the streaming worker | every file read, every decompress or decode that is not the GPU's or the DSP's, music streaming, feeding sound-effect samples to their buffer, releasing what an arena no longer needs, log writes |

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

## 3. Memory

| Class | What | Lifetime |
|---|---|---|
| Level-resident | level data and geometry, scripts, HUD, fonts, heads, scope, Kurt's start set, the 2 MiB sound-effect buffer, music buffers | loaded behind the loading screen, freed at level end |
| Arena-streamed | each arena's textures: HD copies where they exist (512 target), else the packed originals; effect and object textures | requested through the door bubble, released when the arena leaves it |
| Streamed on demand | Kurt's other animations, music | by their streams, bounded caches |
| VRAM (later phase) | static, level-resident textures that are read heavily | placed at level load |

- The arena-streamed set has one budget, sized so the worst case along the
  level's real arena graph (the current pair plus the bubble's arenas) fits on
  the Old 3DS. Nothing per surface is streamed in play.
- When an arena's 512 set cannot fit, that arena steps down a tier, never the
  whole level.

## 4. Streaming: the door bubble

- The bubble decides residency for every arena-streamed asset, HD included: the
  arenas through doorways near the camera are requested ahead (speed-scaled
  lead), nearest first; arenas leaving it are released by the worker.
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
- Small 2D art made once per level (HUD, fonts, heads, scope, bomber, screens)
  may stay uncompressed, made at level load, never at first show.
- The movie stays a streamed texture.

## 6. GPU visibility

- Every texture written by the CPU is flushed from the CPU cache before the GPU
  can sample it, per texture, not only by the end-of-frame flush (candidate
  cause of the New 3DS mip glitch, branch `fix/3ds-mip-chain`).
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
3. **Remaining runtime expansion** to build-time packs (section 5).
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
