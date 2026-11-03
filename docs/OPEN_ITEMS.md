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
  scratch tree. **For a frame later than the first**, `capture_oracle.sh` hardcodes
  `--wait=0`, so run the binary it built (`/tmp/mdk-frame-oracle/src/Mdk.App/bin/Debug/net10.0/mdk_sdl`)
  the same way — inside `unshare --user --map-root-user -m` with a bind-mounted private
  `/tmp/.X11-unix` and its own `Xvfb :99 -nolock` — with `--wait=N`: in test mode the
  oracle advances 1/60 s per frame, so `--wait=1` is frame 60 (1/60 rounds UP in binary,
  so the accumulator reaches N/60 exactly on step N).
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

- **`FrameBlend.Lerp` is `Vector3.Lerp`, and this port does not reproduce its
  rounding for every input.** `game/frame_blend.cpp::lerp` writes
  `from + (to - from) * t`; the oracle's `Vector3.Lerp` is not that expression.
  Measured with a probe over 400 pseudo-random `(from, to, t)` triples printing
  `Vector3.Lerp`'s raw bits: `from + (to-from)*t` differs from the oracle on 187
  of the 400, `from*(1-t) + to*t` on 96, and `fmaf(from, 1-t, to*t)` on **none** —
  that is the oracle's arithmetic. It bites in the game: at level 3 tick 120 the
  `XF`'s interpolated x at alpha 0.75 is `C2C34BB6` in the oracle and `C2C34BB5`
  here (1 ULP), which is why `golden/L3_t120_blend.txt` does not exist while t1,
  t2 and t1200 do. **Why it is not a one-line fix:** `fmaf` is what matches, and
  devkitARM lowers `__builtin_fmaf` to `bl fmaf` at `-march=armv6k -mtune=mpcore
  -mfloat-abi=hard -mfpu=vfp` (verified: a call with stack spills, three of them
  in a function that runs per object per frame), while the current spelling
  lowers to one contracted `vmla.f32` per component. So the replacement is a
  target codegen decision to measure, not a substitution — and note the two
  targets already disagree: the 3DS build compiles this expression to three
  `vmla.f32`, because the ARM backend contracts a multiply-add to VFPv2's VMLA
  regardless of `-ffp-contract` (`mdk_3ds/Makefile` measures the shipped object
  byte-identical under the default, `=off` and `=fast`: only the VFPv4 `vfma`
  form obeys that flag, and this is VFPv2 — so there is no flag to reach for,
  only the expression), and the host goldens pin the host build. Start by
  deciding what `Vector3.Lerp` the 3DS can produce for free.
- **`ObjectMotion.Bank` is ported (2026-10-31) and the object goldens went
  multi-tick.** `ScriptRuntime::motionBank` is `ObjectMotion.Bank` (`0x43b65c`),
  called between `Roll` and `PreviousPosition` exactly where the C# calls it, and
  it is the only writer of `roll` for a walking alien. What was left of that item
  was the level-7 `SW_H150` animation below, and that is now CLOSED too: L3/L4/L5/
  L6/L8 at t120, L3 at t1200 and L7 at t1000 are byte-identical to the oracle
  including the transforms.
