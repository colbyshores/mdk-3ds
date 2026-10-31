## What this batch did (Phase 2 iteration 24 — the 1.240% frame gap, decomposed and two causes closed)

This batch is measurement, not code: it closes two of the three things the frame
gap is made of, and names the third precisely enough to be an increment. No
render-path file changed, because every candidate change was measured to not be
the cause, and the gate's ceiling is unchanged.

- **Built a valid control for the oracle.** `tests/frames/capture_oracle.sh`'s
  recipe was reproduced in a scratch tree so the oracle could be instrumented
  without touching the read-only `src/Mdk.Game/**`. The instrumented build's
  screenshot is byte-identical to the pinned reference frame, which is what makes
  everything below evidence rather than speculation. It dumped the sky's
  `clip_to_direction`, the panorama uniforms, the camera axes, the projection, the
  panorama index bytes and the palette bytes; a second patch encoded the oracle
  GPU's own per-pixel panorama cell into the screenshot.
- **The sky's inputs are identical, so the sky gap is not an input mismatch.** The
  panorama index bytes and palette bytes are byte-identical; the uniforms
  (`wrap_width 1800`, `horizon_row 240`, `offset 300`, `height 360`, `top_colour
  240`, `bottom_colour 231`, `sampling 0`) and the projection are identical; the
  C++'s `clip_to_direction` differs from the oracle's only by the Vulkan `clip.y`
  fold (row 1 negated, which is the documented convention) plus ~1e-7 per entry.
  Feeding the C++'s own matrix into a CPU model of the oracle's shader reproduces
  the oracle's panorama cell on **76797 of 76800 pixels with zero colour
  differences** — so the camera, the matrix, the data and the interpolated ray
  field are all excluded.
- **Named the residual: the conditioning of `clip_to_direction`'s w row.**
  `[2][3] = -1.99995005` and `[3][3] = 2.00000024` cancel to `5.019e-5`, so
  `direction.xyz / direction.w` inherits the rounding of two numbers near 2: ~1e-7
  absolute, ~2e-4 of the cancelled result. A uniform scale would cancel in the
  `normalize`, but the three corners' scale errors do not, and ~2e-4 rad is ~0.06
  panorama columns — the right size and the right shape (scattered single texel
  swaps, max delta 99) for the 774 bad sky pixels. Whether the small `cx*c2d[0][3]`
  terms survive the cancellation depends on the summation order and FMA contraction
  of the compiled shader, and the two sides are the same maths through two
  compilers (DXC's HLSL, glslang's GLSL). `OPEN_ITEMS.md` now says the sky needs
  the oracle's operation order in the C++ vertex stage, and that the panorama,
  palette, uniforms, projection and camera are NOT to be re-opened for this gap.
- **The `max_delta=247` patch is not Kurt, and `CONTINUATION.md` said it was.**
  Dumped the decoded sprite on both sides: the C++'s `K_STILL` frame 0 and the
  oracle's are 52x146 with **0 differing texels**. Diffing a capture with Kurt
  drawn against one without gives the 1600 pixels the sprite actually covers
  (x 151-185, y 123-220); the 8x5 patch at x 174-181, y 127-131 is not among them.
  The oracle draws something opaque and dark there that the port does not draw at
  all, i.e. a scene element from the unported object pass. **`max_delta` cannot
  move until the object pass lands**, which is now the named next increment.
- **Bands re-measured with the comparator's own definition** (max-channel delta per
  pixel, mean over the band): sky 774 px / 3.023% / mean 0.8185 / max 99; middle
  173 px / 0.676% / mean 0.3446 / max 247 (40 of them the object patch, 133
  scattered single edge pixels); bottom 5 px / 0.020% / mean 0.0054 / max 58.
- Temporary debug hooks left in the tree by the previous, timed-out iteration
  (`mdk_linux/src/main.cpp`, `mdk_linux/src/render/renderer_vk.cpp`, both marked
  "not for commit") are reverted; the tree is clean.



## What this batch did (Phase 2 iteration 23 — P5.0 frame-golden harness; the camera handedness fix it exposed)

- Built the P5.0 frame-golden harness (`SPEC-COMPLETION.md` §3 item 4), which
  `docs/CONTINUATION.md` listed as the owner's next priority and as the gate that
  must be green before any renderer batch. `tests/frames/capture_oracle.sh`
  builds the C# oracle in a scratch tree and runs it headlessly on display `:99`
  inside `unshare --user --map-root-user -m` with a bind-mounted private
  `/tmp/.X11-unix` (the host's socket and lock files belong to another user and
  `:99` is stale, and AGENTS.md P7 forbids the operator's display).
  `tests/frames/capture_cpp.sh` renders the same pinned scene from the C++ build.
  `tests/frames/compare_frames.py` compares two uncompressed 32-bit BMPs — the
  oracle's own `Bmp.Write` container — and names the first offending pixels so a
  regression is diagnosable from the gate log alone.
- Added the C++ capture path that made that possible: `--capture=`,
  `--capture-frame=N`, `--size=WxH`, `--level=N`, `--at=x,y,z[,yaw]` and
  `--fixed-step` in `mdk_linux/src/main.cpp`. `--fixed-step` advances exactly the
  C#'s 1/60-second test step per loop instead of the host's wall-clock delta, so a
  capture frame is a simulation state and two runs of the same arguments are
  byte-identical (verified: three runs, one SHA-256). `mdk_render_end_frame`
  copies the presented swapchain image into a host buffer through a
  present→transfer→present layout chain and writes it as a top-down BGRA BMP with
  the oracle's 54-byte header. `vk_context` now requests
  `VK_IMAGE_USAGE_TRANSFER_SRC_BIT` on the swapchain.
- `--level=N` (3–8) replaces the three hardcoded level-3 sites:
  `LevelData::load`, the `LEVEL%d.CMI` path and `SoundBank::forLevel`.
- `tests/frames/regenerate_reference.sh` captures both sides of each pinned frame
  and writes `tests/frames/reference.sha256`. No ROM-derived pixels are
  committed: the manifest holds hashes and measured deltas only. For the pinned
  frame (level 3, 320x240, seed 1, frame 1) it records
  `oracle=b60e7255…`, `cpp=2b586df2…` and the gap
  `bad_percent=12.680 max_delta=255 mean_delta=5.1350` at tolerance 0.
- `tools/runner/gates_frames.py` is gate 7 of `tools/check.sh`. Per pinned frame
  it checks (1) two live captures are byte-identical, (2) the capture matches the
  recorded `cpp=` baseline hash, and (3) the gap to the oracle does not exceed the
  recorded ceiling when the oracle pixels are on disk. It SKIPS (not passes) with
  a stated reason when there is no manifest, no `build-linux/mdk`, or no game
  data, the way `gates_smdh` skips before a CIA exists.
- The two hashes are deliberately not interchangeable and the gate says so in its
  own header: `oracle=` is the reference and the only number that means "the
  original look"; `cpp=` is a regression baseline. The C++ frame is 12.680% away
  from the oracle and closing that is P5, so a byte-exact-vs-oracle gate would be
  red from the day it was written — the same argument AGENTS.md §2 makes about the
  waived corpus-age clocks, because a permanently red gate trains the reader to
  skip the gate. The baseline check is byte-exact in exchange, so any change to the
  picture in any direction is a FAIL, and the only way through is to regenerate
  the manifest, which re-measures the oracle gap and puts that number in the diff.
  The ceiling may only shrink.
- PROVEN TO FAIL ON A SEEDED REGRESSION. Dropping the `flipClipY` call from
  `mdk_render_draw_mesh` (an upside-down level) and rebuilding made the gate print
  `the frame changed: 6134e5f7… != recorded 2b586df2…` and `the gap to the oracle
  grew: 76.997%/255/77.6020 vs the recorded ceiling 12.680%/255/5.1350`, and
  `bash tools/check.sh` exited 1 with `FAIL gates_frames`. The seed was reverted,
  the binary rebuilt, and the gate returned to `VERDICT: CONTRACT CLEAN` with the
  original hash.
- THE HARNESS IMMEDIATELY FOUND A REAL BUG. `buildLookAtCamera` built a
  LEFT-HANDED basis (`right = cross(up, forward)`, `forward` as the third axis).
  The C# oracle uses `System.Numerics.Matrix4x4.CreateLookAt`, which is
  RIGHT-HANDED: `zaxis = normalize(eye - target)`, `xaxis = normalize(cross(up,
  zaxis))`, `yaxis = cross(zaxis, xaxis)`. The old build negated x and z relative
  to the oracle — the world was mirrored and the solid geometry's front faces were
  culled away. `camera_builder.h` now states the oracle's convention and the
  projection is `CreatePerspectiveFieldOfView`'s (`zScale = -far/range`,
  `zOffset = -near*far/range`, `w = -z_view`), whose NDC depth still runs 0 (near)
  to 1 (far), so the Citro3D `row[2] -= row[3]` depth conversion is unchanged.
- The Vulkan backend now folds the clip.y flip into the combined matrix
  (`flipClipY`, right-multiply by `diag(1,-1,1,1)`, i.e. negate column 1) instead
  of relying on a negative viewport height, and the sky's clip-to-direction — the
  INVERSE — carries the same flip on its LEFT side (`flipClipYLeft`, negate row 1)
  and is uploaded verbatim rather than pre-transposed. `palette.vert` and
  `sky.vert` multiply `M * v`, which is GLSL's column-major reading of the
  engine's row-major row-vector matrix; the previous `v * M` was the transpose.
  The canvas ortho is written straight for the y-down framebuffer, so its
  translation moved to row 3 (indices 12/13).
- `vk_context::beginFrame`'s viewport is now POSITIVE. It was dead state —
  `mdk_render_begin_frame` sets its own viewport immediately afterwards and the
  last set wins — and this was established rather than argued: the pinned frame
  hash is byte-identical before and after the change. Two comments in the tree
  claimed the negative viewport was what restored the oracle's orientation, which
  contradicted the live code and would have been "fixed" the wrong way by the next
  reader.
- `renderer_c3d.cpp`'s cull enum moves from `GPU_CULL_FRONT_CCW` to
  `GPU_CULL_BACK_CCW` because the mirror is gone and the winding moved with it;
  the file header records that the mirror and the cull enum move together and that
  getting one right on its own shows up as INVISIBLE GEOMETRY. **This is UNVERIFIED
  on hardware** (AGENTS.md P6): no 3DS or emulator run was performed.
- Addressed review-22's MAJOR. The recorded death-beat divergence was false: the
  C# play tick calls `_scripts.Update(Step)` with no `Dead` guard
  (`Viewer.cs:509-512`), so the oracle advances scripts through the whole death
  beat, exactly as the C++ does. Verified against the oracle and rewritten in both
  `mdk_linux/src/main.cpp` and `docs/CONTINUATION.md`: the real divergences are
  that the C# skull fade is `HurtFlash/SkullFade` with `SkullFade = 2*Ticks = 60/s`
  and `SkullEnd = 255` (`Kurt.Combat.cs:26-27,238-253`) — at most ~4.25 s, less if
  `HurtFlash` starts partway up — and that on completion `Died` fires and
  `Game.cs:448-456` writes `SaveGames.LastGame` and leaves for the menu, whereas
  the C++ has no menu, no save and no level end and respawns in place after a fixed
  4.0 s.
- Fixed three latent bugs in the inherited harness scripts while getting them to
  run: glslangValidator needs `-D` (without it the HLSL `#include` is parsed as
  GLSL and fails) and an ATTACHED `-I<dir>`; the output names must be
  `<name>.ps.spv`, not `<name>.frag.spv`, or the csproj silently links the stale
  zero-byte placeholder shaders; and an apostrophe inside the inline single-quoted
  `bash -c` script for the Xvfb namespace closed the quote and re-split every
  following argument (`mount --bind "one" /tmp/.X11-unix`). The inner script is now
  a file written with a quoted heredoc. `cp -a` became `cp -R` because the working
  tree lives on a mount that cannot carry the source's mode.
- Verification on this tree:
  - `make -C tests/scripts clean && make -C tests/scripts` exited 0 and ended
    `All cases match the oracle.`
  - `bash tools/check.sh` exited 0 (`CHECK: ALL GATES PASSED`); SMDH SKIPPED (no
    CIA built) and `gates_frames` PASS with `L3_f1.bmp sha=2b586df212dc8ed8`.
  - `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C
    mdk_3ds` exited 0, linked `mdk.elf`, staged 69 RomFS files (79M), built
    `mdk.3dsx`.
  - `cmake --build build-linux --target mdk -j8` exited 0.
  - `tests/frames/regenerate_reference.sh` exited 0. Evidence: the oracle ran on
    display `:99` inside a private mount namespace on lavapipe/llvmpipe
    (`GPU: llvmpipe (LLVM 20.1.2, 256 bits) (Vulkan via SDL_GPU)`), reported
    `Level 3: 17 arenas, 9708 triangles, 705 outlines`, `Look: Original`,
    `render target 320x240, swapchain B8G8R8A8_UNORM`, and `Kurt at -4.00 0.00
    192.98 yaw 96`; the C++ capture reported `arena: HMO_1 (17 arenas)`,
    `view: 154 batches, 1188 vertices, 399 triangles` and the same Kurt position.
- No VM golden changed; opcode coverage is unchanged at 211 true dispatches / 238
  with 27 remaining. No emulator or Original 3DS run was performed and Old-3DS
  timing remains UNMEASURED. `/tmp/kilo` is not writable by this user, so captures
  went to `/tmp/mdk-frame-reference` and `/tmp/mdk-pi`, with the display recorded.

## What this batch did (Phase 2 iteration 22 — opcode 174 ammo condition)

- Ported opcode 174 (`if_ammo`) from
  `src/Mdk.Game/Scripts/ScriptVm.cs:999-1005` into the C++ VM. Kind 0 reads
  `Inventory.SuperChainGun`, kinds 1-5 read the corresponding
  `Inventory.Ammo` entry, and any other kind reads zero. The five ordinary
  comparison operators and their between/outside second bound use the same
  comparison path as opcode 175, including the oracle's unknown-operator
  false result.
- Extended `mdk::Kurt` with the super-chain-gun tick counter and five
  sniper-ammo entries from `Inventory.cs`. Their pickup writers and the
  chain-gun/sniper combat writers remain deferred with those subsystems, so
  the dispatch is classified partial rather than presented as complete.
- Added `IfAmmoReadsSuperChainGunAndSniperAmmo` to `tests/scripts/vm_tests.cpp`.
  It pins the fresh zero state, the distinct super-chain-gun source, all five
  ammo slots, the out-of-range kinds 6 and 255, strict/between/outside
  comparisons, the unknown-comparison false branch, fall-through, and the
  absence of opcode 174 from the unimplemented bucket. Opcode 174 does not
  fire in the level 3-8 corpus, so no golden changed.
- Recount-verified by brace-matching both dispatch methods: C# 238 cases, C++
  213 numeric labels / 212 unique / 211 true dispatches, 27 remaining. The
  nested `case 0`/`case 1` arms inside opcode 249's switch account for the
  extra raw label and duplicate. Classification is 16 partial, 195 complete.
- Review-21 had verdict COMPLETE and no BLOCKER or MAJOR finding. Its
  frontend MINOR findings and NITs are now explicit in
  `docs/CONTINUATION.md`: the post-camera solid-arena refresh matches the
  oracle's timing but pins one arena rather than its current-plus-active-
  second `SolidArenas` list; scripts continue during the 4-second death beat
  rather than ending with the level; and one `--seed` is reused for every
  respawn rather than drawing the C#'s fresh unseeded generator per life.
  Level 3 remains hardcoded in three frontend places and is also recorded.
- Code commits: `eb8c9f3` (`Port opcode 174 if_ammo`) and `6f68c52`
  (`Clarify opcode 174 and frontend caveats`).
- Verification on the final tree:
  - `make -C tests/scripts clean && make -C tests/scripts` exited 0 after 47
    VM tests and the complete oracle suite (`All cases match the oracle.`,
    354 golden cases).
  - `bash tools/check.sh` exited 0 (`CHECK: ALL GATES PASSED`); SMDH was
    skipped because no CIA was built.
  - `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C
    mdk_3ds -j8` exited 0, linked `mdk.elf`, staged 69 RomFS files (79M), and
    built `mdk.3dsx`.
  - `cmake --build build-linux --target mdk -j8` exited 0. A 120-frame
    `DISPLAY=:99` headless run with `--seed 1` exited 0 and reported the
    script assets/runtime ready.
  - `MDK_DATA_DIR=<repo>/MDK tests/scripts/build/tick1 probe 30000` exited 0
    with no unimplemented opcodes on levels 3-8.
- No emulator or Original 3DS run was performed; Old-3DS timing remains
  unmeasured. Every remaining opcode needs a deferred subsystem, so the next
  increment is the P5.0 frame-golden harness rather than a blind opcode port.

## What this batch did (Phase 2 iteration 21 — Linux frontend runtime integration)

- The Linux frontend now loads `LEVEL3.CMI` and the level sound bank, then
  constructs `ScriptRuntime` with the live `Player` and runs it after Kurt's
  1/60-second step and before the follow camera, matching `Viewer.cs:505-535`.
  The runtime is rebuilt on respawn, as the C# level reload does. This closes the missing frontend binding that kept 170, 219 and 220
  partial.
- `ScriptRuntime::update` now implements the C#'s own 1/30-second accumulator
  (`ScriptRuntime.cs:378-390`) rather than treating every call as one tick.
  The frontend can therefore call it at its 60 Hz simulation rate while the
  scripts run at 30 Hz. `FrontendTickCadenceCopiesLivePlayer` pins the first
  half-tick, the completed tick, the live Player copy and the next boundary.
- The follow camera now consumes opcode 203: `cameraTrackPitch` wins while
  `cameraTrackTicks` remains, otherwise the scripts' current arena supplies
  the pitch (`Viewer.cs:746-747`). The collision world's solid arena is
  refreshed after the camera step, matching the oracle's order. Opcode 203
  remains partial only for its save/load snapshot consumer.
- Ordinary runs use a fresh entropy seed for the ported legacy RNG and
  `--seed` requests a reproducible one. This is explicitly not the C#'s
  unseeded modern .NET generator; that generator remains unported. The
  runtime's combat/inventory `Kurt` also remains separate from movement
  `Player`, so health/items/object rendering and the 3DS frontend binding are
  not claimed complete.
- Addressed review-20's MINOR. The Makefile and prior progress narrative now
  state the correct mechanism: VFPv2 `VMLA.F32` is the fused, single-rounded
  MAC and GCC contracts to it even under `-ffp-contract=off`; only VFPv4
  `VFMA` is gated by that flag. Re-measured after this batch, the current
  `script_runtime.o` is byte-identical under default/off/fast with 161
  `vmla`/`vmls` and 0 `vfma`. ARM11 VMLA-versus-C# rounding remains UNMEASURED
  on hardware.
- Verification on the final tree:
  - `make -C tests/scripts clean && make -C tests/scripts` exited 0 after 46
    VM tests and the complete oracle suite (`All cases match the oracle.`,
    354 golden cases).
  - `bash tools/check.sh` exited 0 (`CHECK: ALL GATES PASSED`); SMDH was
    skipped because no CIA was built.
  - `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C
    mdk_3ds -j8` exited 0, linked `mdk.elf`, staged 69 RomFS files (79M), and
    built `mdk.3dsx`.
  - `cmake --build build-linux --target mdk -j8` exited 0.
  - `MDK_DATA_DIR=<repo>/MDK DISPLAY=:99 build-linux/mdk --headless --frames
    120 --seed 1` exited 0 and logged `scripts: runtime ready (seed 1)`.
    The capture could not be written under `/tmp/kilo` because that directory
    is owned by another user and is not writable by this harness; stdout was
    captured to `/tmp/mdk-frontend-runtime.log`.
  - `MDK_DATA_DIR=<repo>/MDK tests/scripts/build/tick1 probe 30000` exited 0
    with no unimplemented opcodes on levels 3-8.
- No golden changed: the frontend is not part of the oracle corpus and the
  script-tick change preserves the existing 1/30-second harness calls. No
  emulator or Original 3DS run was performed; Old-3DS timing remains
  unmeasured. Next isolated opcode family: 174 (`if_ammo`) over the runtime
  inventory seam, landing partial until the ammo pickup writers land.

## What this batch did (Phase 2 iteration 19 — opcode 219 state, oracle reproduction, FP-contraction measurement)

- Finished the opcode-219 batch that `91385f2` committed without its state
  files. `docs/OPCODE_COVERAGE.md` now records the gun-aim batch, removes 219
  from the remaining set, and classifies it partial: its lead and its fire roll
  read the observed `kurtVelocity` field, which no shipped frontend writes.
  Recount-verified by brace-matching both dispatch methods: C# 238 cases, C++
  212 numeric labels / 211 unique / 210 true dispatches, 28 remaining; the
  nested `case 0`/`case 1` arms inside opcode 249's switch account for the
  extra raw label and the duplicate. Classification: 18 partial, 192 complete.
