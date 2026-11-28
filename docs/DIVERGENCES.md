# C# vs C++ divergences: root causes, measured (2026-11-20)

The port agents for packages 1, 4/8, 5, 6, 13, 16, 18b and 19 each recorded a place where the
C++ stops matching the C# oracle bit for bit. Several blamed the trig lookup table
(`mdk::fastSin/fastCos`, `util/math_lut.h`, through `Player::facing()`) without testing it.
This file settles each one by measurement. For each, it gives the first step and field that
diverge, the root cause on both sides, the evidence, and whether a player could see it.

**How it was measured.** Every case is the C# probe (`tests/scripts/*_probe`, built per its
header against .NET 10.0.15 at `/tmp/mdk-dotnet10`) run beside its `tick1 dump` twin. Both
sides were instrumented temporarily (nothing committed) to print the state in question as IEEE
bits each step: Kurt's feet, yaw and speeds; every debris piece and effect; a traced object's
position, push, velocity, yaw and speed at `ObjectMotion`'s motion step. The packages that are
not on this tip yet were measured on exports of their own commits (`git archive`): #19 at
5d1d4ab, #13 at 8362320 and #18b at 9de8765. All three sit on package 8 (53605b5), as this tip
does. The prior-solution lookup is in the ledger entry.

**Owner direction (2026-11-20).** The trig table stays, and bit-identical trig-derived values are
not required ("I am not working with galaxy sized scale"). A drift whose only cause is
floating-point rounding is recorded below as **rounding, accepted** and left alone. Logic
divergences, and rounding that changes behaviour, are fixed and pinned.

| # | Divergence | Root cause | Kind | Status | Visible to a player |
|---|---|---|---|---|---|
| D1 | Kurt's walk and aim match only at yaw 0 or level/upward scope pitch | `Player::facing()` uses the LUT; the C# uses `MathF.Cos/Sin` | rounding (LUT) | accepted | no |
| D2 | L8: walking XG's `Push` is 1 ULP off | root motion's `Vector3.Transform` is fused on .NET 10; the C++ rotation is not | rounding (FMA shape) | accepted; one-line fix verified, not landed | no |
| D3 | L3 HMO_6: the port spawns objects the C# doesn't | the fans let out no `FanSpark` before package 8, so the shared RNG stream shifted | logic (missing behaviour) | **resolved by package 8** | yes, before package 8 |
| D4 | L5: Gunter's yaw drifts next to Kurt | D1: Gunter turns toward Kurt's feet, which the LUT moved | rounding (LUT, via D1) | accepted | no |
| D5 | L8 GUNT_2: one extra debris piece over s=152-161 | debris bounce scaled by `dot*1.4` instead of `(n*dot)*1.4`; the effects' bounce did the same | port bug (operation order) | **fixed**, golden `L8_hazards_debris` | barely: one piece lasts 1/6 s longer |
| D6 | Stream: 45 pixels on Kurt | a UV exactly on a texel-row boundary; the matrix grouping's rounding picks the row | rounding (presentation) | accepted | no |
| D7 | L3 fan room drifts before the save | same as D3 | logic | **resolved by package 8** | as D3 |
| D8 | What may hide Kurt's sprite: only surfaces between the camera and him, not the walls and floors of his own space | owner decision (fork divergence): upstream's floor-only depth pull replaced by the original's draw-order rule | **deliberate divergence** (owner, 2026-11-27) | **shipped** (fix/kurt-sprite-occlusion) | yes: his gun arm and legs no longer vanish into walls and ledges |

## D1. Kurt's walk and aim: only the trig table

