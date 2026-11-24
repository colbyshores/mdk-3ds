# Overnight Astra-deferred campaign — 2026-11-24 (night)

Branch `perf/astra-deferred` off `75cac202` (verified Astra items 1–6 +
refresh-backoff fix). Appliance: `old3ds-lan167` (192.168.4.167, Old 3DS,
268 MHz, L2 off). Each increment: implement → tests + `tools/check.sh` →
`hw_cycle.sh --profile` on the appliance → verdict here → own commit.
Nothing pushed to remote. Console `.90` (owner's, ftpd-pro) gets a fresh
plain `.3dsx` at the end of the night unless a build is clearly better
mid-night.

Deferred Astra ideas under test (from `docs/mdk_3ds_streaming_review.md`
§"Memory and what the second core can buy", items 41/60/65/66/67/79, and
the handoff's deferred list):

1. Immutable door table (review §41)
2. Precomputed depth layers
3. Menu logo jump diagnosis + fix (review §79)
4. Frame-803 packed-original cluster diagnosis
5. Early bounded reads on the existing reader (review §60 first step)
6. Per-room manifest + pinned minimum drawable set (review §65)
7. Unified bounded queue driven by door intent (review §66)
8. Staged pre-tiled publication (review §67)
9. Second-core worker comparison (review §60 second step)

## Baseline (HEAD 75cac202)

Cycles (all FRESH, Old 3DS model 0, 268 MHz, L2 off, stereo on):

- No-LEVEL menu run: frames=0 in the gameplay profile (front-screen loop does
  not feed harnessProfileFrame — the known instrumentation gap, item 3).
- LEVEL=7 standing still: mean 19.68 ms, p99 38.2 ms, 50 frames >33.3 ms,
  0 arena events (no walk script = no crossings).
- LEVEL=7 mid-tunnel walk (`UNLOCK=650,750,790,830,870
  WALK='forward+turbo@60-800;forward+turbo@800-1120'`, log
  /tmp/kilo/mdk-baseline-walk.log): **steady mean 18.26 ms, p50<=16.8,
  p95<=22.0, p99<=57.7 ms, 121 frames >33.3 ms**. Events: DANT_1 hint at
  570, entered CDANT_1 at 584.

Flagged frames (the hazards every increment is measured against):

| frame | period | signature |
|---|---|---|
| 372 | 139.5 ms | draw 112.7, first=4 pack=4 — synchronous packed-original imports mid-DANT_1 |
| 570 | 57.6 ms | first hint frame, first=1 pack=1 |
| 584 | 110.8 ms | door activation (draw 35.3) |
| 585–592 | 83–134 ms | post-crossing: sim 9–20 ms, texMs 6.6–21.8, HD imports at 587–588 (hdMs 21.0/15.9) |
| 596 | **305.2 ms** | texMs 286.1, first=7 pack=7 — the frame-803-class packed-original cluster, now at frame 596 |
| 599–600 | 68.9/83.5 ms | trailing first=1 pack=1 imports |

Verdict for later rows: compare mean/p99/over33.3 and these frames.

## Increment 1 — time-budgeted surface pump (8 ms/frame) — REVERTED, regress

Changed `pumpSurfaces(2)` to pump one make at a time until 8 ms elapsed.
Cycle FRESH (log /tmp/kilo/mdk-inc1-pumpbudget.log): steady mean 18.245 ms
(vs 18.258), p99 56.0 (vs 57.7), over33.3=124 (vs 121) — and the packed-original
cluster GREW: frame 598 = 401.5 ms, first=8 pack=8 texMs 283.6 (baseline
frame 596: 305.2 ms, 7 makes).

Cause: one packed-original make costs ~35 ms, so a check-after-each-make
budget degenerates to ONE make per frame for exactly the expensive makes —
below the old two. The pump cannot outrun the set at any per-frame budget
that fits the frame's ~10 ms slack: the total (~286 ms) must either start
earlier (manifest/queue at door intent) or be split inside the make (staged
publication). Count or time budgets only move the spike. Reverted; the
analysis stands as the case for §65/§67.
