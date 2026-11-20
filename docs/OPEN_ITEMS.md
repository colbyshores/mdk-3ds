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
  scratch tree. **For a frame later than the first**, set `MDK_FRAME_WAIT=N` (seconds;
  since 2026-11-18, it was a hardcoded `--wait=0` before -- and that edit to
  `capture_oracle.sh`, a protected contract file, is UNCOMMITTED in the working
  tree until the owner approves it; `git diff tests/frames/capture_oracle.sh`): in test mode the oracle
  advances 1/60 s per frame, so `MDK_FRAME_WAIT=1` is frame 60 (1/60 rounds UP in
  binary, so the accumulator reaches N/60 exactly on step N) and pairs with
  `capture_cpp.sh`'s `MDK_FRAME_CAPTURE=60`. `MDK_FRAME_AT=x,y,z[,yaw]` places Kurt
  on both sides (the app's `--at`). At `MDK_FRAME_WAIT=0` the oracle's drawn list is
  still empty and it draws EVERY reachable arena; past the first tick it draws the
  scripts' list. **At a size that is not 4:3 set `MDK_FRAME_TRUE_ASPECT=1` for
  `capture_cpp.sh`** (the Linux build's `--true-aspect`): without it a 400x240 port
  frame has a different field of view from the oracle's and reads as a 33-44% gap
  that is not there (measured 2026-11-18 in level 3's CHMO_1 corridor: with it the
  same frame is 0.032%).
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

## The arenas' own sounds are played by nobody: an ORACLE gap (found 2026-11-18, arena streaming Phase 5)

Every MTO arena block carries its own effects in the tail of its models section:
24-byte entries in the SNI's layout (name[12], u16 flags, u16, u32 offset from the
section, u32 length) and the WAVs they point at. The original registers them when
the arena's stream completes (`arena_load_step` 0x41983c -> 0x4310cc ->
0x403a7c(flags, models + offset), at most 16 handles at 0x573b94) and they go with
the arena. **85 names over levels 3-8** (level 3: BUBBLE1, DBOARD, DOORLAND,
H9CRASH, H9IDLE, H9START, MARIO, SMASH, SPLAT, XB3_HBUT, XG_CRASH, XM3_ROLL,
XW3_SHOT, XW3_TURN; level 4's SUB* set; level 6's CANNON, GLASS, PENDULUM...),
280-944 KB a level, and **not one of them is in any SNI archive**.

The C# parses them (`Arena.Sounds`, `Arena.cs:109-115`) and nothing reads that
dictionary: `SoundBank` looks only in the three SNIs. So in the oracle, and in
this port, a script that plays SMASH or H9START gets "no such sound" -- silence,
where the 1997 game plays it. This is a gap IN THE REFERENCE, so the port does
not close it on its own: a sound that newly resolves starts a voice, and voices
are script-visible (`IsPlaying`, voice ids, the 16-voice ceiling), so the goldens
would move. It needs the owner's call and a matching change to the oracle.

What the port did (Phase 5): it no longer KEEPS those bytes (`Arena::parseModels`
finds the first sound and the kept models span stops there: 0.28-0.94 MB a
level). If they are to be played, they should arrive the way the original's do --
with the arena's stream (`ArenaStream`: read the block's models tail on switch,
hand it to the bank, drop it on trim), not by keeping them resident again.

Related, same cause: level 3's `HMO_2` names `AMB_DRIP` as its music and `HMO_9`
`H9WATER`; neither is in `LEVEL3O.SNI`, so both arenas have no music in the
original's own lookup (0x431590 searches the SNI directory) and none here.

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
  lowers to one `vmla.f32` per component. So the replacement is a
  target codegen decision to measure, not a substitution — and the two targets do
  **not** already disagree: the 3DS build compiles this expression to three
  `vmla.f32`, VFPv2's VMLA is unfused (it rounds its product before accumulating),
  and so it computes the same twice-rounded value the host does
  (`mdk_3ds/Makefile` and "Iteration 87": the shipped object is byte-identical
  under `-ffp-contract` default, `=off` and `=fast`; only the VFPv4 `vfma` form
  obeys that flag, and this is VFPv2 — so there is no flag to reach for, only the
  expression). The gap is port-vs-oracle on both targets, not host-vs-target, and
  the host goldens pin the host build. Start by
  deciding what `Vector3.Lerp` the 3DS can produce for free.
- **The host builds do not pin `-ffp-contract`, so an FMA-capable host build would stop
  describing the shipped 3DS object (review-29 F2).** Several records — `frame_blend.cpp`,
  `mdk_3ds/Makefile`, the pinned spin — now say the two targets compute identical bits for a
  twice-rounded expression. That is true **for the flags the builds actually use**: neither
  `tests/scripts/Makefile` nor `mdk_linux/CMakeLists.txt` sets `-march` or `-ffp-contract`, and the
  x86-64 baseline has no fused multiply-add, so nothing can contract. Measured on this machine:
  the same source at `-O2 -march=native` (znver3) emits **4 `vfmadd*`**, at the default **0**. So a
  host built with an FMA-capable `-march` would compute a third value and the goldens — the
  fidelity ceiling — would describe that instead of the 3DS. The fix is one line in each host build
  (`-ffp-contract=off`), and it is provably a no-op today for exactly the reason above; it is not in
  this batch because touching the goldens harness's flags is a build change, not a record fix.
  Note the dialect is NOT the lever: `-std=c++17`, `-std=gnu++17` and `-std=c++20` all report
  `-ffp-contract=fast` on devkitARM 15.2 and host gcc 13.3.
