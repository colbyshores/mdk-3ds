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
| 795 strip make | 138 | 138 | 135 | 134.8 | one 64x704 strip, 77 ms, on-demand in draw |
| 827 burst | 317 | 292.5 | **50.6** | 50.6 | was 7 duplicate texture makes (134 ms) — gone |
| sustained 774–826 | ~50 | ~45–50 | ~45–50 | ~45–50 | corridor's own visible cost |

Whole-run stats (same 90 s route):

| Run | mean | p50 | p95 | p99 | over-33 ms | vblank-1 frames |
|---|---|---|---|---|---|---|
| baseline | 27.9 | 27.7 | 41.7 | 52.7 | 377 | 1238 |
| cull | 22.6 | 18.7 | 35.2 | 49.2 | 248 | 2983 |
| palette share | 23.1 | 19.2 | 35.4 | 49.4 | 259 | 2831 |
| pump bound | 22.8 | 19.0 | 35.4 | 49.4 | 249 | 2769 |

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
   The ~19 ms GPU is the corridor's own stereo fill at close range (original
   drew this mono at 400×240; we draw two eyes). NOT yet proven whether it is
   fill-rate or per-batch/state overhead — first next step: count CDANT_1's
   batches and screen coverage vs DANT_1, or a capture with the corridor
   half-off-screen. Any real fix (internal resolution, fewer layers) is a
   **fidelity decision — owner call**, per `docs/DOCTRINE.md`.
2. **Frame-795 77 ms strip make** (64x704, once per crossing). Fixes that
   don't touch fidelity: split the indexed expansion across ~8 pump frames
   (chunked expansion in the 3DS texmake path), or form the pair earlier
   (relocates the spike into the walk-up, doesn't remove it). Note the pump
   already gives an unfinished view the whole budget (2/frame).
3. **DANT_2 entry ~527 ms** (from the board/ledger, AT route): `draw 448 ms`
   = arena GEOMETRY upload, not textures (orig pack + byname index already
   killed the texture tax there: 788→88 ms). Plus the three PICA-refused
   >1024 strips (`sl_big 64x1920`, `fire 64x2628`, `explode 128x3328`) have
   no pack copy and expand at draw. Geometry upload is the next big item for
   the DANT_2 side.
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
