# MDK HOME banner v3: shipping candidate

**Status (2026-11-27): APPROVED.** The owner installed it as test title "R3-a MDK ship" (0xF7A58) on their New 3DS: "fantastic" — faces the camera, no flicker, plays once and holds. This is the banner for MDK. The other candidates (v1, v2, Astra's, round 2, R3-b, R3-c) were deleted on the owner's word; the text below still names them as history. The source audio is `../mdk_clip.wav` (Audacity project `../mdk_clip.aup3`); the zip carries the compiled CWAV.

Original status: this was a candidate and had not yet run on a console. Astra's 4-mesh banner (`../v3 Astra/`) did run on the owner's New 3DS without freezing. That proved the freeze was caused by v2's layout. On the console, Astra's banner showed three new faults:

1. the whole scene rotated;
2. Kurt disappeared at every pose change;
3. the animation looped.

This build keeps the low-count layout that ran, fixes all three faults, and fixes the flag defect found in every pycgfx file.

## Files

| File | What it is |
|---|---|
| `mdk_banner_v3.bnr` | The shipping candidate: CBMD with the LZ11 CGFX and v2's CWAV (unchanged). |
| `mdk_banner_v3.cgfx` | The same CGFX, uncompressed (484,376 bytes). |
| `mdk_banner_sim.mp4` | A simulation of the first 6 s through the HOME camera, with sound. |
| `mdk_banner_sim_v2_vs_v3.png` | v2's simulation (left) against this build's (right). |
| `mdk_banner_source.zip` | The generator. Rebuilding from it gives a byte-identical `.bnr` (commands below). |

## Structure audit (read with a struct-walking parser, not a byte scan)

| | v2 (froze) | Astra (ran; rotates, flickers, loops) | Round-2 fix R2-1 (not run) | **v3 ship** | Forsaken (approved) | T2K (ships) |
|---|---|---|---|---|---|---|
| decompressed CGFX (bytes) | 514,808 | 485,888 | 349,272 | **484,376** | 489,256 | 400,552 |
| meshes / bones / animated members | 30 / 31 / 29 | 4 / 7 / 3 | 3 / 3 / 1 | **4 / 3 / 1** | 4 / 6 / 3 | 6 / 4 / 1 |
| texture bytes | 458,752 | 458,752 | 327,680 | **458,752** | 360,448 | 8,192 |
| member flags | 0x03B80000 ×29 | 0x0040AFC0 ×3 | 0x003F0000 | **0x007F0000** | 0xA00, 0x3F0000, 0x3B80000 | 0x3870000 |
| rotation-W slot (+0x2C) | not flagged absent, raw 0 | absent | not flagged absent, raw 0 | **absent** | not flagged absent, raw 0 | not flagged absent, raw 0 |
| mesh-node names | 30/30 empty | 4/4 set | 3/3 set | **4/4 set** | 4/4 set | 6/6 empty |
| key interpolation | STEP | STEP, holds as far-apart keys | LINEAR, 0.06-tick jumps | **LINEAR, 1/4096-tick jumps** | LINEAR | LINEAR |
| rig | 30 bones off the origin, no billboard | billboard on an inner `name` bone; bg and strips are its children, strips off the origin | root-level billboard-5 bones at the origin | **root-level billboard-5 bones at the origin** | same as v3 | same idea |
| render layers (bg / Kurt) | 0 / 0 | 0 / 0, lit | 0 / 1, unlit | **0 / 1, unlit** | 0 / 1 / 2, unlit | |
| CANM loop / frame size | 1 / 360 | 1 / 360 | 1 / 360 | **1 / 36,057** | 1 / 36,057 | 1 / 685.44 |
| mesh record stride, IMAG length, DATA dictionary slots | 0x74, excludes header, 15 | 0x80, includes header, 16 | 0x74, excl., 15 | 0x74, excl., 15 | 0x74, excl., 15 | 0x74, excl., 15 |

### Astra's claims about v2

- **The rotation-W flag is true.** In v2 the slot is neither absent nor constant, and it holds a raw 0. But Forsaken and T2K carry the same defect and run on hardware, so this flag cannot be the whole cause of the freeze. v3 now marks the slot absent anyway, in `make_cgfx.py`, so the build no longer depends on the HOME Menu tolerating it. The build also fails if any curveless slot is unflagged; that check was confirmed to fail when the fix is switched off.
- **The empty mesh-node names are true.** T2K's are empty too and it ships, so they are not the cause on their own. v3 sets them, as Forsaken does.
- **The 0x74 mesh stride and the IMAG length are true.** Every pycgfx file has these, including both hardware-proven banners. Not a cause; unchanged.

### Why Astra's banner rotates

The billboard sits on an inner `name` bone. Its children (`bg`, `kurt*_strip`) have no billboard, and the strips are off the origin (y = −192, 288, 240).

Forsaken's test 1 found that a billboard does not carry over to children. Its tests 2 and 3 found that every flat layer needs its own root-level bone at the origin with billboard 5.

v3 copies Forsaken's bone records exactly: `bg` and `kurt` are children of the scene root, with flags 0x1DF, billboard 5 and translation (0, 0, 0). These are the same values Forsaken's `bg` and `donna` bones have.

Kurt's bone only ever moves along y, which is the turn axis, so it cannot orbit.

### Why Astra's Kurt disappears

`verify_frames.py` (in the zip) checks the banner using only the CGFX. It evaluates every curve every 1/8 tick under three readings: step, linear and nearest-key. At each sample it counts the Kurt quads the HOME camera can see.

Astra's banner holds each pose as two keys far apart, on three separate STEP tracks. That is correct only if the HOME Menu really steps:

| Reading | Result |
|---|---|
| step | correct |
| **linear** | **37 ticks with no Kurt**, one gap at every pose change (e.g. ticks 0.875–1.125, 3.375–3.625), plus ticks with two Kurts and 173 wrong poses |
| nearest-key | ticks with two Kurts |

The owner's report, "Kurt is completely getting removed between frames", matches the linear reading. So the HOME Menu evidently interpolates these keys rather than stepping them.

v3 holds every pose with explicit key pairs on one track. Each jump takes 1/4096 of a tick, so the curve is constant everywhere else whatever interpolation the HOME Menu uses. Under all three readings, at 1/8-tick and at 1/4096-tick resolution:

- exactly one Kurt is visible at every sample;
- all 29 poses show, changing at ticks 0, 2, 6, … 78, 102, 138.

Two other suspects were checked and are not present in v3:

- **Draw order:** v3 uses Forsaken's unlit layers (bg layer 0 with depth write, Kurt layer 1 with depth test only, z = 2 in front of the logo at z = 0).
- **Bleed between neighbouring sprites:** each sprite has a 2-texel transparent margin plus 1 texel of packing gap, its UVs are half a texel inside the sprite, and there are no mipmaps.

### Play once and hold

The HOME Menu restarts the animation when the title is selected and loops it at the CANM frame size. Curves hold their last value after their final key.

Forsaken uses this to play its intro once, with a frame size of 36,057, on the console it was approved on. v3 uses the same frame size: Kurt flies in once, lands at 2.3 s, and holds his final pose for about 10 minutes. The entrance would only replay if the title stayed selected that long.

The sound is v2's CWAV and plays as the HOME Menu plays it.

A one-time CANM (loop flag 0) has never been tried on hardware. It is offered only as the experiment R3-c below.

### What v3 changed from v2's look

Nothing intended: the same 29 frames, timing, background and sound, at v2's own 0.5 sprite scale. Astra's banner ran with this texture load (458,752 bytes), so the 0.42 down-scale used in round 2 is gone.

The background and Kurt now face the camera like a diorama, as Forsaken's do, instead of orbiting.

## Test CIAs

All three use the round-1 kit: `bannertest.elf`, MDK's rsf with only Title, ProductCode and UniqueId changed, smdhtool SMDH flags 0x141, and v2's CWAV. They are in `/tmp/claude-1000/-mnt-nas-Development-mdk-sdl/2f972c2a-8aa6-4574-a611-8613dbb56e37/scratchpad/bannertest/round3/`.

| CIA | UniqueId | HOME name | What it is | Expected |
|---|---|---|---|---|
| `banner_r3_a_ship.cia` | 0xF7A58 | R3-a MDK ship | **Shipping candidate**: diorama rig, no-gap keys, play once and hold | No freeze, no rotation, no flicker; Kurt lands and stays |
| `banner_r3_b_diorama_loop6.cia` | 0xF7A59 | R3-b MDK diorama loop | The same with v2's 6 s loop (frame size 360) | Diorama and no flicker; the entrance replays every 6 s |
| `banner_r3_c_onetime.cia` | 0xF7A5A | R3-c MDK one-time exp | Experiment: CANM loop flag 0, frame size 360 | Unknown: plays once and holds, or the HOME Menu misbehaves |

Round 2 is in the sibling `round2/` folder (R2-1 to R2-6, 0xF7A52–0xF7A57). It now matters only for explaining the original freeze. R2-6 is v2 with only the 29 rotation-W slots flagged absent (29 bytes changed).

## Rebuild

You need Python ≥ 3.12 with `gltflib==1.0.13 pillow numpy scipy rectpack`, plus pycgfx `skyfloogle/pycgfx@1f78850` (or `6c2e0cd`) with `pycgfx_patch.diff` applied.

```
MODE=strip TEX_SCALE=0.5 MAX_SHEETS=3 HOLD_FRAMES=36057 python build_scene.py out/ship
HOLD_FRAMES=36057 PYCGFX=<pycgfx> python make_cgfx.py out/ship   # applies + checks the hardware rules
python make_bnr.py out/ship/COMMON.cgfx mdk_banner.bcwav out/ship/mdk_banner.bnr
python verify_frames.py out/ship/mdk_banner.bnr                    # exactly one Kurt at every 1/8 tick
python sim.py out/ship out/ship/sim.mp4 [audio.wav]
# every variant (round 2 + round 3): PY=<venv python> PYCGFX=<pycgfx> [V2_BNR=<v2 bnr>] ./build_all.sh <out> mdk_banner.bcwav
```

`make_cgfx.py` fails the build if any of these rules is broken:

- the model and the skeletal animation are both named `COMMON`;
- there is no material animation and no skinning;
- every animation member is Transform, and every curveless slot (rotation W included) is flagged absent or constant;
- every flat layer is a root-level billboard-5 bone at the origin, and the bone order matches the glTF;
- textures are RGB565 or RGBA4, power-of-two, ≤ 256;
- the budget holds: ≤ 8 meshes, ≤ 8 bones, ≤ 4 members, ≤ 458,752 texture bytes, CGFX < 512 KiB.

The sprites are cut from v2's sheets in `v2build/`, because v2's zip does not include the FLC work directory.
