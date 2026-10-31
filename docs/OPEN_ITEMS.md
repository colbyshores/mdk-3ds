# Standing open items and traps

Extracted from `docs/CONTINUATION.md` on 2026-10-29 so that file stays at
"current state + priority order". Nothing here is new policy; each item records a
measured or cited reason, and the batch that closes one should delete its entry.

## Testability and the oracle harness

- Oracle/goldens compare fixed table + script fields, not velocity, `alarm_ticks`,
  `Items.Bomb`/`Decoy`, or all combat/effect state. Use `vm_tests` for otherwise
  unreachable opcode behavior. Tick corpus matches through t30000 on levels 3-8.
- Oracle driver: `/tmp/kilo/oracle/Driver/Program.cs`, args `L T 1 script`, with
  `DOTNET_ROOT=/tmp/kilo/dotnet10` and `MDK_DATA_DIR=<repo>/MDK`. The source oracle
  `src/Mdk.Game/**` is read-only; `tests/frames/capture_oracle.sh` builds the
  scratch tree.
- `/tmp/kilo` is readable but NOT writable by the Pi harness (it belongs to the
  other loop's user); captures go under `/tmp/mdk-frame-reference` or
  `/tmp/mdk-pi`, with the display recorded.
- Oracle capture environment, all three learned the hard way: glslangValidator
  needs `-D` and an ATTACHED `-I<dir>`; `NUGET_PACKAGES` must reach `ppy.SDL3-CS`
  (there is no network); the host `/tmp/.X11-unix`/`:99` lock is unusable, so the
  oracle runs `Xvfb :99` inside `unshare --user --map-root-user -m` with a private
  `/tmp/.X11-unix`. Do not simplify any of them.
- `--level=N` (3-8) selects the level in `LevelData::load`, `LEVEL%d.CMI` and
  `SoundBank::forLevel`; the default remains 3. No `GameEngine` exists to wire
  opcode 140 — do not fake call-site wiring.

## Known divergences from the C#

- **`ObjectMotion.Bank` is not ported, so the port never writes `MdkObject::roll`
  from motion.** The C#'s automatic banking (`ObjectMotion.cs:749-763`:
  `roll = clamp((wrap(roll) - turn) * 0.95, -10, 10)` with the turn clamped to
  2°/tick, skipped under flag 0x80 / 0x1 / 0x40 or while thrown) is the only
  writer of roll for a walking alien, and the port has no `Bank` at all — the
  only writers are the script VM's turn opcodes. Measured at level 3: the
  oracle's XF has `roll=-6.3411` at t120 and `9.0251` at t1200 where the port
  has `0.0000`; seven objects differ by t15. The object-table golden does not
  contain roll, which is why this was invisible until the object-pose golden
  landed. That is also why `golden/L3_tN_transform.txt` is pinned at t1 only:
  every multi-tick transform turns by a roll the port does not have. The pose
  goldens are unaffected (roll is not a pose input) and pass at t1/t120/t1200.
- In the object pass, **the GPU half of the draw stage is the next slice**.
  `ObjectView::addObject` (game/object_view.{h,cpp}) is `ObjectView.Draw` up to the
  seam: it fills a per-frame `ObjectStream` (posed corners + one queued
  `ObjectStreamDraw` per batch, with the stream offset, the surface, the animated
  frame and the transform) and is pinned by `golden/L3_t1_draw.txt`. What is left is
  the part the C# does inside `Renderer`: map each batch's `Surface` to a backend
  texture and palette (that is `LevelView::textureId`/`flatColourId`'s logic — the
  frame-stacked height and the one-pixel flat-colour texture — and it should be
  factored out of `LevelView`, not copied), upload the stream once per frame with
  `mdk_render_create_dynamic_mesh`/`mdk_render_update_mesh`, issue the queued draws,
  and call the whole thing from `mdk_linux/src/main.cpp` after `view.draw()` (the
  3DS frontend has no `ScriptRuntime`, so it has no objects to draw yet). That batch
  changes the picture, so `tests/frames/reference.sha256` has to be rewritten by
  `regenerate_reference.sh` — which means an oracle capture — and not by hand.
  **The design decision `OPEN_ITEMS` used to leave open is made**: the C#'s
  one-buffer-per-frame stream is what the port does, and it needs no seam change,
  because a dynamic mesh already is "one buffer, uploaded once before the frame,
  drawn at an offset". One dynamic mesh per object was rejected: its lifetime would
  hang off a script-created object's identity and it would fragment linear memory
  mid-play. The stream grows to the frame rather than carrying the C#'s `1 << 18`
  bound; measured on the oracle the whole pass is 2559 corners at L3 t1 and 5616 at
  L8 t1. The mods' model replacements (`Source::Mod`) stay deferred with cause: the
  mods are the enhanced look's and are not ported.