- **The 3DS build's libm trig leaf is unmeasured against the oracle, and it is the
  last unmeasured link in the pinned shatter spin.** Every `sinf`/`cosf` the port
  calls resolves on the target to **newlib's** `libm.a` (`arm-none-eabi-nm
  mdk_3ds/build/debris.o` shows `sinf`/`cosf` undefined; the link takes `-lm` from
  devkitARM), and what batch 58 proved bit-identical to `MathF.Sin`/`MathF.Cos` was
  **glibc's**, on the shatter's own angles. The angle→quaternion arithmetic is now
  confirmed oracle-exact on the shipped object too ("Iteration 87": the shipped
  `randomSpin` grouping, replayed with a rounded product, reproduces 0/72 wrong
  against the pinned `s=` bits), so if a capture ever shows a spin or trig
  divergence the libm leaf is where to look, not the MACs and not `degreesToRadians`.
  Not closable on this host: devkitARM ships no newlib sources, there is no
  `qemu-arm` here, and the only faithful measurement is an Old-3DS capture (P6) or
  a host build of newlib's `s_sinf.c`/`s_cosf.c` from upstream.
- **`powf` joins that leaf list, measured on the host.** The follow camera's arena-pitch
  ease is `powf(0.85f, delta * 30)`, and review-5 oq3 correctly noted the comparison to
  `MathF.Pow` had never been made. Measured 2026-11-05: .NET 10 `MathF.Pow(0.85f, 0.5f)`
  is `0x3F6C0535` and host `powf` gives the same bits, and **0.5 is the only exponent the
  shipped path can produce** — `mdk_linux/src/main.cpp:523` is the only shipped caller and
  it passes the fixed `1/60` step. The five pinned arena-pitch bits already reproduced from
  `powf(0.85f, 0.5f)` alone, so this measurement confirms what the pin implied rather than
  changing it. The same run re-confirmed the fused `Single.Lerp` shape on 256 fresh triples
  (fused 0/256 wrong, twice-rounded 53/256), which is review-5 oq1. What is NOT measured is
  newlib's `powf` on the target — same unclosable-here cause as the trig leaf above.
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
    `--capture=<out>.bmp --capture-frame=60` — both halves of the C++ spelling, because
    since batch 170 a capture frame with no file named is refused rather than run and
    silently skipped; measured that way the pass shrinks the gap from
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
- **RESOLVED (batch 83): the 3DS `Pass` implementation now covers blend as well as
  culling and depth.** The C# derives cull, blend and depth-test/write from `Pass`
  (`Renderer.cs:565,596,606-607`); the depth half landed in batch 29 and the blend
  half is `pica::blendStateForPass`, installed per pass by `renderer_c3d.cpp`.
  **Two premises this file carried were wrong, and both are measured now.** (a)
  "`renderer_c3d.cpp` never calls `C3D_AlphaBlend`, so glass draws opaque" — the
  conclusion is right that the state was never set, but the mechanism is the
  opposite: `C3D_Init` calls
  `C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA,
  GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA)` (read out of
  the linked `libcitro3dd.a`, `base.o`), so blending was ON for every pass of
  every frame and glass looked right by accident while the four opaque passes
  mixed when the oracle forbids it. **What batch 84 corrects is the proof this entry
  offered, not the mechanism.** The before/after capture was not the parent build: a
  clean `f47d8fc` worktree build differs from that `before.png` by 33795 px of the top
  panel and from the `after` by **0**, so the "`(255,0,0)` was landing at
  `(30,30,45)`, i.e. an opaque arena surface drawn at 6%" example is retracted. The
  register state was wrong and is now the oracle's; the visible effect in the start
  arena is nil, because every fragment there has alpha 255 — which is what a
  `SRC_ALPHA` blend and a replace pair drawing byte-identical 400x240 panels means
  (`docs/evidence/2026-11-06-L7-blend-ab-{parent,current}.txt`). (b) "there is no
  alpha to blend with on this path, and the
  PICA has no discard" — the flat-colour path carries the material's alpha in the
  1x1 texture, and the PICA's discard equivalent is the alpha test, which is what
  `C3D_AlphaTest(true, GPU_GREATER, 0)` installs here: the expansion encodes index
  0 as alpha 0 and every other index as 255, so `alpha > 0` is the shader's
  `if (index == 0) discard;` and, unlike a blend, a discarded fragment does not
  write depth. What is still open is NOT the mechanism: the alpha test changed no
  coverage in the pinned scene, so the one-sided view is still unexplained, and a
  textured surface whose material alpha is partial still cannot reach the fragment
  (the combiner replaces alpha with the texture's, where the shader multiplies by
  `colour * input.colour`) — that needs a second combiner stage, not a blend call.
  The review's batch-counting detail stands and is useful: non-`Solid` batches live
  in **10 of level 3's 17** arenas (HMO_2, HMO_3, HMO_5, HMO_7, HMO_8, HMO_9,
  HMO_10, CHMO_4, CHMO_8, CHMO_9; `HMO_3`: 75 Blended; `HMO_9`: 25 Mirror + 49
  Blended; `HMO_2`: 4 Blended). The seven pure-`Solid` arenas are HMO_1, HMO_4,
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

## CLOSED by batch 145: the c3d_frame mutation suite was inert — the root cause of the red `make -C tests/scripts`

`make -C tests/scripts` had been exiting 2 with 93 `c3d_frame` mutation arms failing and one
check red. It was red at the parent commit too, with a byte-identical FAIL set (proved by
stashing and re-running at `1069a79`), so it was inherited — but inherited is not the same as
unfixable, and the loop status was right to demand a root cause.

**The root cause was two stale literals, not an architecture change.** `drawDiagnostic` still
draws immediately; what changed is its SHAPE. The shipped diagnostic used to be a white
TRIANGLE, `C3D_DrawArrays(GPU_TRIANGLES, 0, 3)`. It is now a 16x16 canary in the canvas's own
pixel space — two triangles, **six** vertices, `C3D_DrawArrays(GPU_TRIANGLES, 0, 6)` at
`renderer_c3d.cpp:2632`. `tests/scripts/c3d_frame_test.sh:826` still matched the `0, 3` literal,
so it counted zero diagnostic draws and was red for every correct build.

**Why that silenced the entire mutation suite instead of one line.** Each arm must turn red
exactly the check it targets. A check that is already red before any mutation turns red for every
mutant, so all 93 arms reported "turned `diagnostic-drawn-once` red, but it is cut for <its real
target>" and **exercised nothing**. A gate that prints 94 lines a run and verifies none of them.

**A second, independent staleness was hiding behind the first.** Three more arms
(`diagnostic-dropped-from-the-eye-loop`, `diagnostic-after-the-eye-loop`,
`diagnostic-guarded-to-one-eye`) matched the call with `\([A-Za-z0-9_]*\)`, which cannot match
the real `drawDiagnostic(r, mdk_render_canvas_width(rr))` — a comma and a nested call. They
returned the file unchanged and reported "the mutation changed nothing": mutants of nothing, the
exact class review-14 F3 names. They were invisible while the first failure flooded the output.

**The fix, and why it is a tightening rather than a relaxation.** The vertex-count literal went
`0, 3` → `0, 6` in the check AND in the arm that splices a duplicate emission (splicing `0, 3`
into a check counting `0, 6` would have made that arm a mutant of nothing), and three arm
patterns widened from `[A-Za-z0-9_]*` to `[^;\n]*` so they find the call that is actually there.
No check was deleted, no target re-pointed, no arm's expectation loosened: every arm still has to
turn red the one check it names. **Before: 0 of 94 arms adjudicated. After: 94 of 94, and the
whole suite reports `All cases match the oracle` with rc 0.** The invariant
`diagnostic-drawn-once` defends — exactly one diagnostic emission, inside `drawDiagnostic`, not
per real draw — is unchanged, and the comment at the check now says the 6 is load-bearing so it
does not get "tidied" back to a 3.

**The general lesson, which is the reason this is written up at this length.** A structural check
that pins a LITERAL instead of an invariant goes red silently the day the literal legitimately
changes, and in a mutation harness one always-red check does not cost one check, it costs the
whole suite. When a structural check fails, the question is never "is the literal still right"
but "what invariant was the literal standing in for" — and after fixing one stale literal, count
how many arms actually adjudicated, because a second staleness was hiding under the first.

## CLOSED ON HARDWARE (2026-11-17): the 3DS canvas's textures are sampled wrong by the GPU (found in batch 173)

**HARDWARE ACCEPTANCE MET (2026-11-17, `old3ds-lan167`, CFGU model 0 = Old 3DS).** A
harness run (`make AUTOQUIT=90 CAPTURE=600`, `hw_cycle.sh --capture`, FRESH, push
`bytes_out=119372632` verified) wrote a frame-600 readback on the console. Its HUD panel
is the oval with a green "100", not a stripe. Its dominant period is the Morton build's
29.0 px (row-major: 11.6), and best-aligned it correlates r=+0.61 with the emulator's
Morton crop against +0.19 with the row-major mutant (`mdk_3ds/tools/hud_panel_compare.py`).
Pictures and numbers: `docs/evidence/2026-11-17-hw-loop-stable-first-hardware-results.txt` §3.
The history below is kept as written.

**WHAT IS WRONG, IN ONE SENTENCE.** The HUD's panel sprite reaches the panel as a fine
vertical stripe instead of the oval, at a period of ~5.3 crop px where the derived
geometry says 21.3 — **a factor of ~4 too many texels across the panel's width** — and
the scene's textures are probably affected the same way (the level shows a fine
crosshatch that disappears when the uv is divided).

**WHAT HAS BEEN RULED OUT, by measurement and not by reading.** (a) The CPU-side texels
are correct: the expanded RGBA bytes were read back and match the index image and the
palette byte for byte. (b) The geometry is correct: the panel's quad is pos
(500,287)-(584,350) with uv (0,0)-(1,1), and the marker's measured 10.67 crop px for 16
canvas px confirms the canvas->crop scale is 2/3 on both axes, so 84 texels must map to
56 crop px. (c) The uv transform reaches the shader: forcing `uvtransform` from 0.6562
to 1.0 moved the stripe period by exactly that ratio. (d) The shipped `.shbin` is the
shader that was written — `touch` the `.pica`, rebuild, identical md5
`59d5e0e8ec1dc12827b6cd516715de0f`.

**WHAT IS NOT ESTABLISHED, AND IS THE SLICE.** The GPU-side UV->texel rule. Writing
synthetic patterns straight into the texture's linear buffer does not come out as
written: a period-32 square wave renders ~4x too many bands; a left-white/right-black
half renders as **top-white/bottom-black** bands; a block (white where x<42 and y>=32)
renders as vertical stripes confined to the top band. Those three observations are not
consistent with any single scale factor, which is why this is filed as an addressing
question and not as a wrong constant. `C3D_Tex` carries no stride field, so the stride
is computed inside libcitro3d — whose source is not in this tree — from `dim` and `fmt`.
**The next move is to read that source or disassemble `C3D_TexBind`/`C3D_TexInitWithParams`
from `libcitro3d.a`, establish the rule, and pin it with a pattern fixture before any
scale is touched.**

**PARTLY ANSWERED (2026-11-17): the rule is tile ORDER, not a scale.** `C3D_TexLoadImage`
is a `memcpy` into the texture's linear buffer -- citro3d's own `GSPGPU_FlushDataCache`
reference in `texture.o` sits in `C3D_TexFlush`, not in the load -- so the buffer the
backend writes is exactly the buffer the GPU samples, and the GPU samples it in 8x8
Morton tile order. `04_textures.inc` had been writing rows. Both expansions now write
through `pica::textureTexelIndex` (`pica_matrix.h`), which is why the row's three
"synthetic pattern" observations are one fact: a pattern written row-major is read as
tiles, so a period-32 square wave reads ~4x too many bands, a left/right split reads as
top/bottom bands, and an L-block reads as vertical stripes confined to a band. The
horizontal-band signature is now pinned in `testTheTexelTileOrder()` alongside the
permutation and tile anchors. **Measured on the emulator, same sources minutes apart,
only the helper body differing:** the row-major mutant's frame has ~2x the gradient
energy of the tiled build in the level-7 view (|dx| 11.3 vs 5.5, |dy| 10.0 vs 6.7) and
matches the 23:03 pre-fix capture (10.9 / 11.1), so tile order demonstrably reaches the
sampled pixels. Clear-colour coverage did NOT move (83.8% both), because scrambling
permutes colour without touching alpha -- coverage was never the right metric here and
the two earlier "no visible change" readings are not a refutation.
`docs/evidence/2026-11-17-level7-texture-tile-order.txt` has the captures, the crop, the
command, the display, and the mutation control.

**WHAT IS STILL OPEN IN THIS ROW.** The HUD panel's period is now re-measured, and it
is the headline number: the panel no longer reaches the screen as a fine stripe. On the
tiled build the region carries no 3-9 px component and resolves into **2 lobes across
its 58 crop px** (dominant 29 px, against the 21.3 the geometry predicts for an 84-texel
sprite in 56 crop px); the row-major mutant reproduces **the recorded 5.3 px peak
exactly**, with 5 stripes, in the same scene minutes apart. So the fine stripe was the
layout, and it is gone. `docs/evidence/2026-11-17-173-hud-panel-period.txt` has both
captures, the crop, the profile numbers, the command and the display.

What remained open at this point was **hardware acceptance only**, and it is now met
on the console (see the CLOSED ON HARDWARE note at the top of this row). The `/4` UV
stays forbidden.
Two hardware attempts on 2026-11-17 were both blocked before any push by the ATM `:8080`
wedge (P10 trap 8; the second persisted through two `cmd reboot` sends and 5 min of polling),
and both are recorded at the end of the HUD evidence file above. That "wedge" turned out
to be a parked ATM service loop: one directed `atmctl discover` wakes it, which reboots
never did. `hw_cycle.sh` now wakes the loop before every HTTP read (evidence §1).

**DO NOT SHIP A /4.** It was tried. With the panel's uv scaled by 1/4 the stripes
thin out, and **the health digits stop reading "100"** — a change that makes one pattern
look tidier while breaking the element the HUD exists to show. The digits themselves are
correct as shipped (see the next paragraph), so any fix that damages them is wrong.

**AND A CORRECTION TO THIS ROW'S OWN STORY: THE DIGITS WERE NEVER BROKEN.**
`hud_view.cpp`'s `drawNumber` builds its own `CanvasQuad`s rather than going through
`drawImage`, and the shipped capture shows a crisp, correctly-shaped "100" at crop
(359,208)-(369,215). Every earlier note in this row that said "the digits are smeared"
was describing the panel sprite behind them. Full record:
`docs/evidence/2026-11-14-173-the-verdict-could-not-see-the-hud.txt` §5.

## CLOSED by batch 147: the 3DS canvas drew no pixels (found in batch 142)

**CLOSED, AND THE CAUSE WAS NOT IN ANY OF THE ELEVEN RUNS.** `pica::registers()` wrote each
vertex-uniform register's four words into `C3D_Mtx::m[]` in the order (x,y,z,w); the PICA reads a
register's words as **(w,z,y,x)** — `C3D_FVec` declares `w, z, y, x` and says "Matches PICA
layout", `Mtx_Scale` multiplies offset 12 by its x argument, `Mtx_OrthoTilt` only projects at all
read as (w,z,y,x), and `C3D_UpdateUniforms` hands the staging buffer to `GPUCMD_Add` verbatim. So
every register reached the shader component-reversed: for the canvas matrix that means clip x =
clip w, NDC x = NDC y = 1, and the whole HUD collapsed onto one panel corner. The same defect is
why an identity MVP painted nothing and why the level was a sunburst. `mdk_3ds/tools/emurun.sh
--expect hud` now exits **0** with `HUD VISIBLE -- 121 canary pixels in the panel`, and the
canary's bbox (0,0)-(10,10) is where the canvas's top-left 16x16 lands under the panel's quarter
turn. `docs/evidence/2026-11-12-147-register-word-order.txt` has the disassembly, the arithmetic,
the mutation that reddens the host suite against the old code, and the run.

**Why eleven runs of reading the uniform buffer could not see it:** the buffer held the right
*values* in the wrong *order*, and a probe that prints the buffer prints values.

---

The 2D canvas is implemented and its records are replayed — `canvas: quads=4 draws=2
mesh=2 corners=24 eyes=1` every frame, and the frame census places the panel on the screen
using the same matrix pointer the GPU is handed — and **no canvas pixel reaches the panel**.
As of batch 143 `mdk_3ds/tools/emurun.sh --expect hud` is **not** self-referential: it
demands the log line *and* 25 pixels of the (0,0,255) canary the renderer draws through the
canvas's own state and space, and **it fails**. That failure is this item, stated as a
command. Do not close it by making the gate cheaper.

**THE SHORTEST TRUE DESCRIPTION OF THE BUG, as of batch 146 — read this first; every
"shortest true description" below it in this file is a layer of an investigation, and the ones
labelled 144 and 145 are superseded.** Eleven emulator runs have now removed, one at a time and
each with its own run, everything between "the canvas's quads are queued" and "the GPU draws
them":

* the matrix's **arithmetic** — `.constf` literals holding exactly
  `registers(canvasMatrix(600,360))` paint the canary (100 px, `--expect hud-visible` exits 0,
  the level collapsing to the clear colour, which is what that matrix applied to world geometry
  predicts);
* the uniform **location** (vertex uniform 0 and 2 both give 0 px), the canary's **position**,
  and `C3D_BindProgram` after a write;
* the canvas's **geometry, buffer, texture, combiner, tint, cull, depth, clip and viewport** —
  with `mvp` not read at all, the canary paints 24000 px;
* **whether a per-draw matrix write reaches the shader at all** — it does: forcing the per-draw
  write on and adding `+0.5f` to clip x moves 73427 of the panel's 96000 pixels. Batch 145's
  headline, "a uniform written after `installEye` does not arrive", is withdrawn, and citro3d's
  own `C3D_UpdateUniforms` says why it was always impossible: it uploads every maximal run of
  dirty registers at every draw, so position in a frame cannot matter.

**AND THE BUFFER IS NOW READ, WHICH IS WHERE THAT LEAVES IT.** Immediately before the canary's
`C3D_DrawArrays`, `C3D_FVUnif[GPU_VERTEX_SHADER][0..3]` holds exactly the four register rows
that paint, with `dirty=1111 uv=1 tint=1` — measured at every sampled frame to 2460, in
`docs/evidence/2026-11-12-146-uniform-staging.txt`. The CPU side is correct at the instant
before the draw that produces nothing. **The only link left is the GPUCMD upload and what the
GPU does with it.**

**THE NEXT EXPERIMENT, one run, with the level as the instrument.** In `installEye`, write
`pica::canvasMatrix` INSTEAD of `eyeVp`, and delete every other matrix write in the frame.
Then **diff the panel crop against `docs/evidence/2026-11-12-145-literal-mtx.png`** — 90743 px
of clear `(16,32,48)` plus 100 px of exact `(0,0,255)` is the signature of the canvas matrix
genuinely reaching every draw. Clear colour plus canary means the matrix arrives from
`installEye`'s write and not from a per-draw one, and the question becomes the GPUCMD stream
around `C3D_DrawArrays`; "garbage" instead means the shader does not hold what the buffer
holds. Iteration 144 ran a version of this and recorded "the level turns to garbage as
expected" **without diffing it against the clear-colour signature**, which is the entire
discriminator — do the diff.

**TWO TRAPS ON THE WAY, both measured.** `C3D_FVec` declares its members `w, z, y, x`
(`c3d/types.h:37-64`), so reading the uniform buffer through `.x/.y/.z/.w` prints every
register component-reversed and looks like a transpose bug that does not exist; MDK writes
`C3D_Mtx::m[]` and is safe, because `pica::registers()` emits memory order (x,y,z,w). **[CORRECTED
BY BATCH 147 — this is the sentence that was the bug. The reversal is real, `registers()` emitted
exactly that reversed order into raw memory, and MDK was NOT safe: the trap note described the
hardware correctly and then drew the wrong conclusion about its own code.]** And
`C3Di_LoadShaderUniforms`, run by `C3D_BindProgram`, stages the shader's own FLOAT24 constants
whose re-upload **clears `C3D_FVUnifDirty[stage][id]`** — a real way for an app's uniform write
to be silently dropped, and the vault's `c3d-interleaved-draw-leaves-vertex-uniforms-stale`
hazard with the named cause the vault note records as unproven. It cannot fire on this shader:
its single FLOAT24 constant has register id **95**, not 0-5 (measured from the shbin; picasso
allocates `.constf` downward from 95 while `.fvec` allocates upward from 0, so ~90 `.constf`
lines would start eating live `.fvec` registers).

**DO NOT RE-RUN**: the uniform location, the canary's corner-vs-centre position, rebinding after
a per-draw write, per-draw write ordering, the canary's vertex layout, or any probe that judges
the matrix through the canary alone when a level-sized instrument would settle it in one run.

Measured and rejected, each in its own emulator run. Iteration 142's list is unchanged and
still valid; iteration 143 added these (numbers in `docs/PROGRESS_LOG.md`, iteration 143 —
do not re-derive them):

- **the queue order, which iteration 142 named as the next candidate — CLOSED.** With the
  scene replay skipped so the canvas/diagnostic draw is the ONLY draw in the frame, the
  panel is 100% the clear colour. Position among the scene's draws is not it.
- the tint and `GPU_PRIMARY_COLOR`, re-measured independently (scene combiner at
  `MODULATE(tex, PRIMARY)`: the level renders unchanged, so the primary arrives white with
  alpha 1);
- **command-buffer overflow, measured at the draw and not at frame end**: `C3D_GetCmdBufUsage()`
  is 0.101 when the canvas draws are issued (draws #137-139 of ~150);
- **the canvas's own mesh, texture and matrix**: forcing a known-good arena texture changes
  nothing, and the mesh's first corner reads back as (500.00, 287.00, 0.00) in the same
  linear region as the scene's meshes;
- **the canvas's geometry and transform, by substitution** — **AND THE PROBE THAT
  MEASURED IT, WHICH IS VOID.** Iteration 143 drew mesh 0's first 6 vertices at the tail
  through the frame's own `eyeVp` and got zero pixels, and concluded the tail was dead.
  Mesh 0's first 6 vertices are 2 triangles out of the 1050 the census counts, and the
  census records 90 behind the camera and 13 offscreen: **nothing established that those
  two triangles were visible**, so the null result could not distinguish "the tail is dead"
  from "those two triangles are not on the panel". See the description below.

**THE SHORTEST TRUE DESCRIPTION OF THE BUG, as of iteration 144 — SUPERSEDED by batch 146
above; kept because the runs it summarises are still good measurements of what they
measured.** The frame's tail is
**alive**: with the scene replay skipped, replaying the whole first bucket at the tail —
after `installEye`, after everything iteration 143 blamed — paints the level, 46831 of the
panel's 96000 pixels. Iteration 143's "the tail is dead" is REFUTED, and so is the
explanation it was used to buy. What is left, and it is much narrower:

- **the white texture is fine, and so is the `C3D_TexFlush` theory built on it.** The level
  redrawn with `Pass::Overlay`, the white texture and a green tint paints 644 pixels of
  exactly (0,255,0) on the panel **with that flush already landed**. The white texture
  samples opaque. The flush stays as a P3 obligation on real silicon, not as a fix.
- **the mesh cache flush is not it either, and it cannot be, in this harness.** Mandarine's
  PICA renderer reads the same host memory the emulated CPU wrote, so there is no
  CPU/GPU incoherence for a clean to fix; and thrashing the 8 KB D-cache with 512 KB of
  unrelated linear writes immediately before the draw changes nothing.
- **a per-draw matrix write is not it.** Shifting `r->eyeVp` by 40 world units after
  `installEye` installed the unshifted one, and forcing the write on every draw, MOVES THE
  LEVEL (the panel's colour histogram changes wholesale). A matrix written between draws
  reaches the shader.
- **the canvas's records are well-formed.** Two draws per frame on mesh 2, `valid`, 24
  corners, capacity 384, in the same linear region as the scene's meshes (0x305ea280
  against mesh 0's 0x30249d00), corners at (500,287)-(584,350) and three 8x12 quads, and
  `pica::canvasMatrix` logged float by float as
  `[0 -0.00333 0 0][-0.00556 0 0 0][0 0 0 0][1 1 -0.5 1]`, which is the derivation the
  comment claims: clip = (1-2y/360, 1-2x/600, -0.5, 1).
- **and no canvas-space geometry has ever been shown to rasterise.** Not mesh 2 through
  `canvasMatrix`; not the same 24 corners copied into the arena mesh's proven-drawable
  buffer; not a quad covering the whole canvas (0,0)-(600,360), which is the whole clip
  square; not through a hand-written plain orthographic canvas matrix with the quarter turn
  removed; not through the identity matrix at clip z of 0, -0.01, -0.5 or -0.99; not at w of
  1, 10, 100 or 1000; either winding, so it is not the negative determinant; not from the
  scratch buffer and not from a dynamic mesh.

**A PROBE FLAW THAT VOIDED SIX RUNS, RECORDED FIRST BECAUSE IT WILL BE TEMPTING TO RE-RUN
THEM.** Probes 5f through 5i built their quads at **vertex z = 0** and drew them through the
identity matrix, so clip z landed on exactly 0 — and this GPU's clip volume is [-w, 0], the
fact `pica::canvasMatrix`'s own comment records and the reason it puts the canvas at -0.5.
Every one of those quads looked clipped before rasterisation, which is why the scratch buffer
and the per-draw write were each "proven" guilty in turn. A probe whose geometry cannot reach
the screen proves nothing about the thing it was testing. **The clip-z explanation for those
nulls was later withdrawn** — the bypassed-transform build paints at both clip z = 0 and clip
z = -0.5 — but the probes were still worthless, for the reason two paragraphs above: nothing
about the matrix survives to the shader on those draws, whatever the z.

**THE NEXT EXPERIMENT, and it is the one this item has pointed at twice without running.**
The sibling `tetrisphere-godot` port measured that **citro3d's top screen is a 240x400 memory
target whose rotation interacts with the projection and the viewport**: `C3D_FrameDrawOn`
installs the *memory-space* viewport, a physical-size `C3D_SetViewport(0,0,400,240)` silently
clips the picture to a band, and **replacing `Mtx_OrthoTilt` with `Mtx_Ortho` for a UI ortho
SWAPPED THE TWO AXES** because the `*Tilt` variants carry the top-screen rotation a plain
ortho does not (`docs/validation/c3d-sky-orient-2026-10-27.md`,
`c3d-hud-viewport-2026-10-27.md`, `c3d-hud-shared-2026-10-28.md`, `c3d-ui-space-2026-10-27.md`
in that port). MDK bakes its own quarter turn into every matrix and sets
`C3D_SetViewport(0, 0, 240, 400)`, the memory size, so it is on the good side of the
viewport half — **but nobody has checked whether citro3d applies its own top-screen
flip/rotate below the matrix, which would hit exactly the draws that write their own matrix
per draw and leave the ones that inherit `eyeVp` alone, which is the shape of what is left.**
**BUT THAT DOOR IS NOW CLOSED BY MEASUREMENT, NOT BY READING, so do not spend a run on
it.** The `*Tilt` helpers do exist in the linked `libcitro3dd.a` (`arm-none-eabi-nm
--defined-only` lists `Mtx_Ortho`, `Mtx_OrthoTilt`, `Mtx_PerspTilt`, `Mtx_PerspStereoTilt`),
so the sibling's finding is version-relevant — but a rotation or a viewport cannot explain
what was measured: **a quad whose clip square is exactly (-1,-1)-(1,1) paints nothing**, and
no rotation, tilt or viewport mapping turns a full-clip-square quad into zero pixels. The
same quad drawn through a hand-written plain orthographic matrix, through the identity
matrix at four different clip z, and at four different w, paints nothing either. Whatever
citro3d does below the matrix, it is not what is removing these draws.

**THE DISCRIMINATING EXPERIMENT WAS RUN, AND IT ANSWERED. THE SHADER IS THE SPLIT.**
`mdk_3ds/shaders/mdk_c3d.v.pica` is editable PICA assembly (`picasso` builds it from
`shaders/mdk_c3d.v.pica`), so the four `dp4 outpos.*, mvp[n], r0` lines were replaced, for one
build, with `mov outpos.xyz, inpos` + `mov outpos.w, ones`: **the vertex position becomes the
clip position and `mvp` is not read at all**, so nothing about the uniform — its location, its
dirty range, when it was written — can matter. A probe then wrote a clip-space square
((-0.9,-0.9) to (0.9,0.9) at z = -0.5, w = 1) into the arena mesh's own buffer and drew it
`Pass::Overlay` with the white texture and a green tint.

**THE PANEL FILLED: 58320 pixels of exactly (0,255,0) — and 24000 pixels of exactly (0,0,255),
which is the shipped diagnostic canary, painting for the first time in the project's history.**
24000 is exactly a quarter of the panel, which is what the canary's raw canvas corners
(0,0)-(16,16) clipped into the clip square produce, so the number is not a coincidence
reading. Then `installEye` was made to install `pica::canvasMatrix` as the ONLY matrix the
frame ever writes, with the shipped shader restored: the level turns to garbage as expected
and the canary still paints nothing.

**WHAT THAT CLOSES, ALL AT ONCE, WITH NO FURTHER PROBES NEEDED.** With the transform bypassed
the canvas's own state paints, so every one of these is now proven fine and must not be
re-probed: the Overlay pass and its blend, the white texture, the tint uniform and
`GPU_PRIMARY_COLOR`, the `MODULATE` combiner, the depth state, the cull mode and winding, the
canvas mesh's bytes and its buffer, the scratch buffer, the vertex attribute and buffer
permutation, and the clip window — including clip z = 0 and clip z = -0.5, both of which
painted, so **the "clip z = 0 is this GPU's far plane" explanation recorded above is itself
wrong and is withdrawn.** What is left is the matrix the vertex shader holds: the geometry
survives, the transform does not.

**AND IT RETIRES THE UNIFORM-WRITE-TIMING THEORY TOO.** `C3D_BindProgram` after the per-draw
`C3D_FVUnifMtx4x4` changes nothing, and installing the canvas matrix once at eye scope — the
only matrix the frame ever writes — also changes nothing. So it is not that a write between
draws is dropped; it is that **a canvas-space matrix reaches the shader and does not do what
its arithmetic says it should**, while `eyeVp` through the identical `registers()` +
`C3D_FVUnifMtx4x4` path renders the level correctly.

**THE NEXT EXPERIMENT, and it is a shader experiment, not a C++ one.** Make the vertex shader
report the matrix it holds instead of using it: write `mvp[3]` (squared, because Picasso has
no `abs`) into `outclr` while the position stays the raw vertex position, so the Overlay
combiner paints the matrix column as the pixel colour. **This was attempted and the probe
painted nothing at all, which is itself a clue and has not been explained: reading `mvp` into
`outclr` suppressed a draw that paints when `outclr` comes from `tint`.** Two readings, and
they are cheap to separate: either the `.fvec`-to-colour path is illegal in a way `picasso`
accepts and the GPU rejects, or the uniform the shader reads as `mvp` is not the memory
citro3d was told to write. The second is the one worth the effort, and the way to test it is
to give the shader a SECOND matrix uniform at a location MDK chooses, write the same matrix to
both, and have the position use the new one — if the new location works and location 0 does
not, the collision is the bug and its name is knowable.

**A PROBE THAT COULD NOT HAVE WORKED, RECORDED SO IT IS NOT RE-RUN.** The first version of
the tail probe used `Pass::Solid`. `drawRecorded` forces the tint white on every non-Overlay
draw, so a magenta Solid draw is impossible and its null result meant nothing. Overlay is the
only pass whose tint is the record's colour, and Overlay takes `world` as the whole
transform — hence handing it `eyeVp`. A probe whose failure mode is invisible is not a probe.
**Iteration 144 fell into the same trap from the other side** and lost a run to it: a probe
that switches a record to `Pass::Solid` to reuse a tint it set also switches the combiner to
`REPLACE(tex)`, which ignores the tint entirely, so the probe was painting white while being
scored for green. Score the colour the state actually produces, not the colour the probe
intended.

**What is still shared with the old, unrelated-looking symptom**: the diagnostic draw has
never painted either, and the shader's w note records a previous failed hunt for that. It
is now the canary (batch 143), which makes "the diagnostic does not paint" and "the canvas
does not paint" the same measured statement instead of two suspicions.

**THE HEAD-OF-FRAME PROBE WAS RUN AND IS INCONCLUSIVE — do not re-run it as it was.** Attempt 2
of this row drew a red triangle immediately after `installEye`, before the replay, with the
white diagnostic left at the tail. It cannot answer the question: **the level-7 scene covers
every pixel of the panel** (947 of 1050 triangles on-screen; the capture is a full-panel
sunburst with no sky gap) and the probe's depth test and write are both off, so the scene
paints over it whether or not it landed. The two captures differed only by the scene's own
animation. Any positional probe here must be **observable**: one known-colour quad (pure
magenta, alpha 1, the whole canvas) as the canvas's only quad, diffed against a run with
`hudReady` false — a colour the scene cannot produce.

**PRIOR ART, found by the pi-knowledge lookup and not yet tested (the sibling `tetrisphere-godot`
port hit this same class and wrote it up).** Its validation notes —
`docs/validation/c3d-sky-orient-2026-10-27.md`, `c3d-hud-viewport-2026-10-27.md`,
`c3d-hud-shared-2026-10-28.md`, `c3d-ui-space-2026-10-27.md` — measure that **citro3d's top
screen is a 240x400 memory target whose rotation interacts with the projection and the
viewport**: `C3D_FrameDrawOn` installs the *memory-space* viewport from `frameBuf.width/height`,
a physical-size `C3D_SetViewport(0,0,400,240)` silently clips the picture to a band, and
**replacing `Mtx_OrthoTilt` with `Mtx_Ortho` for a UI ortho SWAPPED THE TWO AXES** because the
`*Tilt` variants carry the top-screen rotation that a plain ortho does not. MDK bakes its own
quarter turn into every matrix (`eyeViewProjection` step (B), and `canvasMatrix` applies the
same one) and sets `C3D_SetViewport(0, 0, 240, 400)`, i.e. the memory size — so the port is on
the good side of the viewport half of that finding, but **nobody has checked whether citro3d
applies its own top-screen flip/rotate below the matrix**, which would hit exactly the two draws
that write their own matrix per draw and no others. That is the first thing to test, and it is
testable from the installed `libcitro3dd` disassembly the sibling already read.

Not the vault's stereo-replay HUD trap: `citro3d-display-list-stereo-replay-non-fast3d.md`
needs a second eye whose callback runs after the replay fires, and this capture is mono.

**ITERATION 145: THE SPLIT IS THE UNIFORM DELIVERY, AND THE LOCATION-COLLISION READING OF
ITERATION 144 IS DEAD.** Five emulator runs on Mandarine r1.2.1 (md5 `0d2adab6…`),
`DISPLAY=:99`, mono, forced llvmpipe, each with its own fresh lane and log. The shipped build
measures `emurun.sh --expect hud` **exit 7**: `canvas: quads=4 draws=2 mesh=2 corners=24
eyes=1`, level rendered (147 distinct panel colours), **0** canary pixels of exact (0,0,255).

1. **`mvp` IS vertex uniform 0.** The guest line reads `uniforms: mvp=0 uvtransform=4 tint=5`.
   The `.constf constants(...)` does **not** push the named `.fvec` uniforms up, which is what
   the long comment at `renderer_c3d.cpp:270-290` asserted; that comment is corrected in this
   batch. Picasso assigns `.fvec` addresses in declaration order — reordering the declarations
   to `uvtransform, tint, mvp[4]` makes the same line read `mvp=2 uvtransform=0 tint=1`.
2. **THE VALUES ARE CORRECT AND SUFFICIENT.** For one build the four `dp4` lines were driven
   from `.constf` literals holding *exactly* the floats `registers(canvasMatrix(600,360))`
   produces — `col0=(0,-2/360,0,1) col1=(-2/600,0,0,1) col2=(0,0,0,-0.5) col3=(0,0,0,1)` —
   with the uniform never read, and the canary moved from the canvas corner to the canvas
   centre. **`--expect hud-visible` EXITED 0 with 100 canary pixels**, and the panel was
   90743 px of the clear colour `(16,32,48)`: the level vanished, exactly as canvasMatrix
   applied to world-space geometry predicts. 100 px is the predicted size of a 16-canvas-px
   square at the centre, so the mapping is confirmed float-for-float, not just directionally.
3. **THE CORNER IS NOT THE REASON.** Shipped uniform path, canary at the centre: **0 px**. The
   canary's position was never the variable, so the corner canary stays.
4. **THE UNIFORM LOCATION IS NOT THE REASON.** `mvp` moved to vertex uniform 2 (`mvp=2`): **0
   px.** Iteration 144's named experiment — a second matrix at a location MDK chooses — is
   therefore answered without being run: there is no location collision, at 0 or at 2.
5. **THE PER-DRAW MATRIX WRITE DOES NOT REACH THE SHADER.** `drawDiagnostic` was made to write,
   through the same `C3D_FVUnifMtx4x4` at the same point in the frame, a matrix that maps the
   canary's canvas (0,0)-(16,16) onto clip (-0.8,-0.8)-(0.8,0.8) — **64% of the panel**, so no
   arithmetic subtlety, clip plane or crop can hide behind it. **0 px.** Re-issuing
   `C3D_BindProgram` immediately after that write also gives **0 px** (which independently
   confirms iteration 144's `BindProgram`-after-the-write result).

**WHAT THAT LEAVES, STATED PRECISELY.** A vertex-shader uniform written by `installEye` reaches
the shader — the level renders, and it renders through the identical `registers()` +
`C3D_FVUnifMtx4x4` path. A vertex-shader uniform written later in the same frame, after
`installEye`'s `C3D_BindProgram`, does not: the shader keeps `eyeVp`, so every canvas-space
vertex is projected by the level's camera and lands off-panel. That is one sentence, and it
explains the level being fine and the canvas being empty with one program and one uniform.

**AND IT IS BIGGER THAN THE HUD.** `drawRecorded` composes and writes `MeshDraw::world` per draw
for exactly the same reason (batch 142's object-placement fix). If post-`installEye` uniform
writes do not land, **no object is placed by its world matrix either** — the arena batch and
Kurt carry identity worlds so the level looks right, which is precisely why nothing else
noticed. Any follow-up must check object placement, not only the HUD.

**AND IT CONTRADICTS THE LIBRARY'S OWN DOCUMENTED USAGE, WHICH IS THE MOST USEFUL THING HERE
AND IS A VAULT FIND, NOT A GUESS.** The devkitPro example in the vault
(`60-references/3ds-platform/citro3d/01-overview-and-init.md:179-186`) is
`C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLoc_projection, &projection);` immediately followed by
`C3D_DrawArrays(GPU_TRIANGLES, ...)`, with the program bound once in `sceneInit` and never
re-bound. **Write-then-draw with no re-bind is the pattern the library's own examples ship, and
it works there.** So "a post-bind uniform write does not reach the shader" is a MEASURED
SYMPTOM IN MDK'S FRAME, not a citro3d rule, and the next iteration should look for what in
MDK's frame breaks the pattern rather than accepting the ordering as a law. The difference
between the example and MDK that stands out is that MDK calls `C3D_BindProgram` once per eye
per frame, and `C3D_BindProgram` calls `C3Di_LoadShaderUniforms` unconditionally — even on the
`beq` path taken when the program is already the current one (`arm-none-eabi-objdump -dr
base.o`) — which re-arms the shader's own param-data upload every frame.

**THE NEXT EXPERIMENT, and it is a C++ one now, not a shader one.** Use the LEVEL as the
instrument instead of a 100-px canary, because the level is a 46000-px signal and cannot be
argued about: in `installEye`, write `pica::canvasMatrix` INSTEAD of `eyeVp` and delete every
other matrix write in the frame. If the level vanishes to the clear colour (the probe-1
signature) the write landed and the rule is about *timing*, not about the value; if the level
renders normally it did not. Then test the two candidate orders directly — bind→write→draw and
write→bind→draw — on the level, not on the canary. `C3D_UpdateUniforms` is called from
`C3Di_UpdateContext`, which `C3D_DrawArrays` calls first (`arm-none-eabi-objdump -dr
drawArrays.o`: `C3D_DrawArrays` is the only caller besides `C3D_DrawElements` and
`C3D_ImmDrawBegin`), so citro3d's own bookkeeping says a per-draw flush *should* happen; the
measurement says it does not, and the measurement wins.

**DO NOT RE-RUN, all measured 2026-11-12:** the uniform location (0 and 2), the canary's
corner-vs-centre position, `C3D_BindProgram` after a per-draw matrix write, and any probe that
judges a matrix through the canary alone when a level-sized instrument would settle it in one
run.

**SEPARATE FOLLOW-UP FROM REVIEW-1 F1, LANDED AS A COMMENT NOT A CODE CHANGE.** The per-mesh
`GSPGPU_FlushDataCache` in `mdk_render_update_mesh` is redundant: `C3D_FrameEnd(0)` flushes the
whole `__ctru_linear_heap` before the queue is submitted, which is AGENTS.md P3's own sentence,
and `GSPGPU_FlushDataCache` is a synchronous `svcSendSyncRequest`. Dropping it is a real
ARM11 win and is gated on an original-3DS capture, because Mandarine cannot distinguish the
call from its absence. The comments now say this instead of claiming P3 needs the call.

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

## Kurt fires (2026-11-19, branch port/kurt-firing): what landed and what is still open

The chain gun was unreachable: `kKeyFire` was read by nothing. Landed, all pinned by
`golden/L8_fire.txt`, `L8_fire_run.txt` and `L7_fire_run.txt` (byte-identical to the C#
probe `tests/scripts/fire_probe/`): `Kurt.UpdateFiring`/`StopFiring`/`UpdateMuzzle` and the
SHOT/RUN_FIRE states (`Player`), the MULTIFIRE/GATTFIRE loop, the K_MUZZF flash drawn behind
Kurt (`KurtSprite`), `ScriptRuntime.FireChainGun`/`AimScore`/`ChainGunHit`/`SparkAt`, the
wall shot through `HitGroup`, the health bar (`ShowBarValues`/`UpdateBar`/`GetBar`, fed to
both HUDs), the kill's `Explode` (sound, `FlashAt`, `BreakUp` with `Debris.BreakUp` and gore
drops, `SpawnExplosion` and the effect branch of `ObjectMotion.Update`), opcodes 181 and 209,
and the Debris `Trail`/`Updraft` delegates. Found on the way and fixed: `Effects` erased from
the middle of a deque of values, which moved the elements behind the hole, so the update
pass skipped the effect after every removal and a wound's pointer named its neighbour.

Still open:
- **Items** (`Kurt.UpdateItems`, the THROW state, `ItemUsed` -> `Items.UseItem`, `Items.cs`,
  `Twister.cs`, the bomb): unreachable until pickups are collected (`CollectPickups` and
  `Inventory.Collect` are not ported, so the inventory is always empty and the C#'s
  `item is None` return is the only path). A slice of its own.
- **Opcode 209's platform half** waits for `Kurt.Platform` (the platform footing).
- **The sniper half of `StopFiring`** (`LeaveSniper`) and the ride's K_SURF muzzle offset
  wait for the sniper and the ride.
- **The run goldens print no `pc=`**: `Player::facing()`'s trig LUT (the owner-gated item
  above) leaves a running Kurt ~1e-5 off the oracle, and the sum of the pieces' centres
  straddles a printed decimal on four lines of 480; with `cosf`/`sinf` it matches byte for
  byte (measured). `life=` pins the random stream there instead.

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

## DEFERRED by the 2026-11-05 owner directive: review-6's remaining `MathF.Min`/`MathF.Max` sites

**Not the next slice.** `docs/OWNER_DIRECTIVE_2026-11-05.md` supersedes the old priority order and
cuts new 1-ULP fidelity work in favour of the integration sprint. This list is retained so the class
is not lost; do not start it while the directive stands.

Review-6 F1's class is wider than the file it reviewed. Batch 65 converted FollowCamera's five
sites to the shared `mathMin`/`mathMax` pair and pinned the reachable NaN/signed-zero differences
(`FollowCameraMathFMinMaxMatchTheOracle`). The remaining candidates in this class are:

- `mdk_core/src/game/player.cpp` — the eight float sites in `Kurt.cs`/`Kurt.Combat.cs`/`Kurt.Ride.cs`:
  the dead-step gravity and death animation, the knock-down slam, air gravity, the chute brake, the
  downhill speed and the final gravity. (`updateAir`'s `std::max` on the integer jump ticks is NOT in
  this class — the C# writes `Math.Max` there.)
- `mdk_core/src/game/kurt.cpp` — the four float `MathF.Max` sites: the two flash fades, the knock
  counter's drain and the invulnerability timer. The same file's integer health/scaled-damage max and
  `hurtFlash_`'s `Math.Clamp` spelling are separate oracle shapes, not blind conversions.
- `mdk_core/src/game/bsp.cpp` — the two `Bsp.cs` sites: the plane offset and the retry `t2`.

One file per batch: match each site to the exact C# line first, pin the branch or value the oracle
can reach, and record any site whose ternary is deliberately the oracle's own spelling rather than a
missed conversion. Do not sweep all three files as a mechanical `std::min`/`std::max` rewrite.

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

## The Linux `--enhanced` path's remaining Vulkan validation errors (2026-11-17)

Found while using the Linux enhanced look as the PC reference for the 3DS
Enhanced Mode work. The original look runs with **zero** validation messages.
The enhanced path's view-type mismatch (bindings 3/4/5 given a 2D_ARRAY view
where `enhanced.frag` declares `sampler2D`), the unrequested `samplerAnisotropy`
and the unrequested `shaderDemoteToHelperInvocation` were fixed in the same
batch, frame pixel-identical. Still open, counts from one level-3 run of 299
frames (`--headless --fixed-step --enhanced`), none of which moves a captured
pixel on this host:

- `vkCmdPipelineBarrier-srcAccessMask-02815` (x3588): an image barrier with
  `srcAccessMask = TRANSFER_WRITE` under `srcStageMask = COLOR_ATTACHMENT_OUTPUT`.
- `VkRenderPassBeginInfo-clearValueCount-00902` (x897): a render pass with two
  CLEAR attachments begun with `clearValueCount = 0`.
- `vkResetDescriptorPool-descriptorPool-00313` (x861): a pool reset while a
  submitted command buffer still uses its sets -- a real race on a GPU that runs
  behind, even though this host does not show it.
- `vkDestroyDevice-device-05137` (x363): images/memory/views still alive at
  device destruction (teardown leaks).

## The 3DS frame is not deterministic at one distant object (2026-11-17)

Two runs of the SAME 3DS build (`mdk_3ds/tools/compare_linux.sh 3 300`, every
Enhanced Mode toggle off, FIXED=1 clock, Mandarine on `:99`) differ in 6 pixels,
crop (216..219, 129..130): a small distant object beside the level-3 tower. The
rest of the frame is bit-identical, and level 7 frame 300 is bit-identical run to
run. Suspect: an ordering keyed on a heap ADDRESS (ObjectView's layout cache keys
on the model's address, `mdk_core/src/game/object_view.*`), so same-depth parts
draw in an order that depends on where the allocator put them. Not yet
investigated; it makes any claim of "N pixels changed" at that place unreadable,
and it is why the haze batch's 7-pixel attribution was withdrawn.
