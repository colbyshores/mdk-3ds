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
  (`Renderer.cs:567,596,607-609`), but `renderer_c3d.cpp` never calls
  `C3D_AlphaBlend`, so `Pass::Blended` glass draws opaque, and the
  `OnTop`/`Overlay` depth exemptions are not modelled. The review measured
  non-`Solid` batches in 12 of level 3's 17
  arenas (`HMO_3`: 75 Blended; `HMO_9`: 25 Mirror + 49 Blended; `HMO_2`: 4
  Blended). They are unreachable today only because the 3DS frontend draws the
  start arena, whose 154 batches are all `Solid`.
- Kurt's sprite is drawn at `Player::feet`, not at the C#'s `FrameBlend.Feet`.
  With blending off — which is what a fixed-step capture and the tests are — the
  two are the same pixel, and sub-step blending is not ported.
- `KurtSprite` cannot reach the states another subsystem owns: the ride's pose
  (`Kurt.Pose`, which loops past its last frame), the beta back-up (`K_BCKUP`) and
  muzzle flash (`K_MUZZF`) are loaded but never selected, and `Kurt.Visible` and
  sniper mode are absent so the C#'s early returns there are the false branch. The
  knock-down, throw, slide, ledge and helmet states have no writer for the same
  reason. Whoever ports each one sets `Player`'s state and the frame counts follow.
- `KurtSprite` (like `HudView`) has no release path: `load()` re-creates its
  palette, mesh and every frame texture. The Linux frontend loads one level per
  process, so nothing leaks today; a level change needs `mdk_render_release`
  around a mark, and the 3DS frontend has no Kurt binding at all yet.
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

## Review findings still open

- Review-24's open question: VK `canvasWidth` reads the swapchain extent while the
  C# reads the render target — indistinguishable while both are 4:3.
- Review-25's MINOR, a latent trap: `getDescriptorSet` caches sets by
  `(texture, palette, repeat)` and neither `mdk_render_update_texture` nor
  `mdk_render_release_texture` invalidates `r->descs`, so a recreated texture id
  leaves a cached set naming a destroyed `VkImageView`. Neither has a caller
  outside `renderer_vk.cpp` today — whoever wires animated textures must erase the
  affected entries (or stamp a generation counter) first.
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

- Review-30 MAJOR, same reachability caveat: eager Kurt-sprite residency does not
  fit the 3DS. `load()` uploads every frame of every state animation at level
  load; the reviewer measured `TRAVSPRT.BNI` as 19 animations / 360 frames, and
  the 3DS expands each to `GPU_RGBA8` padded to POT, which is ~49.9 MiB against
  SPEC's 6 MB VRAM budget (K_CHUTEC alone is 12 MiB). Worse, `evictTextures`
  deletes the `C3D_Tex` and only sets `valid = false`, and nothing re-creates an
  evicted id because the caller holds an `MdkTextureId` and not the source
  indices — so the first failed Kurt allocation silently draws an arena surface
  untextured for the rest of the session. Needs bounded/on-demand residency
  (the frames of the current state, chute frames lazily) before the binding lands.
- Review-30 MINOR, narrower than the release-path row above: `loadAnimation`
  clears `out->frames` on a mid-animation texture failure, and `add()` replaces an
  already-loaded `Animation`, in both cases without `mdk_render_release_texture`,
  so those GPU ids are unreachable. The C# does not diverge here — its cache is
  keyed by `(name, frame)`. No symptom today: the six slide/surf animations are
  absent from `TRAVSPRT.BNI`, so `add()` never replaces a name.

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
