# Handoff prompt/spec — Astra follow-up, Qwen pickup (2026-11-24)

You are picking up an in-flight MDK C++ 3DS renderer/perf task. Work only in:

- **Worktree:** `/mnt/nas/Development/mdk-sdl/.claude/worktrees/arena-streaming`
- **Branch:** `perf/renderer-campaign`
- **Current HEAD when this handoff was written:** `a345b522` (`wip: address validator refresh, failure-kind and HD findings`)
- **Base for this follow-up batch:** `1dfc1d45`
- **Tracking remote:** GitHub fork `colbyshores/mdk-3ds` (git remote `fork`). Never push to `nautilusbits/mdk-sdl` (`origin`) without explicit owner direction.

Read first, in this order:

1. `AGENTS.md` — binding performance/platform/language contract. In particular OG 3DS is the target, no hot-path regressions, C-with-classes style, P10 hardware loop.
2. `docs/mdk_3ds_streaming_review.md` — Astra's review/spec that generated this batch.
3. `docs/evidence/2026-11-24-tunnel-menu-hd.txt` — latest hardware/profile evidence.
4. This file.

Do not use broad filesystem scans, do not inspect grader/oracle artifacts, and do not delete the untracked `mdk_3ds/build-hd-pack` entry in `git status` (pre-existing local staging symlink/data; leave it alone).

---

## Objective

Implement Astra review items 1–6 in this order:

1. Atomic `LevelView::refresh()` publication: a failed replacement mesh must not erase the live room.
2. Arena view failure history: clear only after a view is successfully published/Ready; classify transient allocation vs permanent data vs empty scene.
3. Source-tagged frame/miss attribution: distinguish first texture makes, remakes, MTO demand reads, packed-original imports, successful HD reads vs HD imports, and music demand reads/prefetch stalls.
4. Hardware-profile mid-tunnel and the initial Kurt-logo/main-menu path.
5. Investigate HD admission accounting; reserve decoded size before Tex3DS import if the compressed file size can overshoot the budget.
6. Remove the leftover anonymous `PlaneEntry` in `arena_scene.cpp`; avoid broad streaming rewrites unless data proves them necessary.

Owner constraints:

- Tighten around solutions that work; do not bolt on experiments/band-aids. If a mechanism is superseded, remove it in the same change.
- The door bubble + sliced build + sync guarantee are the accepted door solution. Do not resurrect archived core-1 async expansion into the shipping tree.
- Do not push or FTP a build unless the owner explicitly asks at that time.

---

## What is already done

### Commits on this branch

- `1dfc1d45` — velocity-scaled door bubble radius (pre-batch base).
- `d9cd6d68` — **Item 1**: `LevelView::refresh()` builds a candidate scene/mesh and publishes only after candidate mesh creation succeeds. A failed create retains old scene, old mesh, and torn state.
  - Regression test: `LevelViewRefreshFailureKeepsOldMesh` in `tests/formats/arena_view_tests.cpp`.
  - This test initially failed exactly at the old-mesh draw assertion, then passed after the fix.
- `5c500a09` — **Item 2**: `LevelViewBuildFailure` classification (`TransientAllocation`, `PermanentData`, plus scene-empty handling refined later), failure attempts preserved across `loadBegin()`, history cleared only once a view reaches Ready, residency-exit purge, group refresh signaling.
  - Tests: `ArenaViewsTransientFailureBacksOffAcrossBuildStarts`, `ArenaViewsPermanentSceneFailureIsNotRetriedWhileKept`.
- `0e22844d` — **Item 3**: source attribution.
  - `SurfaceCache::SourceStats`: successful MTO demand reads/bytes and packed-original imports.
  - 3DS stats: textures split first-made vs remade; successful HD file reads/bytes split from HD imports.
  - Harness arena rows append: `first= rem= mto= pack= hdR= music=`.
  - Music demand reads come from `SoundBank::Stats`.
- `5666c8ae` — **Items 4–6 initial pass**:
  - Evidence file above.
  - HD import admission now reserves `kHdLargestBytes` rather than compressed file size.
  - Removed leftover anonymous `PlaneEntry`.
- `a345b522` — **WIP validator remediation** after the adversarial validator returned `INCOMPLETE` on the four-commit diff. It addresses:
  - Failed refresh now leaves `pendingRefresh_` set.
  - `LevelView::retryPendingRefresh()` exists.
  - `ArenaViews::refresh()` records transient refresh failure and sync retries it under the existing backoff.
  - Group-change absent-view refresh clears transient/scene-empty failures only, not permanent data failures.
  - `ArenaSceneBuilder::Failure` distinguishes invalid data from an empty scene; `LevelViewBuildFailure::SceneEmpty` added.
  - MTO counters increment only after successful `indices()` validation.
  - Tests: `LevelViewPendingRefreshRetriesAfterMeshFailure`, `ArenaViewsRetriesFailedRefreshAtSync`.

### Verification already observed