- Incomplete decoy/`AlienTarget` selection.
- Opcode 207's `MathF.Sign(NaN)` exception is accepted because C++ exceptions are
  banned; degenerate-path guards return 0 instead (this now also covers
  `Vector3.Normalize` of a zero vector and `MathF.Sign(NaN)` in
  `render/arena_scene.cpp`, neither reachable from a real arena).
- Linux runtime combat/inventory is not yet the live `Player`; the 3DS frontend has
  no runtime binding.
- Review-21 caveat: the post-camera solid refresh pins only the current arena, not
  the C#'s `SolidArenas`.
- C3D depth state differs from the C#: depth test disabled versus the C#'s
  LESS_OR_EQUAL test/write. Fixing it needs a projection remap, `C3D_DepthMap`, the
  test function, the clear value and a capture — and it is also why
  `renderer_c3d.cpp` ignores the now-ported `depth_layer` (with no depth test the
  painter's order decides, exactly as in the original). The ARM11 cost of the depth
  layer computation is unmeasured.
- The newest external review's MINOR: the 3DS `Pass` implementation covers culling
  only. The C# derives cull, blend and depth-test/write from `Pass`
  (`Renderer.cs:565,596,606-607`), but `renderer_c3d.cpp` never calls
  `C3D_AlphaBlend`, so `Pass::Blended` glass draws opaque, and the
  `OnTop`/`Overlay` depth exemptions are not modelled. The review measured
  non-`Solid` batches in **10 of level 3's 17** arenas (HMO_2, HMO_3, HMO_5,
  HMO_7, HMO_8, HMO_9, HMO_10, CHMO_4, CHMO_8, CHMO_9; `HMO_3`: 75 Blended;
  `HMO_9`: 25 Mirror + 49 Blended; `HMO_2`: 4 Blended). The seven pure-`Solid`
  arenas are HMO_1, HMO_4, HMO_6, CHMO_1, CHMO_2, CHMO_5 and CHMO_6. They are
  unreachable today only because the 3DS frontend draws the start arena, whose
  154 batches are all `Solid`.
- Kurt's sprite is drawn at `Player::feet`, not at the C#'s `FrameBlend.Feet`.
  With blending off — which is what a fixed-step capture and the tests are — the
  two are the same pixel, and sub-step blending is not ported.
- `KurtSprite` cannot reach the states another subsystem owns: the ride's pose
  (`Kurt.Pose`, which loops past its last frame), the beta back-up (`K_BCKUP`) and
  muzzle flash (`K_MUZZF`) are loaded but never selected, and `Kurt.Visible` and
  sniper mode are absent so the C#'s early returns there are the false branch. The
  knock-down, throw, slide, ledge and helmet states have no writer for the same
  reason. Whoever ports each one sets `Player`'s state and the frame counts follow.
- `KurtSprite` (like `HudView`) has no release path for its palette and mesh:
  `load()` re-creates them, and a second `load()` (a level change) leaks both.
  The frames themselves are released now — `load()` releases the previous
  level's drawn frames, `add()` releases the replaced animation's frames, and
  the residency policy releases a frame when it leaves the budget — but `load()`
  still does not release the previous palette and
  mesh. The Linux frontend loads one level per process, so nothing leaks today;
  a level change needs `mdk_render_release` around a mark, and the 3DS frontend
  has no Kurt binding at all yet.
- Footsteps (`FOOT1`-`FOOT4` on the run frames) and the `LAND` sound are not
  ported: `Player::animate` advances the frame but plays nothing, and
  `UpdateSpecial`/`UpdateDeath` skip `UpdateTouched` because there are no touch
  volumes yet.
