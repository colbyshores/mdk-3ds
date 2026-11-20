# 3DS performance experiments — running ledger

Living record of the Old-3DS frame-time work on branch
`experimental/3ds-perf-glow-shadows`. Updated as each experiment is measured, not
after the fact. Raw numbers live in `docs/evidence/2026-11-1*-3ds-*.txt`; this
file is the index and the decisions.

## How an experiment is run (owner, 2026-11-19)

- **Each risky idea gets its own throwaway branch** (`experiment/3ds-*`), usually
  written by a separate agent in its own worktree. Measure it, merge the winner,
  keep the losers' branches until the owner has seen them, then drop them.
- **Measured on the console, serially** (Old 3DS appliance `old3ds-lan167`, CFGU
  model 0, 268 MHz, L2 off, stereo on, level 8, 90 s run, `hw_cycle.sh --profile`).
  The emulator is never a timing source.
- **Two A/Bs per candidate.** *Isolated*: every other enhancement off, so the
  candidate's own cost or gain is visible. *Shipped*: the default config, which is
  the number the player gets.
- **The metric is the steady mean** (frame 3 onwards) plus the vblank spread
  (frames taking 1/2/3/4+ refreshes). The p50 is quantised to 33.3/50 ms and
  misled us once; do not use it to decide.
- A change that alters the picture also gets a capture diff against today's
  console frame.

## Where the frame stands

Level 8, stereo, Old 3DS: 94.5 → 77.1 → 67.2 → 61.4 → 57.8 → 55.7 (4cde286) → 41.2 (E6+E8) →
**40.2 ms** steady mean shipped. The original look alone (every enhancement off) is now 33.6 ms, a locked 30 fps. The renderer is not
the main bottleneck: the engine side (simulation steps, first-use creation, I/O
inside the frame) is where the large wins have come from, the same lesson as
`../forsaken`.

## Ledger

