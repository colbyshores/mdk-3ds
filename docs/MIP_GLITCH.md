# The 3DS mip glitch: the record

What was seen, what was tried, what each try showed. Kept so that no experiment
is run twice. Evidence paths are under `.git/worktrees/` of the main checkout
(not tracked): `ghost-target/hw-logs/`, `mip-chain/hw-logs/`, `mip-root/hw-logs/`.

## 1. The symptom

- Owner's New 3DS only, HD textures on, MIPMAPS on. Photos:
  `mip-root/hw-logs/owner-photos/`.
- On some textures, at a distance or with the scope zoomed out, the lower-left
  quarter of the surface shows a half-size copy of the whole texture. The other
  three quarters are right. Closer, or zoomed in, it is gone.
- Seen on: the practice-room bullseye targets through the scope (BULLSEYE,
  D7_BULL; photo 6 shows the small bullseye, hard-edged, in the lower-left
  quarter of two targets at once) and the right leg of the inner archway in the
  level-7 start tunnel (C7_WALL; photos 2-4).
- Not every session. Owner: "somewhat nondeterministic", "some weird way they
  are in memory". One evening on 4d9264f3 it did not show at all, then came back,
  sometimes less obvious.

## 2. The textures

| Texture | Source PNG | Stored | Format | Levels |
|---|---|---|---|---|
| BULLSEYE, D7_BULL | 256x290 | 256x512 | ETC1 | 0-5 (87,360 B) |
| C7_WALL | 128x366 | 128x512 | ETC1 | 0-4 (43,648 B) |

- `hd_pack_convert.py` RESAMPLES a non-power-of-two image to the power-of-two
  size (it does not pad), so the picture fills the whole texture and the uv spans
  0..1 on both axes. Both textures are taller than wide (h = 2w, h = 4w).
- 3DS texture memory starts at the picture's bottom-left, so "lower-left quarter
  of the surface" is the tiles nearest the start of each level.

## 3. What was tried, and the result

| # | Experiment | Where | Result |
|---|---|---|---|
| 1 | MIPMAPS off in the pause menu | owner, New 3DS | glitch gone |
| 2 | File levels decoded on the host (every level of BULLSEYE, D7_BULL, C7_WALL, C7_FLR; whole-pack scan `hdpack_check.txt`; decoded again 2026-11-26 with an independent ETC1 decoder) | host | every level is a clean reduction of level 0; no small copy in any level |
| 3 | Owner's SD copies of BULLSEYE, D7_BULL, C7_WALL, C7_FLR against `build-hd-pack` | owner's SD | byte-identical |
| 4 | Texture memory hashed per level against the files (`texhash`, `mipprobe-A/A2/P1/L1/L2`) | Old 3DS appliance, tier 256; Mandarine | 1256 + 314 lines byte-exact, 0 differ (CPU read) |
| 5 | Level-colour textures (each level a flat hue, four quadrants at four brightnesses) in place of C7_WALL, C7_FLR, start tunnel | Old 3DS appliance, tier 256 | each level its own hue, no wrong quadrant: no glitch |
| 6 | Owner A/B builds 0 / A (mip filter NEAREST) / B (HD level 0 only) / C (non-HD level 0 only), `ghost-target/hw-logs/owner-ab/` | owner | not known to have been run (the glitch was deferred that day) |
| 7 | Hypotheses from the Forsaken agent: levels never written, wrong level offsets, sampling past the chain | code reading, `fix/3ds-mip-chain` | none holds: the allocation, the offsets (`C3D_TexCalcTotalSize`) and the per-draw `maxLevel` all agree with the file |
| 8 | `C3D_TexFlush` after every Tex3DS import (the CPU-cache theory: New 3DS L2 keeps the chain's last writes dirty), 68e52d7e | owner, New 3DS (it is in the combined build) | glitch still there |
| 9 | Transfer-engine readback of resident textures against the CPU's view (`MIPDIAG=CHECK`), plus its self-test | Old 3DS appliance, tier 256 and tier 512 (HD from RomFS) | 59 MB and 43 MB compared, 0 mismatched. Self-test: an uncleaned write reads 35-59% new, 100% after the frame end's heap-wide clean, 100% after a range clean |
| 10 | Owner builds `mip-chain/hw-logs/owner-builds/` (0-control-check, F-fix-check, N-noL2, C-core0, L-levels, P-poison) | owner | not known to have been run |
| 11 | `MIPDIAG=DRAWLOG`: per draw, the `C3D_Tex` words given to the GPU, checked against the stored chain and the allocation; overlap scan of every resident span | Mandarine r1.2.1, practice room through the scope at zoom 1 / 0.05 / 0.7, ~14,000 frames | 0 anomalies, 0 overlaps; BULLSEYE 256x512 ETC1, max level 5, 87,360 of 87,424 B |
| 12 | Mandarine, every zoom step, tier 512 and after `hdUnpack` at tier 256 | emulator | every picture clean (the emulator does not model the PICA200's texture unit) |
| 13 | New uncompressed HD pack (`tex3ds -z none`) on the owner's SD | owner, New 3DS | glitch still there |
| 14 | Live A/B on the combined build (touch screen steps the mode, `throwaway/combined-owner-test` 75a26a47), 2026-11-26 night: | owner, New 3DS | |
| 14.1 | mode 1: mip filter NEAREST | | no improvement |
| 14.2 | mode 2: texture cache cleared before every draw (a real bind every draw) | | no improvement |
| 14.3 | mode 3: `maxLevel` one less | | no improvement |
| 14.4 | mode 4: `maxLevel` 0 | | glitch gone |

Rows 14.1-14.3 are the owner's words ("zero improvements"); that session's log
was not pulled.

## 4. Ruled out

- **The files.** Rows 2, 3, 13: levels right, SD copies identical, compression
  irrelevant.
- **The level count and offsets the renderer hands over.** Rows 7, 11.
- **A stale CPU cache (cache visibility).** Row 8 on the New 3DS; row 9 shows the
  frame end's clean reaches memory on the Old 3DS. The per-texture flush stays as
  P3 hygiene; it is not the fix.
- **Trilinear blending of two levels.** Row 14.1.
- **Texels left in the GPU's texture cache from an earlier draw.** Row 14.2.
- **Only the smallest level.** Row 14.3.
- **Level 0.** Rows 1, 14.4: with level 0 only, the picture is right.

## 5. Not yet known

- Whether the Old 3DS shows it at all. It was never reproduced there, but no
  appliance run looked at the bullseyes through the scope, and only the start
  tunnel was looked at tier 512 (ten screenshots, none of the owner's view).
- Whether a square HD texture, or an original (non-HD) mipped texture, ever
  shows it. Only tall non-square HD textures have been reported.
- What the New 3DS's GPU reads: every memory comparison was made on the Old 3DS
  or in the emulator.

## 6. Open leads

1. How the PICA200 addresses the lower levels of a non-square texture. A
   half-size copy in the first quarter of a level is what reading level N+1's
   tiles at level N's tile coordinates gives.
2. Placement in the linear heap (the nondeterminism): the address a texture
   lands at, reuse after eviction, load order.
3. What differs on the New 3DS: a larger heap in use, 804 MHz + L2
   (`osSetSpeedupEnable`), longer natural play with evictions.

## 7. The answer

Not found yet. This section is filled in when it is.