- **The 3DS sampler fix stays deferred with cause.** `renderer_c3d.cpp` never calls
  `C3D_TexSetWrap`, so its wrap field is 0 — not `GPU_REPEAT` (`enums.h:39`) — and it
  clamps like Vulkan used to. It cannot take the same one-liner: the 3DS pads index
  textures to POT with TRANSPARENT padding and rescales UVs by real/padded
  (`renderer_c3d.cpp:1133-1147`), so a plain REPEAT tiles at the PADDED period and
  samples index-0 padding, which discards — holes, not tiling. Needs the padding
  filled with the tile pattern; unverifiable without hardware.
- Old 3DS hardware timing is UNMEASURED. Emulator GUI runs must explicitly use
  `DISPLAY=:99`; `tests/frames/capture_cpp.sh` now sets it unconditionally rather
  than inheriting the session's, which is the operator's `:1`.

## What the 1.240% frame gap actually is (measured 2026-10-31)

The gap is `bad_percent=1.240 max_delta=247 mean_delta=0.1819`. It is three
unrelated things, and only one of them is a porting defect. Bands are the
comparator's (max-channel delta per pixel, mean over the band).

| band | bad | mean | max | what it is |
|---|---:|---:|---:|---|
| sky (y 0-79) | 774 (3.023%) | 0.8185 | 99 | shader-arithmetic noise in an ill-conditioned expression |
| middle (y 80-159) | 173 (0.676%) | 0.3446 | 247 | 40 px = one unported scene element; 133 scattered edge px |
| bottom (y 160-239) | 5 (0.020%) | 0.0054 | 58 | nothing |

**The sky is not an input mismatch, and no input change will move it.** Measured
against an instrumented scratch copy of the oracle (built by
`tests/frames/capture_oracle.sh`'s recipe; its screenshot is byte-identical to
the pinned reference, so it is a valid control):

- the panorama index bytes and the palette bytes are **byte-identical**;
- the uniforms are identical (`wrap_width 1800`, `horizon_row 240`, `offset 300`,
  `height 360`, `top_colour 240`, `bottom_colour 231`, `sampling 0`);
- the projection matrix is identical, and `clip_to_direction` differs only by the
  Vulkan `clip.y` fold (row 1 negated) plus ~1e-7 per entry;
- **feeding the C++'s own `clip_to_direction` into a CPU model of the oracle's
  shader reproduces the oracle's panorama cell on 76797 of 76800 pixels** (0
  colour differences). The camera axes, the matrix and the data are therefore not
  the cause, and neither is the interpolated ray field.