- **First divergence:** `items_probe` vs `tick1 dump items 3 400 "left:1-12,fwd:12-400"`. Steps
  1-11 (turning in place) match. At step 12, the first running step, `feet.y` is 4 ULP off
  (`BE25E70B` in the C#, `BE25E70F` in the C++). `feet.x`, `feet.z`, yaw and forward speed are
  identical.
- **Cause:** C# `Kurt.Facing` (src/Mdk.Game/Kurt/Kurt.cs:128) is `MathF.Cos/Sin` of
  `float.DegreesToRadians(Yaw)`. The C++ `Player::facing()` (mdk_core/src/game/player.cpp:206-209)
  is `fastCos/fastSin` of the same radians.
- **Experiment:** `Player::facing()` returned `cosf/sinf` (glibc, which matches `MathF.Sin/Cos`
  on this host, as package 18a found). Nothing else changed.

  | Run (with exact trig) | Differing lines with the LUT | With exact trig |
  |---|---|---|
  | L3, 600 steps | 589 | 0 |
  | L3, 900 steps: turns, strafes, turbo, a jump, back | 871 | 0 |
  | L4, 900 steps | 135 | 0 |
  | L5, 900 steps | 656 | 0 |
  | L6, 900 steps | 253 | 0 |
  | L7, 900 steps | 851 | 0 |
  | L8, 900 steps | 867 | 0 |
  | `sniper_probe` / `tick1 dump sniper`, L3, start yaw 37 / 123.5 / 250 (looks down, sidesteps, zooms, fires, turns) | 584 / 586 / 584 | 0 / 0 / 0 |
  | Same at L7, start yaw 37 / 123.5 | -- | 0 / 0 |

  The items runs compared every printed field with Kurt's feet, yaw and speeds as bits. The
  sniper probe prints every float as bits, including the scope view's matrices.

  The drift **vanishes**: there is no second cause. The experiment was reverted and the LUT
  stays (owner, 2026-11-20). Should the owner ever want the bits, the change is that one line,
  plus `follow_camera.cpp:447`'s copy for the camera.
- **Visible:** no. The table's worst error in this range is 4.76e-06. Over 470 steps that moves
  the feet by about 1e-5 units (docs/OPEN_ITEMS.md, "Player::facing()").

## D2. L8's walking XG: root motion's fused rotation

- **First divergence:** `save_probe 8 600 30 ""` vs `tick1 dump save` at 5d1d4ab (package #19's
  `objects/8/variables/Push`). Tracing the XGs at the motion step finds the first differing field:
  XG #1004's `Push.x` at its 140th motion tick (about step 280), `3E714CBB` in the C# against
  `3E714CBC`. It happens with no move command and speed 0, so it comes from the animation's root
  motion and not from the walk.
- **Cause:** C# `MdkObject.UpdateAnimation` (src/Mdk.Game/Objects/MdkObject.cs:433) does
  `Push += Vector3.Transform(Animation.RootMotion[f], Matrix4x4.CreateRotationZ(...)) / delta`.
  On .NET 10 that transform computes `x*M11` rounded, then one fused multiply-add of `y*M21`.
  This is the shape `Player::carry` already ports (player.cpp:1293-1311, measured 0 misses in
  200000 cases). The C++ root motion (mdk_core/src/scripts/script_runtime.cpp:3487) calls
  `rotatedZ` (script_runtime.cpp:200), which computes the unfused `v.x*c - v.y*s`.
- **Evidence:** in the 5d1d4ab export, replacing the root-motion call with
  `Vec3{fmaf(rm.y, -s, rm.x*c), fmaf(rm.y, c, rm.x*s), rm.z}` makes the whole L8 save scenario
  match: 0 differing lines in the save, the restore, the 30 steps after it, and the XG trace over
  600 steps.
- **Status:** rounding, accepted (owner direction). The fix is verified but **not landed**. It
  changes nothing a player sees, and `fmaf` is a newlib software call on the 3DS (one per
  root-motion frame per animated object). If the owner wants it, it is that one expression, and
  `L8_save_idle` would pin it.
- **Visible:** no. One ULP of a per-tick push.

## D3 / D7. L3 HMO_6's extra objects and the fan room: package 8's fan sparks

- **First divergence (without package 8's spark):** `save_probe 3 200 30 "tp:1:HMO_6:1:2310:-370"`
  (the `L3_save_fan` scenario). The C# save holds 9 objects (`... XM3, XTGUN, BOLT`,
  next_instance 1009). The port without the fans' spark holds 10 (an extra `BOLT`,
  next_instance 1012).
- **Cause:** C# `ScriptRuntime` builds its fans with `Spark = FanSpark` (ScriptRuntime.cs:337).
  Each spark of a live fan draws from the runtime's shared `Random`. Before package 8
  (53605b5), the C++ never bound the spark, so in a room with a fan the RNG stream ran behind
  the C#'s. Every script decision drawn from it afterwards differed, including when HMO_6's XTGUN
  fires a BOLT. Package 8 added `fans.setSpark(&ScriptRuntime::fanSparkTrampoline, this)`
  (script_runtime.cpp).
