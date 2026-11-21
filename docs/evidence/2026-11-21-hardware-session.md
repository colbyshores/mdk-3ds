# End-of-project hardware session (2026-11-21)

The owner handed the Old 3DS appliance back on 2026-11-21 ("you now have access to the hardware appliance for autonomous testing") for the planned end-of-project session: a full test, a profile of the whole game, the parked console A/Bs, then optimisation.

**The setup for every run:**
- **Console:** old3ds-lan167 (192.168.4.167), CFGU model 0 (Old 3DS), 268 MHz, L2 none (by model), stereo **on**, firmware 3a062b049228.
- **Loop:** AGENTS.md P10, `mdk_3ds/tools/hw_cycle.sh --capture|--profile` from a clean harness build. `AUTOQUIT=90` and default toggles, so **lighting is on** (geometry shader).
- **Before the first push:** `transfers.log` ended in Forsaken pushes, but nothing on the workstation was pushing and `atm.log` showed `run=none`.

## Step 1: the baseline, 281b251b plus the PROFILE build fix (1742e84d)

**The capture:** `make -C mdk_3ds AUTOQUIT=90 LEVEL=3 FIXED=1 CAPTURE=600`, then `hw_cycle.sh --capture`.
- Verdict FRESH.
- 2043 frames, 0 spikes, 0 stalls.
- The frame shows level 3's canyon lit and shadowed (geometry shader on), the sky, Kurt with the chain gun, and the HUD.

**The first profile build failed to compile** (`StreamStats::standIns`, gone since E12). It was fixed in 1742e84d; see the ledger.

**The profiles:** `make -C mdk_3ds AUTOQUIT=90 LEVEL=<n> PROFILE=1`, then `hw_cycle.sh --profile`.
- Every run is FRESH.
- Kurt stands at the level's spawn for the whole run: no walk.
- The table is the steady state, frame 3 on. The span columns are ms per frame.

| Level | Spawn arena | Steady mean | ≈fps | vblanks (1:2:3:4+) | sim | draw | begin (vblank wait) | end | Note |
|---|---|---|---|---|---|---|---|---|---|
| 3 | HMO_1 | 35.6 ms | 28 | 0:2391:15:3 | 3.98 | 5.09 | 13.20 | 13.70 | |
| 4 | MEAT_1 | 16.7 ms | 60 | 5145:3:1:0 | 0.30 | 0.70 | 13.55 | 2.13 | spawn room is tiny (69 batches, 393 triangles): not representative of the level |
| 8 | (level 8 start) | 42.5 ms | 23.5 | 0:897:1096:4 | 6.47 | 7.66 | 12.63 | 16.34 | the slowest; arena free 2.2–3.2 MB |
| 5 | MUSE_1 | 33.5 ms | 30 | 324:1690:261:18 | 7.08 | 7.46 | 10.81 | 8.21 | enemies reach Kurt; combat from ~frame 2209, death at frame 2295. In combat a frame runs 4–5 steps at ~4 ms each (runtime 2.22 ms/step) |
| 6 | (level 6 start) | 22.3 ms | 45 | 2594:1265:1:3 | 1.81 | 3.42 | 11.50 | 5.73 | |
| 7 | (level 7 start) | 26.9 ms | 37 | 0:3195:3:0 | 1.35 | 2.39 | 13.61 | 9.65 | |

**Every level** starts with two stalls: frame 1 at 1.6–3.4 s and frame 2 at 0.7–1.8 s. These are the first frame's first-use work, before play.