- Reproduced every opcode-219 expectation against the C# oracle instead of
  trusting the commit message. A scratch driver (not committed) built the
  level-7 `ScriptRuntime` the way `/tmp/kilo/oracle/Driver` does, seeded
  `KurtPosition` from the DTI start (the C# sets it in `RunTick`; the C++
  constructor seeds it), placed the same gun/target geometry and called
  `ScriptRuntime.GunAim` for the same 21 seeds and scenarios. All 21 printed
  the same fired flag, yaw (4 dp) and following `Rng.Next(1000)` draw that
  `GunAimAimsAndFires` pins — including the 48.5-units-per-second case that
  separates `roundf` from a banker's round, and the four-object target cap.
  Recipe: copy `src/`, `assets/` and `Directory.Build.props` to a scratch tree
  (the repo's C# `obj/` is not writable by this user), copy
  `src/Mdk.Engine/obj/shaders` with it, then
  `DOTNET_ROOT=/tmp/kilo/dotnet10 NUGET_PACKAGES=/home/coleshores/.nuget/packages
  dotnet build -p:PrecompiledShaders=spirv`.
- Closed review-18's MINOR-1 (FP contraction) by measurement and reverted the
  `-ffp-contract=off` line `91385f2` had added to `mdk_3ds/Makefile`. With the
  shipped flags `script_runtime.cpp` compiles to a **byte-identical** object
  under the default, under `-ffp-contract=off` and under `-ffp-contract=fast`
  (164 `vmla`/`vmls`, 0 `vfma` in each). The later review-20 correction makes
  the mechanism explicit: VFPv2's `VMLA.F32` is the fused, single-rounded MAC
  form, and GCC's ARM backend contracts `a + b*c` to it even with contraction
  disabled; only the VFPv4 `VFMA` form is gated by `-ffp-contract`. The flag
  therefore changed nothing because the shipped object already contains
  contracted MACs, not because VFPv2 lacked fusion. Whether ARM11 `VMLA`'s
  rounding differs from the C# twice-rounded expression remains UNMEASURED on
  Old-3DS hardware; the Makefile comment points at the expression rather than
  the flag as the place to fix if a capture ever disagrees. Review-18's other
  five findings were applied in `91385f2` (the
  `ScriptRuntime.Snapshot.cs:37-39` reset note, the cutscene early-return
  caveat, the 2.67 s lead comment, the tick-prologue order, the redundant
  `isRemoved` scan inside the 100-pass loop).
- Verification on the final tree:
  - `make -C tests/scripts clean && make -C tests/scripts` exited 0 after 45
    VM tests (including `GunAimAimsAndFires`) and the complete oracle suite
    (`All cases match the oracle.`, 354 golden cases).
  - `bash tools/check.sh` exited 0 (`CHECK: ALL GATES PASSED`; SMDH SKIPPED,
    no `.cia` built).
  - `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C
    mdk_3ds` exited 0, recompiled `script_runtime.cpp` without the contraction
    flag, linked `mdk.elf`, staged 69 RomFS files (79M) and built `mdk.3dsx`.
  - `cmake --build build-linux --target mdk` exited 0.
  - `MDK_DATA_DIR=<repo>/MDK tests/scripts/build/tick1 probe 30000` exited 0
    with no unimplemented opcodes on levels 3-8.
- No golden changed: opcode 219 does not fire in the level 3-8 corpus, and no
  oracle behaviour was edited in this batch. No emulator or Original 3DS run
  was performed; Old-3DS timing remains unmeasured.

## What this batch did (Phase 2 iteration 18 — opcode 220 place near Kurt)

- Ported opcode 220 (`place_x_near_player`) from
  `src/Mdk.Game/Scripts/ScriptVm.cs:1121-1123` into
  `ScriptRuntime::placeNearKurt`, following
  `src/Mdk.Game/Scripts/ScriptRuntime.cs:1136-1186`. The first seeded RNG draw
  selects the random, current-X or predicted-X branch; only the random branch
  consumes a second draw. An X below the minimum wraps once by the range,
  then up to 100 passes move it twelve units left of every live same-arena,
  same-Y object. The C++ deque retains removed objects in stable slots, so
  the pass explicitly skips them to mirror the C# live `Objects` list.
- Added the C# `ScriptRuntime.KurtVelocity` field as the runtime's observed
  feet delta multiplied by 30. It is written before object scripts, matching
  `ScriptRuntime.cs:945-946`, and is distinct from the live `Kurt.Velocity`
  used by opcode 170; that accessor is now `kurtLiveVelocity()`.
- Added `PlaceNearKurtChoosesWrapsAndSpaces` to `tests/scripts/vm_tests.cpp`.
  It pins the observed-velocity first/moved ticks, all three X choices, the
  range and Y fallbacks, the one wrap, dead/cross-arena/different-Y/self
  exclusions, ordered spacing pushes and the following instruction. The
  seeded branch draws were checked against .NET's seeded `System.Random`.
  Opcode 220 does not fire in the level 3-8 corpus, so no golden changed.
- Review-17 (`logs/ralph-pi/run-20261027-223315/review-17.md`) had verdict
  COMPLETE and no BLOCKER, MAJOR or MINOR finding; its three NITs concerned
  unreachable camera-track divergences or missing branch-strength cases and
  required no change to this opcode batch.
- Recount-verified: C# 238 / C++ 211 numeric labels / 210 unique labels / 209
  true dispatches, with 29 remaining. Classification is 17 partial and 192
  complete. Code commit: `bf62628` (`Port opcode 220 place_x_near_player`).
- Verification on the final tree:
  - `make -C tests/scripts clean && make -C tests/scripts` exited 0 after 44
    VM tests and the complete oracle suite (`All cases match the oracle.`).
  - `bash tools/check.sh` exited 0 with all contract gates (`CHECK: ALL GATES
    PASSED`); SMDH was skipped because no CIA was built.
  - `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C
    mdk_3ds` exited 0, linked `mdk.elf`, staged 69 RomFS files (79M), and built
    `mdk.3dsx`.
  - `cmake --build build-linux --target mdk` exited 0 and built the Linux target.
  - `MDK_DATA_DIR=<repo>/MDK tests/scripts/build/tick1 probe 30000` exited 0
    with no unimplemented opcodes on levels 3-8.
- No emulator or Original 3DS run was performed. Old-3DS timing remains
  unmeasured. Next isolated opcode family: 219 (`if_gun_aim`) over the
  existing yaw/object-list seams and the new observed `KurtVelocity` field,
  with a seeded synthetic test for its aim/fire branches.

## What this batch did (Phase 2 iteration 17 — opcode 203 camera track; review-16 NIT)

- Ported opcode 203 (`camera_track`) from
  `src/Mdk.Game/Scripts/ScriptVm.cs:1124-1128` into
  `ScriptRuntime::cameraTrack`, following
  `src/Mdk.Game/Scripts/ScriptRuntime.cs:1188-1216`. Mode 0 uses 70% of the
  way to the model's bounds top; mode 1 uses height × scale above the object.
  The target falls as Kurt turns away, is clamped to 30 degrees up and the
  arena's rest pitch, and is stored for two ticks. The runtime's pitch and
  tick lifetime are new `cameraTrackPitch`/`cameraTrackTicks` fields.
- The runtime now copies a bound `Player`'s feet and yaw at tick start,
  matching `ScriptRuntime.cs:886-889`, so camera tracking reads the same live
  source as the C#. The renderer's follow-camera pitch consumer and the C#
  save/load snapshot's `camera_track_pitch`/`camera_track_ticks` fields remain
  deferred with those subsystems; no shipped frontend binds the Player yet,
  so the dispatch is partial.
- Added `CameraTrackCalculatesPitchAndExpires` to `tests/scripts/vm_tests.cpp`.
  It pins mode 0/model bounds, mode 1/height and scale, the 30-degree clamp,
  turn-away falloff, the behind fallback, live Player yaw, the following
  instruction, and the two-tick expiry. Opcode 203 does not fire in the level
  3-8 corpus, so no golden changed. The tick harness now initializes the
  shared trig LUT before the runtime can use `fastAtan2` on either target.
- Addressed review-16's NIT. The opcode-202 partial row and state files now
  record the C# save/load snapshot's `sky_mode` consumer as well as the
  renderer backdrop; the opcode-203 row names both deferred consumers.
- Recount-verified: C# 238 / C++ 210 numeric labels / 209 unique labels / 208
  true dispatches, with 30 remaining. Classification is 16 partial and 192
  complete. Code commit: `15dad88` (`Port opcode 203 camera_track`).
- Verification on the final tree:
  - `make -C tests/scripts clean && make -C tests/scripts` exited 0 after 43
    VM tests and the complete oracle suite (`All cases match the oracle.`).
  - `bash tools/check.sh` exited 0 with all contract gates (`CHECK: ALL GATES
    PASSED`); SMDH was skipped because no CIA was built.
  - `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C
    mdk_3ds` exited 0, linked `mdk.elf`, staged 69 RomFS files (79M), and built
    `mdk.3dsx`.
  - `cmake --build build-linux --target mdk` exited 0 and built the Linux target.
  - `MDK_DATA_DIR=<repo>/MDK tests/scripts/build/tick1 probe 30000` exited 0
    with no unimplemented opcodes on levels 3-8.
- No emulator or Original 3DS run was performed. Old-3DS timing remains
  unmeasured. Next isolated opcode family: 220 (`place_x_near_player`) over
  the existing RNG/object-list seams, with the C#'s observed `KurtVelocity`
  tick field and a seeded synthetic test.

## What this batch did (Phase 2 iteration 16 — opcode 202 sky mode; review-15 NIT)

- Ported opcode 202 (`set_574304`) from
  `src/Mdk.Game/Scripts/ScriptVm.cs:1613-1615`. Its signed-byte operand is now
  written verbatim to a new shared `ScriptRuntime::skyMode` field. The field
  starts at 0 and is level state, not per-tick state. The C# renderer's
  `SkyModes.BackdropOf` consumer (0 sky, 1 black, anything else keep-last-frame)
  is not ported, so the faithful dispatch is classified partial.
- Added `SetSkyModeWritesSharedLevelState` to `tests/scripts/vm_tests.cpp`.
  It pins the default, 0, 1, signed -1 and an arbitrary value, the following
  instruction's execution, and persistence across the next tick. Opcode 202
  does not fire in the level 3-8 corpus, so no golden changed. An
  unsigned-byte mutation made `vm_tests` exit 1 with exactly the
  `runtime.skyMode == -1` assertion failing; the source was restored before
  the final clean build.
- Addressed review-15's citation NIT. The opcode-170 move/drain/vertical-order
  comment in `player.cpp` and the coverage ledger now cite `Kurt.cs:221-234`,
  which includes the vertical update at line 234, rather than the short
  `221-224` range. Review-15 reported no BLOCKER, MAJOR or MINOR finding.
- Recount-verified: C# 238 / C++ 209 numeric labels / 208 unique labels / 207
  true dispatches, with 31 remaining. Classification is 15 partial and 192
  complete.
- Verification on the final tree:
  - `make -C tests/scripts clean && make -C tests/scripts` exited 0 after 42
    VM tests and the complete oracle suite (`All cases match the oracle.`).
  - `bash tools/check.sh` exited 0 with all contract gates (`CHECK: ALL GATES
    PASSED`); SMDH was skipped because no CIA was built.
  - `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C
    mdk_3ds` exited 0, linked `mdk.elf`, staged 69 RomFS files (79M), and built
    `mdk.3dsx`.
  - `cmake --build build-linux --target mdk` exited 0 and built the Linux target.
  - `MDK_DATA_DIR=<repo>/MDK tests/scripts/build/tick1 probe 30000` exited 0
    with no unimplemented opcodes on levels 3-8.
- No emulator or Original 3DS run was performed. Old-3DS timing remains
  unmeasured. Next isolated opcode family: 203 (`camera_track`) with a runtime
  camera-track state seam and a synthetic test; its renderer-side camera
  consumer remains deferred.

## What this batch did (review-13 correction — live Player source for opcode 170)

- Addressed review-13's MAJOR finding. The previous record said Kurt's
  movement was not ported, but it already exists as `mdk::Player` and is live
  in the Linux frontend. The real gap is that no shipped frontend constructs
  `ScriptRuntime`, so the runtime has no integrated Player binding. The copied
  `kurtVelocity` / `setKurtVelocity` snapshot has been removed.
- `ScriptRuntime` now borrows the same `Player` at construction and reads
  `Player::velocity()` live when opcode 170 dispatches. The source is in u/s
  and is distinct from the C# `ScriptRuntime.KurtVelocity` field, which is the
  observed position delta multiplied by 30. The runtime's position snapshot
  remains part of the larger frontend integration and is not claimed complete.
- Added `Player::velocity()` from `Kurt.cs:132-133`, including the horizontal
  knock-down-push term. The push state, additive writer and per-step drain
  follow `Kurt.Combat.cs`; the knock-down state machine and its producer still
  land with combat. This removes the fidelity hazard where a future Player
  binding would silently omit the C# `_push` term.
- Extended `InheritKurtVelocityAddsScaledKurtVelocity` in
  `tests/scripts/vm_tests.cpp`. It still pins the dispatch, literal scaling on
  all three components, preservation of the receiver velocity, a negative
  scale and fall-through. It now also changes the Player after runtime
  construction (rejecting a cached snapshot) and pins the knock-down-push
  term and its drain. Opcode 170 does not fire in the level 3-8 corpus, so no
  golden changed.
- Corrected the unit and ownership descriptions in `docs/OPCODE_COVERAGE.md`
  and `docs/CONTINUATION.md`: frontend integration must wire
  `Player -> ScriptRuntime`; it must not re-port Kurt movement.
- Coverage is unchanged: 206 true C++ dispatches / 238, 32 remaining.
- Verification on this tree:
  - `make -C tests/scripts clean && make -C tests/scripts` exited 0 after 41
    VM tests and the complete oracle suite (`All cases match the oracle.`).
  - `bash tools/check.sh` exited 0 with all contract gates (`CHECK: ALL GATES
    PASSED`); SMDH was skipped because no CIA was built.
  - `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C
    mdk_3ds` exited 0, linked `mdk.elf`, staged 69 RomFS files (79M), and built
    `mdk.3dsx`.
  - `cmake --build build-linux --target mdk` exited 0 and built the Linux target.
  - `MDK_DATA_DIR=<repo>/MDK tests/scripts/build/tick1 probe 30000` exited 0
    and reported no unimplemented opcodes on levels 3-8.
- No emulator or hardware run was performed. Old-3DS timing remains
  unmeasured. Next isolated opcode family: 202 (`set_574304`) with a runtime
  sky-mode seam and a synthetic test; its renderer consumer remains deferred.

## What this batch did (opcode 170 — inherit Kurt velocity; review-12 fix)

- Ported opcode 170 (`inherit_kurt_vel`) from
  `src/Mdk.Game/Scripts/ScriptVm.cs:1603-1604`. The receiver's velocity gains
  each component of the C# `Kurt.Velocity` source multiplied by the opcode's
  F32 operand. This first implementation used an explicit copied
  `kurtVelocity` / `setKurtVelocity` seam for that source. Review-13 correctly
  found that its record was wrong: Kurt movement was already ported as
  `mdk::Player`. The review-13 correction above replaces the copy with a live
  `Player` source and records the real remaining frontend integration gap.
- Added `InheritKurtVelocityAddsScaledKurtVelocity` to
  `tests/scripts/vm_tests.cpp`. It pins the dispatch, literal scaling on all
  three components, preservation of the receiver's existing velocity, a
  negative scale (rejecting an absolute-value mutant), and fall-through to
  the next instruction. Opcode 170 does not fire in the level 3-8 corpus, so
  no golden changed.
- Addressed review-12's sole MINOR finding. The opcode-135 test now follows
  its NaN request with a finite request and pins that
  `MathF.Max(NaN, finite)` stays NaN. Mutation-checking the runtime
  `mathMax` first arm to return the finite new value makes `vm_tests` exit 1
  with exactly that assertion failing. Review-12 found no BLOCKER or MAJOR
  finding. Its cutscene note remains a constraint on the future cutscene
  slice: the alarm decrement and screen-shake clear must move after the
  oracle's cutscene early return.
- Coverage: 206 true C++ dispatches / 238, 32 remaining. The brace-matched
  recount gives C# 238 cases and C++ 208 numeric labels / 207 unique labels;
  opcode 249's nested `case 0` and `case 1` arms account for the extra raw
  label and the duplicate.
- Verification on this tree:
  - `make -C tests/scripts` exited 0 after 41 VM tests and the complete oracle
    suite (`All cases match the oracle.`).
  - `bash tools/check.sh` exited 0 with all contract gates (`CHECK: ALL GATES
    PASSED`); SMDH was skipped because no CIA was built.
  - `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C
    mdk_3ds` exited 0, linked `mdk.elf`, staged 69 RomFS files (79M), and built
    `mdk.3dsx`.
  - `cmake --build build-linux --target mdk` exited 0 and built the Linux target.
  - `MDK_DATA_DIR=<repo>/MDK tests/scripts/build/tick1 probe 30000` exited 0
    and reported no unimplemented opcodes on levels 3-8.
- No golden changed. No emulator or hardware run was performed. Old-3DS timing
  remains unmeasured. Next: opcode 202 (`set_574304`) with a runtime sky-mode
  seam and a synthetic test; its renderer consumer remains deferred.

## What this batch did (iteration 68 — opcode 135, screen shake; review-11 fixes)

- Ported opcode 135 (`raise_573aa8`) from `ScriptVm.cs:1581-1582`. The VM
  forwards the F32 operand to `ScriptRuntime::raiseShake`, which keeps the
  strongest request for the current tick with the oracle's NaN-propagating
  `MathF.Max` semantics. `ScriptRuntime::runTick` clears the value at the start
  of each tick, matching `ScriptRuntime.cs:899`. The C# `ShakeRaised` event's
  follow-camera consumer is not ported, so the dispatch is classified partial.
- Added `RaiseShakeKeepsTheTickMaximum` to `tests/scripts/vm_tests.cpp`. It
  pins the dispatch and operand alignment, the 2.5/1.0/4.0 maximum sequence,
  the negative request, NaN propagation, and the next-tick reset. The opcode
  does not fire in the level 3-8 corpus, so no golden changed.
- Addressed review-11's three MINOR findings. The opcode-195 test now includes
  `hit_type = 7` against operand `-7` and `hit_type = -3` against operand `3`;
  an absolute-value mutant now fails two assertions and exits 1 (previously it
  exited 0). The ledger records that `ScriptRuntime::hitGroup` is ported but
  uncalled, lists its C# group-hit call sites, and adds Kurt's
  `HitTypeKurt = -11` touch. The Complete/Partial rule was made consistent:
  an unwritten input or missing consumer makes a faithful dispatch partial, so
  81, 135 and 195 join 165/166 and the other consumer-gated cases. Current
  classification is 13 partial and 192 complete of 205 true dispatches.
- Review-11 found no BLOCKER or MAJOR. Its PASS-print NIT is pre-existing
  harness behavior; this batch's verification therefore cites exit codes rather
  than treating a `PASS` line as evidence. Its DANT_8 coordinate NIT remains
  unproven and was not changed in this opcode slice.
- Coverage: 205 true C++ dispatches / 238, 33 remaining. The brace-matched
  recount gives C# 238 cases and C++ 207 numeric labels / 206 unique labels;
  the nested opcode-249 switch accounts for the extra label and the duplicate.
- Verification on this tree:
  - `make -C tests/scripts clean && make -C tests/scripts` exited 0 after 40
    VM tests and the complete oracle suite (`All cases match the oracle.`).
  - `bash tools/check.sh` exited 0 with `CHECK: ALL GATES PASSED`; SMDH was
    skipped because no CIA was built.
  - `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C
    mdk_3ds` exited 0, linked `mdk.elf`, staged 69 RomFS files (79M), and built
    `mdk.3dsx`.
  - `cmake --build build-linux --target mdk` exited 0 and built `mdk`.
  - `MDK_DATA_DIR=<repo>/MDK tests/scripts/build/tick1 probe 30000` exited 0
    and reported no unimplemented opcodes on levels 3-8.
  - In a detached worktree, replacing opcode 195's signed comparison with an
    absolute-value comparison made `vm_tests` exit 1 with exactly the two
    sign-crossing failures; the worktree was removed afterward.
- No golden changed. No emulator or hardware run was performed. Old-3DS timing
  remains unmeasured.

## What this batch did (iteration 67 — opcode 195, hit-weapon condition)

- Ported opcode 195 (`if_hit_weapon`) from
  `src/Mdk.Game/Scripts/ScriptVm.cs:1526-1527`. The receiver compares its
  existing `MdkObject::hit_type` with the signed-byte operand and branches on
  exact equality. This is a real dispatch over existing object state, not a
  decoded no-op or a stub. The C++ group-hit scratch writer is ported in
  `ScriptRuntime::hitGroup`, but `hitGroup` has no C++ caller. The C# group-hit
  call sites (Kurt's `HitTypeKurt = -11` touch, the chain gun and `HitGroupAt`,
  the sniper rounds and Items) and the direct object-hit writers for mortars,
  Items, sniper rounds and the cow remain deferred with those subsystems.
- Added `IfHitWeaponBranchesOnExactHitType` to `tests/scripts/vm_tests.cpp`.
  It pins a negative grenade match and bomb mismatch, a positive projectile
  match and mismatch, and the oracle's initial zero state. Replacing the
  equality with false makes exactly three assertions fail; restoring it makes
  the VM suite pass.
- Addressed review-10's only NIT: the prior raycast test comment asserted an
  exact DANT_8 face coordinate that depends on the BSP versus brute-force
  geometry path. It now says "about 4.05". Review-10 found no BLOCKER, MAJOR,
  or MINOR finding.
- Coverage: 204 true C++ dispatches / 238, 34 remaining. The brace-matched
  recount gives C# 238 cases and C++ 206 numeric labels / 205 unique labels;
  the nested opcode-249 switch accounts for the extra label and the duplicate.
- Verification on this tree:
  - `make -C tests/scripts clean && make -C tests/scripts build/vm_tests` then
    `make -C tests/scripts` exited 0 after 39 VM tests and the complete oracle
    suite (`All cases match the oracle.`).
  - `bash tools/check.sh` passed all contract gates (`CHECK: ALL GATES
    PASSED`); SMDH was skipped because no CIA was built.
  - `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C
    mdk_3ds` linked `mdk.elf`, staged 69 RomFS files (79M), and built
    `mdk.3dsx`.
  - `cmake --build build-linux --target mdk` built the Linux target.
  - `MDK_DATA_DIR=<repo>/MDK tests/scripts/build/tick1 probe 30000` reported
    no unimplemented opcodes on levels 3–8.
- No golden changed; opcode 195 does not fire in the level 3–8 corpus. No
  emulator or hardware run was performed. Old-3DS timing remains unmeasured.


## What this batch did (iteration 66 — current-plus-second raycast seam)

- Closed the synthetic raycast coverage gap recorded by review-8 and repeated
  as review-9's UNPROVEN observation. The C# `Raycast` selects the current
  arena plus the second arena whenever a current arena exists, and otherwise
  falls back to every arena. Earlier opcode 121/192 tests exercised only that
  fallback.
- Added `RaycastUsesCurrentAndSecondArena` to `tests/scripts/vm_tests.cpp`.
  It uses level 7's overlapping `DANT_1` and `DANT_8` geometry: after one
  runtime tick establishes `DANT_1` as current, `showArena("DANT_8")` adds
  the second arena. The case pins a hit unique to the current arena, a hit
  unique to the second arena, and both directions of the nearest-hit contest.
  Each assertion covers the selected hit point and the selected triangle's
  geometric normal, so a point from one arena cannot be paired with a normal
  from another.
- The test was mutation-checked: removing `tryArena(secondArena_)` from
  `ScriptRuntime::raycastPoint` makes eight assertions fail (the second-only
  hit and the second arena's winning point/normal), and restoring the arm
  makes the suite pass. No production code changed in this batch.
- Review-9 found no BLOCKER, MAJOR, or MINOR finding. Its only observation was
  the coverage gap closed here. Coverage remains 203 true C++ dispatches / 238,
  with 35 remaining.
- Verification on this tree:
  - `make -C tests/scripts clean && make -C tests/scripts` passed 38 VM tests
    and the complete oracle suite (`All cases match the oracle.`).
  - `bash tools/check.sh` passed all contract gates (`CHECK: ALL GATES
    PASSED`); SMDH was skipped because no CIA was built.
  - `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C
    mdk_3ds` linked `mdk.elf`, staged 69 RomFS files (79M), and built
    `mdk.3dsx`.
  - `cmake --build build-linux --target mdk` built the Linux target.
  - `MDK_DATA_DIR=<repo>/MDK tests/scripts/build/tick1 probe 30000` reported
    no unimplemented opcodes on levels 3–8.
- No golden changed. No emulator or hardware run was performed. Old-3DS timing
  remains unmeasured.


## What this batch did (iteration 65 — opcode 192, no flat floor)

- Ported opcode 192 (`if_no_flat_floor_at`) from
  `src/Mdk.Game/Scripts/ScriptVm.cs:1696-1701`. The receiver rotates its
  forward/side operand by its yaw, raises the resulting point one unit, drops
  the third operand plus one unit, and branches when the ray misses or the
  absolute Z of the hit triangle's normal is below the fourth operand. The
  implementation preserves the oracle's left-associated degree conversion,
  rotation expression, short-circuit on a miss, and comparison.
- Extended `ScriptRuntime::raycastPoint` to carry the selected triangle's
  geometric normal instead of approximating it. The normal is computed from the
  same triangle returned by the nearest-hit walk, using the oracle's
  `cross(b-a,c-a)` normalization and +Z fallback for a degenerate face. The
  existing `rayIn` seam now shares that helper, so the debris and script ray
  paths cannot drift in their normal calculation.
- One synthetic `vm_tests` case, `NoFlatFloorAtUsesRotatedPointAndHitNormal`,
  pins the dispatch against level 7's real geometry: a flat floor hit is false,
  a zero drop and a far point are true, the probe's one-unit clearance is
  exercised from just below the floor, DANT_1's sloping face is true at a 0.5
  limit and false at 0.25, and a yaw-90 forward offset must land on that face
  rather than east of it. The pair of threshold cases pins the normal operand
  and comparison direction; the rotation case also rejects swapped forward/side
  operands.
- Review-8 found no BLOCKER or MAJOR. Its MINOR records a pre-existing
  synthetic-coverage gap: the raycast tests exercise the all-arenas fallback,
  not the current-plus-second arena pair. That seam test is now the next slice.
  Its NIT concerns attribution of the prior floor-below test's drop-distance pin
  and requires no code change.
- Coverage: 203 true C++ dispatches / 238, 35 remaining. The brace-matched
  recount gives C# 238 cases and C++ 205 numeric labels / 204 unique labels;
  the extra numeric label is the nested mode arm inside opcode 249, leaving
  203 true dispatches.
- Verification on this tree:
  - `make -C tests/scripts clean && make -C tests/scripts` passed 37 VM tests
    and the complete oracle suite (`All cases match the oracle.`).
  - `bash tools/check.sh` passed all contract gates (`CHECK: ALL GATES
    PASSED`); SMDH was skipped because no CIA was built.
  - `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C
    mdk_3ds` linked `mdk.elf`, staged 69 RomFS files (79M), and built
    `mdk.3dsx`.
  - `cmake --build build-linux --target mdk` built the Linux target.
  - `MDK_DATA_DIR=<repo>/MDK tests/scripts/build/tick1 probe 30000` reported
    no unimplemented opcodes on levels 3–8.
- No golden changed; opcode 192 does not fire in the level 3–8 corpus. No
  emulator or hardware run was performed. Old-3DS timing remains unmeasured.


## What this batch did (iteration 64 — opcode 121, floor below)

- Ported opcode 121 (`if_floor_below`) from
  `src/Mdk.Game/Scripts/ScriptVm.cs:1319-1320`. The receiver drops a segment
  of the operand's length from its position and branches when the segment
  crosses any triangle of the oracle's active ray arenas. The implementation
  uses the existing `ScriptRuntime::raycastPoint` seam, whose nearest-hit and
  active-arena semantics match the C# `Raycast`; no normal or arena-specific
  floor-only approximation is introduced.
- One synthetic `vm_tests` case, `FloorBelowBranchesOnARayHit`, pins the
  dispatch, the blocked and clear branches, and the operand's role as the
  drop distance. It uses the level-7 floor five below the level start: a
  ten-unit drop crosses it, while a four-unit drop stops above it. The test
  was mutation-checked: inverting the hit result, reversing the ray, and
  replacing the operand with a fixed ten-unit drop each make the test fail.
- Addressed review-7's comment NIT. Movement command 229 still uses the
  oracle's `Parameter * 0.5f` condition, but its source and test comments now
  call that half of the recorded flight time rather than an apex. No behavior
  changed.
- Coverage: 202 true C++ dispatches / 238, 36 remaining. The brace-matched
  recount gives C# 238 cases and C++ 204 numeric labels / 203 unique labels;
  the extra numeric label is the nested mode arm inside opcode 249, leaving
  202 true dispatches.
- Verification on this tree:
  - `make -C tests/scripts clean && make -C tests/scripts` passed 36 VM tests
    and the complete oracle suite (`All cases match the oracle.`).
  - `bash tools/check.sh` passed all contract gates (`CHECK: ALL GATES
    PASSED`); SMDH was skipped because no CIA was built.
  - `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C
    mdk_3ds` linked `mdk.elf`, staged 69 RomFS files (79M), and built
    `mdk.3dsx`.
  - `cmake --build build-linux --target mdk` built the Linux target.
- No golden changed; opcode 121 does not fire in the level 3–8 corpus. No
  emulator or hardware run was performed. Old-3DS timing remains unmeasured.


## What this batch did (iteration 63 — opcode 229, turn and jump)

- Ported opcode 229 (`turn_and_jump_to_dest`) from
  `ScriptVm.cs:1136-1138` and its movement command from
  `ObjectMotion.cs:387-391/426-461`. The receiver first turns toward its move
  destination by at most the requested degrees per second. Once it faces the
  destination, the implementation sets the oracle's yaw, enters movement
  command 229, chooses the arc top (at least ten above the higher end and
  thirty above the lower end), derives the initial vertical speed and total
  flight time from gravity, and sets the horizontal speed so that linear
  friction still carries the object to the destination. The flight command
  advances `parameter_timer` and ends with zeroed velocity on floor contact
  after the apex; gravity still acts later in the same motion tick, matching
  the oracle's update order.
- The implementation reuses the existing `move_destination`, `yaw`,
  `velocity`, `gravity`, `friction`, `parameter`, `parameter_timer` and
  `contact_flags` fields. `ScriptRuntime::turnAndJump` preserves the C#'s
  operation order and NaN-propagating `MathF.Max`; the existing `yawTo` seam
  supplies the oracle's compass bearing. This is a real dispatch and movement
  command, not a decoded no-op or a stub.
- One synthetic `vm_tests` case,
  `TurnAndJumpTurnsLaunchesAndStopsOnFloor`, pins the two-degree-per-tick
  turn limit, the false branch while turning, the exact launch yaw and true
  branch, the vertical speed, friction-adjusted horizontal speed and flight
  time, the first gravity/friction motion tick, the exclusion of floor
  contact before the apex, and the landing stop. The test was
  mutation-checked: doubling the turn rate, shortening the arc, dropping the
  friction compensation, allowing a landing before the apex, and removing the
  landing stop each make the test fail.
- The newest external review file,
  `logs/ralph-pi/run-20261027-223315/review-6.md`, contains only
  `REVIEWER FAILED model=litellm/qwen rc=1`; it supplied no BLOCKER, MAJOR,
  MINOR or NIT finding to address. Review-4's remaining documentation point
  was already fixed in iteration 61.
- Coverage: 201 true C++ dispatches / 238, 37 remaining. The brace-matched
  recount gives C# 238 cases and C++ 203 numeric labels / 202 unique labels;
  the extra numeric label is the nested mode arm inside opcode 249, and one
  nested label is duplicated, leaving 201 true dispatches.
- Verification on this tree:
  - `make -C tests/scripts clean && make -C tests/scripts` → 35 VM tests PASS
    and the full oracle suite ended with `All cases match the oracle.`
  - `bash tools/check.sh` → `CHECK: ALL GATES PASSED`; SMDH SKIPPED (no CIA
    built).
  - `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C
    mdk_3ds` → linked `mdk.elf`, staged 69 RomFS files (79M), built
    `mdk.3dsx`.
  - `cmake --build build-linux --target mdk` → built the Linux target.
- No golden changed; opcode 229 does not fire in the level 3–8 corpus. No
  emulator or hardware run was performed. Old-3DS timing remains unmeasured.


## What this batch did (iteration 62 — opcode 204, object avoidance)

- Ported opcode 204 (`avoid_objects`) from `ScriptVm.cs:1715-1729`. The
  receiver computes its world bounds once, walks the live same-arena objects
  in stable list order, and turns away from the first one whose strict world
  bounds overlap its own. Dead objects, objects flagged `FlagNotSolid` or
  `FlagNotSolid2`, and cross-arena objects are skipped. The turn is the
  oracle's wrapped `YawTo(other) + 180 - yaw`, clamped to half a turn times
  one tick, and the scan stops after the first eligible overlap.
- The implementation reuses the existing `ScriptRuntime::worldBounds`,
  `objects`, and `yawTo` seams. The local box intersection is strict on every
  axis, matching `ScriptMath.Intersects`: boxes that merely touch do not
  overlap. This is a real dispatch, not a decoded no-op or a stub.
- One synthetic `vm_tests` case, `AvoidObjectsTurnsAwayFromTheFirstSolidOverlap`,
  pins the dispatch and the self/dead/cross-arena/non-solid/touching
  exclusions, both turn directions, stable first-object order, and the clamp
  to the available angle. It also pins the six-degree-per-tick limit. The
  test was mutation-checked: accepting dead objects, accepting non-solid
  objects, making the bounds inclusive, continuing after the first overlap,
  and replacing the available-angle clamp with a fixed step each make the
  test fail.
- The newest external review file, `logs/ralph-pi/run-20261027-223315/review-5.md`,
  contains only `REVIEWER FAILED model=litellm/qwen rc=1`; it supplied no
  BLOCKER, MAJOR, MINOR, or NIT finding to address. Review-4's remaining
  documentation point was already fixed in iteration 61.
- Coverage: 200 true C++ dispatches / 238, 38 remaining. The brace-matched
  recount gives C# 238 cases and C++ 202 numeric labels / 201 unique labels;
  the extra numeric label is the nested mode arm inside opcode 249, and one
  nested label is duplicated, leaving 200 true dispatches.
- Verification on this tree:
  - `make -C tests/scripts clean && make -C tests/scripts` → 34 VM tests PASS
    and the full oracle suite ended with `All cases match the oracle.`
  - `bash tools/check.sh` → `CHECK: ALL GATES PASSED`; SMDH SKIPPED (no CIA
    built).
  - `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C
    mdk_3ds` → linked `mdk.elf`, staged 69 RomFS files (79M), built
    `mdk.3dsx`.
  - `cmake --build build-linux --target mdk` → built the Linux target.
- No golden changed; opcode 204 does not fire in the level 3–8 corpus. No
  emulator or hardware run was performed. Old-3DS timing remains unmeasured.


## What this batch did (iteration 61 — the alarm state: movement command 15 and opcode 27)

- Ported the oracle's alarm state machine. `ScriptRuntime` now has the shared
  `alarmTicks` countdown: it is decremented once at the start of every
  non-cutscene tick (`ScriptRuntime.cs:896`), movement command 15 sets it to
  ten for an alarm object in Kurt's current arena
  (`ObjectMotion.cs:374-385`), and opcode 27 (`if_alarm`) branches while it
  is positive (`ScriptVm.cs:1692-1693`). Command 15 also plays `ALERT` at
  the object's position when `TickCount & 31` is zero, preserving the
  oracle's every-32-ticks cadence and its `Mixer.PlayAt` placement at the
  object's position.
- The current-arena guard, the countdown value, its exact ten-tick lifetime,
  the true and false `if_alarm` branches, and the sound cadence are all
  pinned by one new synthetic `vm_tests` case,
  `AlarmSoundsForTenTicksAndPlaysEveryThirtyTwo`. It drives the public motion
  seam with a level-7 runtime, uses a fresh probe for the false branch so a
  stale variable cannot make the assertion pass, and reaches `TickCount` 32
  to distinguish the 32-tick sound from a sound on every tick.
- The C#'s Rides subsystem is the other oracle writer of this countdown
  (`Rides.cs:219,224`). Rides is still unported with its player/vehicle
  slice, so the riding vehicle's alarm source remains deferred; the shared
  countdown, command-15 producer, and condition are present and tested.
- Addressed review-4's MINOR documentation finding. The old handoff text
  called `release_children` (49) the one place the oracle clears an inbound
  `leader`, but command 30 clears one in two places as well. Both the
  coverage document and the iteration-60 narrative now say that 49 is the
  only OPCODE that clears one and that the chain motion's own let-go
  (`ObjectMotion.cs:470/480`) is the only other place.
- Coverage: 199 true C++ dispatches / 238, 39 remaining (recounted with the
  brace-matched method spans; the raw C++ label count is 201 with one nested
  switch arm and one duplicate label in it). Opcode 15 leaves the partial
  table; opcodes 15 and 27 are now in the complete set.
- Verification on this tree:
  - `make -C tests/scripts clean && make -C tests/scripts` → 33 VM tests PASS
    and the full oracle suite ended with `All cases match the oracle.`
  - `bash tools/check.sh` → `CHECK: ALL GATES PASSED`; SMDH SKIPPED (no CIA
    built).
  - `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C
    mdk_3ds` → linked `mdk.elf`, staged 69 RomFS files (79M), built
    `mdk.3dsx`.
  - `cmake --build build-linux --target mdk` → built the Linux target.
- No golden changed; no emulator or hardware run was performed. Old-3DS timing
  remains unmeasured.


## What this batch did (iteration 60 — the chain-link family: opcodes 29/49 and movement command 30)

- Ported the chain that hangs off an object: opcode 29 (`spawn_chain`), opcode
  49 (`release_children`) and movement command 30 (the chain's own motion,
  `ObjectMotion.cs:464`), which had no C++ counterpart. The oracle's
  `LastFollowerId` (`ScriptVm.cs:273`) and `LastChainLink` (`ScriptVm.cs:288`)
  are now `ScriptRuntime::lastFollowerId` and `ScriptRuntime::lastChainLink`,
  and `ScriptRuntime::updateChain` ports `UpdateChain` with the C#'s `Links`
  list (the live followers, ordered by instance id, in the reused member list
  the C# keeps in a field).
- The details that matter: the links are numbered after the highest instance
  id of the object's LIVE followers (a removed follower does not count, so its
  id is reused), and the oracle's loop counts down, so the FIRST link spawned
  has the HIGHEST id and the last one has id 0 — the link that drives the
  chain. The chain's motion hangs each link's reference point 0 at the
  previous link's reference point 1 and turns the links 90 degrees apart; only
  the link with id 0 drives the chain, a gap in the chain makes the later
  links let go, and a dead leader makes them let go too. `release_children`
  lets go of the HIGHEST-id chain link (same arena, movement command 30) per
  count and branches TRUE when the chain is empty. It is the only OPCODE that
  clears an inbound `leader`, and it clears exactly the links it releases; the
  chain motion's own let-go (`ObjectMotion.cs:470/480`) is the only other
  place the oracle clears one.
- Three `vm_tests` tests pin it: `SpawnChainNumbersItsLinks` (the numbering,
  the live/dead follower rule, the per-leader numbering),
  `ChainLinksHangOffTheirLeader` (the assembly, the 90-degree turns, that
  only the id-0 link drives the chain, the broken chain, the dead leader) and
  `ReleaseChildrenLetsGoOfTheChain` (the release order and the branch when
  the chain runs out). Mutation-checked: breaking the follower-id scan, the
  yaw, the release order, the gap check, the dead-leader check or the
  "only id 0 drives" rule each fails a check (the first pass of the last one
  was vacuous — re-assembling an assembled chain is a no-op — so the test now
  drives a link BEFORE the chain is assembled).
- Addressed review-3's NIT: the `link_partners` comment no longer claims that
  180/213/214/246 depend on the retained dead link — they read the field but
  reject a dead partner exactly as they reject a null one; variable source 3
  and getFlags/setFlags are what read the retained pointer.
- Recorded review-3's open question in the coverage doc: the chain-command
  readers (`Links`, `LastFollowerId`, `LastChainLink`) compare against the
  RETAINED leader pointer, so nothing may clear a link's `leader` except
  `release_children` (49) and the chain motion's own let-go.
- Coverage: 198 true C++ dispatches / 238, 40 remaining (recounted with the
  brace-matched method spans; the raw C++ label count is 200 with one nested
  switch arm and one duplicate label in it).
- The known pre-existing gap in the shared `spawn()` helper — the C#'s
  `Stats.CountEnemy(type, Created)` call is missing — applies equally to
  opcode 29 and to every other spawn opcode (86/149/156/179/206); it was
  already recorded (progress log, iterations 30 and 57) and the level
  goldens do not observe those counters. Left untouched here so the batch
  stays one subsystem.
- Verification on this tree:
  - `make -C tests/scripts clean && make -C tests/scripts` → 32 VM tests PASS
    and the full oracle suite ended with `All cases match the oracle.`
  - `bash tools/check.sh` → `CHECK: ALL GATES PASSED`; SMDH SKIPPED (no CIA
    built).
  - `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C
    mdk_3ds` → linked `mdk.elf`, staged 69 RomFS files (79M), built
    `mdk.3dsx`.
  - `cmake --build build-linux --target mdk` → built the Linux target.
- No golden changed; no emulator or hardware run was performed. Old-3DS timing
  remains unmeasured.


## What this batch did (iteration 59 — the oracle's Remove clears no inbound reference)

- Addressed the newest external review's two MINOR findings and two NITs on the
  partner/removal semantics. The review found that `ScriptRuntime::remove`
  nulled inbound `leader`, `attached` and `cow_target` pointers, while the
  oracle's `Remove` (`src/Mdk.Game/Scripts/ScriptRuntime.cs:1648-1673`) clears
  no inbound reference at all. The review could not prove the `attached` clear
  safe — the oracle's `Attached` read sites test only for null
  (`ObjectBehaviors.cs:238,267`), so nulling is NOT equivalent there — and
  asked for the divergence to be removed rather than justified.
- `ScriptRuntime::remove` now clears nothing, matching the oracle: a widow
  keeps its dead partner, a removed chute keeps its leader, a pickup keeps a
  chute removed under it. Every C++ read site was checked against its oracle
  twin — `leader` (`followAttachment`, `mayCommand`, opcode 34, command
  selector 7/8), `attached` (the pickup fall/shrink paths) and `cow_target`
  (no C++ read yet; the oracle's is `is { Dead: false }`) — and each reads a
  retained dead pointer the same way the oracle's retained managed reference
  reads. The removed object's deque slot is permanent, so the retained pointer
  stays valid; the object-list loops skip removed objects by `dead`, which
  `remove` alone sets.
- Corrected the header comment that claimed `kill` leaves an object "dead but
  PRESENT": `kill` leaves the object present with `health == 0` and
  `dead == false`; `dead` is written in exactly one place in each language —
  `remove`, immediately before the object leaves the list.
- Corrected the `link_partners` comment: admitting a dead link to the loose
  list is unobservable for the pairing itself (a widow is skipped as `i` and
  as `j`, since both loops skip every non-null link), so the comment no longer
  claims the two steps matter. The clause is kept verbatim because it IS the
  oracle's filter, and the retained link it admits is what the other `linked`
  consumers (variable source 3, 180, 213/214, 246) read.
- The `vm_tests` link_partners fixture no longer fabricates any state: the
  widow's pair is built by a real `link_partners` run (the only two BEAMS in
  the arena), the cross-type already-linked pair by `spawn_partner` (which
  links its receiver to the spawned partner whatever the type), and the
  keep/preserve links by `spawn_partner` as well; the redundant hand-set
  re-links in the clear section were deleted because the pair program above
  already produced that state.
- Added `RemoveKeepsInboundReferences`: `attach_to` (74) makes one object the
  leader of another, the leader is removed, and the follower still points at
  the dead leader (the oracle's field state), while `if_no_leader` (34) still
  reads it as no leader and clears the move command. Mutation-checked: putting
  the inbound clears back fails this case (`follower->leader == leader`), and
  putting the `linked` clear back fails 11 checks in the link_partners case.
- No opcode count changed: 196 true dispatches / 238, 42 remaining.
- Verification: `make -C tests/scripts clean && make -C tests/scripts` passed
  29 VM tests and the complete oracle suite (`All cases match the oracle.`);
  `bash tools/check.sh` passed all contract gates (SMDH skipped, no CIA);
  `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds`
  built `mdk.3dsx` after staging 69 RomFS files (79M), and `cmake --build
  build-linux --target mdk` built the Linux target. No golden changed; no
  emulator/hardware run. Old-3DS timing remains unmeasured.

## What this batch did (iteration 58 — preserve dead partner links)

- Addressed the newest external review's MAJOR and MINOR findings on the
  partner-link family. The C# `ScriptRuntime.Remove` marks the object dead and
  removes it from `Objects`, but deliberately leaves inbound `Linked`
  references intact. The C++ runtime had normalized those references to null,
  making a surviving partner look unlinked; `link_partners` then re-paired it,
  unlike the oracle.
- `ScriptRuntime::remove` now preserves the dead partner pointer. This is
  safe because `std::deque` object slots are stable and removed slots remain
  allocated, matching the managed object's continued readability in C#. The
  existing live-partner guards on opcodes 180/213/214/246 and command
  selector 9 remain in force; variable source 3 deliberately reaches the
  removed partner's persistent variables, as in the oracle.
- Rebuilt the opcode-228 synthetic fixture around a real pair and a real
  `ScriptRuntime::remove(partner)` call. It now pins the retained dead link,
  the widow's exclusion from re-pairing, and a source-3 variable write into
  the removed partner's array. The previous hand-set `dead` flags represented
  states the runtime could not produce and were removed.
- The redundant `isRemoved` term in the live-candidate filter was dropped;
  removal always sets `dead`, exactly as the C# filter's `!other.Dead`
  requires. No opcode count changed: 196 true dispatches / 238, 42 remaining.
- Verification: `make -C tests/scripts clean && make -C tests/scripts` passed
  28 VM tests and the complete oracle suite (`All cases match the oracle.`);
  `bash tools/check.sh` passed all contract gates (SMDH skipped, no CIA);
  `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds`
  built `mdk.3dsx` after staging 69 RomFS files (79M), and `cmake --build
  build-linux --target mdk` built the Linux target. No golden changed; no
  emulator/hardware run. Old-3DS timing remains unmeasured.

## What this batch did (iteration 57 — partner spawn/snap/link family)

- Ported the partner-link family from `ScriptVm.cs:1451-1488`: opcode 179
  (`spawn_partner`) spawns the named type at the receiver's position with
  yaw zero and links the new object and receiver in both directions; opcode
  180 (`snap_to_partner`) copies a live partner's position and then adds the
  optional three-float offset; opcode 228 (`link_partners`) pairs live,
  same-arena objects of a named type whose squared distance is below 10000,
  while its list form with a zero second value clears only the receiver's
  link.
- `ScriptRuntime::linkPartners` preserves the oracle's exact two-stage
  filter: the candidate snapshot admits a null link or a link to a dead
  object, then the pairing loop skips every non-null link. It follows stable
  object-list order (the first eligible later object, not the geometrically
  nearest), uses case-insensitive exact type matching, excludes dead and
  cross-arena objects, and leaves existing links untouched when no pair is
  made.
- Added three synthetic `vm_tests` cases. They pin both directions of the
  179 link, spawn type/arena/position/yaw, 180's null/dead/live and offset
  paths, and 228's case folding, type/arena/dead/already-linked exclusions,
  dead-link rule, list order, strict 100-unit boundary, mutual links, and
  asymmetric receiver-only clear. The opcode-pair family adds three to the
  per-opcode test count (28 opcodes total).
- Recounted the brace-matched method spans: C# 238 cases; C++ 198 numeric
  labels / 197 unique labels, of which 196 are true dispatches; 42 remain.
  `docs/OPCODE_COVERAGE.md` updates the remaining set, subsystem grouping,
  owned-case classification and per-opcode test count. All three oracle
  `Linked` writers (245/179/228) are now ported, so partner pairs created
  by the new opcodes can feed conditions 213/214.
- The existing `ScriptRuntime::spawn` omission of the C#'s
  `Stats.CountEnemy` call remains unchanged and applies equally to every
  spawn opcode; the level goldens do not observe those counters.
- A local mutation probe over the new paths killed five variants: a one-way
  179 link, an ignored 180 offset, a clear-on-any 228 list operand, an
  inclusive 100-unit range boundary, and a case-sensitive type match.
- Verification: `make -C tests/scripts clean && make -C tests/scripts` passed
  28 VM tests and the complete oracle suite (`All cases match the oracle.`);
  `bash tools/check.sh` passed all contract gates (SMDH skipped, no CIA);
  `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds`
  built `mdk.3dsx` after staging 69 RomFS files (79M), and `cmake --build
  build-linux --target mdk` built the Linux target. No golden changed; no
  emulator/hardware run. Old-3DS timing remains unmeasured.

## What this batch did (iteration 56 — review-3 partner-condition fixture hardening)

- Addressed external review-3's MINOR/NIT findings on opcodes 213/214. The
  synthetic fixture now gives the receiver a non-origin position and a
  non-zero yaw, makes the partner differ in z, separates the receiver from
  Kurt, and uses the oracle's approximate-equality comparison. It now pins
  the full 3D distance, the receiver-relative distance origin, the floor of
  one, a live due-east angle of zero, and
  `wrap(receiverYaw - bearingToPartner)` (the negated turn required to face
  the partner), in addition to null/dead-partner behavior.
- A local mutation probe rebuilt the four implementations review-3 found
  previously unconstrained: 2D distance, no angle wrap, ignored receiver yaw,
  and a Kurt-relative distance origin. All four now fail the fixture.
- Standardized the angle description in the test and documentation to
  `wrap(receiverYaw - bearingToPartner)`, the negated turn required to face
  the partner. `docs/OPCODE_COVERAGE.md` now also records that 213/214 are
  complete over the existing `linked` field but that only the `find_object`
  writer (245) is ported: the oracle's `spawn_partner` (179) and
  `link_partners` (228) writers remain in the remaining 45, so those paths
  cannot yet feed the conditions. The stale reachability count was corrected
  from 66 to the current 45.
- Runtime behavior and oracle goldens are unchanged.
- Verification: `make -C tests/scripts clean && make -C tests/scripts` passed
  25 VM tests and the complete oracle suite (`All cases match the oracle.`);
  `bash tools/check.sh` passed all contract gates (SMDH skipped, no CIA);
  `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds`
  built `mdk.3dsx` after staging 69 RomFS files (79M). No golden changed; no
  emulator/hardware run. Old-3DS timing remains unmeasured.

## What this batch did (iteration 55 — partner conditions 213/214 and review-2 fixture isolation)

- Ported opcode 213 (`if_partner_distance`) and 214 (`if_partner_angle`) from
  `ScriptVm.cs:1441-1449`. Both read the existing `MdkObject::linked` field: a
  null or dead partner gives distance/angle zero; a live partner gives the full
  3D distance floored at one (`mathMax`, the C#'s `MathF.Max`) or
  `wrap(receiverYaw - bearingToPartner)`, the negated turn required to face
  the partner. No subsystem was invented;
  `find_object` (245) already writes the link.
- Added a synthetic `vm_tests` case, `PartnerConditionsUseOnlyLiveLinkedObject`,
  covering no partner, a dead partner, the distance floor, the 3D distance, and
  the signed angle (due north is -90, not 0 or +90). The opcode-pair addition
  moves coverage to 193 true dispatches / 238, with 45 remaining; the audit
  counts, remaining list, grouping notes, owned-case classification and the
  per-opcode test count were updated consistently.
- Addressed external review-2's MINOR/NIT findings on the opcode-241 fixture:
  `testIfPlayerHealthComparesKurtHealth` now constructs a dedicated
  `ScriptRuntime` (so `hurtKurt(1)` cannot leak into later tests), the receiver
  health assignment comments now say the default/defensive rationale, and the
  `OPCODE_COVERAGE.md` causal wording no longer credits the explicit pin with
  branch-discriminating strength. Runtime behavior and oracle goldens are
  unchanged.
- Verification: `make -C tests/scripts clean && make -C tests/scripts` passed
  25 VM tests and the complete oracle suite (`All cases match the oracle.`);
  `bash tools/check.sh` passed all contract gates (SMDH skipped, no CIA);
  `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds`
  built `mdk.3dsx` after staging 69 RomFS files (79M); `cmake --build
  build-linux --target mdk` built the Linux target. No golden changed; no
  emulator/hardware run. Old-3DS timing remains unmeasured.

## What this batch did (iteration 54 — correct opcode 241 fixture claims)

- Addressed the newest external review's mutation finding. `MdkObject::health`
  already defaults to 100, so iteration 53's two explicit assignments did not
  add branch-discriminating strength: iteration 52's true/false assertions
  already rejected an implementation that read the receiver instead of Kurt.
  Retain the assignments as defensive pinning against a future default change,
  and correct the iteration, coverage, and continuation claims accordingly.
  Runtime behavior and oracle goldens are unchanged.
- Verification: `make -C tests/scripts clean && make -C tests/scripts` passed
  24 VM tests and the complete oracle suite (`All cases match the oracle.`);
  `bash tools/check.sh` passed all contract gates (SMDH skipped, no CIA);
  `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds`
  built `mdk.3dsx` after staging 69 RomFS files (79M). No golden changed; no
  emulator/hardware run. Old-3DS timing remains unmeasured.

## What this batch did (iteration 53 — explicitly pin opcode 241 health fixture)

- Explicitly pinned the receiver object's own health to 100 while Kurt is
  damaged to 99. The later mutation review established that this added no
  branch-discriminating strength: `MdkObject::health` already defaulted to 100,
  and iteration 52's true/false assertions already rejected a receiver-health
  read. The explicit pin is retained as protection against a future default
  change. Runtime behavior and oracle goldens are unchanged.
- Verification: `make -C tests/scripts clean && make -C tests/scripts` passed
  24 VM tests and the complete oracle suite (`All cases match the oracle.`);
  `bash tools/check.sh` passed all contract gates (SMDH skipped, no CIA); the
  3DS build staged 69 RomFS files (79M) and produced `mdk.3dsx`. No golden
  changed; no emulator/hardware run. Old-3DS timing remains unmeasured.

## What this batch did (iteration 52 — opcode 241 `if_player_health`)

- Ported the C# comparison against `runtime.Kurt.Health` in the C++ VM. Added
  synthetic coverage for a true (`100 > 99`) and false (`100 < 99`) branch and
  confirmed opcode 241 leaves the unimplemented bucket. Updated the opcode
  audit: 191/238 true dispatches, 47 remaining. No level golden changes.
- Verification: `make -C tests/scripts clean && make -C tests/scripts` passed
  24 VM tests and all oracle cases (`All cases match the oracle.`); `bash
  tools/check.sh` passed all contract gates (SMDH skipped, no CIA); the 3DS
  build staged 69 RomFS files (79M) and produced `mdk.3dsx`. No emulator or
  hardware run; Old-3DS timing remains unmeasured.

## What this batch did (iteration 51 — exact `attach_to` type matching)

- Addressed review-10's remaining fixture gap: a live same-arena `BOLT_1000`
  candidate collides numerically with `BEAMS_1000`. The existing `BEAMS_1000`
  request must still bind to `second`, rejecting a resolver that ignores the
  type component. The test also asserts the candidate's fixture identity.
  Updated `OPCODE_COVERAGE.md` to list this discrimination alongside the
  existing exact-ID near matches. Runtime and oracle goldens are unchanged.
- Verification: `make -C tests/scripts clean && make -C tests/scripts` passed
  23 VM tests and the full oracle suite (`All cases match the oracle.`);
  `bash tools/check.sh` passed all gates (SMDH skipped, no CIA); the 3DS build
  staged 69 RomFS files (79M) and produced `mdk.3dsx`. `git diff --check`
  passed. No golden changed, no emulator/hardware run. Old-3DS timing remains
  unmeasured.

## What this batch did (iteration 50 — signed opcode 217 and percent truncation)

- Strengthened the opcode 217 `GameStats` VM test with a negative S32 operand
  that subtracts from `headShots`, plus a `Percent(2, 3) == 66` check to pin
  integer truncation. The disabled-counting assertion now verifies it leaves the
  resulting zero unchanged. Runtime implementation and goldens are unchanged.
- Verification: `make -C tests/scripts clean && make -C tests/scripts` passed
  all 23 VM tests and all oracle cases (`All cases match the oracle.`);
  `bash tools/check.sh` reported all gates passed with SMDH skipped (no CIA);
  `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds`
  staged 69 RomFS files (79M) and produced `mdk.3dsx`. No golden changed; no
  emulator/hardware run. Old-3DS timing remains unmeasured.

## What this batch did (iteration 49 — prove exact `attach_to` ID matching)

- Addressed review-9's finding that the prior fixture rejected suffix-ignoring
  logic but did not force exact equality. Added live same-arena IDs 999 and
  10001 after the valid candidates: a lower-bound/range resolver and a
  canonical-name prefix resolver now select a near-match last and fail the
  exact ID assertions. Runtime code and oracle goldens are unchanged.
- Verification: `make -C tests/scripts clean && make -C tests/scripts` passed
  all 23 VM tests and all oracle cases (`All cases match the oracle.`);
  `bash tools/check.sh` reported all gates passed with SMDH skipped (no CIA);
  `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds`
  staged 69 RomFS files (79M) and produced `mdk.3dsx`; `git diff --check`
  passed. No golden changed; no emulator/hardware run. Old-3DS timing remains
  unmeasured.

## What this batch did (iteration 48 — pin `attach_to` ID matching)

- Addressed review-8's test gap: every matching candidate previously used
  instance ID 1000, so ignoring the requested `TYPE_ID` suffix could still pass.
  Added a live same-arena `BEAMS_1001` candidate. The `BEAMS_1000` request must
  select the last eligible ID-1000 object, while a separate `BEAMS_1001` request
  must select the ID-1001 object and update both attach points.
- Verification: `make -C tests/scripts clean && make -C tests/scripts` passed all
  23 VM tests and all oracle cases (`All cases match the oracle.`).
  `bash tools/check.sh` reported all gates passed; `gates_smdh` was skipped
  because no CIA exists. `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM
  make -C mdk_3ds` staged 69 RomFS files (79M) and built `mdk.3dsx` (the
  existing `mdk.elf` was up to date). `git diff --check` passed. No golden
  changed; no emulator or hardware run. Old-3DS timing remains unmeasured.

## What this batch did (iteration 47 — strengthen `attach_to` exclusion test)

- Addressed review-7's finding that the dead/cross-arena exclusion assertions
  were masked by later valid matches. The test now inserts the valid candidates,
  self, then dead and cross-arena matches, in that order. Since `attach_to`
  retains the last eligible match, removing either exclusion independently
  makes that excluded object the selected leader and fails the assertion.
- Verification: `make -C tests/scripts clean && make -C tests/scripts` passed all
  23 VM tests and all oracle cases (`All cases match the oracle.`). `bash
  tools/check.sh` reported all gates passed; `gates_smdh` was skipped because no
  CIA exists. `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM
  make -C mdk_3ds` staged 69 RomFS files (79M) and built `mdk.3dsx` (the
  existing `mdk.elf` was up to date). No golden changed. No emulator or
  hardware run; Old-3DS timing remains unmeasured.

## What this batch did (iteration 46 — opcode 74 `attach_to`)

- Ported opcode 74 from `ScriptVm.cs:968`: scan live objects in stable runtime
  order, excluding self and removed/dead objects, require same arena, compare
  formatted `TYPE_ID` case-insensitively, and keep the last match. On a match,
  assign leader, both attach-point indices, and movement command 74; on a miss,
  preserve all object state. Existing movement-command 74 behavior follows the
  attachment and copies the leader's reference-point position and orientation.
- External review-6 found that C# auto IDs begin at 1000 while C++ began at 0,
  and that the original test inferred its operand from C++ state and did not
  actually prove duplicate-ID selection or state preservation. Set
  `nextInstance_` to 1000 and use an isolated-runtime test with oracle operand
  `BEAMS_1000`, duplicate explicit IDs, dead/cross-arena/self exclusions, case
  folding, and seeded leader/point/move state on a miss. Restored the displaced
  `findObject` source comment to its function.
- Verification: `make -C tests/scripts clean && make -C tests/scripts` passed
  all 23 VM tests and all oracle cases (`All cases match the oracle.`). The
  first test run caught a harness mistake (the second receiver was not in the
  runtime object collection); corrected it by executing the program on the
  spawned self object, then reran the full command successfully. `bash
  tools/check.sh` reported all gates passed; `gates_smdh` was SKIPPED because no
  CIA exists. `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM
  make -C mdk_3ds` linked `mdk.elf`, staged 69 RomFS files (79M), and built
  `mdk.3dsx`. `git diff --check` passed. No golden changed. No emulator or
  hardware run; Old-3DS timing remains unmeasured.

## What this batch did (iteration 45 — finish deferred object inventory review)

- Followed up on external review-4 of iteration 44's `object.h` behavior list.
  The list now records `PoseParts` as unported; sound positioning is implemented
  in `SoundMixer::voicePosition`, and distance behavior is already inlined at
  landed call sites. Clarified that `PartCenter` is a rest-pose box centre,
  unlike current-pose `part_bounds()` / `pose_bounds()`.
- Documentation/comments only; no runtime behavior, oracle fixtures, or goldens
  changed.
- Verification: `make -C tests/scripts clean && make -C tests/scripts` passed
  all 22 VM tests and all oracle cases (`All cases match the oracle.`);
  `bash tools/check.sh` reported `CHECK: ALL GATES PASSED` (`gates_smdh`
  skipped because no `.cia` exists); `DEVKITPRO=/opt/devkitpro
  DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds` linked `mdk.elf`, staged
  69 RomFS files (79M), and built `mdk.3dsx`. No emulator/hardware run; Old-3DS
  timing remains unmeasured.

## What this batch did (iteration 44 — reconcile deferred object behavior)

- Addressed external review-3's documentation finding in `mdk_core/src/scripts/object.h`:
  reconciled the object behavior status against the current tree. The deferred
  list excludes implemented `rotation()`, `playAnimation()` / `advanceAnimation()`,
  `poseBounds()`, `partCenter()`, and runtime `yawTo()`; it retains unported
  Footing, frame-blend/transform reads, and pose/part baking. Review-4 found that
  the then-current comment still incorrectly listed sound-position and distance
  as deferred and omitted `PoseParts`; iteration 45 corrects that inventory.
  Documentation-only; no runtime behavior or goldens changed.
- Verification: `make -C tests/scripts clean && make -C tests/scripts` passed
  all 22 VM tests and all oracle cases (`All cases match the oracle.`);
  `bash tools/check.sh` reported all gates passed (`gates_smdh` skipped because
  no `.cia` exists); `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM
  make -C mdk_3ds` linked `mdk.elf`, staged 69 RomFS files (79M), and built
  `mdk.3dsx`. No hardware or emulator run was needed/performed. This is a
  documentation-only correction and does not claim Old-3DS readiness.

## What this batch did (iteration 40 — opcode 77 `debug_msg` no-op)

- Ported opcode 77 as an explicit no-op in `ScriptVm::execute`, matching the
  C# `case 77` at `ScriptVm.cs:1713`. The C# deliberately ignores debug output;
  no logging subsystem or side effect is implied. Decoder continues to consume
  the U8 and Pascal-string operands before dispatch.
- Added a synthetic `vm_tests` program encoding mode 3 and `"debug text"`, then
  a `set_var` instruction. It verifies opcode 77 is not in the unimplemented
  bucket and that execution advances and performs the following write.
- No level golden changes: opcode 77 does not fire in levels 3-8. Coverage is
  now 188 true dispatches / 238, with 50 remaining.
- Verification: `make -C tests/scripts clean && make -C tests/scripts` passed
  all 21 VM unit tests and `All cases match the oracle.`; `bash tools/check.sh`
  reported all gates passed (SMDH gate skipped because no `.cia` exists);
  `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds`
  linked `mdk.elf` and built `mdk.3dsx`; `git diff --check` passed. The
  adversarial reviewer was unavailable (review-37 is a prior quota failure and
  no reviewer response was available for this iteration); self-review compared
  behavior and decoder operand order against the read-only C# oracle.


## What this batch did (iteration 11 — pickup/chute/death lifecycle slice)

- **`ScriptRuntime::kill()` now mirrors C# `Kill` (ScriptRuntime.cs:1678)**:
  `health=0`, then either the death-script switch (move_command=0,
  FlagNotTarget, restart/wait_resume = death_script, wait_time=0,
  death_script=0) or the Explode branch. The Explode branch's EFFECTS
  (PlaySoundAt/FlashAt/BreakUp/SpawnExplosion — mixer, flash, debris, explosion
  model) remain unported, but its REMOVAL ACCOUNTING now lands: C# `Explode`
  calls `Remove(obj)` (cs:1717), so the C++ branch calls `remove(obj)` instead
  of the old `dead=true` placeholder (which left the object counted in
  `objectCount()` — found by the adversarial validator). No golden to t30000
  exercises the Explode branch (every corpus death is a death-script death),
  so the missing effects stay the documented gap.
- **`ScriptRuntime::remove()` now mirrors C# `Remove` (cs:1648)**: a live
  `Attached` (a pickup's chute) is removed FIRST (before the parent is marked
  dead), with an `isRemoved()` early-return guard for idempotence (the C++
  `removed_.push_back` is not idempotent, unlike C# `List.Remove`). A pickup
  that falls out (`fallOut` → `remove`) takes its chute with it. The stale
  `fallOut` NOTE (calling kill() a placeholder) was corrected.
- **The frontier moved from ~t400-900 to the end of the corpus.** Probed tick
  by tick against the oracle (table AND script dumps): L3 matches to t30000
  (11 live objects), L8 to t30000 (10), L6 to t30000 (5), L7 to t30000 (7),
  L4 to t10000 (0 objects — the arena empties at the same ticks as the oracle;
  the old "L4 empties past t600" divergence was the kill() placeholder). L5
  matches to t2302; t2303 is the new blocker (BOLT spawn, priority 1 above).
- 36 new golden cases, 72 golden files, all generated from the oracle driver
  (`dotnet mdkoracle.dll <level> <ticks> 1 [script]`). The temp `RALPH_DUMP`
  probe used for the probing was removed from `tick1.cpp` before committing.
- Adversarial validator run over the diff: one MAJOR (Explode-branch count
  accounting) found and fixed as above; the death-script branch field-for-field
  and the Attached cascade order were independently confirmed against the C#.
- Call-site audit vs the C#: C# `Kill` has three call sites (ScriptVm.cs:494/505
  = opcodes 16/56, ported; cs:1554 = opcode 184 `explode`, in the remaining-77
  set, gated behind the unported `Items.Blast`). C# `Remove` call sites map to
  the C++ ones (opcode 110, fallOut, chute-shrink, the Attached cascade); the
  arena-unload removal (cs:677) and the collected-shrink removal (cs:2610)
  belong to unported slices (arena streaming, collected shrink) and do not fire
  in the corpus.


## What this batch did (iteration 8 — deep-tick golden frontier)

- Extended the golden corpus to each level's honest deep-tick frontier, generated
  from the oracle driver (table + script dumps), all matching byte for byte:
  **L3 t360/t420** (pickups/chutes resolve, objects drop 14→12), **L5 t660** (the
  XGEN region past t600), **L6 t660/720/780/840/900** (the XD keeps walking — the
  table genuinely cycles, not a frozen steady state: object 002's pos/yaw/restart
  change across the range), **L7 t660/t900** (a steady arena bracketed to prove no
  slow drift). 10 new cases, 20 new golden files.
- **The frontier is the unported-slice boundary, confirmed empirically, not
  assumed.** Probed deeper ticks against the oracle and committed only the matching
  ones; the first divergent tick per level is the unported lifecycle: **L8 diverges
  at t420** (SW_H01 + SW_CHUTE removed between t360 and t420 — the pickup/chute
  removal slice), **L3 at t480**, **L5 at t720**, and **L4 empties to 0 objects
  past t600** (degenerate, not committed). None of the divergent goldens were
  committed (they would fail).
- **Independently re-verified the testability map's blocker** (so the next context
  does not re-litigate it): `grep` over every golden shows `move=15/` appears **0
  times** — movement command 15 (the only `AlarmTicks` setter, ObjectMotion.cs:385)
  never fires in levels 3-8 to t600, so porting the alarm-ticks state machine +
  dumping `alarm_ticks` would yield an all-zeros column that exercises nothing.
  The four reachable unimplemented opcodes (109/138/175/249) fire but their effects
  are not in the dumped field set (Kurt health, Kurt inventory, audio) or need an
  unported subsystem, so they remain blind-port-forbidden.
- **Corpus cannot widen by level**: only `MDK/TRAVERSE/LEVEL3..8` data is present
  (no level 1/2/9+), so the only axis left to densify is ticks, and that axis is
  now bounded by the unported lifecycle slices above.
- Test-only change; no `mdk_core` code touched, so both targets are unaffected.
  Verified: `make -C tests/scripts` (clean) → `All cases match the oracle.`;
  `bash tools/check.sh` → `CHECK: ALL GATES PASSED` (`gates_smdh` SKIPs: no `.cia`);
  `make -C mdk_3ds` → links `mdk.3dsx`; `cmake --build build-linux --target mdk` →
  `Built target mdk`.


## What this batch did (iteration 7 — target-tracking goldens)

- Executed the testability map's recommended unblock (the "extend BOTH the oracle
  driver's `script` dump and `tick1.cpp` `dumpScript`" move), scoped to the
  runtime fields the C++ **already** maintains so the change is a pure
  regression-lock with no behaviour change: `target_pos`, `target_yaw`,
  `alien_target`. These are `ScriptRuntime`-level state (C# `TargetPosition`/
  `TargetYaw`/`AlienTarget`, ScriptRuntime.cs:199-208), reset to Kurt each tick
  (cs:887-889) and overridable per object by `SelectTarget` (cs:418-441); the C++
  mirrors this in `selectTarget` (script_runtime.cpp:2192) and the constructor.
- **Measured before committing**: extended both dumps, ran the C++ against the
  OLD goldens (117 cases differ — the SCRIPT line changed), then confirmed the
  C++ `got` SCRIPT lines equal the oracle's for L3_t300/L5_t600/L8_t300/L4_t600
  byte for byte. Regenerated all 114 script goldens from the oracle → all match.
- **Sensitivity proven**: a `+0.0001f` perturbation of the dumped `target_yaw`
  makes the script checks FAIL (then reverted) — the new fields are genuinely
  compared, not ignored.
- `alien_target` is `-1` in every current case (opcode 251 does not fire in
  levels 3-8 to t600), so the index mapping is correct-but-unexercised; it is
  computed over the same removed-skipped enumeration as the OBJ lines, so the
  two views agree when it does fire.
- **`alarm_ticks` deliberately NOT added**: the C++ has no `alarm_ticks` field
  (the alarm-ticks state machine — the per-tick decrement cs:896 + the setters in
  movement command 15 / Rides — is unported), so dumping it would diverge and
  force a blind port. It stays the next field to add, paired with the alarm
  subsystem slice. `Items.Bomb`/`Decoy` also stay out: they live on a separate
  `Items` subsystem not reachable from the driver, and the C++ `itemsBomb` is
  always null (matches the C# when no bomb is in play).
- **Oracle-driver provenance** (the driver is `/tmp/kilo/oracle/Driver/Program.cs`,
  not in the repo — same as every existing golden): the `script` dump mode's
  SCRIPT line gained `target_pos={X:F4},{Y:F4},{Z:F4} target_yaw={Yaw:F4}
  alien_target={idx}`, where `idx` is `AlienTarget`'s index in `scripts.Objects`
  or `-1`. Regenerate with
  `dotnet mdkoracle.dll <level> <ticks> 1 script > golden/L<level>_t<ticks>_script.txt`.
- Verified: `make -C tests/scripts` (clean) → `All cases match the oracle.`;
  `bash tools/check.sh` → `CHECK: ALL GATES PASSED` (`gates_smdh` SKIPs: no
  `.cia`); `make -C mdk_3ds` → links `mdk.3dsx`; `cmake --build build-linux
  --target mdk` → links `mdk`. Test-only + oracle-harness change; no `mdk_core`
  code touched, so both targets are unaffected.


## What this batch did (iteration 6 — verification, review, doc correction)

- **Tree re-verified green** on a clean rebuild (iteration 5 timed out mid-final-
  verification after committing the scene code `003f9a3` but before committing its
  `CONTINUATION.md` update):
  - `make -C tests/scripts` (after `clean`) → `All cases match the oracle.` (all
    table + script + `L{n}_dti` + the three scene checks PASS).
  - `bash tools/check.sh` → `CHECK: ALL GATES PASSED` (`gates_smdh` SKIPs: no built
    `.cia`).
  - `make -C mdk_3ds` → stages RomFS (69 files, 79M) and links `mdk.3dsx`.
- Committed iteration 5's dangling `CONTINUATION.md` update (the dynamic-scene
  batch note) as its own commit — the code was already committed, only the doc was
  left in the working tree.
- **Adversarially reviewed the scene commit `003f9a3`** against the C# `LevelView`
  (the four external reviews all timed out at rc=124 mid-verification and produced
  no findings). Checked: the run-break at group boundaries (a batch is single-group,
  matching the C#'s per-group meshes batched by surface); the orphaned-vertex path
  when a not-drawn triangle splits a same-material/same-group run (the split triangle's
  3 vertices are pushed but referenced by no batch — wasted buffer, never drawn, and
  the neighbouring batches' `first`/`count` stay correct); `arenaBatchVisible`
  (group 0 has no stored state → always drawn); `LevelView::refresh()` (rebuilds the
  whole arena mesh for a `group_set_texture`, the C# per-group `Rebuild` produces the
  same picture; the texture/flat-colour caches grow monotonically, matching the C#
  "old mesh lives until the level ends"). **No bug found.**
- **Corrected a real mis-classification** in `docs/OPCODE_COVERAGE.md`: opcode **27**
  was grouped under "Audio (no mixer port)". It is `if_alarm`, a branch on
  `runtime.AlarmTicks > 0` (ScriptVm.cs:1693) — gated behind the **alarm-ticks state
  machine** (a runtime `alarm_ticks` field + the per-tick decrement
  ScriptRuntime.cs:896 + the setters in movement command 15 ObjectMotion.cs:385 and
  Rides Rides.cs:219), none of which exists in the C++. 107/233/249 are the real
  audio opcodes. 27 does not fire in levels 3-8 to t600, so there is no golden to
  exercise it — it stays deferred, now with the correct gate named.


## What this batch did (dynamic scene updates — open item 1)

- Open item 1 ("a mid-play `group_state`/`group_set_texture` change does not
  change what is drawn until the scene is rebuilt") is **closed at the scene
  level**, faithful to the C# `LevelView`, which checks Hidden at DRAW time
  (LevelView.cs:126) and rebuilds a group's mesh on a material change (the
  `Changed` event → `Rebuild`, LevelView.cs:251).
- `ArenaBatch` gained a `group` field, and `buildArenaScene` now breaks a run at
  a **group boundary** as well as a material change, so every batch is single-
  group. The build-time Hidden skip is **gone**: hidden groups' geometry stays in
  the mesh and `arenaBatchVisible(arena, batch)` (a new free function) decides
  per batch at draw time — so `group_set_state` hide/show needs **no rebuild**.
  `LevelView::draw()` skips a batch whose group is Hidden now.
- `LevelView::refresh()` is the `group_set_texture` path (the C# `Rebuild`): it
  re-runs `buildArenaScene` against the arena's current group materials and
  rebuilds the GPU mesh (release + create, like the C# creating a new mesh). It
  is the integration hook a `group_set_texture` calls between frames; there is no
  `GameEngine` yet to call it, so the mechanism is in place and tested at the
  scene level, and the per-opcode wiring lands with the engine. A hide/show does
  NOT call it (draw reads hidden live).
- **Regression test** (`tests/scripts/tick1.cpp`, scene section): over **all 104
  arenas** of levels 3-8 it checks (a) the batches partition the drawn triangles
  by group — a batch spanning two groups breaks this equality, so this proves the
  run-break; **880 cross-group same-material adjacencies** exercise it (removing
  the run-break makes it FAIL on HMO_2/HMO_3/MEAT_*/…, confirmed); (b) hiding a
  group after the build makes its batches not-visible while others stay visible;
  (c) `group_set_texture(-1)` on a textured group makes its batches flat on a
  rebuild. `arena_scene.cpp` + `material_resolver.cpp` added to the test Makefile.
- Verified: `make -C tests/scripts` (clean) → `All cases match the oracle.` (new
  scene PASSes); `bash tools/check.sh` → `CHECK: ALL GATES PASSED`; `make -C
  mdk_3ds` → links `mdk.3dsx`; `cmake --build build-linux --target mdk` → links
  `mdk` (both targets build the changed core).


## What this batch did (DTI record discrepancy — reproduced and closed)

- Open item 4's "C++/oracle DTI record discrepancy" is **CLOSED with a concrete
  reproduction**, not an argument (the prior batch had only argued the layouts
  match field-for-field).
- Reproduction: dumped every arena's object records for levels 3-8 from BOTH the
  C# oracle (`Dti.ParseRecords`) and the C++ `Dti::parseRecords`, printing every
  float (angle, position[3], boxEnd[3], arena pitch) as **raw IEEE bits** so a
  one-ULP difference cannot hide behind `%.4f`. Result: **byte-identical across
  all 6 levels, 19 arenas each, every record** — type/id/angle/position/boxEnd
  (the three-floats-then-`skip(-12)`-then-name(12) rewind quirk) and name. The
  claim was unsubstantiated; there is no DTI record discrepancy.
- Locked it with a **permanent regression test**: `tests/scripts/tick1.cpp` now
  dumps the same raw-bit record view and compares to committed goldens
  `tests/scripts/golden/L{3..8}_dti.txt` (generated from the oracle). The test is
  sensitive — a probe aliasing bug during authoring made the C++ dump differ, and
  the check caught it, confirming it fails on a real divergence.
- Reproduce/regenerate: the oracle driver gained a `dti` dump mode
  (`/tmp/kilo/oracle/Driver/Program.cs`, 4th arg `dti`) that prints the records
  in this format; the goldens are that output. (The driver is the oracle harness,
  not in the repo — same as the existing goldens' provenance.)
- Test-only change; no `mdk_core` code touched, so both targets are unaffected.
  Verified: `make -C tests/scripts` (clean) → `All cases match the oracle.` (six
  `L{n}_dti` PASS); `bash tools/check.sh` → `CHECK: ALL GATES PASSED`;
  `make -C mdk_3ds` → links `mdk.3dsx`.


## What this batch did (owned-opcode classification — Task 2 remainder)

- Open item 2 (classify the 161 owned cases) is **done**. The classification is
  in `docs/OPCODE_COVERAGE.md` §"Owned-case classification", produced by a
  reproducible brace-matched extraction of the numeric `case` labels from the C#
  `ScriptVm.Execute` and the C++ `ScriptVm::execute` (re-confirmed C# 238 / C++
  161 / remaining 77; the C++ owns no case the C# lacks).
- Result: **0 stubs**, **11 partial** (faithful dispatch whose gameplay effect
  is gated behind a partial/unported subsystem), **150 complete**. Each partial
  is verified against the code, not asserted:
  - 6, 15 — set `move_command` 6/15 but `ScriptRuntime::updateCommand`
    (script_runtime.cpp:1561) has no case for them → no motion.
  - 16, 56 — call `kill()`, a death-slice placeholder (script_runtime.cpp:2451
    sets only `dead`; death script / debris / drops unported).
  - 24, 151 — store sound names whose playback needs the unported mixer.
  - 89, 248 — honest no-ops (mixer / Kurt knock-back unported; no state the
    goldens read).
  - 140 — sets group material VM state; renderer ignores it (open item 1).
  - 165, 166 — read `itemsBomb`, never assigned (script_runtime.h:226) → always
    "no bomb" until the Items slice.
- Test-coverage axis recorded separately: all 161 owned cases are covered only by
  the end-to-end (level, tick) goldens; there are no per-opcode unit tests.
- Doc-only change; no `mdk_core` code touched, so both targets are unaffected.
  Verified: `make -C tests/scripts` → `All cases match the oracle.`;
  `bash tools/check.sh` → `CHECK: ALL GATES PASSED`; `make -C mdk_3ds` → links
  `mdk.3dsx`.


## What the prior batch did (L5 deep-tick goldens — divergence closed)

- The open "L5 `XG[004]` golden divergence ~tick 577 (root cause unknown)" is
  **resolved**. Reproduced against the oracle: at t600 the oracle's object count
  oscillates 8→9→8 (the XGEN spawner's 5th XG opens ~t585 and dies ~t595) and
  object 004 takes a `move_near_target` (opcode 43) at ~t577. The current C++
  matches the oracle **byte for byte** (table AND script state) at t577/585/590/
  595/600. The old divergence was the same degree/radian constant-fold ULP class
  the left-associative spelling already resolves (the stale `/tmp/kilo/cpp_L5_t600_*.txt`
  dumps predated that fix and misled the earlier note).
- Added golden cases `{5,577},{5,585},{5,590},{5,595},{5,600}` (table + script)
  to `tests/scripts/tick1.cpp`, generated from the oracle. Verified:
  - `make -C tests/scripts` → `All cases match the oracle.` (new cases PASS,
    table + script).
  - `bash tools/check.sh` → `CHECK: ALL GATES PASSED`.
  - `make -C mdk_3ds` → links `mdk.3dsx`.
  - Test-only change; no `mdk_core` code touched, so both targets are unaffected.


## What the previous batch did (all verified against the C# oracle)

- `TriangleGroups.SetState` semantics: stored per-group state on `Arena`
  (`groupState`/`setGroupState`/`hasGroup`), full 1..255 group numbers, the
  0/2/3/4/5/other operation table, write-through to every triangle of the
  group. `ScriptRuntime::setGroupState` mirrors it; the old per-triangle
  derivation and the ≤16 clamp are gone.
- Load-time `Disable`: `Arena::applyDisabledTriangles()` runs in the
  `ScriptRuntime` constructor (after clip flags), not in `Arena::parse` —
  LEVEL8 GUNT_2's diagonal wall is now non-solid like the original.
- `raycastGroup` mirrors `raycastPoint` exactly (`kSegmentAny`,
  `kClipPassesThrough`, second arena unconditional, all-arena fallback,
  nearest-hit by squared distance, same `>= best` skip);
  `getKurtFloorGroup` uses it (+1 to −10 under Kurt).
- `HitGroup` implemented, including the scratch-object run path: the arena's
  `hit_scripts` `MdkObject` gets hit_type=weapon, hit_event=0, wait_time=0,
  cleared gosub stacks, restart=script, then `vm_.run()`.
- Group materials: `Arena::groupMaterial(group, int32_t* out)` returns
  presence (negative materials like −1 are valid; the old −1 sentinel was a
  bug the validator caught). `setGroupTexture` (opcode 140) wired with the
  existence check.
- Renderer build-time skips: clip/disabled triangles and hidden groups.
- Opcodes audited per Task 1.7: 245 `find_object` (factored into a runtime
  helper), 246, 250 `if_in_box` (six-value zero-filled array + sender), 87
  `kurtLooksAt` (constants match ScriptRuntime.cs:2701), 6/15, 62, 207.
- VM group cases 98/99/140/162/163/168/194 match C# operand layout.


## Iteration 14 — Kurt health/inventory state + opcodes 109/175 (commit 4bdcba6)

- New `mdk_core/src/game/kurt.h/.cpp`: the Hurt/Damage half of `Kurt.Combat.cs`
  (health 0x574324, red flash clamp 75..180 at 25/damage, invulnerability
  0x573bd4, knock damage 0x573b20, the Dead/Knocked/GetUp/HardLand gate, the
  difficulty scaling 2/3-easy-min-1 / 2x-hard) and the `Inventory.cs` slot list
  (5 slots, the Item enum). The movement state machine and pickup collection are
  later slices, so in the corpus the state moves only through the scripts.
- `ScriptRuntime::hurtKurt` mirrors C# `HurtKurt` (ScriptRuntime.cs:2676)
  including the `Rides.TakesHits` gate (ride slice unported → nothing ridden →
  Kurt takes every hit, as the C# reads). VM: case 109 → hurtKurt(I(o[0]));
  case 175 → countItem + compareOps(count, o[1], o[2], o[3] ?? 0).
- The oracle driver's script dump gained a `KURT` line
  (health/hurt_flash/invulnerable/knock/state/slots); all 170 script goldens
  regenerated from the oracle and matched byte for byte. L3 t480 pins health 96
  / flash 150 / knock 4 (two hurts, flash clamp); L5 pins Kurt dead (health 0,
  knock 101) by t2303; L4/L6/L7/L8 pin him untouched (100/0/0). state=0 and
  slots=0 everywhere confirm push_kurt never knocks him and nothing is collected
  in the corpus — the gates are exercised at their false branch.
- Gates: `audit_r11_idiv` flagged the easy-difficulty `/3`; the constants are
  now `constexpr` (the audit trusts constexpr divisors; house style in object.h
  already uses them). `std::max` needed `(int32_t)` literals on arm-none-eabi
  (int32_t is `long` there).
- Verified: tests 341 PASS/0 SKIP, check.sh ALL GATES PASSED (smdh SKIP, no
  .cia), mdk_3ds links mdk.3dsx, linux `Built target mdk`.

## Iteration 17 — the audio slice: mixer, sound bank, device control plane; 89/107/233/249

- In-flight from iterations 15-16 (which died before committing); this iteration
  verified, fixed, and banked it. New `mdk_core/src/audio/`: `sound_bank.{h,cpp}`
  (SNI archive lookup, newest-archive-wins, cached-missing, WAV-header metadata
  only — samples belong to the platform backend), `sound_mixer.{h,cpp}` (the
  voice table, Start modes New/Restart/Once, 2D/point/source placement, Doppler,
  scope listening — a 1:1 port of SoundMixer.cs; the spare-Voice pool and the
  Func-follow overload are documented non-ports), and `sound_device.{h,cpp}`
  (the AudioDevice control plane: ids from 1, the 64-voice cap that drops a new
  sound, per-voice gain/pitch/pan, the frame-consumption lifecycle the muted
  device would run). The C# `ISoundSource` virtual is replaced by the two fields
  that answer it (the object's position and yaw pointers into the never-vacated
  object deque) per AGENTS.md §10.
- VM: case 89 → playSound(str(o[2]), I(o[0]), o[1]) with the flags decode
  (mode 0-3, Flat/Following/AtReference/AtPoint/Tracked); case 107 →
  setLoopSound; case 233 → isSoundPlaying(upper(name)); case 249 → the
  TrackedVoice/TrackedSound branch. ScriptRuntime::remove now stops the loop,
  tracked, and following voices as the C# Remove does.
- The oracle driver's script dump gained the `AUDIO voices=N list=id:NAME,...`
  line (reflection on `_voices`) and per-object `tsnd/tvoice/tplay/lsnd/lvoice/
  lplay`; all 170 script goldens regenerated from the oracle. L5 pins nonzero
  tracked voices with tplay=1 (249's true branch fires); L3's table fills to the
  64-voice cap with RICO1 on both sides (the headless harness never calls
  mixer.update, on either side). Six goldens re-diffed against a freshly built
  oracle binary: byte-identical.
- Fixes this iteration: the R6 audit flagged `rotatedZ`'s sinf/cosf — pinned with
  the established oracle-fidelity R6 pin (glibc matches MathF.SinCos bit-for-bit;
  the LUT would move a voice's pan). The R11 screen flagged the WAV frame-count
  division in sound_bank.cpp — the geometry moved to `Wav::frames()` (formats/,
  the contract's cold-parsing scope) instead of pinning a cold site with a
  hot-only rule (§5). A stray extra printf argument in tick1.cpp.
- `docs/OPCODE_COVERAGE.md` recounted with its own brace-matched method: C# 238 /
  C++ 168 / remaining 71 (it had also been stale on 115, ported at iter 11);
  partials 11→10 (89 complete; 24/151 re-gated to their now-unblocked call-sites).
- Verified: tests 341 PASS/0 SKIP, check.sh ALL GATES PASSED (smdh SKIP, no
  .cia), mdk_3ds links mdk.3dsx, linux `Built target mdk`.

## Mixer call-sites + Muse swarm migration (2026-10-25)

- Wired the last two mixer call-sites: the motion frame-sound trigger
  (`ObjectMotion.cs:133`) and the door state machine's `DoorSounds`
  (`ObjectBehaviors.cs:90-115`, `Play` at 418-424). All three gates pass
  with no golden change; adversarial review found no semantic divergence
  (observability MAJOR rebutted in `docs/CONTINUATION.md`).
- The swarm moved from Kilo/litellm to the Muse subscription: `ralph.sh`
  now drives `muse exec` (builder `muse-spark-1.3`, reviewer
  `muse-wonder-1.0` with `--disable-write`), prompts live in
  `ralph/builder.md` + `ralph/validator.md`, and `.kilo/` is removed.

## Debris slice: opcodes 137/138/139 (2026-10-25)

- `mdk_core/src/scripts/debris.{h,cpp}`: 1:1 port of `Debris.cs`
  (Shatter/Spark/Update, 600-piece pool, seeded-RNG call order, banker's
  rounding for `MathF.Round`, .NET's double-derived degrees→radians
  factor — the float spelling was 1 ULP off in ~46% of draws, proven by
  the adversarial review). Runtime: `ShatterPoint`/`ShatterDirection`,
  Ray/GroupFacets delegates wired; Trail/Updraft wait for Effects/Fans.
- Oracle harness fix: the driver never registered its `TriangleGroups`
  (only the viewer's `LevelView` calls `Add`), so Shatter was a no-op in
  every golden. It now registers all arenas; L5's 138 (t482) yields 134
  pieces. All 340 goldens regenerated from the oracle; the script dump
  gained a `DEBRIS` line on both sides.
- Verified: tests/scripts "All cases match the oracle", check.sh ALL
  GATES PASSED (one R6 pin for spawn-time spin trig), mdk_3ds links,
  Linux builds. Coverage 171/238, remaining 68. Adversarial review: no
  BLOCKER/MAJOR; the 1-ULP MINOR above was fixed.
- Note: the runtime half of this slice (constructor/rayIn/groupFacets)
  was banked inside the preceding audio commit by a staging overlap;
  the tree at HEAD is consistent, that one intermediate commit is not
  bisect-clean on its own.

## Synthetic opcode harness + if_cheat_key (2026-10-26)

- The corpus is spent for opcodes (nothing unported fires in levels 3-8 to
  t30000), so the second route in is a synthetic harness:
  `tests/scripts/vm_tests.cpp` is a 1:1 C++ twin of
  `tests/Mdk.Game.Tests/ScriptVmTests.cs` — same `Code` builder bytes (gap,
  FileBase-4 targets), same level-7 runtime shape, same 7 programs and
  assertions — wired as a second binary in `tests/scripts/Makefile` that runs
  before the goldens in `make -C tests/scripts`. Both binaries link the same
  core objects (shared `COMMON_SOURCES` list).
- Opcode 13 (`if_cheat_key`) ports through it: an explicit
  `return branch(obj, ins, false)` mirroring `ScriptVm.cs:1709`, instead of
  falling into `noteUnimplemented`. The twin pins both the value (goto not
  taken, `variables[0] == 1`) and the clean `unimplemented()` bucket (the C#
  owns 13 explicitly, so it never touches its `Unimplemented` dict either).
- Authentic failure observed: pre-port, 6/7 twin tests passed and only the new
  `!saw13` assertion failed; post-port all 7 pass
  ("All VM unit tests match the oracle."). Goldens unchanged, as predicted
  (13 never fires in the corpus).
- Verified: `make -C tests/scripts` → twin 7/7 + "All cases match the oracle.";
  `bash tools/check.sh` → ALL GATES PASSED (smdh SKIP, no .cia);
  `make -C mdk_3ds` (devkitARM env) → links `mdk.elf`, builds `mdk.3dsx`.
- Coverage: brace-match recount → C# 238 / C++ 173 / remaining 66; fixed the
  doc's stale 171/68 classification numbers and the 11-vs-8 partials slip.
- Gap (environmental, not code): the C# `ScriptVmTests` suite could not run in
  this sandbox — AF_UNIX sockets are denied (`socket()` → EPERM, probed) and
  .NET's MSBuild/test servers require them; no prebuilt test dll exists. The
  twin was verified against the in-repo C# file line by line instead, and the
  6/7 first-run pass corroborates the program bytes. Re-run the C# suite
  outside the sandbox when convenient.
- Next: unfired opcodes land through the harness, one family per batch — the
  parts bucket (32 `clear_parts_mask`, 129/132/136) is the natural next slice.

## 2026-10-26 — iteration 24: opcodes 32 + 129 (parts pair) via vm_tests (coverage 175/238)

- Opcode 32 (`clear_parts_mask`) is a one-liner through the existing
  `partsMask` helper with `hide=false`; opcode 129 (`blow_off_parts`)
  validates the gore mode (`mode > 2`, null model, mode-2-without-gore break),
  resolves names via `findPart`, and ORs the mask into `locked_parts` and
  `hidden_parts` (`ScriptVm.cs:1523-1524,1746-1769`). No new subsystem: all
  fields and helpers already existed for opcode 31.
- 132/136 (`spawn_effect`/`spawn_debris`) stay with the Effects slice: both
  draw RNG and delegate to `Effects.SpawnBubble`/`SpawnDrop` (effect list,
  textures, frame counts, per-tick update) — a dump-line subsystem slice, not
  a dispatch.
- Tests: `pstr` builder + synthetic 3-part model fixture (`partsModel`) in
  `vm_tests.cpp`; three tests pin hide/show-by-name, ALL-vs-locked,
  gore on/off, mode 1 vs 3, unknown names, and the 129→32 interaction, each
  with the `unimplemented()`-bucket check. No C# counterpart exists to twin
  (that suite covers 11 opcodes), so they pin the oracle's ScriptVm.cs
  directly with identical program bytes — noted in the file header.
- Authentic failure observed: with the port stashed, 14 checks fail (state +
  bucket assertions); restored, all 10 tests pass
  ("All VM unit tests match the oracle.").
- Verified: `make -C tests/scripts` → 10/10 + "All cases match the oracle."
  (goldens unchanged — 32/129 never fire in the corpus);
  `bash tools/check.sh` → ALL GATES PASSED (smdh SKIP, no .cia);
  `make -C mdk_3ds` (DEVKITPRO/DEVKITARM) → `mdk.elf` (fresh link, newer than
  the port) + `mdk.3dsx`.
- Coverage: brace-matched recount → C# 238 / C++ 175 / remaining 64 (exact set
  difference). Noted in the doc: the C++ label count carries a nested-switch
  artifact (the `case 0:` mode arm inside opcode 249, present since the mixer
  slice), so true dispatches are 174 and 174 + 64 = 238.
- Next: unfired opcodes land through the harness, one family per batch —
  132/136 need the Effects slice; the Kurt/items, arena, movement and
  camera buckets in OPCODE_COVERAGE.md keep their subsystem gates.

## Iteration 28 — rope-swing pair: opcodes 226 `jump_to` + 242 `set_2d0_block`

- 226 starts the swing below the pivot (`swing_pivot`/`speed`/`gain`/`yaw`/
  `length` + `FlagSwinging`); 242 sets the rope lines (colour 1, mask 0xFF,
  four points) or clears the mask on count zero. Pure object-state over
  fields the C++ already owns (`object.h`); 242 guards an empty block like
  the existing `partsMask` helper. 85 (`set_rolling`) stays out: it snapshots
  the full `Rotation` matrix and `rotation()` is deferred to FrameBlend.
- Tests first: two `vm_tests` tests failed authentically in the
  `unimplemented()` bucket (20 checks), then passed after the dispatch.
  Pin `ScriptVm.cs:1139-1166` with identical program bytes (F32×5 for 226;
  u8 mode + 12 F32 for 242).
- Gates: `vm_tests` 12/12; goldens "All cases match the oracle" (unchanged —
  neither opcode fires in the corpus); `tools/check.sh` ALL PASSED;
  `mdk.3dsx` links. Coverage: brace-matched recount → C# 238 / C++ 177 /
  remaining 62 (true dispatches 176; `case 0:` artifact inside 249 noted).
- Next: one opcode family per batch through the harness; 132/136 still need
  the Effects slice; 85 waits on the FrameBlend `rotation()` slice.

## Iteration 29 — spawn slice: opcode 156 `spawn_at_point` + review-28 closed (coverage 178/238)

- 156 is `runtime_.spawn` at `obj->reference_point`, operand layout `[U8, Pstr, Code32]`
  (`ScriptVm.cs:1573`, `MdkObject.cs:574-582`). Both helpers already existed for the
  earlier spawn cases, so the dispatch is three lines with no new subsystem. R11/R4/R6
  clean (no division, no double, no libm in the dispatch).
- Test pins the oracle directly (no C# counterpart exists): BEAMS is a model of the
  DANT_1 arena (LEVEL7O.MTO) with no init script (no `DANT_1$BEAMS` key in the CMI
  object scripts — verified in the binary), so the spawn runs no nested script.
  Asserts type/arena/position (refpoint + position), spawn_position, yaw 0,
  instance sequencing (-1 takes next id twice), the out-of-range fallback to the
  position, and the clean `unimplemented()` bucket.
- Review-28 MINOR closed: 242 now requires `block.size() >= 13` before the draw loop
  (decoder-unreachable short blocks clear the mask instead of UB — commented as a
  deliberate deviation). NIT closed: the 242 test asserts all four points and
  colour/point preservation on the zero path.
- Verified: `vm_tests` 13/13; `make -C tests/scripts` → "All cases match the oracle."
  (goldens unchanged — 156 never fires in the corpus); `bash tools/check.sh` →
  ALL GATES PASSED (smdh SKIP, no .cia); `make -C mdk_3ds` → `mdk.3dsx` links.
  Brace-matched recount: C# 238 / C++ 178 / remaining 61 (true dispatches 177).
- Known pre-existing gap (out of scope, noted): C++ `spawn()` omits the C#'s
  `Stats.CountEnemy` call — affects all spawn opcodes equally, goldens don't observe it.
- Next: one opcode family per batch through the harness; 132/136 still need the
  Effects slice; 85 waits on the FrameBlend `rotation()` slice.

## Iteration 30 — review-29 findings closed: 156 test hardening + 242 comment scope (no new opcode)

- Review-29 (VERDICT: INCOMPLETE, no BLOCKER/MAJOR) left two MINOR test-strength
  gaps on the iteration-29 spawn slice, both closed here; coverage unchanged
  (C++ 178/238, remaining 61).
- Gap 1 (rotation/scale unpinned): the 156 test ran at yaw 0 / scale 1, so a
  sign error in `reference_point`'s RotZ or a missing `* scale` passed. Added a
  third spawn: parent yaw 90, scale 2, ref point (3,1,0.5) → hand-computed
  (8,26,6), asserted with 1e-3 tolerance (cosf(pi/2) ≈ -4.4e-8, not 0 —
  verified with libm before writing the test). Each wrong shape lands
  elsewhere (no scale: (9,23,5.5); no rotation: (16,22,6); transposed sign:
  (12,14,6)). Also pins `previous_position == position` and nonzero `restart`.
- Gap 2 (`restart` plumbing unpinned): the test never asserted the `code32`
  script operand reaches the object. Now asserts `created.restart != 0` AND
  that it points at the trailing kEnd byte of the rebuilt program
  (`readOffset` adds FileBase back, so the stored `label-4` decodes to the
  exact label offset — a wrong operand index would leave restart 0 and fail).
- Open question closed: the Beta dialect has no 242 mapping
  (`scriptBetaOpcodeInfo` returns null → `decodeBeta` rejects the instruction),
  so the "decoder-unreachable short block" hardening holds on both dialects;
  the `script_vm.cpp` comment now says so. No behavior change.
- Verified: `vm_tests` 13/13 PASS, zero FAIL; `make -C tests/scripts` →
  "All cases match the oracle." (goldens unchanged); `bash tools/check.sh` →
  ALL GATES PASSED (smdh SKIP, no .cia); `make -C mdk_3ds` → `mdk.3dsx` built.
- Next: one opcode family per batch through the harness; 132/136 still need the
  Effects slice; 85 waits on the FrameBlend `rotation()` slice.

## Iteration 31 — review-30 findings closed + opcode 81 push_hit_dir (179/238, remaining 60)

- Review-30 (VERDICT: INCOMPLETE, one MINOR + one NIT, no code bug) both closed
  in `vm_tests.cpp`: each 156 spawn now places a separator `set_var` between
  the spawn and its label and asserts `restart == labelOffset` exactly, so a
  fall-through (`ins.next`) regression fails (next points at the separator, not
  the label); the range guard is a single `restartInRange` boolean, no
  out-of-bounds read on a failed check. Applied to all three spawns
  (spawned/landed/go).
- Opcode 81 (`push_hit_dir`, ScriptVm.cs:601-607): `value(o[1])` (literal or
  variable) times `Dt` in mode 1 along `hit_direction`, x/y only, z untouched.
  Operands `[U8, Value]` confirmed against the opcode table (offset 123).
  Port sits after case 80 with a `see push_forward` R6 pin (cosf/sinf match the
  C# SinCos bit-for-bit per the probed exemption). New `vm_tests` test pins
  literal mode-0 (exact), literal mode-1 (30*Dt within 1e-6 of 1), and variable
  speed at heading 90 (+y within 1e-4). No C# counterpart exists to twin.
  81 never fires in levels 3-8, so goldens are unchanged by construction.
- Recount-verified: C# 238 / C++ 179 / remaining 60 (true dispatches 178; the
  179th label is the nested `case 0:` inside 249).
- Verified: `vm_tests` 14/14 PASS; `make -C tests/scripts` → "All cases match
  the oracle."; `bash tools/check.sh` → ALL GATES PASSED (smdh SKIP, no .cia);
  `make -C mdk_3ds` (DEVKITPRO/DEVKITARM from /opt/devkitpro) → `mdk.3dsx`.
- Next: one opcode family per batch through the harness; 132/136 still need the
  Effects slice; 85 waits on the FrameBlend `rotation()` slice.

## Iteration 32 — opcode 206 spawn_along_path (180/238, remaining 59) + review-31 doc fix

- Review-31 (VERDICT: INCOMPLETE, one MINOR + one NIT, no code bug) addressed:
  OPCODE_COVERAGE no longer claims 81's inputs are "all already ported" — only
  the `hit_direction` FIELD is ported; its writers (ChainGunHit, the mortar
  path, Twister, sniper rounds, TouchDamage) have no C++ counterpart yet, so in
  integrated play 81 always pushes due east until those slices land, and each of
  those slices' acceptance checks must re-verify 81 end-to-end. The R6 shorthand
  pin stays (consistent with cases 80/201/126; the gate-grammar question is
  flagged for the owner, not unilaterally changed).
- Opcode 206 (`spawn_along_path`, ScriptVm.cs:440-448): from time 0 to before
  the path's last key, one flagged spawn per step at `pathPosition(path, time)`,
  yaw 0, instance -1. Operands `[Data32, F32, Pstr, Code32]` confirmed against
  the opcode table (offset 425). The float accumulation is oracle-exact by
  design (an integer-count rewrite would shift spawn positions by ulps); a zero
  step hangs the oracle identically, so this matches it. New `vm_tests` test on
  LEVEL7.CMI path 53729 (3 keys, frames [0,5,10], step 5 → times 0 and 5):
  count, exact positions, flagged flags, instance chain, restart operand, clean
  unimplemented bucket. Mutation check: 15 checks fail with the port reverted,
  green with it applied.
- Recount-verified: C# 238 / C++ 180 / remaining 59 (true dispatches 179; the
  180th label is the nested `case 0:` inside 249); the remaining set matches the
  doc list exactly.
- Verified: `vm_tests` 15/15 PASS; `make -C tests/scripts` → "All cases match
  the oracle." (goldens unchanged — 206 never fires in the corpus);
  `bash tools/check.sh` → ALL GATES PASSED (smdh SKIP, no .cia);
  `make -C mdk_3ds` → `mdk.3dsx`.
- Next: one opcode family per batch through the harness; 132/136 still need the
  Effects slice; 85 waits on the FrameBlend `rotation()` slice.

## Iteration 33 — review-32 hardening batch (180/238, remaining 59, gates green)

- Review-32 (VERDICT: INCOMPLETE, 1 MAJOR + 3 MINOR + 1 inherited, no code
  bug in the 206 port) addressed as one hardening batch (no new opcode):
- MAJOR (float-accumulation pin gap): `testSpawnAlongPathSpawnsEveryStep`
  gains a second run with non-exact step 0.1f to end 10 — 100 spawns, each
  position bit-compared against `pathPosition` at the accumulated time, plus
  a `differ > 0` guard proving the case discriminates accumulation from
  `i*step`. Independent oracle corroboration: a throwaway .NET 10 program
  (outside the repo, `/tmp/kilo/acc`, SDK at `/tmp/kilo/dotnet10`) running
  the exact C# loop (`for (var time = 0f; time < end; time += step)`,
  ScriptVm.cs:445) prints `count=100 last=9.900002 firstDiff=7
  acc7=3F333334 mul7=3F333333` — the count and the first-divergence hex
  pinned in the test comment come from the oracle toolchain, not the port.
- MINOR (absolute positions): the exact-step case now asserts literal
  `x/y/z == 0` for t=0 and `z == 6.0f` for t=5, so a Hermite regression
  moving both sides together fails here instead of passing silently.
- MINOR (T2 pin grammar): NOT added as a pin — proven by direct test that
  `EXEMPT_RE` in `tools/runner/gates_vfp.py` accepts R-rules only, so a
  `/* @vfp-exempt T2 ... */` pin would trip a MUST "invalid @vfp-exempt
  grammar" finding and break `tools/check.sh`. The 206 loop keeps a prose
  rationale that names T2 and records the owner-grammar decision, consistent
  with the review-31 precedent. The gate author (owner) owns the grammar
  extension; the port does not change gates unilaterally.
- MINOR (doc): `docs/OPCODE_COVERAGE.md` movement/AI grouping no longer lists
  206 as remaining (it landed in iter 32; the remaining-59 set already
  excluded it).
- Inherited (degenerate path): recorded in CONTINUATION open item 3 —
  `pathKeyCount`/`pathKeyFrame` return 0 on invalid paths where the C#
  throws (span reads); 206 inherits the divergence, unreachable in shipped
  scripts (206 never fires in the corpus).
- Verified: `vm_tests` 15/15 PASS ("All VM unit tests match the oracle.",
  incl. hardened 206 test); `make -C tests/scripts` exit 0, 376 PASS lines,
  0 FAIL, "All cases match the oracle." (goldens unchanged); `bash
  tools/check.sh` → ALL GATES PASSED (smdh SKIP, no .cia); `make -C mdk_3ds`
  → `mdk.3dsx` (118 MB, rebuilt 2026-10-26).
- Next: one opcode family per batch through the harness; 132/136 still need
  the Effects slice; 85 waits on the FrameBlend `rotation()` slice.

## Iteration 35 — stats slice: opcode 217 via new GameStats (181/238, remaining 58, gates green)

- Ported 217 (`global_573c4c_add`) with a new header-only
  `mdk_core/src/scripts/game_stats.h`: a 1:1 port of
  `src/Mdk.Game/Scripts/GameStats.cs` — the exact 33-entry enemy table
  (order-verified against GameStats.cs:14-17), `isEnemyType`
  (`ToUpperInvariant` → `toUpperAscii`, ASCII-identical on these names),
  `countEnemy` (Created/Killed split), `percent` (truncating shape,
  0 without shots). Header-only on purpose: no object file, so no
  `notdir` collision in the 3DS build (same reason as
  `formats/ascii_case.h`). `GameStats.IndexOf` deliberately NOT ported —
  it is `GameState` order (save-game/Viewer slice), nothing ported reads it.
- `ScriptRuntime` gains a `stats` member (zero-initialised, which is what a
  fresh level has); `case 217` writes `headShots` only when the first
  operand is 1, exactly `ScriptVm.cs:1606-1611`. Other writers (chain-gun
  fire, sniper rounds, kill path, town flags) land with their own slices.
- Pinned by `HeadShotsAddOnlyWhenEnabled` in `vm_tests` (no C#
  `ScriptVmTests` counterpart exists — the test pins the oracle directly:
  +3/+2 accumulate, first-operand 0 counts nothing, plus `isEnemyType`
  (`xb1` yes / `KURT` no), `countEnemy` created/killed separation, and
  `percent` truncation incl. 0/0). Goldens unchanged — the counters are
  not dumped (dump-field template does not apply).
- Mid-iteration gate fix (no gate relaxed): the R11 screen flagged
  `hits * 100 / shots` via file-level loop inference from `isEnemyType`'s
  table scan; pinned `/* @vfp-exempt R11 ... */` with a level-end-only
  rationale (Score-o-matic math runs at most once per level, never per
  frame; float math would change the truncation) — same surface as
  `renderer_c3d.cpp:1130`.
- Recount-verified: C# 238 / C++ 181 / remaining 58 (true dispatches 180;
  the 181st label is the nested `case 0:` in 249); remaining set matches
  the doc list exactly (217 removed).
- Iteration mechanics: the iter-35 worker was killed by subscription quota
  exhaustion (API 429) AFTER the two code commits landed but BEFORE the
  state commit — iter 36 inherited a dirty tree (`docs/CONTINUATION.md`
  only), re-verified all three gates on the final tree itself, appended
  this narrative, and committed the state. No code changed in iter 36.
- Verified (iter 36, on the final tree): `vm_tests` 16/16 PASS ("All VM
  unit tests match the oracle.", incl. the new 217 test); `make -C
  tests/scripts` (after `clean`) exit 0, "All cases match the oracle."
  (goldens unchanged); `bash tools/check.sh` → ALL GATES PASSED (smdh
  SKIP, no .cia); `make -C mdk_3ds` (with
  `DEVKITARM/DEVKITPRO=/opt/devkitpro/...`) → `mdk.3dsx`.
- Next: one opcode family per batch through the harness; 132/136 still need
  the Effects slice; 85 waits on the FrameBlend `rotation()` slice.

## Iteration 37 — vision-condition slice: opcode 243 if_point_sees_player (182/238, remaining 57)
- `case 243` in `ScriptVm::execute` (script_vm.cpp): `reference_point(ival(o[0]))`
  within `fval(o[1])` of `kurtPosition()` (feet) AND `raycastClear(point, chest)`
  with chest = feet + 4z — exactly `ScriptVm.cs:1703-1708`. The *u16* range rides
  an int operand on both sides (ScriptDecoder.cs:217), so fval's int arm is the
  C# F int arm, exact. Single `&&` expression keeps the C# short-circuit (NaN
  distance is false, never a raycast). sqrtf matches the established bit-for-bit
  glibc pattern; 243 never fires in the corpus (cold), no pin needed.
- Pinned by `testPointSeesPlayerNeedsRangeAndAClearLine` in `vm_tests` (no C#
  `ScriptVmTests` counterpart — pins the oracle directly): at-feet/range-1000
  sees (feet->chest clear at the L7 spawn), 5000-east/range-100 misses (distance
  gate, geometry-free), ref-0-5000-east with body at feet misses (pins o[0] as
  ref index, geometry-free), 100-below/range-1000 misses (segment crosses the
  ground — pins the raycast-false path). New `Code::u16` helper (little-endian,
  matching binU16). `OPCODE_COVERAGE.md` recounted: C# 238 / C++ 182 / 57
  (true dispatches 181); 243 removed from the camera/cutscene group (it never
  needed the camera — only reference_point/kurtPosition/raycastClear).
- Review-36 (stats slice) was VERDICT COMPLETE: MINOR house-wide int-overflow
  note (no action — unreachable magnitudes), NIT negative-add coverage gap
  (deferred to next touch of that test). Nothing to address.
- Verified on the final tree: `vm_tests` 17/17 PASS ("All VM unit tests match
  the oracle."); `make -C tests/scripts` exit 0, "All cases match the oracle."
  (goldens unchanged); `bash tools/check.sh` → ALL GATES PASSED (smdh SKIP,
  no .cia); `make -C mdk_3ds` → `mdk.elf` + `mdk.3dsx`.
- Next: one opcode family per batch through the harness; 132/136 still need
  the Effects slice; 85 waits on the FrameBlend `rotation()` slice.

## Iteration 38 — value-slot slice: opcodes 199 set_104 and 210 set_2c0 (184/238, remaining 55)
- Inherited a half-batched tree: `vm_tests.cpp` already carried
  `testUnidentifiedValueSlotsStoreTheirOperand` (uncommitted), but neither
  dispatch case nor the main() registration existed. Finished exactly that:
  `case 210` / `case 199` in `ScriptVm::execute` (script_vm.cpp) —
  `obj->value_2c0 = value(obj, op(ins, 0))` / `obj->value_104 = ...`,
  exactly `ScriptVm.cs:1663-1668` (`obj.Value2c0/Value104 = Value(obj, o[0])`).
  The C# itself labels both "not identified": the write IS the oracle
  behaviour, both fields were already on `MdkObject` (object.h:136-137), and
  no C++ code reads them, so there is nothing to diverge — same shape as the
  ported 205 (`set_21f`) / 218 (`set_f13c`). No new pin needed (no float→int,
  no loop, no transcendental; plain stores).
- Test (twin of nothing — no C# `ScriptVmTests` counterpart, pins the oracle
  directly): literal operand lands bit-for-bit in its own slot (7.5f /
  -2.25f), variable operand resolves through the shared `Value()` helper
  (own var 0 feeds both slots), and the `unimplemented()` bucket stays clean
  for both opcodes. Registered in main() after the 243 test.
- Review-37 was an environment failure (reviewer quota exhausted, 429), no
  findings to address.
- `OPCODE_COVERAGE.md` recounted with the brace-match script: C# 238 / C++
  184 / remaining 55 (true dispatches 183; the 184th label is still the
  nested `case 0:` in 249). 199/210 removed from the remaining list and from
  the "Not identified (no safe port exists)" group — the group note now
  records that porting the write is exact and only a future *reader* for the
  slots would need reclassification.
- Verified on the final tree: `vm_tests` 18/18 PASS ("All VM unit tests match
  the oracle."); `make -C tests/scripts` exit 0, "All cases match the oracle."
  (goldens unchanged — neither opcode fires in levels 3-8); `bash
  tools/check.sh` → ALL GATES PASSED (smdh SKIP, no .cia); `make -C mdk_3ds`
  (DEVKITPRO/DEVKITARM=/opt/devkitpro/...) → `mdk.elf` + `mdk.3dsx`.
- Tooling note: the `commit` tool silently produced no commit twice (HEAD
  unchanged after "commit attempt finished"); landed the batch with a direct
  `git add -A && git commit` (cc09097).
- Next: one opcode family per batch through the harness; 132/136 still need
  the Effects slice; 85 waits on the FrameBlend `rotation()` slice.


## Iteration 39 — opcode 239 `set_27c` explicit no-op

- Ported opcode 239 in `mdk_core/src/scripts/script_vm.cpp` as an explicit
  no-op, matching `src/Mdk.Game/Scripts/ScriptVm.cs:1783`. The decoder's
  generated layout already consumes six F32 operands; no behavior was inferred
  from the unidentified field name. The `vm_tests` synthetic program gives it
  six floats, then writes a second object variable. It verifies the no-op,
  clean `unimplemented()` bucket, and correct instruction alignment by checking
  that the following write executes.
- C++ dispatch coverage recalculated against brace-matched method spans:
  C# has 238 unique cases; C++ has 189 numeric labels, 188 unique labels, of
  which 187 are dispatches (the nested `case 0` in opcode 249 isn't an opcode).
  Thus 51 remain. Added 239 to the completed list and adjusted the deferred
  dispatch count; this is a one-opcode batch, no golden changes.
- Review-37 was unavailable (reviewer quota 429); no newer adversarial review
  was present. Self-review confirmed the C# no-op and decoder's six-F32 record.
- Verification on this tree:
  - `make -C tests/scripts` → VM tests 20/20 PASS; full suite ended with
    `All cases match the oracle.`
  - `bash tools/check.sh` → `CHECK: ALL GATES PASSED`; SMDH check SKIPPED
    (no CIA built).
  - Bare `make -C mdk_3ds` failed because `DEVKITARM` was unset. Retried as
    `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C
    mdk_3ds` → linked `mdk.elf`, staged 69 RomFS files (79M), built
    `mdk.3dsx`.
- No Original 3DS or emulator run was performed; this opcode has no rendered
  output and hardware performance remains unmeasured.

## What this batch did (iteration 42 — opcode 85 oracle-rounding review fixes)

- Addressed the external review's two MAJOR findings: `rotation()` now uses the .NET `float.DegreesToRadians` evaluation order (multiply by pi, then divide by 180), and matrix composition uses System.Numerics' first product followed by three fused multiply-adds in `Vector4.Transform` order.
- Replaced the loose orientation tolerance assertions with a full 16-element exact-bit fixture for roll=30, pitch=20, yaw=40, generated by the .NET 10 C# oracle. Removed stale comments claiming opcode 85 and `rotation()` were deferred.
- Verification: `make -C tests/scripts clean && make -C tests/scripts` → all VM cases, goldens, and supporting checks PASS, ending `All cases match the oracle.`; `bash tools/check.sh` → `CHECK: ALL GATES PASSED` (SMDH SKIPPED: no `.cia`); `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds` → `mdk.elf` linked, 69 RomFS files staged, `mdk.3dsx` built. `git diff --check` clean.
- Toolchain check: devkitARM GCC 15.2.0; ARM object disassembly references `fmaf` for the matrix FMA sequence. This does not establish hardware behavior or performance: no Original 3DS or emulator run was performed; Old-3DS readiness remains unmeasured.

## What this batch did (iteration 41 — opcode 85 `set_rolling`)

- Added `MdkObject::rotation()` using row-vector `Mat4` composition matching the C# `RotationOf(roll, -pitch, yaw)` order, plus `set_rolling(bool)` preserving the transition semantics: enabling captures only on off→on; disabling clears the flag without changing the saved basis.
- Added opcode 85 dispatch and a synthetic VM test for a 90° yaw basis, no recapture while already rolling, basis preservation on stop, and recapture on the next start. The oracle case is `MdkObject.SetRolling` / `Rotation` (`src/Mdk.Game/Objects/MdkObject.cs:279-285,327-341`); no level golden changes because opcode 85 is unreachable in the level 3-8 corpus.
- Raw trig in the new gameplay orientation path is R6-pinned because using the approximate LUT would perturb the saved gameplay basis. The first `tools/check.sh` run caught the missing pin; the pin was added and the complete gate rerun passed.
- Updated opcode counts: 189 true C++ dispatches / 238, 49 remaining; opcode coverage now includes a direct VM test for 85.
- Verification after the final code/doc edits: `make -C tests/scripts` → VM tests PASS including `SetRollingCapturesRotationOnTransition`; all table/script, DTI, scene checks PASS and `All cases match the oracle.`; `bash tools/check.sh` → `CHECK: ALL GATES PASSED` (SMDH gate skipped: no built `.cia`); `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds` → linked `mdk.elf`, staged 69 RomFS files and built `mdk.3dsx`; `git diff --check` clean. No OG hardware capture was performed. Latest external review remains unavailable: review-37 records quota exhaustion; this iteration self-reviewed the C# matrix order and transition behavior.

## What this batch did (Phase 2 iteration 15 — review-14 opcode-170 hardening)

- Addressed review-14's MINOR constructor finding. `ScriptRuntime`'s borrowed
  `Player` argument no longer has a `nullptr` default. The six `tick1.cpp`
  constructions and all synthetic-test constructions now state their choice
  explicitly: harness-only runtimes pass `nullptr`, while the opcode-170 case
  passes the live `Player`. This matches the C# constructor's mandatory Kurt
  at the API boundary and prevents a future frontend from accidentally
  omitting the binding.
- Addressed review-14's move/drain-order NIT. The opcode-170 VM case now pins
  the C# `Kurt.cs:221-234` walk displacement from the PRE-drain knock-down
  push as well as the existing post-drain velocity. Tightened the matching
  `Player::update` comment to state that the move captures the pre-drain push,
  then the push drains before the walk move is applied and before the vertical
  update consumes the move.
- Mutation-checked the new ordering pin in a temporary source copy: moving
  `drainKnockdownPush` above the move computation made `vm_tests` exit 1 on
  exactly the two new displacement assertions. The source was restored before
  the final clean build.
- No oracle behavior changed and no golden changed. Verification on the final
  tree: `make -C tests/scripts clean && make -C tests/scripts` exited 0 after
  41 VM tests and the complete oracle suite (`All cases match the oracle.`);
  `bash tools/check.sh` exited 0 (`CHECK: ALL GATES PASSED`, SMDH SKIPPED with
  no built `.cia`); `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM
  make -C mdk_3ds` exited 0, linked `mdk.elf`, staged 69 RomFS files (79M),
  and built `mdk.3dsx`; `cmake --build build-linux --target mdk` exited 0.
- No emulator or Original 3DS run was performed. Old-3DS timing remains
  unmeasured. Coverage remains 206 true C++ dispatches / 238 with 32
  remaining; the next isolated opcode family is 202 (`set_574304`).

## What this batch did (Phase 2 iteration 24 — P5 HUD canvas/state fidelity)

- Closed the first measured renderer slice after P5.0. The frame golden's C++
  baseline moved from `2b586df212dc8ed8` to `4da3474d1c5533f0`, and the measured
  gap to the C# oracle fell from **12.680% / max 255 / mean 5.1350** to
  **7.930% / max 255 / mean 2.1995** at tolerance 0. The oracle hash itself is
  unchanged (`b60e72552c40…`), so the improvement is measured against the same
  reference bytes.
- Fixed the canvas coordinate space. The seam and Vulkan backend had a fixed
  640x360 canvas, while the C# oracle defines `CanvasWidth = 360 * aspect`.
  Added `mdk_render_canvas_width(MdkRenderer)` to the flat seam, implemented it
  from the Vulkan target extent and the 3DS's 400x240 landscape aspect, and made
  `canvasOrtho` and `HudView::draw` use it. The fixed `kCanvasWidth` constant is
  gone so the old assumption cannot silently return.
- Removed the Linux frontend's fake HUD state. The previous frame deliberately
  drew inventory slots `{1,5}`, `{2,12}`, `{3,1}` and a 300/900 target bar. The
  C# starts with an empty `Inventory` and `ScriptRuntime.GetBar()` returns
  `(0,0)` when no target bar is active, so those were stand-ins masquerading as
  state. The frontend now sends only the live player health/death state and the
  default empty inventory/bar fields.
- Ported the HUD blink cadence and skull fade timing. The C# advances
  `HudView._blink` once per 30 Hz boundary (`Viewer.cs:535-538`), not once per
  60 Hz frame; the frontend now accumulates the same boundary. A dead player's
  skull alpha now follows `HurtFlash / 255` with `SkullFade = 2*Ticks = 60/s`
  (`Kurt.Combat.cs:26-27,238-253`). The frontend still respawns in place after
  the fade because menu/save/level-end flow remains unported.
- Review-23 had no BLOCKER or MAJOR finding. Its minors (stale C3D depth comment,
  oracle-pixel hash verification, frame-number guard, lavapipe extension wording,
  and normalize epsilon) remain open and were not mixed into this renderer slice.
- Verification on the final tree:
  - `make -C tests/scripts clean && make -C tests/scripts` exited 0 and ended
    `All cases match the oracle.`
  - `bash tools/check.sh` exited 0 (`CHECK: ALL GATES PASSED`); SMDH SKIPPED for
    no built CIA, and `gates_frames` PASS with
    `L3_f1.bmp sha=4da3474d1c5533f0`.
  - `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C
    mdk_3ds` exited 0, linked `mdk.elf`, staged 69 RomFS files (79M), and built
    `mdk.3dsx`.
  - `cmake --build build-linux --target mdk -j8` exited 0.
  - `tests/frames/regenerate_reference.sh` exited 0. The oracle ran on display
    `:99` inside a private mount namespace on lavapipe/llvmpipe and reported
    `Level 3: 17 arenas, 9708 triangles`, `Kurt at -4.00 0.00 192.98 yaw 96`.
- No VM golden changed and opcode coverage is unchanged at 211 true dispatches /
  238 with 27 deferred. No emulator or Original 3DS run was performed; Old-3DS
  timing remains unmeasured.

## What this batch did (Phase 2 iteration 25 — P5 sampler address mode: index textures REPEAT)

- Closed the second measured renderer slice. The frame golden's C++ baseline
  moved from `4da3474d1c5533f0` to `f4f91150cd35fd56`, and the measured gap to
  the C# oracle fell from **7.930% / max 255 / mean 2.1995** to **4.398% / max
  255 / mean 1.4939** at tolerance 0. The oracle hash is unchanged
  (`b60e72552c40…`), so the improvement is measured against the same reference
  bytes, and the manifest was rewritten by `regenerate_reference.sh`, not by
  hand.
- **The defect: one sampler where the oracle has two.** `Renderer.cs:249-250`
  creates a REPEAT and a CLAMP sampler, and `DrawOne` binds them per resource
  (`Renderer.cs:1098-1099`): the surface's index texture with REPEAT, its
  palette with CLAMP. `BindPanorama` binds CLAMP for the sky's panorama and its
  palette (`Renderer.cs:1029-1030`). The C++ had a single CLAMP sampler for
  everything.
- Why that is visible and not a technicality: the arena's texture coordinates
  are in TEXELS and are normalised by the texture's size
  (`LevelView.cs:327`, ported at `arena_scene.cpp:113`), so a surface that shows
  a texture more than once has a coordinate above 1. The original tiles the
  texture there; a clamped sampler stretches the edge texel over the rest of the
  surface instead, which is what the C++ did — the cobblestone ground and several
  walls were flat brown. The palette is fetched by texel (`Load` / `texelFetch`)
  in both implementations, so its address mode is moot and it stays clamped
  exactly as the oracle binds it.
- The Vulkan backend now creates `samplerRepeat` alongside `sampler` (same
  NEAREST filters and `maxLod = 0`; the C#'s `CreateSampler` sets all three
  address axes to the one mode, `Renderer.cs:504-506`, and the C++ matches it
  even though W is unused for a 2D view). `getDescriptorSet` is keyed by
  `(texture, palette, repeat)` because the sampler is baked into the set: the
  mesh path and the canvas path ask for repeat — the canvas quads go through the
  same `DrawOne` — and the sky asks for clamp. The descriptor pool (4096 sets)
  is not at risk from the extra variant, and an allocation failure returns
  VK_NULL_HANDLE so the draw survives — though the caller then draws with no set
  bound, which is undefined shader reads, so "survives" is the honest word and
  not "handles".
- Verified against the oracle's pixels, not just the hash: the pinned frame was
  measured band by band. Top third (sky) 5.625% bad / mean 1.4922 / max 99;
  middle 3.816% / 2.1723 / 247; bottom (ground and HUD) 3.754% / 4.3832 / 255.
  The ground tiling is now structurally the oracle's; the sky is the largest
  remaining band and is washed out rather than missing (max delta 99).
- Deferred with cause, the 3DS half. `renderer_c3d.cpp` never calls
  `C3D_TexSetWrap`, so its wrap field is 0, which is not `GPU_REPEAT`
  (`3ds/gpu/enums.h:39`), and it clamps like the Vulkan backend used to. It
  cannot take the same one-line change: the 3DS pads index textures to
  power-of-two with TRANSPARENT padding and rescales the UVs by real/padded in
  the vertex shader (`renderer_c3d.cpp:1133-1147`), so a plain REPEAT would tile
  at the PADDED period and sample the transparent padding — index 0, which
  discards — punching holes in the walls instead of tiling them. That needs the
  padding filled with the tile pattern (exact only when the real size divides
  the padded one) or a different UV scheme, and it is unverifiable without
  hardware, so it is recorded rather than guessed at.
- Review-24 had no BLOCKER or MAJOR finding. Its MINOR was the stale divergence
  record: the death comment still said "a fixed 4.0 s" after the constant became
  `kSkullEnd / kSkullFadePerSecond` = 4.25 s. The comment now records 4.25 s and
  says why the C#'s shorter beat (dying with `HurtFlash` already partway up) is
  unreachable here — only because the alive-state damage flash is not ported, so
  `state.hurtFlash` is 0 while alive. Both NITs are fixed too: the rewrapped
  canvas-ortho comment that lost its possessive, and the 3DS canvas-width
  constants' orientation hazard is now stated where the ratio is written.
- Verification on the final tree:
  - `make -C tests/scripts clean && make -C tests/scripts` exited 0 (6m50s) and
    ended `All cases match the oracle.` It was re-run incrementally after the
    last 3DS-only comment edit and exited 0 again with the same last line.
  - `bash tools/check.sh` exited 0 (`CHECK: ALL GATES PASSED`); SMDH SKIPPED for
    no built CIA, and `gates_frames` PASS with `L3_f1.bmp sha=f4f91150cd35fd56`.
  - `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C
    mdk_3ds` exited 0, staged 69 RomFS files (79M) and built `mdk.3dsx`, both
    before and after that comment edit.
  - `cmake --build build-linux --target mdk -j8` exited 0.
  - `tests/frames/regenerate_reference.sh` exited 0 in 24 s. The oracle ran on
    display `:99` inside a private mount namespace on lavapipe/llvmpipe; the C++
    capture ran on `DISPLAY=:99` with the headless backend. The oracle hash came
    out identical to the committed one.
- No VM golden changed and opcode coverage is unchanged at 211 true dispatches /
  238 with 27 deferred. No emulator or Original 3DS run was performed; Old-3DS
  timing remains unmeasured.

## What this batch did (Phase 2 iteration 26 — P5 sky: the camera's own axes, and .NET's own inverse)

The sky was the largest remaining band (5.625% of its pixels differing, max
channel delta only 99), and CONTINUATION recorded it as a washed-out panorama
rather than missing geometry. It was two separate last-bit divergences in how the
sky's clip-to-direction matrix is built, both against `CameraMath.View`
(`src/Mdk.Game/Kurt/FollowCamera.cs:395-401`):

- **The axes.** The C# builds the sky rotation as
  `Matrix4x4.CreateLookAt(Vector3.Zero, camera.Forward, camera.Up)` — the follow
  camera's OWN `Forward`/`Up` fields. The C++ took the rotation out of the view
  matrix instead (copy its first three columns, zero the translation). Those are
  not the same floats: `buildLookAtCamera` recovers forward as
  `normalize((position + forward) - position)`, and that cancellation lands on a
  different ULP than the `forward` that went in. `buildFollowCamera` and
  `buildFollowCameraCleared` now store the exact `followCameraOrient` axes on
  `Camera::forward`/`Camera::up` after building the view, and `drawSky` reads
  them. `Camera` already had those fields; nothing else in either backend read
  them, so the change is confined to the sky. `drawSky` also now normalizes
  `zaxis`, which `CreateLookAt` does and the previous code skipped — it happens
  to be a no-op for this frame's value (byte-identical capture), but it is what
  the oracle does and it is not a no-op in general.
- **The inverse.** `Matrix4x4.Invert` is not "the cofactor formula". It is a
  fixed SSE shuffle/multiply-add sequence whose `MultiplyAddEstimate` lowers to a
  fused operation, and the cancellation in the 3x3 minors makes that fused
  rounding observable. `mdk_linux/src/render/system_numerics_inverse.cpp` is a
  mechanical translation of that sequence, `tN` names kept in IL evaluation
  order so nobody "simplifies" it. It is a genuine inverse — checked standalone
  against a random matrix, `M * M^-1 = I` to 2.4e-7 — and it rejects a singular
  matrix the way .NET rejects one below `float.Epsilon`, which the old generic
  cofactor version did not.

Measured against the oracle's pixels on the pinned frame (level 3, 320x240,
seed 1, frame 1, tolerance 0), and re-measured band by band:

| | bad % | mean channel delta | sky band | middle | ground+HUD |
|---|---:|---:|---|---|---|
| before (`f4f91150cd35`) | 4.398 | 1.4939 | 5.625% / 0.8416 | 3.816% / 1.6067 | 3.754% / 3.5272 |
| this batch (`3d67efbeab85`) | **3.323** | **1.3783** | **3.023% / 0.4664** | 3.191% / 1.5197 | 3.754% / 3.5272 |

The ground band is byte-identical before and after, which is the check that this
touched only the sky. Max delta stays 255 and comes from the ground band, not the
sky (whose max is 99).

Two ablations, because a claim that a 200-line translation "helps" has to be
measured and not asserted:

- exact axes + the OLD cofactor inverse: 3.862% / mean 1.4376, sky band 4.461%.
  So the inverse translation is worth ~0.54 points on its own and the axes are
  worth ~0.53; neither half is decoration.
- An experiment that FAILED and is deliberately not in the tree: making
  `follow_camera.cpp`'s `normalize` divide by the length the way
  `Vector3.Normalize` reads, instead of multiplying by a reciprocal, REGRESSES
  the sky from 3.023% to 5.730%. The reciprocal form is what the oracle actually
  matches. Recorded so the next reader does not "correct" it into a regression.

The remaining sky difference is sparse and scattered (3–17 pixels per row out of
320, no band or edge), which is a last-bit direction landing on the far side of a
sharp panorama texel boundary, not a structural mismatch. The largest band is now
the ground (3.754% bad, mean 3.5272, max 255) and that is the next slice.

Review-25's two NITs are fixed in the previous entry: the `CreateSampler`
citation is `Renderer.cs:504-506`, and the descriptor-allocation-failure sentence
no longer claims the null-set path "covers" it (the caller still draws with no
set bound, which is undefined shader reads). Its MINOR about the descriptor-set
cache never being invalidated on `mdk_render_update_texture`/
`mdk_render_release_texture` is a real latent trap but has no caller anywhere in
the tree today, so it is a tracked follow-up for whoever wires up animated
textures, not this batch's subsystem. Its second MINOR — the 3DS still clamping
where Linux now tiles — is unchanged and still hardware-gated for the recorded
reason.

Verification on this tree:

- `make -C tests/scripts clean && make -C tests/scripts` exited 0 and ended
  `All cases match the oracle.` (clean rebuild, because a header was added). No
  VM golden changed.
- `bash tools/check.sh` exited 0: `CHECK: ALL GATES PASSED`, SMDH SKIPPED for no
  built CIA, `gates_frames PASS` with `L3_f1.bmp sha=3d67efbeab85c8a4`.
- `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds`
  exited 0 and built `mdk.3dsx` (69 RomFS files, 79M staged).
- `tests/frames/regenerate_reference.sh` exited 0. The oracle ran on display
  `:99` inside a private mount namespace on lavapipe/llvmpipe (`Level 3: 17
  arenas, 9708 triangles`, `Kurt at -4.00 0.00 192.98 yaw 96`); the C++ capture
  ran with `DISPLAY=:99` on the headless backend. The oracle hash came out
  byte-identical to the committed one (`b60e72552c40…`), so the reference moved
  not at all and only the C++ baseline and the ceiling did.

Opcode coverage is unchanged at 211 true dispatches / 238 with 27 deferred. No
emulator or Original 3DS run was performed; Old-3DS timing remains unmeasured.

## What this batch did (Phase 2 iteration 28 — arena depth layers and the depth pull)

Inherited a dirty tree from a timed-out iteration: the depth layers and the
depth pull, half-written. Finished it, hardened the two places where corrupt
arena data would have been undefined behaviour in C++, verified and committed.

**Why the feature exists.** The original draws with no depth buffer, back to
front, so a poster lying in its wall's plane simply covers the wall. With a
depth buffer the two fight for the same depth and flicker. The oracle therefore
gives every arena triangle a **depth layer** (`src/Mdk.Game/Level/Layers.cs`)
and draws it pulled towards the eye by a share of its distance
(`src/Mdk.Engine/Render/DepthPull.cs`): the corners stay exactly where they
were — moving a detail off its wall would open cracks to its neighbours — and
only its depth changes.

**What landed.**

- `mdk_core/src/render/arena_scene.cpp` — `computeArenaDepthLayers`, the shared
  geometry half of the C# `Layers.Of`: triangles grouped into planes keyed by a
  rounded normal and plane distance (`MathF.Round(value * 64)`, midpoint-to-even,
  reproduced exactly including the negative half-way cases), sorted biggest
  first with the earlier triangle winning a tie, and each smaller triangle put
  one layer above the highest bigger one it overlaps. The overlap is the C#'s
  Sutherland-Hodgman `ClipBy` in the plane's own 2D basis around the first
  corner (small numbers, so the cancellation stays small). `ArenaBatch` carries
  the layer, the run-break now breaks on it as well as on material and group,
  and `level_view.cpp` copies it into `MeshDraw::material.depth_layer`.
- `mdk_core/src/render/depth_pull.{h,cpp}` — `buildDepthPullMatrix`, the C#
  `DepthPull.Of`: 1/256 of the eye-to-surface distance per layer, 1/128 more
  for lines, expressed as `Matrix4x4.CreateScale(1-share, eye)` in the oracle's
  row-vector layout, i.e. translation `eye * (1 - scale)` — the product the
  oracle's SIMD path uses, not `centre - centre*scale`.
- `mdk_linux/src/render/renderer_vk.cpp` — `clip = world * depth_pull * view *
  projection`, the C# `Renderer.cs:1072` chain (`command.World *
  DepthPull.Of(view.Position, ...) * ViewProjection`). The pull is skipped
  entirely for the ordinary case (layer 0, triangles) so nothing changes for
  the overwhelming majority of draws.
- `tests/scripts/layer_tests.cpp` — the C# `LayerTests.cs` and
  `DepthPullTests.cs` cases run from `vm_tests`: the six synthetic arenas
  (poster on a wall, poster half on a wall, neighbours sharing an edge, poster
  on a poster, two equal triangles, a different plane), the two level-6 arenas
  (OLYM_5's hub rim 159 over the glass floor 115, OLYM_7's dark floor 2 under
  the mirror tile 56), and the depth-pull camera (eye -2222/3200/-2885, target
  one unit below, world up +Z, 1.2 rad, 0.5–20000) checking that a pulled point
  keeps its screen position to 1e-4 while its depth moves by more than four
  24-bit depth steps.

**Two corrupt-data guards, both UB in C++ and neither a fidelity choice.** A
triangle whose area is NaN (only a corrupt arena can do that) is dropped rather
than fed to `std::sort`: a NaN makes the comparator inconsistent, and an
inconsistent comparator is undefined behaviour in C++ where the C#'s merge sort
merely orders it arbitrarily. And `roundToEven` saturates instead of casting an
out-of-range float, which is UB here and `INT_MIN` there. `normalize` of a zero
vector and `MathF.Sign(NaN)` keep the documented degenerate-guards-return-0
divergence, noted at the site; neither is reachable from a real arena, because
the plane normal is a unit vector and the projection axis is chosen never to be
parallel to it.

**Measured, not assumed.** The whole computation on x86 at `-O2`, every arena of
a level: level 3 5.8 ms / level 4 24.0 ms / level 5 11.2 ms / level 6 18.6 ms /
level 7 10.5 ms / level 8 8.4 ms, worst single arena 10.3 ms (level 4 `MEAT_5`)
and 0.9 ms for the pinned level's worst. That is once per arena build — level
and arena load, never per frame — so it is cold by §2 and stays cold. The layers
are not decoration: non-zero in real geometry, from 5 of 1014 triangles in
`HMO_3` to 257 of 1042 in `OLYM_7`.

**The pinned frame did not move, and that is the honest result, not a missing
wire.** The C++ capture is byte-identical to the committed baseline
(`3d67efbeab85c8a4`), so `reference.sha256` was NOT regenerated and the ceiling
is unchanged at `bad_percent=3.323 max_delta=255 mean_delta=1.3783`. The reason
is that the coplanar details in the pinned view are already resolved by draw
order under `LESS_OR_EQUAL`, so pulling them nearer changes no pixel. That was
falsification-tested rather than trusted: forcing every mesh draw one layer
nearer changes 0.009% of the frame (max delta 58), which proves the matrix
reaches the GPU and the path is live.

Also in this batch: `tests/frames/capture_cpp.sh` sets `DISPLAY=:99`
unconditionally instead of `${DISPLAY:-:99}`. Inheriting the session's display
is exactly how a graphics run lands on the operator's screen (AGENTS.md P7);
this machine's session display is `:1`.

**The 3DS half.** `depth_pull.cpp` compiles into the 3DS build (the core glob)
and the layers are computed there, but `renderer_c3d.cpp` does not apply the
pull, because its depth state is still the open item below (depth test
disabled, not the C#'s `LESS_OR_EQUAL` test/write): with no depth test the
original's painter order already decides, which is the same reason the original
needed no pull. The ARM11 cost of the layer computation is UNMEASURED; the 3DS
builds one arena (the start arena) at startup, so the x86 worst case of ~10 ms
is the shape of it, not a measurement.

Verification on this tree:

- `make -C tests/scripts` exited 0 and ended `All cases match the oracle.`,
  including the new `PASS DepthLayersAndDepthPull`. No VM golden changed.
- `bash tools/check.sh` exited 0: `CHECK: ALL GATES PASSED`, SMDH SKIPPED for no
  built CIA, `gates_frames PASS` with `L3_f1.bmp sha=3d67efbeab85c8a4`.
- `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds`
  exited 0 and built `mdk.3dsx` (69 RomFS files, 79M staged).
- `cmake --build build-linux --target mdk -j8` exited 0, and `gates_frames.py`
  re-run against that fresh binary reproduced `3d67efbeab85c8a4`.
- The C++ frame capture ran with `DISPLAY=:99` on the headless backend. No
  oracle re-capture was needed (the reference did not change) and no emulator or
  Original 3DS run was performed; Old-3DS timing remains unmeasured.

Review-27 had no BLOCKER or MAJOR. Its MINOR is real and is recorded as the next
batch's first item, with the reviewer's verified fix: `dot()` in
`mdk_linux/src/render/system_numerics_inverse.cpp` reduces sequentially where
.NET's `Vector4.Dot` reduces pairwise, so the inverse is off by 1–2 ULP on
high-cancellation inputs (bit-exact on all 1305 realistic camera matrices, so
no current player symptom). It is a different subsystem from this one and is
deliberately not mixed into this batch.

## What this batch did (Phase 2 iteration 29 — .NET's exact reduction order)

Review-27's MINOR and review-28's MINOR were two last-bit fidelity errors in
renderer numerics. They are one small slice: both are the C++ side making an
assumption about the .NET oracle's arithmetic instead of reproducing it.

**The sky inverse's dot product.** `System.Numerics.Vector4.Dot` reduces its four
products as `(x + y) + (z + w)`. The C++ translation used
`((x + y) + z) + w`, which differs under cancellation. The determinant of
`Matrix4x4.Invert` is that dot, so one final-ULP difference scales every inverse
element. `system_numerics_inverse.cpp` now uses the oracle's pairwise order and
keeps the volatile roundings that prevent the compiler reassociating it. The
header and source no longer claim a literal mechanical translation: they state
what is actually preserved (the shuffle/FMA grouping and the dot order) and
what is not (identical generated instructions).

`tests/scripts/system_numerics_tests.cpp` is a new exact-bit golden for this
path. Its input is the third invertible matrix from .NET 10.0.402's
`Random(12345)` with values uniform over [-1, 1]; its expected inverse is the
exact `Matrix4x4.Invert` output from that runtime. The old sequential reduction
misses those bits; the pairwise reduction matches all sixteen. A temporary
differential harness over the same 500-matrix seed set found 500/500 exact
matches with the new reduction and its first mismatch at the third matrix with
the old one. The permanent test is linked into `vm_tests` and reports
`PASS SystemNumericsMatrixInverse`.

**The arena layer's corrupt-input boundary.** Review-28 correctly found that
`arena_scene.cpp`'s comment attributed .NET's cast result to x86 conversion
instructions. On .NET 10 `(int)MathF.Round(value)` saturates: NaN becomes zero,
positive overflow becomes `INT32_MAX`, and negative overflow becomes
`INT32_MIN`. The C++ guard now includes the exact negative bound
`-2147483648.0f`; in particular, `-2147483520.0f` falls through and casts to
itself instead of incorrectly becoming `INT32_MIN`. The comment now records
the .NET semantics rather than a mistaken `cvttss2si` story. This input is
unreachable from valid arena coordinates, but the port's corrupt-data path now
matches the oracle rather than merely avoiding undefined behaviour.

Review-28's other MINOR is latent and deliberately not changed: the C# does not
apply the depth pull (or view/projection) to an Overlay mesh, but no reachable
Overlay mesh path exists in C++ yet. The exact requirement is now recorded in
`docs/OPEN_ITEMS.md` for whoever adds that path.

Verification on this tree:

- `make -C tests/scripts clean && make -C tests/scripts build/vm_tests`, then
  the full `make -C tests/scripts`, exited 0. The latter ended `All cases match
  the oracle.` and included `PASS SystemNumericsMatrixInverse` and
  `PASS DepthLayersAndDepthPull`. No VM golden changed.
- `cmake --build build-linux --target mdk -j8` exited 0.
- `bash tools/check.sh` exited 0: `CHECK: ALL GATES PASSED`, SMDH SKIPPED for no
  built CIA, `gates_frames PASS` with `L3_f1.bmp sha=3d67efbeab85c8a4`.
- `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds`
  exited 0 and built `mdk.3dsx` (69 RomFS files, 79M staged).
- The pinned frame is byte-identical (`3d67efbeab85c8a4`), so the oracle and
  reference were not re-captured and the gap remains 3.323%. This batch changes
  no rendered pixel, so no new frame-record image was published.
- The Pi harness cannot run the Muse write-disabled validator (`muse exec` is
  unavailable until 2026-11-01); the external loop reviewer remains required.
  No emulator or Original 3DS run was performed, and Old-3DS timing remains
  unmeasured.

## What this batch did (Phase 2 iteration 30 — Kurt's sprite and his state machine)

The inherited dirty tree was this batch, unfinished: `kurt_sprite.{h,cpp}` new,
`Player`'s state machine, `ArenaSpace::floorCrossing`, `SoundBank::animation` and
the Linux wiring all present, nothing built, no gates, no frame. This iteration
finished it, fixed what the review of the diff turned up, and ran the gates.

**The sprite** (`mdk_core/src/game/kurt_sprite.{h,cpp}`, the C#
`Kurt/KurtSprite.cs`). A frame is a world-space quad at Kurt's feet: one sprite
pixel is `2 * depth * tan(fov/2) / 360` world units, the hotspot sits 101 pixels
above the feet, and the quad is drawn as two row bands split `FeetRise/pixel`
pixels up. The body band is scaled towards the eye by `InFront`, which brings it in
front of the nearest floor the eye's rays to a 3x3 grid of the frame's corners,
hotspot column and middle cross — `ArenaSpace::FloorCrossing` is the port of the
probe in `ArenaSpace.Probes.cs`, and a floor only counts while the feet are within
`FloorBelow` under its plane. The rows below the split get `Nearer`, two world
units nearer and never past half the depth, which is what keeps the shadow on
uneven floor. `Pick` is the C#'s: the ride pose (unreachable), then K_CHUTE's
closing frame 7 once on `ChuteClosing`, then the state's animation with the
chute's K_CHUTEC ping-pong past frame 4, then the loop/once wrap and the helmet's
reversed play. Frames are decoded and uploaded once at level load; the per-frame
path rewrites twelve vertices.

**The state machine** (`game/player.cpp`). The C#'s `Kurt` is one class; this port
splits movement (`Player`) from combat (`Kurt`), so the visual state now lives with
movement, which is the half that changes it. `UpdateState`, `Animate`,
`UpdateSpecial`, `UpdateDeath`, `SetState` and `RunRate` are ported with the
oracle's constants (`IdleDelay 6`, `MovingSpeed 1`, `TurnFrameDegrees 45`,
`CrashTicksPerFrame 2`, `CrashHoldTick 8`) and in its order, including the two
places the order is load-bearing: `Update` clears `ChuteClosing` before the death
branch, and a hard landing returns from `Update` before `UpdateState` so K_CRASHL
owns the next steps. A dead Kurt is no longer frozen: `UpdateDeath` keeps him
falling until he lands and then advances only the death frame. The states another
subsystem owns — firing, knock-down, getting up, slide, ledges, the ride, the beta
moves — have no writer here, so their oracle branches are taken the way they are
false, and `K_BCKUP`/`K_MUZZF` load but are never selected.

**Three fixes found by reading the diff against the oracle.** (1) The chute sway
computed `(frame - 5) % (last * 2)`, which is a division by zero when K_CHUTEC has
exactly one frame — the C# throws there, and a SIGFPE is not a behaviour to port,
so a one-frame sway animation now falls through to the ordinary frame. (2) `done`
was `count <= 0 || frame >= count - 1`; the C# has no such guard and reads
`frame >= count - 1`, which differs when a missing animation meets a negative
frame, so the guard is gone. (3) `Animate`'s Turn branch skipped the assignment when
K_TRN45 was missing where the C# multiplies by zero; it now matches. The frame
counts come from the loaded animations (`KurtSprite::frameCounts`), so a missing
animation is zero frames, as `frameCount` is in the C#.

**R11.** The frame wrap was the oracle's `((f % c) + c) % c`, two software
`__aeabi_idivmod` calls on a target with no integer divide. For a positive divisor
the first remainder is already in `(-c, c)`, so one remainder plus a conditional add
is the same value; the site is then pinned as the contract requires.

**The frame moved, and it moved a long way.** The pinned frame had no Kurt in it at
all, and the oracle does: the gap falls from `bad_percent=3.323 max_delta=255
mean_delta=1.3783` to `bad_percent=1.240 max_delta=247 mean_delta=0.3895`. By band
(max-channel delta, the comparator's own definition): the sky is unchanged at
3.023% / 0.8185 / 99, the middle third falls 3.191% -> 0.676% and the bottom third
3.754% -> 0.020% (max 58). Every remaining delta above 128 is one 8x5 patch at
x 174-181, y 127-131 — a detail on Kurt's own sprite, not a shading difference —
so the sky is now effectively the whole gap. `reference.sha256` was regenerated by
its own script, which re-captured the oracle first: the oracle reproduced
`b60e72552c40…` byte for byte, so the reference has not moved and only the
regression baseline and the (shrunk) ceiling changed. `docs/frames/2026-10-30-kurt-sprite.png`
and its `.txt` are committed with this batch.

Verification on this tree:

- `make -C tests/scripts clean && make -C tests/scripts` exited 0 and ended `All
  cases match the oracle.` No movement golden moved: the footing, walk and pickup
  cases still print the oracle's values, so the new state machine did not change
  where Kurt stands.
- `cmake --build build-linux --target mdk -j16` exited 0.
- `bash tools/check.sh` exited 0: `CHECK: ALL GATES PASSED`, SMDH SKIPPED for no
  built CIA, `gates_frames PASS` with `L3_f1.bmp sha=58615fc85d02e009`.
- `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds`
  exited 0 and built `mdk.3dsx` (69 RomFS files, 79M staged). `kurt_sprite.cpp` is
  core, so the 3DS links it; its backend implements dynamic meshes, but the 3DS
  frontend still binds no Kurt.
- `tests/frames/capture_oracle.sh` ran on `:99` inside the private mount namespace
  (`.NET 10.0.13`, llvmpipe, `Display: 1280x1024`, capture under `/tmp`) and
  reproduced the recorded oracle hash exactly.
- No emulator run and no Original 3DS hardware run was performed; Old-3DS timing
  remains unmeasured. The Pi harness still cannot run the Muse write-disabled
  validator (`muse exec` unavailable until 2026-11-01), so the external loop
  reviewer remains required.

## What this batch did (iteration 31 — review-30: the oracle's complete `Fall` operation)

Review-30's first MAJOR said a Kurt who dies by falling out of the arena freezes
in mid-air while the oracle's corpse keeps falling, because `updateDeath` skipped
`fallMove` on the `onFloor` branch. The reasoning that led there was right and the
conclusion was not, and the difference is one statement.

**The oracle's `Fall` is two things, not one.** `Kurt.cs:491-502` is `FallMove`
followed by the fall-out test, and that test assigns `Health = 0`,
`VerticalSpeed = 0f` **and `OnFloor = true`**. The review stopped after the move:
a zero-length sweep really does report no hit (`Bsp.TestPlane` bails on `den == 0f`),
and `FallMove` really does execute `OnFloor = false`. But below the arena the
post-move test then sets `OnFloor` back to true in the same step, so the oracle's
corpse does not fall away — it is held at the death point by the same assignment
that the port had moved out of `updateVertical`.

That was checked against the oracle rather than argued. A scratch .NET program
built against the read-only C# source and the real level data put Kurt 51 units
below his arena's bottom and stepped him: `state=Dead onFloor=True vertical=0`
with Z fixed at 52.982224 for 120 further steps.

**The fix is still the faithful one.** `Player::fall` is now the C#'s `Fall` —
`fallMove` then the 50-unit fall-out test — and `updateVertical`, `updateSpecial`
and `updateDeath` all call it, as the C# does. The port is now one function that
matches one oracle function instead of two copies of half of it, so the next
caller cannot get the wrong half. On a real floor the death step clears `OnFloor`,
the next gravity step re-lands Kurt and the creep snap restores his feet, which is
the case the removed comment had reasoned about correctly and over-generalised.

`tests/scripts/vm_tests.cpp` pins both halves: `DeathUsesTheOraclesCompleteFallOperation`
asserts the alternating `OnFloor` and the unchanged feet height for a corpse on a
floor, and that a corpse below the arena stays at its death Z across ten steps with
`OnFloor` true.

**Review-30's MINOR on the R11 pin was correct and is fixed.** The
`@vfp-exempt R11` pin sat on `wrap()`, which runs at most once per frame and is not
in a loop, so it suppressed nothing — and AGENTS.md §5 forbids hot-only R-rule pins
on cold code. The pin is gone and the reduction moved to
`Player::wrapAnimationFrame`, where it reads as what it is: the C#'s `Wrap`, one
remainder plus a conditional add, which the review proved equivalent for a positive
count. `WrapAnimationFrameIsTheOracleReduction` pins positive, negative and
zero-count behaviour.

**Review-30's NIT on the two stale lines is fixed.** `main.cpp`'s death comment
still described the early return this batch removed; it now describes the death
beat. And the iteration-30 narrative reported `mean_delta=0.3895` where the frame
manifest, the published sidecar and this batch's re-capture all say `0.1819`; the
`0.3895` was a transcription error in prose, never a gate input, and the
CONTINUATION now says so instead of repeating it.

**The two 3DS MAJORs are recorded, not fixed, and they are latent.** Both concern
a Kurt the 3DS frontend does not bind yet (`grep -rn KurtSprite mdk_3ds/src` is
empty). `Pass::DoubleSided` is a no-op on the Citro3D backend, which sets
`C3D_CullFace(GPU_CULL_BACK_CCW)` once per frame and never reads
`draw.material.pass` — and because the sprite mirrors `at.right` for a left
strafe, the mirrored quad winds the other way and would be culled. Separately,
eager residency uploads all 360 frames (~49.9 MiB of POT-padded RGBA8 against
SPEC's 6 MB budget) and the 3DS eviction cannot restore what it evicted. Both are
in `docs/OPEN_ITEMS.md` with the reviewer's measurements and the fix that does not
diverge from the oracle's vertex count, and each is its own next batch. The
narrower `KurtSprite` replacement/failure texture leaks are recorded too.

Verification on this tree:

- `make -C tests/scripts clean && make -C tests/scripts` exited 0 and ended `All
  cases match the oracle.`; the two new focused cases passed and no movement golden
  moved, so nothing about where a living Kurt stands changed.
- `cmake --build build-linux --target mdk -j16` exited 0.
- `bash tools/check.sh` exited 0: `CHECK: ALL GATES PASSED`, SMDH SKIPPED for no
  built CIA, `gates_frames PASS` with `L3_f1.bmp sha=58615fc85d02e009` — the
  pinned frame is byte-identical, as expected when no renderer path changed.
- `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds`
  exited 0 and built `mdk.3dsx`.
- `tools/publish_frame.sh review30-fall` published
  `docs/frames/2026-10-30-review30-fall.png` + `.txt` with
  `bad_percent=1.240 max_delta=247 mean_delta=0.1819`, committed with this batch.
- No emulator run and no Original 3DS hardware run was performed; Old-3DS timing
  remains unmeasured. The Pi harness still cannot run the Muse write-disabled
  validator (`muse exec` unavailable until 2026-11-01), so the external loop
  reviewer remains required.

## What this batch did (iteration 32 — review-30's Citro3D pass culling)

Review-30's second MAJOR was real even though the 3DS frontend does not bind
Kurt yet: `renderer_c3d.cpp` installed `GPU_CULL_BACK_CCW` once per frame and
never read `draw.material.pass`. The Vulkan backend culls only `Pass::Solid`, but
the Citro3D backend made every mesh subject to the same rule. That is wrong for
Kurt's sprite: the C# marks it `Pass::DoubleSided` because `at.right` mirrors
the quad for a left-facing view, and the mirror reverses the winding. The
mirrored sprite was therefore a back face under the frame default and vanished.

The Citro3D draw path now sets culling explicitly for each draw, matching the
Vulkan pipeline rule: `Pass::Solid` uses `GPU_CULL_BACK_CCW`; DoubleSided,
Mirror, Blended and OnTop use `GPU_CULL_NONE`. The backend restores
`GPU_CULL_BACK_CCW` after the draw. No duplicate winding or vertex was added, so
the oracle's geometry and vertex count are unchanged. The frame's initial
`GPU_CULL_BACK_CCW` remains as the explicit default and covers the diagnostic
triangle.

This closes review-30's `Pass::DoubleSided` MAJOR. It is still **not a hardware
result**: the code compiles for devkitARM, but no emulator or Original 3DS run
was performed, and the 3DS frontend still has no Kurt binding. The next
renderer slice is the separate texture-residency MAJOR: eager sprite residency
is ~49.9 MiB of POT-padded RGBA8 against the 6 MB budget, and the current 3DS
eviction destroys a texture without retaining enough source to restore it.

The latest external review also corrected the previous batch's narrative. Its
linked old/new probes showed the corpse's position was bit-identical before and
after the `Fall` refactor in both the floor and fall-out cases; only `OnFloor`
parity differed. Review-30's mid-air-hang MAJOR was therefore void, not fixed.
The current continuation and code comments now say that precisely. The review's
two small polish findings are also applied: `wrapAnimationFrame` cites
`Kurt.Wrap` (`Kurt.cs:712`) and the inline copy in `KurtSprite.Pick`
(`KurtSprite.cs:325`), rather than a nonexistent `KurtSprite.Wrap`, and the
floor-death test's Kurt now starts with health zero. These are comment/test
polish; they do not change runtime behaviour.

Verification on this tree:

- `make -C tests/scripts clean && make -C tests/scripts -j4` exited 0 and ended `All
  cases match the oracle.`; `DeathUsesTheOraclesCompleteFallOperation` and the
  other focused VM cases passed, and no movement golden moved.
- `cmake --build build-linux --target mdk -j16` exited 0.
- `bash tools/check.sh` exited 0: `CHECK: ALL GATES PASSED`, SMDH SKIPPED for no
  built CIA, and `gates_frames PASS` with `L3_f1.bmp sha=58615fc85d02e009`.
- `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds -j4`
  exited 0 and built `mdk.3dsx` (69 RomFS files, 79M staged).
- `tools/publish_frame.sh c3d-pass-culling` published
  `docs/frames/2026-10-30-c3d-pass-culling.png` + `.txt` with
  `bad_percent=1.240 max_delta=247 mean_delta=0.1819`. The PNG is byte-identical
  to the previous C++ frame, as expected because the changed renderer is the
  3DS backend and the pinned capture is Linux/Vulkan.
- No emulator run and no Original 3DS hardware run was performed; Old-3DS timing
  remains unmeasured, and the new culling state is compile-verified but not
  hardware-verified.

## What this batch did (iteration 33 — Citro3D cull-state caching and the `Pass` gap)

The newest external review found no BLOCKER or MAJOR in the previous Citro3D
pass-culling batch, but its two MINORs were actionable. The first was a real
per-draw cost introduced by the fix; the second was an incomplete coverage claim.

**Culling now changes only on a pass transition.** `C3D_CullFace` stores the mode
and unconditionally marks the effect block dirty; Citro3D flushes that dirty bit
at the next draw, and the flush rebinds the whole effect block rather than one
register. The previous code called it before and after every mesh, so an
all-Solid frame dirtied and flushed the block once per draw. `RendererC3D` now
caches the mode installed for the active render target. `begin_frame` installs
`GPU_CULL_BACK_CCW` for each eye and resets the cache; `mdk_render_draw_mesh`
calls `C3D_CullFace` only when the requested pass differs from the installed
mode and restores the frame default only on the transition back. An all-Solid
frame now makes no per-draw cull call. The behavior is unchanged; the cost is
what changed. The reviewer's cycle estimate remains an estimate — no Old-3DS
timing was measured.

**The `Pass` coverage claim is narrowed where it was incomplete.** The previous
batch closed the culling half of the oracle's `Pass` behavior, but the C# also
derives blend and depth-test/write from it. `renderer_c3d.cpp` still never calls
`C3D_AlphaBlend`, so `Pass::Blended` glass would draw opaque, and the
`OnTop`/`Overlay` depth exemptions are not modelled. `docs/OPEN_ITEMS.md` now
records that gap with the reviewer's arena measurements: level 3 has non-Solid
batches in 12 of 17 arenas, while the currently drawn start arena has 154 Solid
batches and no non-Solid batch. The deleted culling entry is not allowed to be
read as full `Pass` support.

Two narrative corrections from the same review are applied: the Citro3D comment
lists `Overlay` and notes the oracle's mesh/non-shadow conjuncts, and the
fall-out death record now says both `OnFloor` **and** `VerticalSpeed` parity
differed, not `OnFloor` alone. The mirrored-sprite sentence is phrased as the
static inference it is, because no 3DS or emulator run was made.

Verification on this tree:

- `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds -j4`
  exited 0 and built `mdk.3dsx` (69 RomFS files, 79M staged).
- `make -C tests/scripts clean && make -C tests/scripts -j4` exited 0 and ended
  `All cases match the oracle.` No golden moved.
- `bash tools/check.sh` exited 0: `CHECK: ALL GATES PASSED`, SMDH SKIPPED for no
  built CIA, and `gates_frames PASS` with `L3_f1.bmp sha=58615fc85d02e009`.
- `cmake --build build-linux --target mdk -j16` exited 0.
- `DISPLAY=:99 tools/publish_frame.sh c3d-cull-cache` published
  `docs/frames/2026-10-30-c3d-cull-cache.png` + `.txt` with
  `bad_percent=1.240 max_delta=247 mean_delta=0.1819`. The PNG is byte-identical
  to the previous C++ frame because the changed backend is 3DS-only.
- No emulator or Original 3DS hardware run was performed. The cull-state change
  is compile-verified only, and Old-3DS timing remains unmeasured.

## What this batch did (iteration 34 — bounded, restorable sprite residency)

Review-30's texture-residency MAJOR and its narrower replacement/leak MINOR are
now closed in code. The 3DS frontend still binds no Kurt and no Original 3DS
was available, so the new residency bound is compile-verified rather than
hardware-verified.

**Kurt's sprite frames are decoded at load and uploaded on first draw.** The
C# eagerly uploads and retains every frame. The port now retains the decoded
palette indices instead: `TRAVSPRT.BNI`'s 19 present Kurt animations contain
360 frames, measured at 5.30 MiB of indices, 49.94 MiB when expanded to the
3DS's padded RGBA8. `MdkCapabilities::sprite_residency_bytes` lets the backend
name its bound without an `#ifdef`: Vulkan answers 0 (no bound, the C#'s own
behaviour), while Citro3D answers 1 MiB. `SpriteResidency` applies that bound
by dropping the least-recently-drawn frame and never dropping the frame about
to be drawn. `PASS SpriteResidencyPolicy` pins the accounting, bound, victim
choice, replacement and reset; this is a port policy and therefore has no C#
twin.

**Citro3D eviction is reversible.** Every texture now keeps its source bytes
and palette in system memory. The draw path rebuilds an evicted texture from
that source instead of leaving an arena surface untextured for the session.
Texture replacement and release wait until `C3D_FrameBegin`, where the previous
queue has completed, when the old GPU copy may still be in flight. Released
texture slots can be reused without growing the id table for ever. The POT
expansion also copies each source row into its own padded row; copying a
non-square source as one contiguous run shifted every row after the first.

**The replacement and reload paths release what they drop.** Replacing an
animation releases its old frames. Re-loading `KurtSprite` releases the
previous level's drawn frames and clears the residency accounting. The palette
and dynamic mesh still need a level-scope release and remain recorded in
`docs/OPEN_ITEMS.md`. Vulkan now forgets cached descriptor sets before a
texture or palette image view is replaced or destroyed, so a reused id cannot
inherit a set naming the old view.

The newest review's three small documentation/comment findings are also
applied: the non-`Solid` arena count is 10 of 17 (not 12), the cull cache is
documented as global to the Citro3D context rather than per render target, and
the oracle's `Pass` line citations are corrected to `Renderer.cs:565,596,
606-607`.

Verification on this tree:

- `make -C tests/scripts clean && make -C tests/scripts -j4` exited 0. The focused
  run printed `PASS SpriteResidencyPolicy` and `All VM unit tests match the
  oracle.`; the complete suite ended `All cases match the oracle.` No golden
  moved.
- `cmake --build build-linux --target mdk -j16` exited 0.
- `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds -j4`
  exited 0 and built `mdk.3dsx` (69 RomFS files, 79M staged).
- `bash tools/check.sh` exited 0: `CHECK: ALL GATES PASSED`, SMDH SKIPPED for no
  built CIA, and `gates_frames PASS` with `L3_f1.bmp sha=58615fc85d02e009`.
- `DISPLAY=:99 tools/publish_frame.sh sprite-residency` published
  `docs/frames/2026-10-31-sprite-residency.png` + `.txt` with
  `bad_percent=1.240 max_delta=247 mean_delta=0.1819`. The PNG is byte-identical
  to the previous C++ frame, as expected because the pinned capture is
  Linux/Vulkan and the picture does not change.
- No emulator or Original 3DS hardware run was performed. The 1 MiB bound, the
  kept-source system-RAM cost and Old-3DS timing remain unmeasured.

## Iteration 35 (2026-10-31): the object pass's stage 1 — place, pose and layout

The priority order's next item was the object pass, and the frame gap's
`max_delta=247` patch is an unported scene element from it. This batch is the
pass's first stage: everything that decides *where an object's vertices are*
before anything is drawn. The draw stage is the next batch, and why it needs a
seam decision is written up in `docs/OPEN_ITEMS.md`.

**The place.** `MdkObject::transform()` is the C#'s
`CreateScale(Scale) * (rolling ? RollingBasis : Rotation) *
CreateTranslation(Position + UnitZ * Lift)`, composed in that order. The
System.Numerics element arithmetic that `rotation()` spelled out inline (one
rounded product followed by three `fma`, because .NET contracts the dot product
into FMA and the last bit depends on it) moved from a local lambda into
`mdk::multiply` in `util/matrix.h`, with `create_scale` and `create_translation`
beside it, so the composition and the rotation share one multiply rather than
having two that can drift.

**The pose.** `MdkObject::pose_parts()` is the C#'s `PoseParts`: the rest
vertices, or `bake(model, animation)[AnimationFrame]`, from caches shared by
every object (the C#'s static `RestPoses` and `Baked`) keyed by model address and
released by `forget_poses()`. `std::map`, because the reference handed out has to
survive later insertions. The out-of-range frame is clamped rather than thrown,
the same guard `ScriptRuntime::poseBounds` already uses; the C# would throw.

**The layout.** `game/object_view.{h,cpp}` is `ObjectView`'s layout stage:
batches by surface in first-seen order, each carrying the part, vertex index and
UV of every corner plus, per part, its corner count and the sequence number of
its first triangle; `arrange` insertion-sorts the batches that still have a
visible triangle by that first triangle; the corner count per batch is the
draw-time hide check. UVs are texels of one frame, scaled by the texture's own
size — and zero for a flat surface, which is what the C# does and which the port
has to reproduce rather than tidy. The mods' `Source::Mod` batches are not
ported (the mods are the enhanced look's) and are recorded as deferred with
cause. `MaterialResolver::resolve` took the C#'s `Pass` argument, which the port
had dropped: the arena draws Solid, objects draw DoubleSided, and the shading
follows the pass. The default argument keeps the arena path byte-identical.

**The goldens.** `golden/L3_t{1,120,1200}_pose.txt` and
`golden/L3_t1_transform.txt` come from a probe built against a pristine copy of
`src/Mdk.Game` (the driver's own setup, plus a dump of `Transform` and
`PoseParts` per object), so they are oracle output. The pose goldens at t120 and
t1200 are the first ones in the repository that pin a **baked animation pose** —
the existing goldens pin the object table, which has no vertices in it.

**What the goldens found.** The multi-tick transform golden could not be pinned:
by t120 the oracle's XF has `roll=-6.3411` (and `9.0251` by t1200) where the port
has `0.0000`, because `ObjectMotion.Bank` — the automatic lean into a turn,
`roll = clamp((wrap(roll) - turn) * 0.95, -10, 10)` — is not ported at all, and
the only writers of `roll` in the port are the script VM's turn opcodes. Seven
objects differ by t15. The object-table golden never showed this because the
table has no roll in it. The transform golden is therefore pinned at t1 (where
`yaw=270` still exercises the rotation) and the multi-tick cases are held back
until the motion slice writes roll; the pose goldens are unaffected, because
roll is not a pose input.

Verification on this tree:

- `make -C tests/scripts -j8` exited 0 and ended `All cases match the oracle.`
  The new cases printed `PASS L3_t1_pose`, `PASS L3_t120_pose`,
  `PASS L3_t1200_pose` and `PASS L3_t1_transform`. No existing golden moved.
- `cmake --build build-linux --target mdk -j8` exited 0.
- `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds -j8`
  exited 0, compiled `build/object_view.o` and built `mdk.3dsx`.
- `bash tools/check.sh` exited 0: `CHECK: ALL GATES PASSED`, SMDH SKIPPED for no
  built CIA, `gates_frames PASS` with `L3_f1.bmp sha=58615fc85d02e009` — the
  pinned frame is byte-unchanged, as it must be: nothing in the draw path moved.
- `DISPLAY=:99 tools/publish_frame.sh object-pose-layout` published
  `docs/frames/2026-10-31-object-pose-layout.png` + `.txt` with
  `bad_percent=1.240 max_delta=247 mean_delta=0.1819`, byte-identical to the
  previous C++ frame.
- No emulator or Original 3DS hardware run was performed; Old-3DS timing stays
  unmeasured.