- `make -C tests/formats -j4 test`: **158,491 checks, 0 failures** after the remediation tests were added. Note: when running `tests/formats/build/format_tests` manually, run it through `make -C tests/formats test` or from `tests/formats`; running from the worktree root makes an unrelated Kurt writer fixture path fail because paths are cwd-relative.
- Project gates passed for the four individual commits before `a345b522`.
- A final combined `bash tools/check.sh` for `a345b522` was started but **terminated early at the owner's request due to subscription budget**. It had printed format-test success and the first gates clean (`audit_c_with_classes` clean, VFP pass), but **do not claim the full check passed** until you rerun it.

### Hardware evidence already gathered

- OG 3DS appliance: `old3ds-lan167`, 192.168.4.167, PROFILE builds.
- Mid-tunnel route: `LEVEL=7`, `UNLOCK=650,750,790,830,870`, `WALK='forward+turbo@60-800;forward+turbo@800-1120'`.
  - Door frames remain clean (`viewMs=0.0` near frames 769–771).
  - Worst follow-on found: frame 803, 407.0 ms, `viewsMs=288.4`, `first=7`, `pack=7`.
  - A packed-original cluster at frame 549: 115.6 ms, draw 94.5, `first=4`, `pack=4`.
  - Several end-span spikes had all source columns zero; do **not** call those GPU time without new instrumentation.
- Main menu / Kurt logo:
  - No-LEVEL PROFILE run reached main menu screen 1 and played 5186 frames.
  - Harness profile reports `frames=0` because the front-screens loop does not call `harnessProfileFrame`; this is an instrumentation gap.
  - Guest log showed a 600×360 menu/video texture, one 18.6 ms streamed-texture make near frame 1, later starts around frame 607, steady GPU processing around 6–7 ms per 300-frame window. No large MDK12 decode stall was proven.

---

## What remains to do — exact pickup steps

1. **Start in the worktree and confirm state**
   ```bash
   cd /mnt/nas/Development/mdk-sdl/.claude/worktrees/arena-streaming
   git status --short
   git log --oneline -6
   ```
   Expected: only `?? mdk_3ds/build-hd-pack` untracked. Do not commit or delete it.

2. **Run the full project gates**
   ```bash
   bash tools/check.sh
   ```
   Record the actual result. If it fails, fix only failures caused by this batch; do not weaken tests/gates.

3. **Compile the current HEAD for 3DS** (the last full 3DS compile predates some `a345b522` edits):
   ```bash
   export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM
   make -C mdk_3ds -j"$(nproc)" AUTOQUIT=30
   ```

4. **Rerun focused format tests if you change any core/test code**
   ```bash
   make -C tests/formats -j4 test
   ```

5. **Rerun the adversarial validator** using `ralph/validator.md` over `1dfc1d45..HEAD` with an inline diff and no tools, as prior rounds did. The previous validator verdict was `INCOMPLETE`; its High findings were F1 (group refresh clearing permanent/backoff), F3 (failed refresh dropped), and F2 (HD reservation semantics). Those were addressed in `a345b522`, but **the validator has not been rerun**. Address only confirmed Critical/High findings; do not churn endless Low/Needs-investigation items without evidence.

6. **If code changes after validation**, rerun the affected gates/tests and, only if renderer/frame behavior changed, a P10 hardware cycle. Read `AGENTS.md` §11 P10 before touching the console. The runbook command shape is:
   ```bash
   export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM
   make -C mdk_3ds -j"$(nproc)" AUTOQUIT=90 PROFILE=1 FIXED=1 LEVEL=7 UNLOCK=650,750,790,830,870 WALK='forward+turbo@60-800;forward+turbo@800-1120'
   mdk_3ds/tools/hw_cycle.sh --profile --ip 192.168.4.167 --out /tmp/mdk-qwen-verify.log
   ```

7. **Close the batch cleanly**
   - If `a345b522` needs no code changes and validator is clean, add a small closing commit if you update evidence/status; do **not** rewrite existing commits/history unless the owner explicitly asks.
   - Suggested closing evidence update: amend `docs/evidence/2026-11-24-tunnel-menu-hd.txt` or add a short follow-up evidence note with the final gate/validator results.
   - Push only if the owner explicitly asks. If asked, push `perf/renderer-campaign` to `fork` and fast-forward `fork/main` only if that remains the owner's current instruction.

8. **Deferred Astra ideas are saved separately in project memory** (`astra-deferred-ideas.md`): per-room manifests, unified bounded queue, immutable door table, staged tiled publication, precomputed depth layers, and second-core experiments. Do not start those unless the owner explicitly reopens them after this batch is closed.

---

## Cautions / traps

- `MDK_OWNER_APPROVED=1 git commit ...` was used for these batches because the pre-commit frame gate demands a new pinned frame or owner approval. Follow the local pattern; do not bypass other gates.
- Do not claim a hardware result from a New 3DS or emulator as an OG pass. Record console, clock/cache, stereo mode for captures.
- `hw.profile-og`, CIA, sleep/resume and other completion-ledger hardware rows are not closed by this batch.
- The exact owner-visible menu hitch is **not yet localized**. Do not invent a menu optimization; the next honest step would be front-screen frame-period instrumentation, separately approved/scoped.
- The mid-tunnel frame-803 cluster is evidence for a follow-up, not a mandate to rewrite streaming in this batch.
