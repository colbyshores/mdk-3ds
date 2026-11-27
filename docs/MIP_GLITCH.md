# The 3DS mip glitch: the record

What was seen, what was tried, what each try showed, and the answer. Kept so
that no experiment is run twice. Evidence paths are under `.git/worktrees/` of
the main checkout (not tracked): `ghost-target/hw-logs/`, `mip-chain/hw-logs/`,
`mip-root/hw-logs/`, `mip-nonsquare-hw/hw-logs/`.

## 1. The symptom

- First seen on the owner's New 3DS, HD textures on, MIPMAPS on. Photos:
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
| BULLSEYE, D7_BULL | 256x290 | 256x512 | ETC1 | 0-5 (87,360 B); level 5 is 8x16 |
| C7_WALL | 128x366 | 128x512 | ETC1 | 0-4 (43,648 B); level 4 is 8x32 |

- `hd_pack_convert.py` RESAMPLES a non-power-of-two image to the power-of-two
  size (it does not pad), so the picture fills the whole texture and the uv spans
  0..1 on both axes. Both textures are taller than wide (h = 2w, h = 4w).
- 3DS texture memory starts at the picture's bottom-left, so "lower-left quarter
  of the surface" is the tiles nearest the start of each level.
- 291 of the pack's 1,382 textures are taller than wide; 942 are square, 149
  wide.

## 3. The answer

**The PICA200 misreads one kind of mip level, and the Old 3DS does it too.** In
an ETC1 texture that is not square, when the level whose short side is 16 texels
is blended with the level after it (short side 8: one 8x8 tile), part of the 16
level is drawn from the 8 level's tiles. In a texture taller than wide that part
is the quarter of the level nearest its start: the 8 level's picture, tile for
tile, in one quarter of the 16 level. On the screen that is the half-size copy
in the corner. (A quarter exactly, 56 of 224 pixels, where each pixel reads one
level; with trilinear the wrong part is blended with the level below and a
fifth to a half of the quad's pixels are off, by shape.)

It is the GPU, not the renderer and not the files:

- **A 150 KB test program shows it with nothing of MDK in it**
  (`mdk_3ds/tools/mipprobe` on `throwaway/mip-nonsquare-hw`). It draws one
  screen-aligned quad with a texture whose every tile is a flat colour naming
  its level and its place, and reads the target back through the transfer
  engine. On the Old 3DS appliance the 256x512 ETC1 chain's 16x32 level reads
  back with a quarter of its pixels in the 8x16 level's colour, tiles x 0,
  y 0..1 (the whole 8x16 level), at every one of 512 placements in linear
  memory. In Mandarine the same programs read every level right.
- **Told to stop at the 16 level, the same chain reads right at every level
  measured**: the same bytes in memory, the LOD register's max level one less.
  Twelve chains measured wrong and then right this way (row 20).
- **It is that kind of level only** (rows 19, 20, all on the appliance):

  | ETC1 shape | Reads |
  |---|---|
  | tall 2:1, 4:1, 8:1 (256x512, 128x512, 64x512, 128x256, 64x128, 32x256, 32x64, 16x64, 16x32) | wrong at the 16-wide level, every one |
  | wide 4:1 and 8:1 (512x128, 256x64, 128x32, 64x16; 512x64, 256x32, 128x16) | wrong at the 16-high level, a smaller part (52 of 288, 27 of 608 pixels) |
  | wide 2:1 (512x256, 256x128, 128x64, 64x32, 32x16) and 16:1 (512x32) | right |
  | square (256x256, 128x128, 64x64, 32x32, 16x16) | right |

  ETC1A4, RGB565, RGBA5551, RGBA4 and RGBA8 read right, tall and wide.
- **It needs the two levels together.** Trilinear shows it in one draw. With
  mip NEAREST, or NEAREST everywhere, one quad alone reads right (724 tests, 0
  off); a second quad of the same texture one level down, drawn first with the
  texture left bound, brings it back. A level picked by an LOD bias on a large
  quad reads right. That is why neither the owner's mode 1 (mip NEAREST: the
  targets are several draws of one texture at different distances) nor mode 2
  (the cache cleared before every draw: trilinear does it inside one draw)
  helped.
