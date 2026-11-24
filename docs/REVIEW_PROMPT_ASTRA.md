# External review prompt — door-bubble branch (for ChatGPT Astra, max settings)

Copy everything below the line into the review session, with the repository
https://github.com/colbyshores/mdk-3ds branch `review/door-bubble-2026-11-23`
checked out (tip 1dfc1d45).

---

You are reviewing a C++ port of MDK (1997) to the Nintendo 3DS. The target
that matters is the ORIGINAL 3DS: 268 MHz ARM11, L2 disabled, PICA200 GPU,
400x240 stereo. The visual reference is pixel-fidelity to the original game;
gameplay-affecting math is fidelity-gated and must not be "optimized" into
different behavior. The house style is "C with classes": no virtual
functions, no RTTI, no exceptions, no project templates, no std::function,
no assert(). Binding contracts are in AGENTS.md (performance rules R1-R11,
temperature rules T1/T2, platform rules P1-P10) -- read it first; proposals
that violate it will be rejected, so check your ideas against it.

## What this branch contains (vs main at 76abc8fb)

A campaign against door/transition stalls, in three layers:

1. SLICED BUILD (commit 22270393). The incoming arena's view construction
   used to run synchronously in one frame (~29 ms). buildArenaScene and
   computeArenaDepthLayers were extracted verbatim into resumable builders
   (ArenaSceneBuilder: Setup, then 1200-triangle loop steps, then Finish;
   DepthLayersBuilder: Collect per triangle, Assign per overlap check).
   LevelView builds in stages (Scene -> Mesh -> Large); ArenaViews::sync
   steps ONE building view per sync, oldest first, one new build per sync.
   EffectsView::warmStep resolves one of six effect textures per frame.

2. DOOR BUBBLE (6f25a4a1). Owner playtest of (1): stalls improved but
   geometry was visibly missing while loading (pop-in reads worse than a
   stall). So: ScriptRuntime::doorBubbleArenas names arenas through
   doorway boxes (formats::Dti connection records + the runtime's
   connections_ map) within a radius of the camera; ArenaViews keeps and
   builds them like the loaded pair; the effects warm and the music
   prefetch follow the nearest hint; and THE GUARANTEE: a drawn view
   still building at sync is finished synchronously there (view phase,
   outside the frame), so a drawn arena is never absent. The draw-path
   drain from (1) was removed as superseded.

3. VELOCITY-SCALED RADIUS (1dfc1d45). Radius = 120 + smoothed
   time-normalised eye speed * 2.5 s, cap 420, hysteresis +40, at most 3
   hinted names kept, only the nearest built ahead per sync.

Key files: mdk_core/src/render/arena_scene.{h,cpp},
mdk_core/src/game/level_view.{h,cpp}, mdk_core/src/game/arena_views.cpp,
mdk_core/src/game/effects_view.cpp, mdk_core/src/scripts/script_runtime.cpp
(doorBubbleArenas), mdk_3ds/src/main_3ds.cpp (bubble block, music block).

## Measured evidence (Old 3DS hardware, PROFILE builds, LEVEL=7 scripted
route DANT_1 -> CDANT_1, frame PERIOD in ms, lower is better)

- Baseline (before 1): activation frame 179.3 (view build 29.0 in one sync)
- Sliced only: 110.5 -- but owner saw missing geometry in real play
- + bubble: 80.3, view work at the door 0.0; next frame 37.5 (corridor
  band ~33); incoming room built ~145 frames before the door
- + scaled radius: 82.2 / 36.3, band by the third frame
- Whole-run mean 18.6-18.9 ms and linear-heap headroom (7188 KiB) are
  unchanged across all of the above. The sync guarantee fired once per
  run, at level start. Evidence files: docs/evidence/2026-11-23-*.txt.

## Known open issues (owner-reported, unprofiled)

- Mid-tunnel hitches during corridor traversal.
- A hitch on the initial menu screen (Kurt jumping onto the MDK logo).
- Explosions tank the frame rate (GPU fill suspected; effects are already
  batched into one dynamic mesh; an explosion is an animated-texture
  OBJECT plus debris, not a sprite draw).
- The worst corridor pocket still runs ~2 vblanks (24-27 ms).
- Average frame rate has NOT improved through any of this; only worst
  frames have. The frame is believed GPU/begin-bound in steady state.

## What I want from you

1. CORRECTNESS REVIEW of the three layers above: lifetime (builder
   pointers into arena/resolver/palette), the guarantee's termination,
   hint residency/keep logic, retry interplay, the doorBubbleArenas
   query (records, connections, const_cast), anything that can draw a
   wrong or missing picture. Cite file:line for every claim.
2. BAND-AID AUDIT: the owner is explicitly worried about accumulated
   experiments and code smell. Name anything in this branch that should
   be DELETED or simplified, with reasoning. In particular: is the sliced
   build still earning its complexity now that the bubble provides lead
   time, or should LevelView go back to a synchronous load triggered by
   the bubble (keeping only the guarantee)? Argue from the code and the
   counters (stats_.syncFinishes / buildSteps in arena_views.h).
3. NEW IDEAS, ranked by expected impact / effort, for: door/transition
   smoothness, mid-tunnel hitches, the menu-screen hitch, explosion fill,
   and steady-state frame rate on this hardware. Ideas must respect the
   contracts above (no fidelity changes to gameplay math, no CPU fallback
   for GPU features, original look is sacred). Say what you would
   MEASURE first for each idea and what result would kill it.
4. Anything in the wider renderer (mdk_3ds/src/render/c3d/*.inc) or the
   frame loop (mdk_3ds/src/main_3ds.cpp) that looks like a missed
   optimization the measured campaign did not find.

Be adversarial and specific. "Looks good" without file:line evidence is
not useful. If a claim in this prompt looks wrong from the code, say so.