| # | Experiment | Branch | Result | Status |
|---|---|---|---|---|
| — | No SD file poll / periodic log flush inside the frame | merged (4cde286) | −5.1 ms; the 30-frame spikes gone | **merged** |
| — | E6+E8 together (merged tip 7dac08c) | — | shipped **41.2 ms**, isolated **33.6 ms** (2,559 of 2,577 frames at 2 vblanks) | measured |
| — | No dedupe/texture stats line written to SD every 300 frames in play/profile builds (the line E6 added) | merged | shipped 41.2 → **40.2 ms**; the spikes at frames 600/1200 (end span 27–32 ms) gone; 2-vblank frames 1,166 → 1,441 of ~2,100 | **merged** |
| E3 | Sun shadow map sampled NEAREST | deleted | no change | rejected |
| A | Static light baked once per rig on the CPU | `experiment/3ds-baked-light` | +1.1 ms isolated | rejected |
| B | Haze through the PICA fog unit (W-buffer) | `experiment/3ds-fog-haze` | scene nearly black on console | rejected (broken) |
| D | Light in the vertex shader from face normals | `experiment/3ds-vertex-light` | +7.4 ms | rejected |
| E5 | 16-bit texture stores (RGB565 / RGBA5551 / RGBA4) | `experiment/3ds-texture16` | speed: noise alone (44.6 vs 45.1 isolated; shipped 51.5 vs 52.2 with E8's base); memory −9 to −12 MB | **merged with E8** (the fallback store under ETC1A4) |
| E6 | Skip per-draw GPU state the GPU already holds (draw dedupe) | `experiment/3ds-draw-state` | isolated **48.0 → 43.8 ms (−4.2)**, 2-vblank frames 55% → 75%; shipped **56.6 → 54.2 ms (−2.4)**, 2-vblank frames 16% → 32%; command list 49,288 → 23,824 words in the longest frame; console capture identical (max diff 0) | **merged** |
| E7 | Prewarm first-use textures/layouts/poses at level load, ≤1 texture/frame in play (v2: without the Kurt part, which E8 made redundant) | `experiment/3ds-prewarm-v2` (84fb5df) | shipped 41.2 → 40.0 ms (within run-to-run noise: the same base measured 40.2 earlier); isolated 33.52 → 33.51 ms. First-use creation inside the draw across a whole 90 s run: 30 textures, **2.6 ms in total** without it, so there is nothing left to save after E8. Costs 4 MB of linear heap at load | rejected (branch kept) |
| E8 | Kurt's sprite frames as ETC1A4, start set preloaded; E5's 16-bit store as the fallback | `experiment/3ds-kurt-etc1` (07399b3, on 6541a2e = E5) | shipped **52.2 → 42.9 ms (−9.2)**, 4+ vblank frames 256 → 4, spikes 40 → 4, draw build 13.9 → 7.5 ms; isolated **42.8 → 33.6 ms (−9.2)**, 2,566 of 2,580 frames at 2 vblanks; Kurt mean diff 2.9/255 | **merged** |
| E10 | Kurt's ETC1A4 set as ONE file (`romfs:/kurt/kurt.pak`): one open and one read (index + start set) at level load, the other animations streamed a range at a time on the HD reader's thread, bounded 256 KiB staging cache, 16-bit stand-in until the bytes land | `experiment/3ds-kurt-pack` | level load: Kurt **4,557 ms → 62 ms** (1 file, 270,233 bytes, 2 reads), **0 bytes** of .t3x kept on the heap (was 1.1 MB); 5 animations prefetched in play, 0 frames stood in 16-bit; shipped 41.1 ms / isolated 33.6 ms (unchanged, within noise); console capture identical to E8 (max diff 0) | **merged** |

### E8 as built (2026-11-19)

- `mdk_3ds/tools/kurt_etc1_convert.py`, run by `stage_romfs.sh` in every
  `make -C mdk_3ds`: Kurt's original TRAVSPRT.BNI frames, decoded by mdk_core's own
  parsers with each level's palette, alpha flood-bleed as `hd_pack_convert.py`,
  padded (not scaled) to a power of two, `tex3ds -f etc1a4 -q high`, no mips by
  default. 408 frames, 1.18 MB, staged as `romfs:/kurt` (no SD copy needed;
  .3dsx +1.3 MB). Deterministic; `--check` proves the alpha equals the colour key
  on every frame. No flip needed: the frame rectangle matches the HD pack's.
- Runtime: `mdk_render_create_compressed_texture` imports the .t3x bytes, no
  palette expansion; the bytes are read at level load, never from SD mid-frame.
  The HD pack's 128 copy still wins when present. Fallback without a compressed
  copy: E5's RGBA5551.
- The start set (K_STILL, K_FALL, K_LAND, K_IDLE: 105 frames) is preloaded at
  level load under a separate 3 MiB limit; the 1 MiB residency then only rotates
  the other animations (every one fits whole except K_CHUTEC, 3 MiB on its own).

| Start set, 105 frames | Bytes | Frames in the 1 MiB residency |
|---|---|---|
| RGBA8 | 10,649,600 | 12 |
| RGBA5551 | 5,324,800 | 17 |
| ETC1A4 | 2,662,400 | 36 (and the whole set preloaded) |

A/B: `TOGGLES='kurtCompressed=0;texture16=0'` (RGBA8),
`TOGGLES='kurtCompressed=0'` (RGBA5551), default (ETC1A4).

### E8 measured (Old 3DS, level 8, stereo, 90 s, steady mean; base 4cde286, no E6)

| Kurt store | Shipped mean | 2 / 3 / 4+ vblank frames | Spikes >60 ms | Draw build | Isolated mean |
|---|---|---|---|---|---|
| RGBA8 | 52.2 ms | 388 / 996 / 256 | 40 | 13.9 ms | 42.8 ms |
| RGBA5551 | 51.5 ms | 571 / 839 / 241 | 40 | 14.5 ms | — |
| **ETC1A4** | **42.9 ms** | **855 / 1,123 / 4** | **4** | **7.5 ms** | **33.6 ms** |

**Why it won.** Not the bytes but the churn: under RGBA8 the texture cache held
57 textures yet had made 105 by frame 300 (evict, remake), and Kurt re-made a
256 KB frame most frames of his fall, landing and idle. Under ETC1A4 154 textures
sit in 12.1 MB (29.4 MB if they were RGBA8) and nothing is remade in play. 16-bit
alone halves the bytes but keeps the churn, which is why it barely moved.

**Look.** Console capture at frame 60: Kurt upright and in place; mean |d| 2.9/255
in his box, 0.4 % of his pixels over 16; the rest of the frame within 16 everywhere.

**Costs and follow-ups.**
- Level load **+4.6 s**: 408 small `.t3x` files read from RomFS one by one. Pack
  them into one file (one open, one read).
- The 1.1 MB of `.t3x` bytes stay on the main heap (the tighter heap now: ~22.6 of
  40.9 MB used at the loop). One packed file read on demand would release it.
- No mip chain on Kurt's frames by default (`--mips` adds one).

### E10 as built (2026-11-19): the pack and the stream
- `kurt_pack.py` (run by `kurt_etc1_convert.py`) writes `kurt.pak`: a 32-byte
  header (`KPAK`, v1), a 40-byte index entry per frame (name, level, size, offset,
  length), the `.t3x` bytes 16-byte aligned; start set first, then each animation's
  frames together. On the GOG data: 408 frames, 1,255,937 bytes; the index plus
  start set is the first 270,233 bytes. Deterministic; the RomFS stages only it.
- Level load: one `fopen`, a 32-byte read, ONE 270,201-byte read (index + start
  set), the 105 start-set frames imported, the buffer freed. Logged as one line
  (`sprites: ... 1 file opened, N bytes read in M ms ... kept on the heap`); the
  console figure replaces the 4,557 ms / 1,122,760-byte one.
- Play: a frame outside the start set with no staged bytes asks for its
  animation's range (one read in flight, on the HD reader's thread, its own
  channel) and draws the 16-bit index texture until the next draw after it lands.
  The pump prefetches the state machine's next animations into free room. Staged
  bytes are held at most 256 KiB, least recently drawn dropped first.