- **Evidence:** at 5d1d4ab (package 8 plus #19), the scenario is byte-identical to the C#, both
  as committed (200 steps + 30) and longer (1200 steps + 60). Commenting out that one
  `setSpark` line brings the divergence back, from the first saved line: 10 objects against 9.
  This is the "fan room" package 19 saw drift before the save (D7), and it is the same cause.
- **Visible:** yes, before package 8: the fans showed no sparks, and the turret's shots came at
  other times. Resolved. `L3_save_fan` (package #19) pins it once #19 merges.

## D4. Gunter's yaw in L5: D1 again

- **First divergence:** `cutscene_probe` vs `tick1 dump cutscene` at 8362320, tracing XGUNTAM.
  With Kurt standing next to Gunter (`at:420:80:-2263:0`, `at:440:60:...`,
  `at:380:120:...`, 400 steps), Gunter turns and both sides are identical. The same holds while
  Kurt walks at yaw 0, and while he walks straight at yaw 124 for 50 steps (no drift reached
  Gunter's turn in that run). In `at:420:80:-2263:0,left:1-30,fwd:30-90`, Gunter's yaw goes 1 ULP off (`43A66AC7` vs
  `43A66AC6`) about 7 steps after Kurt starts walking at his turned yaw.
  `...,right:1-20,fwd:20-200,left:200-230,fwd:230-400` differs on 282 trace lines.
- **Cause:** Gunter's script turns him toward Kurt (`MdkObject.YawTo`, MdkObject.cs:600-602,
  `MathF.Atan2` of Kurt's position). Kurt's feet carry D1's LUT drift.
- **Evidence:** with D1's exact-trig `Player::facing()`, both runs are identical: 0 trace lines
  and 0 probe lines over 600 steps.
- **Visible:** no. Rounding, accepted.

## D5. GUNT_2's explode: the bounce's operation order (fixed)

- **First divergence:** `hazards_probe` vs `tick1 dump hazards 8 400
  "tp:1:GUNT_2:-209:480:40,run:3:XBANG:24840:0:105:0,fire:60-300,watch:XBANG,watch:BOLT,watch:BIGBOLT"`.
  The printed lines differ only at s=152-161 (284 pieces vs 283). Dumping every piece shows
  the first divergence much earlier. At s=68, piece 318, which bounced that step (ticks 87 -> 66),
  has velocity.y 2 ULP off (`BE8425A2` vs `BE8425A4`) while its centre is identical. The
  effects (drops and smoke trails) first differ at s=66.
- **Cause:** C# `Debris.Move` (src/Mdk.Game/Scripts/Debris.cs:399) and `Effects.Move`
  (Effects.cs:334) write `v -= h.Normal * Vector3.Dot(v, h.Normal) * Bounce`. That is
  `(normal * dot) * 1.4`, two rounded products per component. The C++ (`debris.cpp`
  `Debris::move`, `effects.cpp` `Effects::move`) scaled the normal by `dot * 1.4`, a different
  float. A bounced piece's path then differs, and one piece hit something a step later and kept
  its 20 ticks of life. `ObjectMotion`'s bounce (script_runtime.cpp's `normal.x * d * -1.8f`)
  and the mortar's (sniper_rounds.cpp) already had the C#'s order. Twister's `* 2f` is exact
  either way.
- **Fix:** both now scale in the C#'s order (debris.cpp:486, effects.cpp:296). After the fix,
  every piece's centre, velocity and ticks and every effect's position, velocity and life are
  bit-identical over 400 steps in this run, `L8_hazards_bang`, `L8_hazards_kill` and
  `L4_hazards_blast`.
- **Pinned:** `golden/L8_hazards_debris.txt` is the C# probe's output for the scenario above plus
  a new `flying` token. The token appends `fly=P,E`, FNV-1a hashes of every piece's and effect's
  motion. With the old debris bounce it fails at s=68; with the old effects bounce it fails at
  s=66. No existing golden changed.
- **Also in the pieces: their drawn orientation.**
  - **Fixed:** the orientation step itself. `Quaternion.Normalize(orientation * spin)` (Debris.cs:365)
    is, on .NET 10, a Vector128 shuffle-and-accumulate product and a divide by the square root
    of a **pairwise** dot. Measured over 2000 random pairs, the old textbook product with a
    sequential dot was wrong on 1225; the new `scripts::quaternionMultiplyNormalize`
    (debris.cpp:138) is wrong on 0. `system_numerics_tests` pins six of those pairs, and the old
    code fails it.
  - **Accepted:** the spin's creation. `Quaternion.CreateFromYawPitchRoll` (Debris.cs:338) takes
    its sines from `Vector3.SinCos`, a vectorised polynomial, not libm. glibc's `sinf/cosf`
    differ from it on 47 of 3000 spin angles, and with `Vector3.SinCos`'s values the port's
    formula matches all 3000. Porting that polynomial is presentation-only work, so the
    `flying` hash leaves orientation out. Rounding, accepted.
- **Visible:** barely. One spark or chunk of an explosion lives 1/6 s longer.

## D6. The stream's 45 pixels on Kurt: a texel boundary

- **First differing input:** in the frame-600 pose, Kurt's parts 13 and 17 have UV corners at
  v = 0.98787874, which is 163/165: exactly on the line between texture rows 162 and 163. Both
  renderers sample the nearest texel, so each pixel's row is decided by the last bits of the
  interpolated v. Those bits follow the clip-space vertices.
- **Cause:** the C# composes `command.World * view.ViewProjection` with System.Numerics
  (src/Mdk.Engine/Render/Renderer.cs:1072-1073; `CreateLookAt` and
  `CreatePerspectiveFieldOfView` in FollowCamera.cs:393-401). The C++ composes
  `(world * view) * projection` with a scalar multiply (mdk_linux/src/render/vulkan/12_draw.inc:36,49;
  camera_builder.cpp normalises by reciprocal square root). The shaders agree, and it is not
  z-fighting: a CPU rasterisation of the dumped mesh puts the front triangle 7e-4 ahead at every
  patch pixel.
- **Evidence** (Linux build of 9de8765 against the oracle app, both on lavapipe, `DISPLAY=:99`
  in a private namespace, `--stream=7` frame 600 / 10 s, 320x240):
  - As shipped: 45 differing pixels, 40 of them in the patch box.
  - Matrix grouped as the C# does it: 67.
  - Kurt's v nudged 4 ULP down: 35. Nudged up: 71.

  The two colours that swap are rows 162 and 163 of the same dark texture.
- **Visible:** no. Rounding, accepted. Matching it would mean porting .NET's matrix
  construction, and lavapipe's interpolation would still decide each boundary pixel.

## D8. Kurt's sprite and the surfaces of his own space (a deliberate divergence)

Unlike D1-D7 this is not a drift measured and explained: it is a **deliberate departure from
upstream**, decided by the owner and made in its own commit (SPEC.md §15.1: "a divergence from
upstream is allowed when the owner decides it, in its own commit, recorded in
docs/DIVERGENCES.md").

- **What upstream does.** Upstream's C++ (`origin/main` 367cc6fc,
  `src/game/kurt/KurtSprite.cpp` `FloorDepth`, lines 275-299, `FloorBelow = 0.5`) -- and its C#
  before it, and this port until this change (`KurtSprite::floorDepth`) -- depth-tests Kurt's
  camera-facing sprite at his feet and pulls it towards the eye only in front of a FLOOR the
  eye's rays cross at most half a unit above his feet. Walls are never considered. The sprite is
  ~3 units wide at the follow camera's distance and his collision box is +-0.6, so the wall he
  walks along hid his gun arm and side, and the edge of a step more than half a unit high hid
  his legs (found and measured on fix/kurt-sprite-depth, 30caebea: L3 HMO_1/4/5/6, CHMO_8; L4
  MEAT_1; L5 MUSE_2/3; L6 OLYM_4/7; L7 DANT_1; L8 GUNT_1).
- **What the original does.** No depth buffer: the arena is drawn back to front by its BSP and
  an object is drawn in its place in that walk (docs/original-engine.md, Rendering), so the
  floor he stands on and the walls of the space he is in come BEFORE him and never cut him.
  (Not re-checked in Ghidra: the ReVa connection was down; the C#'s and upstream's own comments
  say the same.)
- **The owner's direction** (2026-11-27): "kurt/kurts sprite needs to not sink in to geometry at
  all ... fixes that arent individual band-aids but root cause solutions", and "I am good with
  departing from the C# version at this point as its a fork ... as long as its under its own
  commit for this with documentation as to why we are doing it".
- **The rule now** (`mdk_core/src/game/kurt_cover.h`): an arena triangle may hide Kurt only when
  its own BSP plane has the camera on one side and Kurt's ordering point on the other -- it
  stands between them. Every other triangle is drawn under him. The ordering point is the centre
  of his walking collision box, feet + 3.0 (ArenaSpace `kWalkBox.z` 2.5 + `kWalkLift` 0.5), so
  the floor under him and a step lower than half his height are on his side; a pillar, another
  room's wall, a ledge taller than that are between. No threshold, no per-room exception. The
  full painter's order ("the first splitting plane that separates the face from Kurt") was
  modelled and rejected: the arenas' splitting planes run through whole subtrees and often pass
  between Kurt and the wall he walks along, so 1701 of the 20036 cut frames below would still be
  cut.
- **How it is drawn, the same on both targets** (render_types.h `KurtPass`, draw_queue.h):
  the triangles of the drawn arenas in the pyramid from the eye through his sprite, cut at his
  depth, that the rule says he is drawn over, are found through a box tree (Morton order, built
  a slice a frame) and drawn into the stencil, clipped to the pyramid, where they are the nearest
  thing drawn; his sprite is drawn depth-tested where no mark is and with no depth test over the
  marks; glass of his own space gets its own bit and is not drawn over him. His passes run after
  the mirror bucket (so a mirror floor is marked like any floor) and before the glass. Objects
  and effects still decide by depth, as before. The Linux scene depth gained a stencil
  (D32S8/D24S8; without one he is only depth-tested, and the log says so). The 2-unit pull of the
  rows at his feet stays (his painted shadow over a platform object he rides); the floor pull is
  gone.
- **Evidence** (Linux headless, lavapipe; the kurt-sprite-depth detector: 36 scripted walks on
  L3-L8, 3000 frames each, and 3000 random floor starts with 260 frames of walking; every second
  opaque sprite pixel ray-cast against the drawn arenas, and the worst frame of every episode
  rendered with the before and after builds, normally and with Kurt on top -- the pixels that
  differ are Kurt hidden; `.git/mdk-review/kurt-sprite-occlusion-2026-11-27/`):
  - Before: 20036 probe frames with Kurt cut (>= 5 samples) by surfaces of his own space, 5792
    hidden by a surface between camera and him.
  - 1257 own-space episodes rendered: the before build hid him in 919; the after build shows him
    whole in 824 of those. Of the 95 left, 41 are hidden in that frame by a surface between
    (the rule), 11 are random starts under the level, 16 are the first frames of a run (the box
    tree is built over the first frames of a level or of a newly drawn arena), 9 differ by <= 5
    pixels, 18 are objects and effects in front of him (laser beams, an explosion, the
    snowboard, enemies, landing dust). One episode is newly hidden, by 1 pixel.
  - 432 episodes where a surface between camera and him hid him: 257 hidden before and after,
    168 not visible either way, 7 visible after (below; known limit).
- **Cost, Old 3DS** (old3ds-lan167, 268 MHz, L2 off, stereo on; LEVEL3 HMO_1 standing at the
  ramp wall, a PROFILE A/B on one harness build with the cover off and on, the final code):
  steady frame period 18.12 -> 19.36 ms (CPU 7.19 -> 7.77 ms, GPU 12.51 -> 12.99 ms); Kurt's
  draw 51 -> ~360 us of CPU (~48 triangles tested, ~4 marked). The box tree's first slice (~3000 steps) costs one frame ~0.7 ms more than the baseline's
  first Kurt frame. Before the marks were clipped to the pyramid they cost ~2 ms of GPU (a wall's
  whole screen area), and the one-frame median-split tree ~8 ms: both measured and replaced.
- **Known limits.** A glass surface between camera and Kurt that lies in front of a glass surface
  of his own space does not tint him there (seen only with Kurt fallen under L4's ice); the first
  frames after an arena is first drawn are depth-tested only while its tree is built; objects and
  effects in front of him decide by depth as before (upstream draws objects in the BSP walk too).
- **Visible:** yes, by design. Upstream's and the C#'s pictures differ wherever a wall or ledge
  cut him.

## What is left

Nothing in this list is an unexplained divergence. D1, D2, D4, D6 and D5's spin creation are
accepted rounding. D8 is a deliberate, owner-decided divergence from upstream. D3 and D7 were fixed by package 8. D5 is fixed here. Should the owner revisit
the trig table, D1 and D4 both close with the one line in `Player::facing()`. D2 needs its own
one-line `fmaf`.