The residual is the conditioning of `clip_to_direction`'s w row: `[2][3] =
-1.99995005` and `[3][3] = 2.00000024` cancel to `5.019e-5`, so the per-corner
`direction.xyz / direction.w` carries a scale error of the order of the rounding
of two numbers near 2 (~1e-7, i.e. ~2e-4 of the cancelled result). The three
 corners' scale errors do not cancel in the interpolation, and ~2e-4 rad is
~0.06 panorama columns — enough to pick a neighbouring texel on a few percent of
the sky, which is exactly the scattered pattern and the ~99 maximum. The two
shaders are the same maths in two source languages (DXC's HLSL and glslang's
GLSL), and whether the small `cx*c2d[0][3]` terms are absorbed by the big
cancellation depends on the summation order and FMA contraction each compiler
chose. **Closing this needs the C++'s sky vertex stage to emit the oracle's
operation order, not different inputs.** That is a shader-codegen question; the
port's inputs are already exact. Do not re-litigate the panorama, the palette,
the uniforms, the projection or the camera for this gap.

**The `max_delta=247` patch is not Kurt.** It is 8x5 px at x 174-181, y 127-131.
The C++'s decoded `K_STILL` frame 0 is **byte-identical to the oracle's** (52x146,
0 differing texels, dumped from both sides), and the pixels the C++'s sprite
actually covers (1600 of them, x 151-185 y 123-220, measured by drawing Kurt and
not drawing Kurt and diffing) do not include them. The oracle draws something
opaque and dark there that the port does not draw at all: a scene element from
the object pass, which is not ported. **Porting the object pass is what moves
`max_delta`**; nothing in the sprite path will.

## Review findings still open

- Review-24's open question: VK `canvasWidth` reads the swapchain extent while the
  C# reads the render target — indistinguishable while both are 4:3.
- Review-25's MINOR is CLOSED: `getDescriptorSet` caches sets by
  `(texture, palette, repeat)`, and `forgetDescriptorSets()` now drops every
  cached set that names a texture or palette whose image view is about to be
  replaced or destroyed. `mdk_render_update_texture`,
  `mdk_render_release_texture` and `mdk_render_release_palette` call it before
  the change, so a recreated texture id cannot inherit a set that names a
  destroyed `VkImageView`.
- Review-29's MINOR, still open: `tools/publish_frame.sh` reads the gap from
  `compare_frames.py --measure` without checking its exit status, so a corrupt or
  unreadable oracle writes `bad_percent= max_delta= mean_delta=` into the committed
  frame manifest. The `[ -s ]` guard covers a missing file, not an unreadable one.
- Review-29's MINOR, still open: `publish_frame.sh` defaults `VK_ICD_FILENAMES` to
  lavapipe while `tools/check.sh`'s frame gate does not, so on a GPU host the
  published picture and the gated picture could be rasterized by different drivers.
  Both land on lavapipe here, so there is no symptom on this machine.
- Review-28's latent Overlay trap: the C#'s `Renderer.DrawOne` uses
  `command.World` unchanged for `Pass.Overlay` — no depth pull and no
  view/projection. Linux's `mdk_render_draw_mesh` currently applies the pull to
  every layered/lines draw and always applies view/projection. No reachable
  Overlay mesh path exists today; whoever adds one must mirror the C# exemption.

- Review-30's texture-residency MAJOR is **CLOSED** (iteration 34). The sprite
  frames are decoded, not uploaded, at level load: `KurtSprite` keeps the
  palette indices and uploads a frame the first time it is drawn. The 3DS names
  a 1 MiB residency budget (`MdkCapabilities::sprite_residency_bytes`), the
  shared cache keeps the least-recently-drawn frames inside it, and a frame that
  leaves the budget gives its GPU copy back. The Citro3D eviction is now
  **restorable**: every texture keeps its source (the indices for an index
  texture), and the draw path re-makes an evicted texture from it, so an arena
  surface can no longer stay untextured for the rest of the session. What is
  still open: the 3DS frontend binds no Kurt yet, so the bound is
  compile-verified only and **UNMEASURED on hardware**; and the kept sources
  cost system RAM — ~5.3 MiB of sprite indices for `TRAVSPRT.BNI` alone, plus
  the arena textures' own sources — which has not been measured against the
  3DS's heap.
- Review-30's narrower leak MINOR is **CLOSED** with it: `add()` releases the
  replaced animation's frames, and `loadAnimation` uploads nothing, so its
  failure path has no GPU id to lose. The C# does not diverge here — its cache
  is keyed by `(name, frame)`.

## The frame-record directive (closed as an open item, kept as a rule)

Owner directive 2026-10-29 landed mid-iteration 28, written by the other loop
sharing this checkout (`ralph/builder.md`'s new section, `docs/frames/README.md`,
`tools/publish_frame.sh`, all committed in ab97297 so the next iteration does not
inherit a dirty tree):

- A batch that changes what the renderer draws publishes the pinned frame with
  `tools/publish_frame.sh <label>` AFTER the gates and commits
  `docs/frames/<date>-<label>.png` and its `.txt` beside it, with the measured gap
  movement in the commit message.
- Only the C++ port's own derived 320x240 frames are tracked. The oracle's frame
  is never committed — it is ROM-derived pixel data and lives in the scratch
  directory. That is consistent with `regenerate_reference.sh`; the first reading
  here, that the directive contradicted it, was wrong.
- Never hand-edit a published frame. `tools/runner/gates_frames.py` decides
  whether a frame is acceptable; the record only says what the build produced.

## Documentation drift

- `AGENTS.md`/`SPEC.md` cite an absent `docs/DOCTRINE.md`; SPEC §9.4 names an
  unwritten `gates_romfs`. Flagged to the owner; do not invent the policy.
