# Handoff — Old-3DS arena-transition stutter (level 7 crossing route)

Written 2026-11-23 for muse cli. Work lives in this git worktree
(`.claude/worktrees/arena-streaming`) on branch `feature/level-residency`.
**Work here is committed and the tree is clean.** The main checkout
(`/mnt/nas/Development/mdk-sdl`) is on `experimental/3ds-perf-glow-shadows` —
to continue this work autonomously, **work in this worktree** (or check the
branch out); the harness build flags and every measurement below are from it.

## 1. Commits in scope (oldest first)

| Commit | What |
|---|---|
| `4fea3380` | pair residency + texture pump + MTO pixel fallback (retired the arena stream) |
| `3e4ea298` | Linux `--walk` fails loudly when no player is simulated |
| `949ca8f5` | per-eye frustum cull (off-screen arena batches skipped before the vertex pipeline) |
| `3b750fae` | corridors share their arena's palette OBJECT (kills duplicate texture expansion) |
| `295aafb0` | pump budget is a hard bound on the frame's makes |

Each was gated (`tools/check.sh` ALL PASSED, golden `L3_f1.bmp` byte-identical),
adversarially reviewed on a different model family (reviews found one real
transpose bug and one budget double-spend, both fixed), and hardware-verified
on the Old 3DS before committing.

## 2. The harness (how the measurements are made)

The console is `old3ds-lan167` at `192.168.4.167` (Old 3DS, 268 MHz, L2 off,
stereo on). Full runbook: `AGENTS.md` §11 P10. The one command:

```bash
export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM
make -C mdk_3ds -j"$(nproc)" AUTOQUIT=90 PROFILE=1 FIXED=1 LEVEL=7 \
     UNLOCK=650,750,790,830,870 WALK='forward+turbo@60-800;forward+turbo@800-1120'
python3 /mnt/nas/Development/netload_stub/tools/atmctl.py --ip 192.168.4.167 discover
mdk_3ds/tools/hw_cycle.sh --profile --ip 192.168.4.167 --out /tmp/kilo/<name>.log
```

Harness build flags (all `-D` via make): `AUTOQUIT=90` (harness payload —
required, `hw_cycle.sh` refuses a payload without it), `PROFILE=1` (frame
table), `FIXED=1` (frame *f* == sim step *f*, so runs are comparable),
`LEVEL=7`, `WALK='seg;seg'` with segments `dir[+turbo]@from-to` (drives Kurt;
`forward+turbo` ≈ 0.66 units/frame), `UNLOCK=f1,f2,...` (clears door lock bits
at those frames — the level-7 doors start locked and the real opener is a
script event not yet located), `AT=...` (teleport, see the DANT_2 route below).

