# Spec: HD textures on the 3DS (ETC1 / ETC1A4)

Owner direction: 2026-11-14 (SPEC-COMPLETION.md item 6, "ETC textures for the 3DS
build path") and 2026-11-17 ("we will be using ETC1 and ETC1A4 on the 3DS
specifically. our build system and mip map chain needs to be oriented around
that fact"; "we will be taking HD textures and making them work on the 3DS").
Reference implementation: `../forsaken` (`regen_hd_textures_*.sh`,
`render_c3d.c`, `MIPMAP_EVIDENCE.md`, `STREAMING_PHASE2.md`).

## 1. What is fixed

- **Two formats on the 3DS, chosen per texture by its alpha.** `ETC1` (4 bits per
  texel) for an opaque texture, `ETC1A4` (8 bits: ETC1 colour + 4-bit alpha) for
  any texture with transparency. The PICA200 decodes both natively. ETC1 is 8x
  smaller than the RGBA8 the original look expands to; ETC1A4 is 4x.
- **Compression and mip chains are built on the HOST, at build time, by
  `tex3ds`** (`-f etc1` / `-f etc1a4`, `-q high`, `-m gaussian`), into `.t3x`
  files. The 3DS never compresses: an ETC1 encoder on a 268 MHz ARM11 at load
  time is not a budget this port has. The runtime imports the `.t3x` (tex3ds
  library, `Tex3DS_TextureImportStdio`) chain and all.
- **The original look is untouched.** The GOG textures keep their lossless
  palette -> RGBA8 expansion (SPEC.md 7.3); their Enhanced Mode mip chain is the
  one built at texture creation (pica::buildMipChain, `enableMipmaps`). The HD
  layer replaces a texture; it never re-encodes an original one.
- **One HD pack serves both builds.** The source is the PC's own mod tree and
  key scheme (`mdk_core/src/game/mod_images.h`, `game/hd_key.h`):
  `mods/<m>/textures/NAME@<hash>.png` (exact view), `.../LEVELn/NAME.png`,
  `.../NAME.png`, `mods/<m>/images/NAME.png` (2D images). The Linux build decodes
  the PNG; the 3DS build ships the `.t3x` the build made from it, under the same
  key.
- **Behind a toggle.** `enableHdTextures` joins SPEC.md 7.9's EnhancedToggles
  (config.ini, the SELECT menu). It is a LOAD-TIME toggle (a texture is imported
  or not when it is made), like `enableMipmaps`' allocation; off means the
  original texture.

## 2. Build side (`mdk_3ds/tools/build_hd_pack.*`, host)

`build_hd_pack.sh` makes the HD mod (`hd_export/hd_tex_export`, then
`build_hd_mod.py`: the C# generator's export, padding, Real-ESRGAN 2x and crop),
then `hd_pack_convert.py` converts it. For every PNG under the mod tree:

1. **Bleed the transparent texels' colour** before anything filters it. `tex3ds`'s
   gaussian mips and the ETC1 block fit both average colour across alpha-0
   texels, which pulls cut-out edges toward black -- the defect the runtime path
   already fixes with pica::prepareForFiltering. The host tool runs the same
   rule (iterated until every alpha-0 texel within the mip footprint has a
   colour), from the same source file, built for the host as the tests already
   build pica_matrix.cpp.
2. **Size to a power of two, capped by a per-console budget.** The PICA wants
   POT and <= 1024; ETC1 wants multiples of 4 (every POT >= 8 is). The cap is a
   pack parameter, measured, not assumed: forsaken ships 256^2 for the Old 3DS
   and 512^2 for the New. A `.t3x` is POT, so the runtime's UV padding transform
   is the identity for an HD texture.
3. **Pick the format by alpha**: any alpha < 255 -> `etc1a4`, else `etc1`.
   (Whether a 1-bit-alpha texture should stay ETC1A4 or be split is open; ETC1A4
   is the safe default.)
4. **`tex3ds -f <fmt> -q high -m gaussian -z none in.png -o out.t3x`**, then a
   manifest line per texture: key, file, format, width, height, levels, bytes.
   Uncompressed since 2026-11-26 (docs/3DS_ARCHITECTURE.md 5): on the Old 3DS the
   worker on core 1 reads 3.5 MB/s but reads and unpacks tex3ds' default
   compression at only 0.77 MB/s of file. `--from-pack` converts an older pack.
5. **Deterministic and stale-checked**: same input bytes -> same `.t3x` bytes; a
   rebuild only re-encodes what changed. The pack lands in RomFS (or on SD if its
   size makes the `.3dsx` push/CIA impractical -- measured, decided then).

## 3. Runtime (3DS backend)

- **The seam already exists and is a stub on the 3DS**:
  `mdk_render_replace_colour` / `mdk_render_replace_image`
  (`c3d/05_resources.inc`). The 3DS implementation resolves the key through the
  manifest (the shared ModImages lookup decides WHICH key; the backend loads its
  `.t3x`) instead of decoding a PNG.
- **The texture record becomes format-aware**: `C3dTexture` carries its
  `GPU_TEXCOLOR` (RGBA8 / ETC1 / ETC1A4), its source kind (an expanded original,
  or an imported `.t3x`), and its level count from whichever made it. Byte
  accounting -- eviction, `mdk_render_index_texture_bytes`, the sprite residency
  budget -- goes through ONE per-format size function
  (`RGBA8 w*h*4`, `ETC1 w*h/2`, `ETC1A4 w*h`, each chain x ~4/3).
- **Sampling is shared with the original path**: the FILTERING and MIPMAPS
  toggles drive the same per-draw filter, `maxLevel` and mip filter for an HD
  texture as for an original one; the half-cover alpha cut applies to ETC1A4's
  4-bit alpha (17-step values: 136 kept, 119 cut).
- **Animated textures**: an HD replacement of a stacked-frame texture must keep
  the stacking (or be per-frame); decided when the first such pack texture is
  met, not guessed.

### 3.1 The HD rule (owner, 2026-11-24)

"These are ETC1 and ETC1A4 and we should NOT be loading the SD textures AT ALL
when HD is turned on." With `enableHdTextures` on, a surface the HD pack covers
is drawn from its HD `.t3x` and nothing else: its standard-definition texture is
never read, palette-expanded, filtered, mipped or content-hashed -- not the CPU
expansion and not the packed original in `romfs:/orig`. It never blocks either.

- **Found by name, not by hash.** The orig pack's `byname.txt` row (level, arena,
  name -> `textures/name@<key>`) carries the texture's content key in its file
  name; the HD pack's files carry the same key (`textures/LEVELn/NAME@<key>`, the
  PC mod's names). `main_3ds.cpp` joins the two at level load, by string, so the
  lookup answers (palette, name) -> HD file with no hash on the console. The key
  is the texture through that palette (game/hd_key.h), so a name drawn through
  two palettes is two rows with two keys: a name never maps to another palette's
  texture. A row is also checked against the orig manifest's original size; a
  texture of another size under that name is not answered. No pack is rebuilt:
  the owner's SD pack (1382 textures) is keyed this way already.
- **Made from the files alone.** The pair is created with both files
  (`mdk_render_create_compressed_file`'s `hdPath`; `C3dTexture::hdOnly`). Toggle
  on: every ask (creation, the pump, the warms, a draw) goes to the HD stream;
  the packed original is not read. Toggle off: the packed original, as before.
  The older step -- an HD copy attached after the fact to a palette-expanded
  texture (`SurfaceCache::surfaceId` calling `mdk_render_replace_compressed`,
  counted as `hdAnsweredSd`) -- was removed on 2026-11-25: no lookup answered
  for it any more, so it never ran. (Kurt's HD frames and the HUD's pictures used `mdk_render_replace_compressed` /
  `mdk_render_replace_image_compressed` until 2026-11-25; both seams are gone: Kurt
  has no HD copy and the pictures are made by `mdk_render_create_hd_image`.)
- **Not arrived yet:** the draw draws nothing (or an original already resident
  from before the toggle went on); the early asks (the pump's walk of resident
  views, hidden batches, the object and effects warms) make it rare. A draw's
  ask is read before any hint's; a hint is read and imported only into budget
  room and never evicts.
- **Live toggle.** Off: the HD copies a draw still uses stand in until the
  packed original is imported (no main-thread read); the rest are freed. On:
  originals already resident stand in until the HD copy lands.
- **Where SD still loads, and why:** an animated (stacked) texture (the pack's
  strip is resampled to a power of two, squeezing its frames); a texture the
  game writes into (a bullet hole: the HD image does not have it); a texture the
  name index does not know (over the PICA's 1024, or outside the arenas'
  archives -- with an HD pack it is expanded, never hashed); Kurt's frames,
  ALL of them (`kurt.pak`, owner 2026-11-19: the pack's 128x128 frames are fewer
  texels; since 2026-11-25 his level animations -- the slides' K_SLIP/K_SLIDE/
  K_FSLIDE/K_BSLIDE on level 6, the board's K_SURF/K_SURFJ on level 4 -- are in
  kurt.pak too, and the 3DS asks the HD pack for no Kurt frame, so none streams
  or pops in; the HD pack's K_* files go unused);
  Original-shaded surfaces (the
  sky, glass) the PC never replaces. `hdrule:` lines at level teardown count it.

### 3.2 The level's resident set (owner, 2026-11-24/25: "I dont want surface pop in")

**Superseded 2026-11-26 by the HD window** (docs/3DS_ARCHITECTURE.md 3-4,
`mdk_3ds/src/hd_window_3ds.h`): only Kurt's arena, its neighbours and the arenas
it names are resident, every level at 512; an arena is not drawn until its files
are, the frame waiting for them as BSPShow did. `pinHdSet3ds`/`holdHdSet3ds` are
gone (`buildHdWindow3ds`/`holdHdWindow3ds`, `mdk_render_set_hd_window`). What
follows is the record of the level-wide set.

Per-surface streaming cannot promise a copy before its surface is seen -- the
Old 3DS reads ~13 HD files a second in play -- so pop-in was luck. Since
2026-11-25 the HD set is decided at level load and kept for the level:

- **The set** is every HD file the name index (3.1) pairs with the level's rows,
  sized exactly from the HD manifest's w/h/format.
- **The tier**, by arithmetic against the free linear heap less a 7 MiB play
  reserve (non-HD growth after load, measured: 6.27 MB on level 7, 6.06 MB on
  level 8): 512 (as built) if it fits; else 256 (levels over 256 texels dropped
  at the read: the reader thread rewrites the uncompressed .t3x with its size
  fields, level count and subtexture sizes one level down -- a quarter of the
  bytes per level, no re-encode, no pack change); else 256 with the files that
  save the most at 128, one at a time, until it fits; else per-surface
  streaming as before, logged `DOES NOT FIT`.
- **The hold.** The first view is asked for and the loading screen stays up
  (the applet loop served) until the whole set is resident; the same hold runs
  when the toggle comes back on. In play nothing of the set is read, evicted or
  late: harness builds print `hdrule: gate PASS|FAIL`.
- **Measured (Old 3DS, romfs pack):** L7 tier 256, 143 files 4.18 MB, hold
  6.2 s; L8 tier 256/128, 281 files 5.16 MB (103 at 128), hold 13.0 s. The
  4.2 streaming design below remains only as the fallback for a set that does
  not fit. The HUD's pictures (SC_STAT, SKULL, SNIP_TXT, the PICKUPS frames) and
  the bomber's sight (BOMBTARG, CROSS) that the HD pack covers are part of the set
  since 2026-11-25 (`mdk_render_create_hd_image`): with HD on their HD file is the
  only copy -- the indices are never expanded -- pinned and held for with the
  set; with HD off they are made from the indices at their first draw.

## 4. Memory and streaming

**Superseded in part (2026-11-25).** For the arena textures the name index
pairs with an HD file, sections 3.1 and 3.2 replace what follows: the HD copy is
the only copy drawn with the toggle on (never the original as a stand-in), and
the level's whole set is read on the loading screen and kept, so nothing of it
is streamed, budgeted or evicted in play. The per-surface stream below is still
the design for what the set does not hold: a level whose set does not fit (logged `DOES NOT FIT: streamed per
surface`). Sections 4.1 and 4.2 are kept as the record of how it was sized.

An HD pack does not fit the Old 3DS's linear heap all at once on every level, so
residency is part of the feature, in two steps:

1. **Per arena, all levels resident.** Textures live in an arena's own MAT
   archive (ARENA_STREAMING_SPEC.md 3.2), so HD imports follow the arena:
   imported when the arena (or the preloaded neighbour) loads, freed when it is
   freed. This is the arena streaming work's texture half, not a second system.
2. **Mip-tail residency + streamed top levels**, only if step 1 measures over
   budget: keep each texture's small tail (mip 3 and below -- a few KB) always
   resident and stream the large top levels for what is visible, evicting the
   rest (forsaken `STREAMING_PHASE2.md`: 6-10 MB saved on the Old 3DS). The PICA's
   `minLevel` (C3D_Tex lodParam) is what lets a texture draw from its tail while
   its top levels are absent.

### 4.1 Measured (2026-11-17, level 3)

- **The pack source exists only locally, by design.** Upstream
  (github.com/nautilusbits/mdk-sdl, formerly nemo22/mdk-sdl) makes HD textures
  "locally on your GPU, from your copy of the game ... Nothing upscaled is
  distributed"; none of its releases carries a pack. `build_hd_pack.sh` makes the
  mod locally and converts it. Until 2026-11-26 it built the C# oracle and ran its
  `--upscale-textures`; since then it needs no .NET: `hd_export/hd_tex_export`
  (C++, mdk_core's loaders) reads the textures out of the game data and
  `build_hd_mod.py` does the C# generator's padding, upscaling (the same pinned,
  SHA-256 checked Real-ESRGAN release, 44 MB, fetched once) and cropping. Its
  pre-upscale images and, with a fixed stand-in upscaler, its whole mod are pixel
  for pixel the C#'s for levels 3-8 (docs/WORK_LEDGER.md, 2026-11-26).
- Level 3: **578 textures** generated in 217 s on an RX 6600 (82 MB of PNG);
  converted at the 512 cap in 8 m 42 s on 16 cores: **400 ETC1A4** (every one has
  true alpha-0 texels in its source, palette index 0) and **178 ETC1**,
  **21.6 MB** with chains (20.3 MB 3D textures, 1.3 MB 2D images). Most common
  sizes: 256x512 (180), 128x512 (92), 512x512 (91), 256x256 (67).
- The linear heap free after a level-3 load is ~14-16 MB with the original
  textures resident (mem[loop] lin=, filtering/mip batches). **21.6 MB cannot be
  resident at once; streaming is required, not optional** (owner, 2026-11-17).

### 4.2 The streaming design this sets

- **Budget, not residency-by-arena alone**: a fixed HD byte budget in the linear
  heap (measured per console; it must leave the original textures' working set
  and the frame's own allocations room), LRU by last draw.
- **Imported on first draw, never in the draw call**: a draw of a texture whose
  HD copy is not resident binds the ORIGINAL texture this frame and queues the
  HD file; a worker thread (P4: owned, joined, FZ/DN at entry, small stack) reads
  the `.t3x` from SD into a staging buffer; the main thread, between frames
  (after C3D_FrameBegin's completion boundary, P3), turns it into a texture
  (Tex3DS import from memory). A texture appears in HD a few frames after it is
  first seen; nothing blocks the frame on SD.
- **Mip tail first**: when the budget is tight, a texture is imported from its
  smaller levels (minLevel / a tail-only texture) before its full chain, so a
  distant surface never costs its 512^2 level (forsaken STREAMING_PHASE2).
- **Arena-aware eviction**: an arena's HD textures are dropped when the arena is
  freed (ARENA_STREAMING_SPEC.md), so the budget follows Kurt.

## 5. Acceptance

- Emulator: per level, the 3DS with HD on against the Linux enhanced look with
  the same pack, side by side; no black fringes on cut-outs; no visible pop
  beyond what the streaming step logs.
- Hardware (Old 3DS, owner's final pass): peak linear-heap use per level with
  HD on, against the budget and against HD off; frame time per level.
- Build: the pack builder is deterministic (two runs, identical bytes) and its
  bleed is host-tested against the runtime's rule.

## 6. Open, for the owner

- **Which HD pack?** The owner's, built by `build_hd_pack.sh` and copied to
  `sdmc:/3ds/mdk/hd` (answered; none is in the repository).
- **The Old-3DS size cap**: the set's tiers (512, 256, 256/128) are chosen per
  level against the free linear heap (3.2); the pack itself is built at 512.
- **RomFS or SD**: SD (`sdmc:/3ds/mdk/hd`). A RomFS copy is a harness-build
  feature (`HDROMFS=1`, branch `harness/3ds-profiling` / `fix/3ds-hd-only`).