- **MDK on the Old 3DS shows both of the owner's glitches and the fix removes
  them** (rows 16, 21-24), same harness build with and without the fix, the
  game's own frame readback:
  - the practice-room targets through the scope, edge smoothing off. Without: a
    small bullseye in the lower-left of the targets at 24 pixels across, and a
    smear at 16. With: three clean bullseyes at both sizes;
  - the start tunnel, edge smoothing on. Without: a hard-edged dark and white
    block on a leg of the inner arch, in all seven captures of the walk. With:
    the leg is smooth, and nothing else in the frame differs (91 to 211 pixels
    change, all inside the block's 26x50 box).
  Pictures: `mip-nonsquare-hw/hw-logs/pictures/before-after-frame950-6x.png`,
  `before-after-frame800-6x.png`, `tunnel-before-after-frame30-5x.png`.

Why it looked like a New 3DS fault that came and went:

- **The level only matters at one range of sizes.** BULLSEYE's 16-wide level
  starts to blend in when the texture is under 32 pixels wide, or under 64 tall,
  on the target, and the wrong quarter shows once the 8-wide level is reached
  too (under 16 wide, or 32 tall). Edge smoothing (2x1) doubles the target's
  vertical resolution, so with it on the height never decides and the glitch
  needs a target 16 pixels across: a few pixels of wrong colour (row 16). With
  it off the taller axis decides, and the copy sits in a target twice that size
  (row 21). Which of the owner's sessions had it on is not known.
- **Nobody looked at the Old 3DS at the right size.** Every appliance run before
  2026-11-27 was at a closer view, or was read as clean: the level-colour run of
  2026-11-25 (row 5) has the 8x32 level's magenta in a strip of the 16x64 level's
  yellow on the inner arch. That is this glitch; it was taken for a legitimate
  level.
- **The emulator reads the level right**, so every emulator check was clean.

Why the hardware does it is not known; the test program maps what it does, not
why.

## 4. The fix

`mdk::pica::mipLevelsSampled` (pica_matrix.h): a non-square ETC1 texture is
sampled down to the level whose short side is 16, and no further. The draw's
one write of the LOD register's max level (`drawRecorded`, 06_draw_submit.inc)
goes through it. The 8 level stays in the file and in memory (64 and 128 bytes
for the two textures above) and is never sampled. No file changes, no pack
regenerated, one path on both consoles. Square ETC1 and every other format keep
their whole chains. The wide 2:1 and 16:1 shapes, which measured right, are
stopped with the other non-square shapes: the line between the wide shapes that
read right and wrong is the hardware's, and not understood.

The look: a surface drawn from a non-square ETC1 texture so small that its 8
level would have been used is drawn from the 16 level instead.

## 5. What was tried, and the result

| # | Experiment | Where | Result |
|---|---|---|---|
| 1 | MIPMAPS off in the pause menu | owner, New 3DS | glitch gone |
| 2 | File levels decoded on the host (every level of BULLSEYE, D7_BULL, C7_WALL, C7_FLR; whole-pack scan `hdpack_check.txt`; decoded again 2026-11-26 with an independent ETC1 decoder) | host | every level is a clean reduction of level 0; no small copy in any level |
| 3 | Owner's SD copies of BULLSEYE, D7_BULL, C7_WALL, C7_FLR against `build-hd-pack` | owner's SD | byte-identical |
| 4 | Texture memory hashed per level against the files (`texhash`, `mipprobe-A/A2/P1/L1/L2` of 2026-11-25) | Old 3DS appliance, tier 256; Mandarine | 1256 + 314 lines byte-exact, 0 differ (CPU read) |
| 5 | Level-colour textures (each level a flat hue) in place of C7_WALL, C7_FLR, start tunnel | Old 3DS appliance, tier 256 | read at the time as "each level its own hue". Re-read 2026-11-27: `shots-L1/shot_00000001_top.bmp` has a hard-edged strip of the last level's magenta inside the yellow of the level above. The glitch was in this capture |
| 6 | Owner A/B builds 0 / A (mip filter NEAREST) / B (HD level 0 only) / C (non-HD level 0 only), `ghost-target/hw-logs/owner-ab/` | owner | not known to have been run (the glitch was deferred that day) |
| 7 | Hypotheses from the Forsaken agent: levels never written, wrong level offsets, sampling past the chain | code reading, `fix/3ds-mip-chain` | none holds: the allocation, the offsets (`C3D_TexCalcTotalSize`) and the per-draw `maxLevel` all agree with the file |
| 8 | `C3D_TexFlush` after every Tex3DS import (the CPU-cache theory: New 3DS L2 keeps the chain's last writes dirty), 68e52d7e | owner, New 3DS (it is in the combined build) | glitch still there |
| 9 | Transfer-engine readback of resident textures against the CPU's view (`MIPDIAG=CHECK`), plus its self-test | Old 3DS appliance, tier 256 and tier 512 (HD from RomFS) | 59 MB and 43 MB compared, 0 mismatched. Self-test: an uncleaned write reads 35-59% new, 100% after the frame end's heap-wide clean, 100% after a range clean |
| 10 | Owner builds `mip-chain/hw-logs/owner-builds/` (0-control-check, F-fix-check, N-noL2, C-core0, L-levels, P-poison) | owner | not known to have been run |
| 11 | `MIPDIAG=DRAWLOG`: per draw, the `C3D_Tex` words given to the GPU, checked against the stored chain and the allocation; overlap scan of every resident span | Mandarine r1.2.1, practice room through the scope at zoom 1 / 0.05 / 0.7, ~14,000 frames; Old 3DS 2026-11-27 (row 15) | 0 anomalies, 0 overlaps; BULLSEYE 256x512 ETC1, max level 5, 87,360 of 87,424 B |
| 12 | Mandarine, every zoom step, tier 512 and after `hdUnpack` at tier 256 | emulator | every picture clean (the emulator reads the level right: row 18) |
| 13 | New uncompressed HD pack (`tex3ds -z none`) on the owner's SD | owner, New 3DS | glitch still there |
| 14 | Live A/B on the combined build (touch screen steps the mode, `throwaway/combined-owner-test` 75a26a47), 2026-11-26 night: mode 1 mip filter NEAREST; mode 2 texture cache cleared before every draw; mode 3 `maxLevel` one less; mode 4 `maxLevel` 0 | owner, New 3DS | modes 1-3 "zero improvements" (the owner's words for the three together; that session's log was not pulled); mode 4: glitch gone. Modes 1 and 2 agree with the appliance (section 3). Mode 3 does not: it is what the fix does to these textures, and on the appliance it cures (rows 20, 22). See section 7 |
| 15 | T512C: the practice-room targets through the scope, eight zoom steps, real textures, tier 512, edge smoothing on (2x1), the game's own frame readback (no ATM screenshots) | Old 3DS appliance | targets clean from 22 to 200 pixels across; at 12 pixels nothing can be told by eye. DRAWLOG: 0 anomalies, 432 spans, 0 bad |
| 16 | D512C: the same run with texel-address textures in place of BULLSEYE, D7_BULL, C7_WALL (every tile a colour naming its level and place) | Old 3DS appliance | at the smallest zoom (target 12 pixels across, levels 4-5) the target's lower-left reads the 8x16 level's blue (214-218) where the rest reads 190; from 22 pixels up (levels 0-3) uniform |
| 17 | mipprobe p1: a standalone program, one quad, the 256x512 and 128x512 ETC1 chains at 512 placements in linear memory (every 0x80 over 16 KiB, every 4 KiB over 512 KiB, low and high in the heap), four sizes; trilinear, and mip NEAREST and NEAREST with a half-size quad drawn first | Old 3DS appliance | 3,072 of 12,288 tests off: every test of the 16-wide level, at every placement and in every mode. 56 of 224 interior pixels (a quarter) read the 8x16 level, tiles x 0, y 0..1; with trilinear 37 of 224 are past the threshold (the wrong quarter blended). Levels 1-3 clean |
| 18 | mipprobe p1 and p2 | Mandarine r1.2.1 | 0 off |
| 19 | mipprobe p2: one level at a time at the size that samples it, trilinear; and level maps (a large quad, NEAREST, an LOD bias picking one level) | Old 3DS appliance | off: ETC1 256x512 max 5 at LOD 4.3, ETC1 128x512 max 4 at LOD 3.3. Clean: the same chains with max one less; ETC1 512x256, 256x256, 64x64; ETC1A4 256x512, 256x256; RGBA8 128x256, 128x128. Level maps: every level of every chain right, so the glitch needs the level drawn small (the LOD from the size, not from a bias) |
| 20 | mipprobe p3, p4, p5: 62 configurations (format, shape, the max level written), every level at LOD k.3 and k.7 trilinear, and at k.3 with mip NEAREST and with NEAREST | Old 3DS appliance | section 3's table. Told to stop at the 16 level, clean at every level measured: 256x512, 128x512, 64x512, 128x256, 64x128, 32x256, 32x64, 512x128, 256x64, 128x32, 512x64, 256x32 (all twelve wrong with their whole chain), and the wide 512x256 and 512x32. p5's own count: 638 tests, chains within the rule 0 wrong, chains past it 22 wrong. Mip NEAREST and NEAREST, one quad: 724 tests, 0 off. With CLAMP_TO_EDGE the tall 256x512 chain reads one wrong value over the whole quad (180 where 192 is right) and the wide 512x64 reads much as with REPEAT (24 of 608 pixels off against 27) |
| 21 | X1: row 15's run with edge smoothing OFF (targets 240x400), no fix | Old 3DS appliance | the small bullseye is in the lower-left of the targets at 24 pixels across (frame 950), and the targets are a smear at 16 (frame 800): the owner's picture |
| 22 | Y1: X1 with the fix (the same harness build plus `mipLevelsSampled`, the tall shapes' half of the rule) | Old 3DS appliance | three clean bullseyes at frames 800 and 950; from 30 pixels up the targets are as in X1. DRAWLOG: BULLSEYE bound with max level 4 of its chain of 5 |
| 23 | XA: the start tunnel, a six-step walk, real textures, tier 512, edge smoothing on, no fix (`NOMIPFIX=1`) | Old 3DS appliance | a hard-edged dark and white block on a leg of the inner arch in all seven captures. DRAWLOG: C7_WALL bound with max level 4 |
| 24 | YA: XA with the fix (the rule as it is on the branch) | Old 3DS appliance | the block is gone; the two runs' captures differ only inside its box (91-211 pixels a frame, x 162-217, y 213-274 of 400x480). DRAWLOG: C7_WALL bound with max level 3 of its chain of 4 |

Level 0: the large chains' level 0 blended with level 1 was measured (LOD 0.7,
256x512 and 128x512: right); alone at LOD 0.3 it does not fit the test program's
quad. Level 0 of the small chains: 16x64, 16x32, 64x16 and 128x16 wrong with
their 8 level, 32x16 and 16x16 right.

## 6. Ruled out

- **The files.** Rows 2, 3, 13.
- **The level count and offsets the renderer hands over.** Rows 7, 11.
- **A stale CPU cache (cache visibility).** Rows 8, 9. The per-texture flush
  stays as P3 hygiene; it is not the fix.
- **Placement in linear memory.** Row 17: the same at 512 placements.
- **The New 3DS.** Rows 16, 17, 21: the Old 3DS does it.
- **Level 0 of the two textures.** Rows 1, 14.

## 7. Not verified

- **The owner's New 3DS with the fix.** The appliance is an Old 3DS. Both have
  the same GPU, and the glitch on the appliance has the owner's shape, but the
  fix has not run on the owner's console. `mipprobe-owner.3dsx` (40 seconds,
  beside the play build) answers whether its GPU reads these levels as the
  appliance's does.
- **The owner's mode 3.** The owner reported no improvement from modes 1-3
  together. Mode 3 lowers `maxLevel` by one, which on the appliance cures the
  glitch (rows 20, 22). If the fix build still shows the glitch on the New 3DS,
  mode 3 was right and there is a second cause; what to send then is `mdk.log`
  of a session where it shows, the edge-smoothing setting, and a photo.
- **Edge smoothing as the reason the owner's sessions differ.** It follows from
  which axis decides the level (rows 15, 21, measured); the owner's setting in
  each session is not known.
- **The shapes no test covered.** 1024 on a side, and 16:1 tall. The rule stops
  them like the rest.
- **The wide shapes in the game.** Both game A/Bs are of tall textures; the
  wide shapes are wrong and then right in the test program only.
- **Why the hardware does it.**