Routes that reach doors:
- **DANT_1 → CDANT_1** (this handoff's route): spawn `(0,4,9)` yaw 90,
  `forward+turbo@60-800;forward+turbo@800-1120` + `UNLOCK=650,750,790,830,870`.
  Doorway DANT_1→CDANT_1 at `y=481, x∈[-13,13]`; CDANT_1→DANT_2 at `y=673`.
- **DANT_2 entry** (from `docs/WORK_LEDGER.md`): `FIXED=1 AT=0,380,15,90
  UNLOCK=200 HDROMFS=1` — reaches DANT_2 at ~frame 1112. The plain WALK route
  above does NOT reach DANT_2 (walks out of the corridor axis); use the AT route.

Profile table columns (verbatim header in the log):

```
frame period sim(st) begin draw end strmKB strmMs views viewMs tex texMs
tEv hd hdMs hEv linKB kRd kSI log flags
```

`period` = wall ms to next frame; `begin` is mostly the vblank wait (idle);
real work ≈ `draw` (CPU incl. any on-demand texture make) + `end` (GPU sync).
`tex/texMs` = textures made in the draw path and their ms. `linKB` = linear
free. Arena events (`-2-V--` flags) mark pair formation/entry/drop.

Logs (append-only, multiple sessions — always isolate the last session):

```bash
S=$(grep -n "^--- mdk 3ds session ---" LOG | tail -1 | cut -d: -f1)
sed -n "${S},\$p" LOG > last.log
```

- `/tmp/kilo/door-walk-full2.log` session 3 — **baseline** (pre-cull).
- `/tmp/kilo/cull-last.log` — after frustum cull.
- `/tmp/kilo/palette-last.log` — after palette sharing.
- `/tmp/kilo/pump-last.log` — after pump bound (current state).

Linux cross-check: `MDK_DATA_DIR="$PWD/MDK"` is required or data silently does
not load (the walk now fails loudly, `3e4ea298`). Frame indexing agrees with
the 3DS harness (verified: L7 forward+turbo, Linux (0.1,132.4,10.1) @320 vs
3DS (0,131,10)).

## 3. Measurements (Old 3DS, same route, same build flags)

Crossing-sequence frames:

| Frame | Baseline | After cull | After palette share | Current | Cause |
|---|---|---|---|---|---|
| 771 pair/stream-in | 226 | 216 | 154 | 157 | CDANT_1 geometry upload + first makes |
| 791 crossing | 89.9 | 71.4 | 71.9 | 75.9 | entry |
| 795 strip make | 138 | 138 | 135 | **42.8** | TRAIL effect texture (64x704 stacked): chunked expansion + effect warm — see §5.2 |
| 827 burst | 317 | 292.5 | **50.6** | 50.6 | was 7 duplicate texture makes (134 ms) — gone |
| sustained 774–826 | ~50 | ~45–50 | ~45–50 | ~45–50 | corridor's own visible cost |

Whole-run stats (same 90 s route):

| Run | mean | p50 | p95 | p99 | over-33 ms | vblank-1 frames |
|---|---|---|---|---|---|---|
| baseline | 27.9 | 27.7 | 41.7 | 52.7 | 377 | 1238 |
| cull | 22.6 | 18.7 | 35.2 | 49.2 | 248 | 2983 |
| palette share | 23.1 | 19.2 | 35.4 | 49.4 | 259 | 2831 |
| pump bound | 22.8 | 19.0 | 35.4 | 49.4 | 249 | 2769 |
| census (task 1 diag) | 22.9 | 19.0 | 35.3 | 51.4 | 266 | — |
| chunked + warm (§5.2) | 23.2 | 19.0 | 36.1 | 50.1 | 271 | — |
| sorted batches (campaign) | 18.9 | 16.8 | 28.7 | 42.2 | 196 | — |

Sorted build (`e32d30ae`, branch `perf/renderer-campaign`): corridor
frames 785–820 are 35.0–35.8 ms (was ~45–50), `end` 12.0–12.3 (was
19–20), draws/eye 272 → 152 (merged batches span the arena, so the
frustum cull now rejects almost nothing: culled 133 → 13, submitted
triangles +8%). Command list 11.1k → 5.2k words/frame; texture binds
165 → 39/frame. Agent reports that sized this: merge (oracle sort,
135–529 → 12–31 batches/arena), forsaken (~35 µs/draw in both engines;
544 vs ~118 draws/frame), gpu (`end` is CPU replay, not GPU; sky dome
is 2736 of ~4700 tris/eye and never culled; no GPU time has ever been
measured — `gputime:` PROFILE lines now sample
C3D_GetDrawingTime/GetProcessingTime), sim (draw-count reduction is
the lever to one vblank). Next campaign increments, in order: GPU
timing readout, sky-dome culling, sun-frustum cull on shadow casters,
and the unattributed ~10 ms corridor `draw` span (drawsub only prints
on >60 ms spikes).

Campaign update 2026-11-23 (branch `perf/renderer-campaign`):
- **GPU timing in** (`0ece91d4`): steady GPU drawing 10.6 ms/frame
  enhanced, **5.75 ms features-off** (all Enhanced look toggles 0:
  240x400/eye, no lighting/shadows/filtering/mips/edge-smoothing).
  Features-off walk: mean 18.0, p50 16.8, corridor 32.7–33.9 ms.
- **Sky sector cull tried and reverted**: submitted sky triangles
  −14% but GPU drawing unmoved (10.45 vs 10.55) — fill-bound, not
  vertex-bound at this scale.
- **Corridor `draw` attributed** (drawsub at census frames): objects
  ~4.6 ms + effects ~3.8 ms + kurt 0.5 + hud 0.15 + arenas 0.15,
  rest 0. The object pass is per-object layout/arrange/stream work
  from the C# design on a 268 MHz CPU; an off-arena object skip and
  an arrangement memo each measured ~0 (objects are in drawn arenas;
  arrangement is not the cost) and were reverted. The remaining
  obj/fx cost is per-frame geometry layout (transform interpolation,
  posed-vertex copies, stream upload) — the next campaign item needs
  a per-object breakdown before touching it.
- Spark routing note: `muse-spark` is unauthorized on this box's
  litellm key (401) and unknown to the Workflow catalog; the profiling
  agents ran on DeepSeek V4.1 Flash via `pi` instead (the validator's
  route), which is not session-model usage.

GPU `end` in the pair window: 26 ms (baseline) → 18.5–19 ms (cull). The drop
proves the cull removes the departing arena; the residual is the corridor's
own cost.

## 4. How each suspect was proven (the debugging method)

- **Frame-827 burst**: profile row showed `draw=263.6, tex=7, texMs=134.3`.
  The `texmake` log lines (printed by `6d69fe4a`, no frame stamp) were matched
  by summing a contiguous group of 7 makes to 134.4 ms. Three of the seven
  (32x960, 16x480, 64x704) appeared TWICE in the run — the second group two
  frames after the DANT_1 drop. Root cause: cache key `(texture, palette)` +
  corridor got a palette COPY → duplicate expansion; fixed by sharing the
  owner's palette object. Verified on hardware: 827 now 50.6 ms, each strip
  expanded exactly once.
- **Frame-795**: `tex=1 texMs=77.2` = the 64x704 strip's FIRST expansion,
  arriving in the draw path because the pump (2/frame × 20-frame pair window
  = ~40 batches) doesn't reach that batch before the crossing. The pump-rate
  change (`295aafb0`) did not move it — confirmed by identical 795 rows.
- **Corridor cost**: after the cull, post-crossing `end` stayed ~19 ms while
  the departing arena's contribution vanished (26→18.5) → the ~19 ms is the
  corridor's own GPU cost, not the second arena.
- **Transpose bug in the cull**: the plane extraction read rows instead of
  columns (row-vector engine); an identity view passes either way, so the
  pinned test (`testTheFrustumPlanesFollowTheCamera` in
  `tests/scripts/pica_matrix_tests.cpp`) asserts plane-vs-clip-cube agreement
  on rotated views and a finite far plane. It fails on the transposed code.
- **Pre-existing failure**: `tests/scripts` `c3d_frame_test.sh` fails on the
  CLEAN tree (verified by stashing) — "canvas texture pinned RGBA8…arm a"
  isolation check. Not caused by this work; do not chase it as a regression.

## 5. Remaining suspects (ranked, with evidence)

1. **Sustained ~45 ms corridor frames, frames ~771–830 (~2 s of judder)** —
   now the dominant stutter. `draw≈10.5 + end≈19` ⇒ misses vsync (2 vblanks).
   **MEASURED 2026-11-23 (census run `/tmp/kilo/census-last.log`, same route):
   per-batch/state overhead, NOT fill-rate.** A PROFILE-only census (arm at
   frames 765/785/805/815/845 in `03_frame.inc`; submitted draws, culled
   batches, on-screen triangles and NDC area per eye) gave:

   | Frame | Scene | draws/eye | culled | tris submitted | tris on-screen | GPU `end` |
   |---|---|---|---|---|---|---|
   | 765 | DANT_1 only | 148 | 120 | 4178 | 907 | ~12 ms |
   | 785 | corridor pair | 272 | 133 | 4762 | 1126 | 20.4 ms |
   | 805 | corridor pair | 251 | 164 | 4686 | 891 | 18.8 ms |
   | 815 | corridor pair | 234 | 177 | 4625 | 776 | 17.7 ms |
   | 845 | CDANT_1 only | 164 | 66 | 3405 | 567 | 12.7 ms |

   Submitted triangles rise only ~12% and on-screen triangles are flat, while
   submitted draws rise ~70–84% and `end` tracks the draw count almost
   linearly (~65 µs per extra submitted draw). (Counts are first-eye only —
   the census logs after the first eye's replay; the `end` figures cited are
   the uncensused `pump-last` run's, since the census loop perturbs its own
   sampled frames. The census run's sampled `end` values, 17.7–20.4 ms in
   the corridor vs 12.7 ms at frame 845, agree.) Frame 845 is the control: its
   raw NDC area sum is the largest of the five (52.5, unclipped) yet its
   `end` is 12.7 ms at 164 draws. Fill (pixel/triangle load) therefore does
   not explain the corridor's ~19 ms; the per-draw GPU/command cost of
   ~250 small batches per eye (mean ~18 tris/draw; run-total dedupe counters
   show ~165 texture rebinds written per frame, ~146 UV writes per frame)
   does. **No fidelity change is needed to attack this** — candidate fixes
   that keep every pixel: merge same-texture/same-state batches in the arena
   scene build, or cut per-draw state writes in `drawRecorded`. The old
   "stereo fill at close range" reading is disproven; do not reopen it
   without a clipped-area (per-pixel) measurement that contradicts the
   draw-count correlation. (`docs/DOCTRINE.md` is absent on this branch —
   the fidelity rule applied here is AGENTS.md/SPEC.md's.)
2. **Frame-795 77 ms strip make — DONE 2026-11-23.** The strip is `TRAIL`
   (64x64 x 11 frames = a 64x704 stacked texture), a script-effect texture
   (scripts/effects.cpp), first asked for at its spawn frame (~795) -- not
   an arena batch, so neither the sequential pump nor a large-first pump
   scan could ever reach it. The fix is three parts, no fidelity change:
   (a) the 3DS texmake expands a large indexed texture (>= 32768 texels)
   in slices across frame boundaries (rows by a 16384-texel budget, then
   filter, mips, 16384-texel pack slices via the host-tested
   `packTexels16Range`, then the ordinary upload consuming the finished
   buffer as a preset; at most 4 pending chains, 3 pump steps a frame; a
   draw that needs one finishes it synchronously, so no frame binds a
   half-made texture); (b) the core pump starts large surfaces first and
   `LevelView::load` starts them at view load; (c) `EffectsView::warm`
   starts the large script-effect textures of the resident arenas on the
   residency change (3DS frontend, on `streamCurrent`/`streamSecond`).
   Measured (`/tmp/kilo/chunk6-last.log`, same route): TRAIL starts at
   frame 1, ready at frame 26; **frame 795: 135 -> 42.8 ms, `tex=0`** (the
   corridor's ordinary ~43 ms), whole-run p50 unchanged at 19.0 ms.
   Known limits: a texture larger than the measured class (a 780x360
   chain needs ~40 steps) can still finish in its draw if first asked
   late; eviction re-makes stay synchronous, as before. The pump already
   gives an unfinished view the whole budget (2/frame).
3. **DANT_2 entry — re-measured 2026-11-23** (teleport route
   `ARENA=CDANT_1 AT=0,650,15,90 WALK=forward+turbo@60-500 UNLOCK=200,300
   HDROMFS=1`; the ledger's plain `AT=0,380,15,90` route leaves Kurt
   standing in DANT_1 and never reaches DANT_2). Entry crossing itself is
   now ~105 ms; the remaining spike is at entry+4 frames: a **1024x256
   indexed texture re-expands synchronously for ~320 ms**
   (`expandstage: main 17.6 filter 169.7 mip 92.4`). It was made at spawn
   (ready frame 44), evicted while DANT_2 sat resident-but-undrawn
   (linear free falls to ~1 MB in this route), and the first draw after
   entry re-made it whole. The ledger's 448 ms geometry upload does not
   reproduce as a per-entry cost in this route: view builds (2 views,
   607 ms) happen once at the teleport spawn, and the walk route's pair
   formation spreads them. Fix: `mdk_render_prepare_texture` seam +
   `SurfaceCache::prepare` -- the pump/load/warm paths now prepare large
   surfaces (not just ask for their ids), so an evicted large copy is
   re-expanded in slices by the frame pump while its arena is resident;
   the large scan wraps, and its prepares do not spend the sequential
   pump budget (counting them starved the second arena's scan behind
   the current arena's no-op re-prepares -- measured). The three
   PICA-refused >1024 strips (`sl_big 64x1920`, `fire 64x2628`,
   `explode 128x3328`) still expand at draw; they were not the measured
   entry spike in this route.
4. **Frame-771 stream-in 157 ms** — CDANT_1 geometry upload at pair
   formation; same class as (3), smaller.

## 6. Conventions to keep

- Loop: implement → `tools/check.sh` + adversarial validator on a DIFFERENT
  model family (`ralph/validator.md`) → fix → commit → hardware cycle. Never
  commit un-checked; never end a turn with committed work by only summarising.
- Golden: `gates_frames` pins `L3_f1.bmp` byte-identical — any pixel change
  fails the gate and needs an owner fidelity call.
- `AGENTS.md` §§3/10: no virtual/RTTI/exceptions/templates/assert; hot-path
  numeric rules; `-fno-strict-aliasing` stays.
- Hardware: only `hw_cycle.sh` (P10 traps apply: discover before every HTTP
  read; judge pushes by `transfers.log`, never `3dslink`'s stdout; a timeout
  is the console, not the build).
- The `c3d_frame` arm failure in `tests/scripts` is pre-existing (see §4).
- Session note 2026-11-23 (muse): Obsidian/Cognee/Pi-knowledge MCP tools are
  absent from this session's tool list (P9 session fault, not a lookup).
  The vault on disk IS reachable at `/mnt/nas/Vaults/port-docs` (owner:
  `../../Vault/port-docs` from the Development folder); task 1's vault
  search found no corridor/fill note, and
  `findings/3ds-perf-pass-2026-11-19.md` corroborates the per-draw reading
  (state dedupe shipped: command list halved, −2.4 ms). Cognee graph search
  remains unavailable; record any later MCP restoration before claiming it.
