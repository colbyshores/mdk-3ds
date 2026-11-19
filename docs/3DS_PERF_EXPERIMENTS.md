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

Level 8, stereo, Old 3DS: 94.5 → 77.1 → 67.2 → 61.4 → 57.8 → **55.7 ms** steady
mean shipped (4cde286). The original look alone is ≈ 40 ms. The renderer is not
the main bottleneck: the engine side (simulation steps, first-use creation, I/O
inside the frame) is where the large wins have come from, the same lesson as
`../forsaken`.

## Ledger

| # | Experiment | Branch | Result | Status |
|---|---|---|---|---|
| — | No SD file poll / periodic log flush inside the frame | merged (4cde286) | −5.1 ms; the 30-frame spikes gone | **merged** |
| E3 | Sun shadow map sampled NEAREST | deleted | no change | rejected |
| A | Static light baked once per rig on the CPU | `experiment/3ds-baked-light` | +1.1 ms isolated | rejected |
| B | Haze through the PICA fog unit (W-buffer) | `experiment/3ds-fog-haze` | scene nearly black on console | rejected (broken) |
| D | Light in the vertex shader from face normals | `experiment/3ds-vertex-light` | +7.4 ms | rejected |
| E5 | 16-bit texture stores (RGB565 / RGBA5551 / RGBA4) | `experiment/3ds-texture16` | speed: noise (44.6 vs 45.1 isolated); memory −10–12 MB | kept as a memory lever, superseded for sprites by E8 |
| E6 | Skip per-draw GPU state the GPU already holds (draw dedupe) | `experiment/3ds-draw-state` | isolated **48.0 → 43.8 ms (−4.2)**, 2-vblank frames 55% → 75%; shipped **56.6 → 54.2 ms (−2.4)**, 2-vblank frames 16% → 32%; command list 49,288 → 23,824 words in the longest frame; console capture identical (max diff 0) | **merged** |
| E7 | Prewarm: make first-use textures/layouts/poses at level load, ≤1 texture/frame during play, Kurt residency 1 → 8 MiB | `experiment/3ds-prewarm` | not yet measured | host tests, then console |
| E8 | Kurt's sprite frames as ETC1A4 (see below) | `experiment/3ds-kurt-etc1` | in progress | agent implementing |

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