- Errors: a bad header/short read rejects the pack (16-bit for the level), a bad
  entry costs its frame only, a failed stream read costs its animation only; each
  logged once, none retried. One `sprites: stream:` line at exit.
- To measure: level-load ms, heap at the main loop, stand-ins and reads in the
  exit line, and the steady mean against E8's 42.9 ms (the stand-in's expansion is
  the cost this trades for 4.6 s and 1.1 MB).

## Texture formats on the 3DS — the policy

**Owner, 2026-11-19: memory over texel precision.** On the 3DS screen there is no
visible quality difference between ETC1A4 and RGBA4, and the memory saved is
wanted for other things. So the order of preference is:

1. **ETC1** (opaque, 4 bits/texel) / **ETC1A4** (8 bits/texel, 4-bit alpha),
   encoded **offline** with tex3ds. Never encoded on the ARM11.
2. 16-bit (RGBA5551 for colour-keyed images, RGB565 opaque, RGBA4 partial alpha)
   only as the fallback for an image with no compressed copy.
3. RGBA8 only where exact colours are required (the HUD/canvas fonts).

**The reference is `../forsaken` at e8f0774**, whose sprites and walls ship this
way on the Old 3DS with no visible downgrade:

- `tools/texture/regen_hd_textures_4k.sh` (`convert_one`): sprites
  `tex3ds -f auto-etc1 -q high -m gaussian`; the colour key is turned into alpha
  before encoding; opaque walls `-f etc1`; mip chains on everything. `gaussian` is
  the mip-chain filter, not the compression.
- `forsaken_3ds/src/rendering/c3d/11_textures.inc` (`try_load_hd_texture`):
  **when the .t3x exists it replaces the texture outright**, so the original is
  never expanded. Linear min/mag with a nearest mip filter; the alpha test only
  on `GPU_ETC1A4`.
- `tools/texture/t3x2png.py`: the 3DS ETC1/ETC1A4 storage layout (per-block
  alpha first, byte-reversed ETC1 blocks), usable for host-side tests.
- Forsaken bakes a vertical flip into its sources to match its own upload; check
  MDK's orientation against the HD pack's frames that already draw correctly
  before copying that step.

### Finding: MDK keeps two copies wherever an HD copy exists

`mdk_render_create_index_texture` (`mdk_3ds/src/render/c3d/05_resources.inc`)
makes the RGBA8 expansion **eagerly** at creation. The HD pack's ETC1/ETC1A4 copy
(`docs/HD_TEXTURES_3DS_SPEC.md`) is then streamed in **on top** of it
(`06_draw.inc`, `HdCopy`), and the draw uses the RGBA8 copy only until the HD copy
is resident. The RGBA8 copy stays resident afterwards. Kurt's frames do the same
(`mdk_core/src/game/kurt_sprite.cpp`, `ensureTexture`), and his residency budget is
counted in the RGBA8 bytes.

So today HD textures **add** memory instead of saving it. Forsaken's pattern
(the compressed copy replaces the original) is the fix:

- **E8** applies it to Kurt first, with an offline ETC1A4 set of his
  original-resolution frames.
- **Next (after E8)**: the same for the world textures. Release or never make the
  RGBA8 expansion when a compressed copy is present, and consider an offline
  ETC1/ETC1A4 set for the original-look textures too, so nothing is expanded on the
  ARM11 at all.

Kurt's start set (stand, fall, land, idle) in each format:

| Format | One 256×256 frame | Start set |
|---|---|---|
| RGBA8 (today) | 256 KB | ~10.2 MB |
| RGBA5551 | 128 KB | ~5.1 MB |
| ETC1A4, full size | 64 KB | ~2.5 MB |
| ETC1A4, 128×128 (HD pack cap) | 16 KB | ~0.6 MB |

## Queued candidates

- Merge adjacent draws that share state (Forsaken: 33–66 draws per eye; MDK ~245).
- Frustum-cull against the union of both eyes, with per-batch bounds.
- Time the software audio mix and the span between the simulation and
  `begin_frame`.
- Catch-up step cap: experiment only, because it changes gameplay; the owner decides.
- World textures as compressed replacements (above).

## Deferred until the game is profiled and in a great state (owner, 2026-11-19)

Engine refactors wait until the performance work is done, so they do not
collide with the experiment branches:

- Split the frontend monolith `mdk_3ds/src/main_3ds.cpp` (2,212 lines; `main()`
  alone ~1,930) into `mdk_3ds/src/main/NN_*.inc` fragments over one state struct,
  the pattern `renderer_c3d.cpp` + `render/c3d/*.inc` and `../t2k` already use.
  Then pull the logic it shares with `mdk_linux/src/main.cpp` into `mdk_core`.
- Split the grown backend fragments `render/c3d/03_frame.inc` (2,238 lines) and
  `06_draw.inc` (1,495) further.
- Make the c3d source-text pins less brittle: a harmless extra argument to
  `expandIndices` broke two mutation arms in E7.