**Reading.** The frame period is quantised to vblanks, so the targets are "1 vblank" (60 fps) and "2 vblanks" (30 fps).
- **CPU work** (sim plus draw) is under 15 ms everywhere except in combat on level 5 and at the start of level 8.
- **`end`** (the frame's submit and flush) is 13–16 ms on levels 3 and 8. I first read that as GPU-bound, with the geometry-shader lighting the prime suspect. **Wrong, see step 2:** `end` is the CPU's per-eye command replay.
- So E11, which bakes the static world's light and drops the geometry shader for it, is the first A/B.

**To look at later:**
- Level 5's Kurt stream staging peaked at 360679 bytes, over its 262144 bound (5 animations dropped). **By design:** `KurtSprite::pumpStream` adopts the arrived read, records the peak, and only then calls `trimStaged`, which drops the least recently drawn animations back under the bound. A wanted animation is never refused for room. So the peak is the instant between adopt and trim, which is about 100 KB of heap for one frame. Not a leak, and no change.
- The arena table's first-use rows: frame 1 begin 646.8 ms and draw 359.4 ms.

## Step 2: the parked experiments, replayed onto 1742e84d

Both were replayed onto the current tip on throwaway branches:
- E11 is `exp/hw-e11-rebased` 81832d85. The merge kept both sides, and the old base's one-line `mdk_render_mark`/`mdk_render_release` stubs were dropped (HEAD has the real ones).
- E14 is `exp/hw-e14-rebased` 15a50cf6.

The commands are the step-1 commands, built in each branch's worktree.

**E11's look:** a level 3 capture at frame 600, diffed against the step-1 capture.
- 76041 of 96000 pixels are identical; 40 differ by more than 8 levels and 3 by more than 32; the mean channel difference is 0.31.
- The bake reproduces the real-time light: the lit and shadowed canyon, sky and Kurt look the same.
- The log says `light: baked level 3: atlas 512x1024 L4 ... loaded in 57 ms`.

**The profiles:** steady mean, frame 3 on.

| Level | Baseline | E11 (lightBaked) | E14 (workers on core 1) |
|---|---|---|---|
| 3 | 35.6 ms | 34.3 ms (−1.3) | — |
| 8 | 42.5 ms (2-vblank 897, 3-vblank 1096) | 41.0 ms (−1.4; 2-vblank 1240, 3-vblank 823) | 43.4 ms (+1.0) |
| 5 | 33.5 ms | 33.1 ms (−0.4) | — |
| 7 | 26.9 ms | 27.8 ms (+0.9) | 27.9 ms (+1.0) |

**Reading.**
- Run-to-run spread looks like about ±1 ms: E11 and E14 both read about +1 ms on level 7, where neither touches the frame. Step 3 repeats the level 8 baseline to measure it.
- **E11 does not move the frame.** The `end` span barely changes (13.7 → 13.4 ms on level 3).
- So the step-1 guess was wrong: `end` is not GPU time spent on the geometry shader. `mdk_render_end_frame` is the per-eye REPLAY: it builds the GPU command stream for each eye from the queued draws. That is CPU work, and stereo doubles it.
- E11 stays parked. Its look is proven on hardware but its gain isn't, and the cost is ~0.5 MB of bake files a level.
- **E14:** E14's log says "cpu: Old 3DS; syscore time limit 30% ... workers on core 1; probe core 2 refused, core 3 refused". So on an Old 3DS the workers do run on core 1, and cores 2 and 3 do not exist, as the Mandarine note predicted. There is no frame gain at idle: a spawn reads nothing, so the case E14 is for, a streaming read, isn't in these runs.
- The audio-starvation counters are not written in profile builds; a plain `--once` run is queued for them.

## Step 3: the menu video on hardware, and the scenes Mandarine drew dark (cd4bf497)

All five runs are `hw_cycle.sh --capture`, FRESH.

**The menu:** `SPLASH=0 FIXED=1 CAPTURE=2400`.
- The log has `menu: slides loading off the main thread at frame 168 (0 loaded)` and `menu: slides loaded at frame 219 (8 loaded)`. The slides are read DURING the video, by design (c9711c89).
- The capture at 40 s shows an MDKS slide with the menu items over it, so the cycle runs with the RomFS fix.
- The playthrough agent's "the slide lines never print" came from reading emurun's 25-line tail, not the whole log.

**The dark scenes:**

| Scene | Command | Result |
|---|---|---|
| GUNT_7 | `LEVEL=8 AT=23,2643,-290,90 ARENA=GUNT_7 FIXED=1 CAPTURE=300` | near-black floor and left wall |
| GUNT_7, lighting off | same + `TOGGLES=enableLighting=0` | the same |
| DANT_7 | `LEVEL=7 AT=381,4513,-85,90 ARENA=DANT_7 FIXED=1 CAPTURE=300` | half of the left wall black, hard edge |
| MEAT_1 | `LEVEL=4 FIXED=1 CAPTURE=300` | written (log line present); `hw_cycle` said CAPTURE MISSING, see below |

**Compared with the Linux build at the same points.** The command was `build-linux/mdk --headless --level=N --at=... --arena=... --fixed-step --seed 1 --original|--enhanced --capture-frame=300`. Both looks show the same dark floor and the same hard-edged black wall. These arenas ARE dark in the game, so it is not a 3DS or Mandarine defect. The playthrough's "dark surfaces" item is closed.

**`hw_cycle.sh` CAPTURE MISSING on a good capture.** The capture check was `printf '%s' "$SESSION" | grep -qF ...` under `set -o pipefail`.
- grep -q exits at the first match. When the match is early and the session is large, printf still has chunks to write, dies of SIGPIPE, and the pipeline reads as failed.
- MEAT_1's session was 48 KB with the line at 107 of 412.
- Fixed with a here-string. The selftest gains a case that puts the line early in a session past a pipe buffer.