- **The object pass draws now** (`game/object_draw.{h,cpp}` uploads the frame's
  `ObjectStream` once into one dynamic mesh and issues the queued draws;
  `render/surface_cache.{h,cpp}` maps a surface + palette to a backend texture and
  palette and is shared with `LevelView`, as the C# shares one `MaterialResolver`;
  `mdk_linux/src/main.cpp` runs the C#'s `DrawObjects`, skipping the invisible and the
  removed). The 3DS frontend still has no `ScriptRuntime`, so it has no objects to draw
  and never allocates the stream mesh. What is left in this area, in order:
  * **Pass bucketing is CLOSED** (2026-10-31, review-5's MAJOR). The backends now
    queue every `mdk_render_draw_mesh` and submit the frame's draws bucketed by
    `Pass`, in `Pass`'s own order and in queue order inside a bucket, which is what
    `Renderer.cs:1004,1049-1057` does with its one command list. The buckets are
    shared code (`render/draw_queue.{h,cpp}`) because the order is the oracle's, not
    a backend's. The **submission point is per-backend and is not this sentence**: Vulkan
    submits at `canvas_begin`, at an inset's boundary and at `end_frame`; Citro3D submits
    once per eye inside `end_frame` and must not submit anywhere else (`render.h` is the
    seam statement; `draw_queue.h` deliberately names no flush point after review-3 F6).
    Measured on the pinned scene, the reordering
    changes no pixels at frame 1 or frame 60: frame 1 queues 154 Solid + 1
    DoubleSided and frame 60 queues 154 Solid + 30-33 DoubleSided, so the queue order
    already happened to be pass-grouped. It bites in the arenas with `Blended` and
    `Mirror` batches, which the pinned camera never reaches (list below).
  * **The interpolated transform is CLOSED** (2026-10-31, review-5's remaining MAJOR).
    `MdkObject::remember()` keeps the pose a tick started at and
    `position_at()/yaw_at()/transform_at()` blend it to the pose the tick left, with
    all four of the C#'s `Blends()` guards (`alpha < 1`, a snapshot exists, the arena
    is unchanged, the move is not a jump). `ScriptRuntime::tick_blend(ahead)` is the
    C#'s `TickBlend`, and `mdk_linux/src/main.cpp` draws each object at
    `transform_at(fixedStep ? 1.0f : tick_blend(pending))` — the C#'s
    `blend.TransformOf(obj)` with the capture mode's `Blending.Off` (Viewer.cs:280-282)
    spelled as alpha 1, which `transform_at` answers with the raw transform. The
    three statics are `game/frame_blend.{h,cpp}`, the C#'s `FrameBlend.Lerp/
    LerpAngle/Jumped`; the jump test is load-bearing and not a refinement, because a
    teleport lands outside the tick that snapshotted the pose and an interpolator
    without it draws the object back where it was for the rest of the tick. Pinned by
    `golden/frame_blend.txt` (ten poses at five alphas, driven identically on both
    sides, transforms as raw bits, byte-identical to the oracle probe) and
    `golden/L3_t1_blend.txt`; measured A/B in `docs/evidence/2026-10-31-L3-f60-blend-forced.txt`.
    What is NOT ported is the other half of `TransformOf`: `obj == _driven` draws the
    ridden object shifted by the step's lag, which needs `Rides` and `FrameBlend`'s
    Kurt pose (`Record/Update/Turn/KurtAt`). `ScriptRuntime::ridesRidden_` is null for
    every tick of the corpus, so the oracle does not take that branch either here; the
    deferral is with `Rides`, below.
  * **The pinned frame does not exercise the object pass, and cannot.** At capture frame
    1 exactly 1/60 s has passed and `ScriptRuntime` completes a tick per 1/30 s, so
    neither the oracle nor the port has a single object at frame 1: the earlier
    expectation that this batch would have to rewrite `reference.sha256` was wrong, and
    the middle 173 px of the gap is NOT the missing objects. To compare a frame where
    objects exist, capture the oracle with `--wait=1` (60 fixed steps) and the C++ with
    `--capture-frame=60`; measured that way the pass shrinks the gap from
    `1.251 / 231 / 0.1910` to `1.188 / 175 / 0.1405`.
  * The stream keeps the C#'s `1 << 18` **refusal** (a batch that does not fit is
    refused with the cursor unmoved, so a later smaller batch still fits) but the GPU
    buffer is made lazily and grows geometrically instead of the C#'s fixed allocation,
    which on the 3DS would be 5.2 MiB of linear memory spent to bound a case no shipped
    frame reaches (2559 corners at L3 t1, 5616 at L8 t1).
  The mods' model replacements (`Source::Mod`) stay deferred with cause: the mods are
  the enhanced look's and are not ported.
- **The level-7 `SW_H150` animation is CLOSED (2026-10-31).** It was
  `ObjectBehaviors.UpdateRunner` (0x43daf4's hamster branch), which the port had
  deferred as "the Runner's RNG-synchronized animations": idle it replays
  `H150_I` on a 72-in-32768 chance a tick, and running it pushes 40 units/s and
  turns away from Kurt at up to 270 degrees/s. `ScriptRuntime::motionRunner` is
  that, called from `motionPickup` where the C# calls it (after the chute has
  gone, before the Still-list spin), and its animations come from the sprites
  archive through `ScriptRuntime::itemsAnimation` (C# `Items.GetAnimation`), which
  is why `ScriptRuntime` now takes `TRAVSPRT.BNI` the way the C# constructor does.
  The idle chance is drawn from the SAME `Random` every scripted random decision
  takes, and only when the object is idle or done — so the port was silently
  running a different random stream from tick 329 of level 7. Pinned by
  `golden/L7_t1000_{pose,transform,layout,draw,stream_bound}.txt`, all copied from
  the oracle probe: at t1000 the hamster is on `H150_I` frame 36 and every line of
  all five dumps matches.
- **The Runner's RUN branch is CLOSED (2026-11-01), and so is the claim that it
  cannot be pictured.** The running half — `H150_R`, the 40 units/s push, the
  270 deg/s turn clamp, the plan-distance stop test, the return to `H150_I` and the
  idle RNG after it — was ported but unpinned, because no shipped start puts Kurt
  within `RunnerStart` (20) of the hamster: level 7's Kurt begins 22.16 units away.
  It is pinned now by `golden/L7_t120_runner_{a,b}.txt`, which place Kurt at
  (-21,-13,0.73) and (-26,-18,0.73) through a live `Player` and dump the hamster
  after EVERY tick (120 lines each, byte-identical to the oracle probe's `runner`
  mode with Kurt at the same point). Case a runs straight and stops at t22; case b
  turns at the full 9 deg/tick clamp, settles at yaw 16.4781 and stops at t17.
  The previous batch wrote that no picture of the Runner was possible because
  `--at` puts Kurt outside the arena, where nothing draws. That was measured at
  `--at` z=0, where Kurt has no floor and the oracle prints "Kurt died"; at z=9 he
  stands inside `DANT_1` and the scene renders. Two evidence pairs now carry it:
  `docs/evidence/2026-11-01-L7-runner-scene.png` (`--at=-21,-13,9,0`, frame 30,
  `0.422 / 324 / max 58 / mean 0.0358`, hamster behind Kurt) and
  `docs/evidence/2026-11-01-L7-runner-hamster-visible.png` (yaw 45 so the camera
  clears Kurt, `1.298 / 997 / max 87 / mean 0.1580`, and 462 of 14400 px differ in
  the box the hamster projects into). Both sidecars record the correction, because
  a published frame is never edited. The same batch's "byte-identical to the six
  records before it" was also wrong: md5 over `docs/frames/*.png` gives 15 files
  at `6769a13731983d65...`, i.e. fourteen identical predecessors.
- **Unattributed rare segfault in the degenerate `--at` scenario (from review-2's open
  questions, 2026-10-31).** `--at=-21,-13,0,0` — Kurt with no floor, a state no shipped
  start produces — segfaulted (rc 139) in 3 of ~46 Linux-frontend runs, never at
  `--at=...,9` (32 runs) and never under `gdb`, and the crashed run's stdout was buffered
  away, so there is no backtrace and no attribution. The reviewer's own guess (the respawn
  path recreating the `ScriptRuntime` while a pose cache keyed by animation address still
  holds entries) is a guess. Deferred with cause: it needs a memory-checker run in a batch
  that touches respawn, not a guess here. The captures this batch publishes are at z=9 and
  are unaffected.
- **`Player::facing()` takes its trig from the LUT, and the oracle does not.** C#
  `Kurt.Facing` is `MathF.Cos/MathF.Sin` of the yaw; the port's is `fastCos/fastSin`,
  whose documented per-period error is 4.76e-06 (`util/math_lut.h`). Over the 470-step
  walk of `golden/L3_kurt_steps.txt` that drifts the feet by **1.34e-05** (measured:
  20 printed fields differ at 4 decimals, 1 at 3, 0 at 2), which is why that golden
  prints the feet in 64ths of a unit instead of decimals. `util/matrix.h`'s
  `create_from_axis_angle` already carries an `@vfp-exempt R6` pin for exactly this
  reason ("the fastSin/fastCos LUT is not bit-identical to them, so routing this
  through the LUT would move the basis"), and Facing is gameplay: it decides where
  every walk step lands, where the follow camera sits, and where the velocity points.
  **Measured, and owner-gated:** replacing the two calls with `cosf/sinf` (the same
  one-line shape matrix.h uses) makes `golden/L3_kurt_steps.txt` match at four
  decimals with no other change, and moves the pinned frame **closer** to the oracle,
  `1.240 / 247 / 0.1819` → **`1.095 / 247 / 0.1636`** (captured 2026-11-02 on
  `DISPLAY=:99`, lavapipe, against `/tmp/mdk-frame-reference/L3_f1.bmp`). It is not
  landed because it changes the picture, and `tests/frames/reference.sha256` is
  owner-protected: the `cpp=` regression baseline has to be regenerated by the owner
  (`tests/frames/regenerate_reference.sh`), which re-measures the gap into the
  manifest. Whoever lands it should re-tighten the golden's feet to four decimals in
  the same change.
- Incomplete decoy/`AlienTarget` selection.
- Opcode 207's `MathF.Sign(NaN)` exception is accepted because C++ exceptions are
  banned; degenerate-path guards return 0 instead (this now also covers
  `Vector3.Normalize` of a zero vector and `MathF.Sign(NaN)` in
  `render/arena_scene.cpp`, neither reachable from a real arena).
- Linux runtime combat/inventory is not yet the live `Player`; the 3DS frontend has
  no runtime binding.
- Review-21 caveat: the post-camera solid refresh pins only the current arena, not
  the C#'s `SolidArenas`.
- **The C3D depth state is CLOSED (2026-11-02, batch 29).** The backend installs
  the oracle's `LESS_OR_EQUAL` test with the per-pass write rule
  (`pica::depthStateForPass`, `Renderer.cs:606-607`) and clears the depth to the
  buffer's maximum, because the map it installs stores 0 at the near plane. The
  pairing — `C3D_DepthMap(true, 1.0f, 1.0f)` + `GPU_LEQUAL` + clear `0xFFFFFF` —
  is Forsaken's on this hardware with the oracle's tie allowed, and the ordering
  it implies is pinned by `PASS PicaMatrix` (five cases, nine mutations, in
  "Iteration 57"). **What it does NOT pin:** the picture. P6 is open — no 3DS
  emulator, no Old-3DS — so the only hardware evidence this batch can produce is
  the init log line (`depth: map z*1.0+1.0 near=0.0000 far=1.0000
  clear=0xffffff test=LEQUAL`).
- **`DepthPull` on the 3DS is now unblocked but not free.** With a depth test
  installed, the oracle's depth pull (`render/depth_pull.cpp`, a poster over its
  wall) has something to bias against, and `renderer_c3d.cpp` still ignores
  `depth_layer`. It is not a one-liner: the pull needs the **view position** and
  the **primitive**, and the render seam hands the backend neither — the Vulkan
  backend has the camera locally, the C3D one has only `view`/`projection`
  matrices. So it is a seam question (pass the pull in, or pass the eye position),
  not a substitution, and it moves object placement on an unverifiable target.
  The ARM11 cost of the pull per draw is also unmeasured.
- **The PICA's far CLIP plane is `z <= 0` in the `[-1,0]` range** (vault:
  30-architecture/patterns/
  pica200-ndc-z0-is-the-far-clip-plane-billboards-vanish.md — measured on another
  project, five wrong diagnoses first). The engine's far plane converts to exactly
  `z_ndc = 0`, so geometry that has to be *clipped* against the far plane sits on
  the boundary and can be discarded whole. This is a property of the conversion the
  port already ships (`row[2] -= row[3]`, and Forsaken's
  `3DS_PORT_NOTES.md` Problem 1 says the same), **not** something batch 29
  introduced, and nothing in the pinned scene reaches it. Recorded because the next
  batch that touches depth, the far plane, or a CPU-projected quad pass (the
  diagnostic triangle is at clip `z = 0` through an identity matrix) should know
  the boundary is there.
- The newest external review's MINOR, halved: the 3DS `Pass` implementation now
  covers culling **and depth**, but not blend. The C# derives cull, blend and
  depth-test/write from `Pass` (`Renderer.cs:565,596,606-607`); the depth half
  landed in batch 29, and `renderer_c3d.cpp` still never calls `C3D_AlphaBlend`, so
  `Pass::Blended` glass draws opaque. **Why it is not the same one-liner as the
  depth half:** there is no alpha to blend with on this path. The palette expansion
  forces alpha 255 on every non-index-0 texel (the C# gets its transparency from
  the shader's index-0 discard, and the PICA has no discard), and
  `mdk_c3d.v.pica` writes `outclr = ones`, so `C3D_AlphaBlend(SRC_ALPHA,
  ONE_MINUS_SRC_ALPHA)` is arithmetic that changes nothing for glass. The alpha
  source has to be decided first — the original's colour-key, or an alpha in the
  expanded texture — and that is a batch of its own. The review measured non-`Solid`
  batches in **10 of level 3's 17** arenas (HMO_2, HMO_3, HMO_5, HMO_7, HMO_8,
  HMO_9, HMO_10, CHMO_4, CHMO_8, CHMO_9; `HMO_3`: 75 Blended; `HMO_9`: 25 Mirror +
  49 Blended; `HMO_2`: 4 Blended). The seven pure-`Solid` arenas are HMO_1, HMO_4,
  HMO_6, CHMO_1, CHMO_2, CHMO_5 and CHMO_6. They are unreachable today only because
  the 3DS frontend draws the start arena, whose 154 batches are all `Solid`.
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
- **Kurt's sounds are ported and pinned (2026-11-02, batch 36).** `Player` takes a
  `SoundMixer` the way C# `Kurt` takes one in its constructor and plays what the
  ported state machine plays: `FOOT1`-`FOOT4` from `Kurt.Footstep` (the wrapped
  previous/current frame, the pair that flips only on the right foot, and the
  `K_RUNFIR` frames kept for the firing slice), `LAND` on the landing transition,
  `CHUTEOUT` when the chute opens, `CHUTEON` (`Once`) and `CHUTEIN` (the `Stop` plus
  a play) from `UpdateChuteSound`, and the `Stop` of `CHUTEON` on death. Pinned by
  `golden/L3_kurt_steps.txt`, which is also the first golden that drives
  `Player::update` at all. What is left, with its cause: **no shipped frontend sets
  the mixer**, because neither has an audio device (the sample plane of
  `AudioDevice.cs` is not ported — `sound_device.h` says so) and the only mixer in a
  shipped build is `ScriptRuntime`'s own, which it constructs from the bank and
  hands out `const` only. The C#'s shape is one mixer owned by `Viewer` and passed to
  both Kurt and the runtime; reaching that is a `ScriptRuntime` constructor change
  (and a lifetime question, because the `Player` outlives the runtime across a
  respawn), so it is not this batch's call. `UpdateSpecial`/`UpdateDeath` still skip
  `UpdateTouched`, because there are no touch volumes yet.
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
opaque and dark there that the port does not draw at all. It is **not** the object
pass either: the object pass is ported and measured, and at frame 1 neither tree
has a script object in view at all, so this patch is a scene element of a
different kind and it is still open — what it is has not been identified.

## Review findings still open

- Review-5 (run 20261031-053650) — three of its four findings are CLOSED by this
  batch and one is the next slice:
  * **BLOCKER, closed:** the shared dynamic mesh was rewritten, and its old buffer
    destroyed, while an earlier Vulkan frame could still be reading it. A dynamic
    mesh now has one buffer per frame in flight and writes the slot its own frame
    draws from (the slot whose fence `beginFrame` waited), and a released mesh — or
    a resized canvas ring — is retired and destroyed at the frame boundary that
    proves its reading frame has finished. The 3DS backend defers `linearFree` the
    same way it defers a texture's, at the `C3D_FrameBegin` that waits the previous
    queue. The canvas buffer had the same hazard and is a ring now too.
  * **MAJOR, closed:** the pass order (above).
  * **MAJOR, closed:** the frame record for the object pass is now versioned, in
    `docs/evidence/` because the pinned frame cannot show it (above).
  * **MAJOR, closed:** the interpolated transform (above), which was its own slice.
  * Its open question about the Citro3D backend ignoring `MeshDraw::world` is
    **CLOSED (2026-11-01, batch 28)**: the matrix math moved to
    `mdk_3ds/src/render/pica_matrix.{h,cpp}` so a host test can reach it, and
    `drawRecorded` now composes each queued draw's world against the frame's
    per-eye matrix (`world * eyeVp`, one multiply, cached so an identity world
    costs no uniform write). Pinned by `PASS PicaMatrix`
    (`tests/scripts/pica_matrix_tests.cpp`), 8 cases, with 7 mutations of the new
    file each turning them red. Still true: nothing on the 3DS draws an object
    yet, because the 3DS frontend binds no `ScriptRuntime`.
- **The Citro3D backend issues ONE pass, into the last-installed eye target** —
  **CLOSED (2026-11-02, batch 30), and it was worse than "one eye missing".**
  `begin_frame` installed both eye targets and wrote the MVP twice *before a
  single draw existed* (the frontend queues between `begin_frame` and
  `end_frame`), so `flushDraws` put the whole frame into the RIGHT target with
  the right eye's matrix — and with the 3D slider closed the panel displays the
  LEFT buffer (`C3D_RenderTargetSetOutput(targetLeft, GFX_TOP, GFX_LEFT)`,
  `gfxSet3D(true)` at `main_3ds.cpp:98`). On hardware that is a top screen of
  nothing but the clear colour, not a stereo gap. The fix is the vault's
  per-eye-replay pattern (`30-architecture/patterns/
  citro3d-display-list-stereo-replay-non-fast3d.md`): `installEye` +
  `replayQueue` run once per eye from `end_frame`, `pica::eyePlan` decides the
  pass count from the slider (one when closed — Forsaken measures stereo at ~2×
  cost and "Slider = 0 disables stereo and restores full framerate"), and the
  clamp, the sign and the skip are host-tested with 7 mutations. Two follow-ups
  it leaves, both small and neither observable today:
  * **The canvas and inset seams do not flush, and must not** — **CLOSED as a
    latent defect (2026-11-02, batch 31, review-1 F4).** They used to call
    `flushDraws`, which replays the queue and empties it. With the per-eye replay
    at the frame end, that emits this frame's records into **whatever target
    `installEye` last selected** — the previous frame's last eye, or no target at
    all on frame 1 — and leaves the eye loop an empty queue: the scene vanishes
    behind a correct clear colour, which is worse than the "one eye only" this
    bullet first guessed (AGENTS.md P2: a draw must follow the `C3D_FrameDrawOn`
    of the target it lands in). Nothing calls them on the 3DS — `grep` for
    `canvas_begin`/`begin_inset` outside the backends is empty, and
    `mdk_render_canvas_quad` is a no-op on this backend — so the flush was never
    load-bearing. `flushDraws` is deleted and the seams are documented no-ops.
    **The requirement for whoever lands the HUD or the sniper insets:** a canvas
    quad or an inset is a RECORD IN THE QUEUE, drawn inside the eye loop once per
    eye with its own viewport (the C# begins a pass per inset,
    `Renderer.Insets.cs:RenderInset`), never a mid-frame flush **and never an
    immediate draw in the seam itself** — `mdk_render_canvas_quad` is the seam
    `game/hud_view.cpp:162,198,255` actually drives, and it is now covered by the
    gate with the other five (review-3 F1). Pinned structurally by `PASS C3dFrame`
    (`tests/scripts/c3d_frame_test.sh`), which goes red — with its own arms proved,
    and each arm compiled clean by devkitARM first, so no compiler catches these —
    if a flush or a bare draw returns to a seam, if a loop statement is guarded to
    one eye, if the queue is reset inside the loop, or if `installEye` leaves its
    place as the loop's first per-eye statement.
  * **What the HUD's queue record has to be, when it lands** (review-3 oq3, and the
    reason the gate line alone is not the whole answer): `DrawQueue` holds
    `MeshDraw`, which carries no projection, no viewport and no "this is screen
    space" marker, so "just put the HUD in the queue" is not implementable as
    written — the record needs an orthographic/viewport payload **and** a flat flag.
    The vault has the shape and the failure mode: `30-architecture/patterns/
    stereo-shear-2d-ui-must-replay-flat.md` (a 2D draw replayed with the per-eye
    transform picks up real parallax, so it must replay with the eye offset zeroed;
    the symptom is a one-eye edge bar and it is INVISIBLE in mono) and
    `citro3d-display-list-stereo-replay-non-fast3d.md` ("2D/HUD draws issued after
    the replay trigger are the hard case" — Forsaken dropped its HUD and its
    missile-cam inset to one eye this way and ended up disabling replay). So the
    HUD batch is a `MeshDraw`/queue extension plus the flat marker, not a seam
    edit. Not observable today: the 3DS frontend draws no HUD.
  * **`gfxSet3D(true)` is called once at init**, not per frame from the slider.
    That is the common homebrew pattern and it is what the current code needs
    (the left buffer is shown either way), but Forsaken's note says its own
    build calls it per frame from slider state. Untested here: no hardware, and
    Mandarine does not model the slider at all.
- Review-3's three findings are **CLOSED** by this batch: `ObjectStreamDraw` carries
  the look's palette (the C#'s `Material.PaletteId`), the draw golden prints only
  what the queue holds — including the transform's raw bits, so a queued draw that
  lost its transform fails — and the frame stream keeps the oracle's `1 << 18`
  refusal, pinned by the new `golden/L3_t1_stream_bound.txt`. Its open question
  (the level-7 `SW_H150` pose divergence) is recorded above as a pose/animation
  item. Its other open question — that the C# buckets queued commands by `Pass`
  before rendering (`Renderer.cs:1004,1049-1057`) while the Vulkan backend records
  draws as they come — was the GPU slice's and is CLOSED (2026-10-31, `draw_queue.h`).
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

- Review-7 (run 20261102-004416) **F5, Low** — `Player::runRate` drops C#'s
  `!BackingUp`, and **`BetaMoves` is not ported at all**. C# `Kurt.RunRate`
  (`Kurt.cs:738`) is `ForwardSpeed < 0f && !BackingUp ? -rate : rate`, where
  `BackingUp` is `BetaMoves && Current == State.Run && ForwardSpeed < 0f`
  (`Kurt.Beta.cs:25`). The port (`player.cpp`, `runRate()`) negates the rate on any
  negative `ForwardSpeed`. That is **correct today** — `BetaMoves` is never true, so
  `BackingUp` is never true, and `golden/L3_kurt_steps.txt`'s back-up phase matches the
  oracle byte for byte with a negative, wrapping frame — but it is a latent
  divergence, and since batch 36 the animation rate also decides which foot stamps,
  so the day beta moves land it would look like a sound bug rather than a direction
  bug. `grep -rn "Beta" mdk_core/src/game/` is empty: the whole beta-move subsystem
  (`InBetaMove`, `UpdateBetaMove`, `BackingUp`, `BetaMoves`) is unported, with no
  opcode of its own to hang the deferral on. Whoever ports beta moves owns this line:
  port `BackingUp` and the `!BackingUp` term in the same change, and pin a backwards
  `State.Run` while a beta move runs. Recorded rather than fixed here, because the
  code is not wrong today and a term that can never be true would be an unpinnable
  branch.
- Review-7 **F3**'s remediation (batch 37) left a **testability** fact on record, since
  it is the reason the death golden is shaped the way it is: **the fall-out death in
  `Player::fall` is unreachable from a harness that does not pin a solid arena.**
  `ArenaSpace::bottom` answers with the lowest `min.z` of the arenas that either
  contain the feet or *are* Kurt's arena, and `Kurt::kurt` resolves to
  `firstEntry(point)` unless `_solid` is non-empty — so every arena that can supply
  `bottom` also satisfies `feet.z >= min.z - kBoundsMargin`, and the death test
  `feet.z <= bottom - 50` can never be true. The same argument holds in the C#
  (`ArenaSpace.Probes.cs:71`, `Kurt.cs:494`). Pinning that branch needs a harness that
  calls `ArenaSpace::setSolid` the way `ScriptRuntime` does in the shipped game, which
  is a new harness concept on both sides and its own slice; the chute-stop-on-death line
  is pinned instead by a health-zero landing, which reaches the same dead branch.

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

## The closing-chute sprite frame is selected by code no harness runs (review-8 oq3)

`mdk_core/src/game/kurt_sprite.cpp:336` reads `Player::chuteClosing()` to pick the
closing-chute frame, and `src/Mdk.Game/Kurt/KurtSprite.cs:305` is the oracle's copy of that
branch. Batch 38 pinned the **field** — `golden/L3_kurt_steps.txt` now carries a `closing=`
column, and deleting the per-step clear turns it red at step 391 — but nothing on either side
asserts which sprite frame the selector then chooses, because `tick1` builds no sprite. The
fixture that would close it is a sprite-selection dump on both sides: state + `chuteClosing`
+ `chuteOpen` in, the animation name and frame index out, over the same scripted schedule.
That is its own slice, not a follow-up line in a sound golden.

**The named blocker (batch 39).** Neither side can construct the sprite object
headlessly, and that — not effort — is what has to be decided before the fixture
can be written:

- **C#.** `KurtSprite`'s constructor takes a `Renderer` and calls
  `renderer.CreatePalette` and `renderer.CreateDynamicMesh` in its body
  (`KurtSprite.cs:109`), and `Renderer` is a `sealed partial class` over the live
  Vulkan device (`Mdk.Engine/Render/Renderer.cs:133`), so the probe cannot pass a
  stub — which is why the probe already works around it for the frame counts
  (`Probe/Program.cs`: "KurtSprite itself needs a Renderer (it is the drawing
  half)"). Two ways through: reflect the private `Pick` on an instance made by
  `RuntimeHelpers.GetUninitializedObject` with `_sprites` and `_extra` injected
  (that is all `Pick` reads, it runs the oracle's own code, and the probe already
  reflects a private member this way for `ObjectMotion.Roll`); or build a real
  `Renderer` on lavapipe, which makes the whole golden suite need a GPU.
- **C++.** `KurtSprite::load` returns false on a null `MdkRenderer`
  (`kurt_sprite.cpp:155`) and `loadAnimation` refuses to decode while
  `paletteRgba_` is empty (`:256`) — even though the decode copies palette
  *indices* and every GPU texture is made lazily on first draw. So the selection
  half is renderer-bound by a precondition only the drawing half needs, and
  relaxing it is a change to a path that owns GPU lifetime (`palette_`/`mesh_`
  would be `-1`), which is not a test-harness batch's call.

Pick one of the two seams and the fixture is a normal batch. Leaving both open
leaves `pick()` unpinned on both sides.

## Kurt has two healths where the oracle has one (found in batch 39; **CLOSED by batch 40**)

The C# is one object. `Kurt.Health` (`Kurt.Combat.cs:76`) is written by `Damage` —
which the scripts' `hurt_kurt` reaches through `Hurt` and the movement's
`HardLanding` reaches directly — and read by the HUD, by `if_player_health`, and by
the `Health == 0 && OnFloor → State.Dead` transition. The port split that object in
two and never bridged the halves:

| object | field | written by | read by |
|---|---|---|---|
| `mdk::Kurt` (combat, script-facing) | `health_` (`kurt.h:108`) | `hurt()` ← opcode 109 `hurt_kurt` | opcode 241 `if_player_health` (`script_vm.cpp:1648`), the `KURT` script-dump line |
| `mdk::Player` (movement) | `health` (`player.h:48`) | `damage()` ← the hard landing; `setHealth()` | `dead` (`player.cpp:500`), the Linux HUD (`mdk_linux/src/main.cpp:633`), `hp=` in both kurt goldens |

Nothing copies one to the other. So in the shipped unit a script that hurts Kurt to
zero leaves the HUD at 100 and never kills him, and a hard landing moves the HUD
while `if_player_health` still reads 100. Both diverge from the oracle, and neither
was recorded: `docs/OPCODE_COVERAGE.md` lists 109 as landed, and iteration 14's
note that "in the corpus the state moves only through the scripts" predates the
movement health, which arrived with the hard-landing slice.

Why it was not fixed in batch 39: unifying them is a state-model change across `Kurt`,
`Player`, the VM and both frontends, and no existing golden could pin the bridge — the
script goldens tick no `Kurt.Update` and the kurt goldens run no `ScriptRuntime` —
so the fix needs a new probe mode that ticks the runtime and the same `Kurt` object
together. That is its own slice with its own risk class, not a line inside a
review-remediation batch.

**Closed by batch 40 (iteration 68).** `mdk::Player` now owns the combat half
(`Player::combat_`) and the health lives in it alone; `ScriptRuntime::kurt()` returns
`player_->combat()` when a Player is bound, so the runtime hands out the entity's own
object rather than a copy — which is what C# `Viewer` is (`ScriptRuntime.cs:168` hands
its single `Kurt` to the `ScriptRuntime`). The pin is
`tests/scripts/golden/L3_kurt_hurt.txt` (the oracle probe's new `hurt` mode, byte-
identical to `tick1 dump hurt 3 220`), which prints `hp=` off the entity and `vmhp=`
off the runtime's Kurt; in the oracle they are one property. With the runtime holding
its own copy again the golden diverges at the first hurt (`s=20 hp=100 vmhp=90`) and at
the landing (`s=183 state=HardLand hp=90` where the oracle is `Dead`).

## CLOSED by batch 44: a hurt on the floor knocks Kurt down, and now the port knows it

Batch 40 found that `golden/L3_kurt_hurt.txt` hurts Kurt **in mid-air** on purpose: the
oracle's `UpdateKnockDamage` (`Kurt.Combat.cs:214`) knocks him down whenever `KnockDamage >= 5`
while `OnFloor` -- so *any* hurt of 5 or more on a floor puts the oracle into `K_BANG` and, after
`K_BFLIP`, back up, invulnerable for 3 s -- and the port had no knock-down to compare against.
Batches 42 and 43 landed the counter's decline and the timer's decline, and named the three things
the trigger still needed: `ArenaSpace::floor`, the `Knocked`/`GetUp` arms of `UpdateCombatState`,
and `StopFiring` with the firing slice.

**Batch 44 landed the state machine.** `ArenaSpace::floor` is the C#'s `Floor`
(`ArenaSpace.cs:191`): the arenas around **both** ends and the crossing **nearest `from`** -- a
different probe from the port's `floorCrossing` (the C#'s `FloorCrossing`, Kurt's arenas around
`from`, first hit), and `golden/L3_floor_probes.txt` prints both over 48 of the level's segments
and shows them part company on the two long ones through `HMO_9`/`CHMO_8` (`-286.0000` vs
`-326.6710`). `Player::knockDown` is `Kurt.KnockDown` (`:173`), `Player::updateKnockDamage` is
`Kurt.UpdateKnockDamage` whole (`:214`, the floor arm and the air arm with its 13-unit probe and
-64 u/s slam, and the decline as its tail), the `Knocked`/`GetUp` arms of `Player::updateState`
are `Kurt.UpdateCombatState`'s (`:379-399`), and the walk-input suppression is `Kurt.cs:213-217`.
The pin is `golden/L3_kurt_knockdown.txt` (six mutants run) with three `vm_tests` cases for the
branches a schedule cannot reach.

**Batch 46 landed `push_kurt` (opcode 248).** `ScriptRuntime::pushKurt` now passes the C#'s
object-frame or away-from-object push and lift through `Player::knockDown(push)`. The golden
pins the two push modes, zero-distance fallback, knock-down refusal guards and push drain; the
VM test pins bytecode dispatch and exact normalization/snapshot order. The frame-1 image cannot
show this later script-driven behavior (sidecar and golden carry that evidence).

**What that item still leaves open, with the cause named.** `KnockDown`'s `StopFiring()` stays
inert until the firing slice: the port has no firing, sniper or muzzle state at all. The flip's
leftover frame (`AnimationFrame - bang`) is *structurally* unobservable at a fixed 1/60 step --
`Ticks * delta` is exactly 0.5 and both frame counts are integers, so the boundary is always hit
exactly and the leftover is always `0.0000`; the port keeps the C#'s expression anyway. The
combat fade/skull implementation is described in `docs/CONTINUATION.md`'s verified tree state
and the batch 45/46 narratives in `docs/PROGRESS_LOG.md`; remaining flash writers and render
consumers stay deferred with their subsystems.

## CLOSED by batch 41: `Kurt::state_` was a second copy of the entity's state with no writer

Batch 40 removed the duplicated *health*; the same split still duplicated the *state*.
`Kurt::state_` was initialised to `Still` and nothing wrote it, so the C#'s `Hurt`
guard (`Kurt.Combat.cs:125`: `Current is State.Dead or State.Knocked or State.GetUp or
State.HardLand`) was ported as a comparison against a field that never changed. Batch
40 called that "inert rather than wrong" because the `health_ == 0` return above it
covers the dead case and, in its words, "nothing can enter HardLand/Knocked/GetUp and
survive to be hurt in the same step". **Review-11 R11-3 refuted the second half of that
sentence with this repository's own golden**: `golden/L3_kurt_steps.txt` steps 656–759
are one *non-lethal* hard landing, 104 consecutive frames at `hp=90`, and a scripted
`hurt_kurt` inside that window damages the port (90 → 80) where the oracle declines.
The window is 104 frames, not zero, and the dead-case argument only ever covered one of
the four states the guard names.

Fixed by **moving** the field rather than mirroring it: the state lives in `Kurt` alone
(`Kurt::state_`, written only by `Player::setState` through `Kurt::setState`) and
`Player::state()` is the entity's view of it — the shape batch 40 gave the health, and
for the same reason: a copy on either side is a copy the other half never sees.
`Player::_state` is gone, so there is no second copy to drift. The pin is
`golden/L3_kurt_hurtland.txt`: the same 300-unit lift, so the landing at step 183 is a
hard one and K_CRASHL runs to 286; a hurt of 10 at step 220 lands **inside** the window
and the oracle's health stays 90, and a hurt of 4 at step 300 lands **after** it and the
health moves to 86. Both arms were built and run as mutants: HEAD's two-copy shape fails
at `s=220 hp=80` while every other golden passes, and a guard that always fires fails at
`s=300 hp=90` where the oracle is 86.

What stays open is the *other* half of the guard: `Knocked` and `GetUp` cannot be
entered at all until the knock-down slice above lands, so this golden can only show the
HardLand arm — and the second damage is 4 rather than 5 for exactly that reason, since a
hurt of 5 or more on a floor knocks the oracle down and the port nowhere.

## Class bounds worth keeping

Moved here from `docs/CONTINUATION.md` on 2026-11-03 so that file stays at
"current state + priority order". Each of these is a *rule for the next batch*, not a
defect to fix:

**a host harness must return the counter it accumulated on every exit path**
(review-9 F1); **a multi-arm gate must state how many arms it adjudicated and compare that against
the declared number, and an arm that cannot set up is a failure, not a skip** (review-10 F1); and
**a counter must move AFTER the observable it counts, never before it** (review-11 F1 refined by
review-12 F1), because a backstop that fires on a SHORT count cannot see an over-count. From
review-11 F2 extended by **review-12 F4**: **a test helper that builds bytes must bound its own
inputs against the field width it claims to pin**, its refusal must be a fixture diagnostic rather
than a check result, **and that diagnostic must be accounted for** — count the refusals, count the
asserted refusals, and require them to be equal. From review-7 oq1: the collector death loop asserts
the outcome **after** a kill, not that the kill happened; **the first batch that adds a conditional
record write to the collector arm must land the exit-status assertion (require `9` per iteration)
with it.** From review-7 oq2: `lift_fn` is bounded by the definition's own shape *while every
definition in `ralph-pi.sh` keeps its `}` in column 0*. And one declared gap rather than a fixed one:
**the shipped frontend's copy of the missing-archive refusal, `mdk_linux/src/main.cpp:383-394`, is
gated by nothing** (review-10 F2). `sprites_refusal_test.sh` gates `tests/scripts/tick1.cpp`'s copy
only. Narrowing `if (scriptAssetsReady && spritesReady)` there builds a runtime against an unparsed
`Bni` — the random-stream divergence — and every gate stays green. A behavioural gate was rejected on
reachability, not on effort: that branch sits under `data.found()` → `LevelData::load` →
`if (arena)` → `spawnPlayer()`, so it needs the licensed tree, and a gate that skips without the tree
is review-10 F1's defect reintroduced. Trigger that closes it: owner task (10) landing. And one
sibling left standing on purpose: **`tick1`'s own `SKIP: no MDK data` still exits 0**, so the golden
suite can skip into a green `make -C tests/scripts` on a host without `./MDK/`; it is visible as
`SKIP` and as a PASS count that drops from 442 to **3** (measured in "Iteration 52"), and making it
fail is owner task (10), not a batch.
