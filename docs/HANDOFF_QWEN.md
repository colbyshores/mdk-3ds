# Handoff — MDK C++ port, Qwen takeover (2026-11-16)

You are taking over an in-flight port of MDK (1997) to C++ with two
targets: Linux/Vulkan (enhanced look) and Nintendo 3DS/Citro3D
(original look). The C# port in `src/` is the behavioural oracle and is
**read-only**. This document is the whole context: where the authority
lives, exactly where the work stands, what remains and in what order,
and the rules that have already cost this project false greens when
broken. Read §1–§4 before touching code.

HEAD at handoff: `b39d77d` (EffectsView batch; see §3). Previous
release-quality batch: `2183595` (CIA ledger closure). Working tree:
clean except this file.

---

## 1. Authorities, in the order they outrank each other

1. `AGENTS.md` (repo root) — the performance/platform contract. OG 3DS
   (268 MHz ARM11, L2 off) is the perf target. §3/§3b numeric rules,
   §10 "C with classes" language bans, §11 P1–P9 platform rules. A rule
   here is not repealed by this document; where they disagree, AGENTS
   wins and this file is stale.
2. `SPEC.md`, `SPEC-COMPLETION.md` — protected. Editing them (or
   `docs/COMPLETION_LEDGER.tsv`) requires `MDK_OWNER_APPROVED=1` at
   commit time and owner authorisation; the owner has authorised
   ledger closures for verified batches. Do not amend a protected spec
   to fit the code (see `spec.voice-pool` in §7).
3. `docs/COMPLETION_LEDGER.tsv` — the definition of "done". A row is
   done only with evidence in-tree and a verified commit that is an
   ancestor of HEAD; `tools/runner/gates_completion.py` enforces it.
4. `docs/OPCODE_COVERAGE.md` — opcode accounting. Its headline counts
   are stale (see §5.1); its per-batch notes and partial table are the
   best map of what the VM still owes.
5. `docs/CONTINUATION.md` — batch journal. **Its top is historical**
   (batch 175, 2026-11-14); tonight's batches supersede it. Read it for
   traps, not for status. `docs/OPEN_ITEMS.md` holds standing traps and
   one live defect (§6.2). `docs/OWNER_REVIEW.md` is the owner's
   register — never "resolve" an item there by loosening a gate.
6. The oracle: `src/Mdk.Game/**`, `src/Mdk.Engine/**`,
   `src/Mdk.Formats/**`. Port 1:1; where the C# looks wrong, the C# is
   still the spec until the owner says otherwise.

## 2. State right now (measured tonight, re-verify before trusting)

- d1: 13/13 done. Remaining open rows are exactly:
  `p6.enhanced-stereo`, `opcodes.remaining-27` (d2);
  `hw.harness-cycle`, `hw.profile-og`, `hw.pixels-rgba8`,
  `hw.cia-boot`, `hw.sleep-resume` (d3, real hardware — **last, and
  out of scope until the owner has a console in the loop**);
  owner-blocked: `spec.voice-pool`, `re.phase-2-sweep` (§7).
- Suites at handoff: `make -C tests/scripts` green incl.
  `All cases match the oracle.`; `make -C tests/formats` 156,536
  checks / 0 failures; `bash tools/check.sh` ALL GATES PASSED with
  `gates_smdh` PASS (a real CIA now exists in-tree); Linux and 3DS
  builds green; Mandarine r1.2.1 boots level 7, HUD marker 121/121.
- The 120 MB `mdk_3ds/mdk.cia` in-tree is gitignored build output, not
  a committed artifact. Rebuild with `make -C mdk_3ds cia`.

## 3. The batch that just landed: EffectsView (pieces + sprite effects)

Files: `mdk_core/src/game/effects_view.{h,cpp}` (CPU stage, oracle
`src/Mdk.Game/Objects/EffectsView.cs`), `effects_draw.{h,cpp}` (GPU
half, ObjectDraw pattern), wired in both frontends after the object
pass and before Kurt's sprite (`Viewer.cs:707` order). Unit pins:
`testEffectsViewGeometry` in `tests/scripts/vm_tests.cpp` (billboard
basis, quad corners/UVs, spark facing index incl. clamps). Covered:
Debris pieces batched by surface, sparks as palette-coloured flat
triangles shaded by |facing|, sprite effects (wounds/drops/bubbles/
trails from the 128/132/136 pool) as billboard quads at
`scale/256` per texel with the pool's frame index. Deliberately NOT
covered (their pools do not exist yet): spawn_box billboards
(opcode 159, §5.4) and twister ribbons (Items, §5.5) — the oracle
draws them in this same pass, so they join `EffectsView::build`, not
a new pass. Frame evidence: original-look pinned frame unchanged
(1.240% gap — frame 1 has no effects); emulator boots and draws.
NOT verified: any frame with live effects in it (no shipped level
fires 128/132/136 in the first ticks; a synthetic in-game trigger is
the honest check and is owed, §5.4 acceptance).

## 4. The loop (every batch, no exceptions)

1. Prior-solution lookup (AGENTS §0): Obsidian/Cognee MCP if the
   session has them, else the vault read-only at
   `/mnt/nas/Vaults/port-docs`, plus pi-knowledge `mdk-sdl`. Record
   unavailability; never claim a lookup that did not happen.
2. Read the oracle code and the C++ seams before writing. Port 1:1,
   including operand decode order and RNG consumption order — the
   goldens are byte-compared and the RNG stream is shared.
3. Test first where a harness exists: `tests/scripts/vm_tests.cpp`
   (synthetic per-opcode, twin of `tests/Mdk.Game.Tests`) for VM work;
   `tests/scripts/tick1.cpp` goldens for anything that fires in
   levels 3–8. A maintained test ships with the batch.
4. Verify, reading the TERMINAL result yourself (never a stale or
   background log): `SUITE_RC=0`, `All cases match the oracle.`, zero
   `FAIL ` lines. Then the builds your change touches and
   `bash tools/check.sh`. 3DS-affecting batches also owe an emulator
   run (§8). This project has recorded two false greens from trusting
   background output; that is why this paragraph exists.
5. `tools/publish_frame.sh <label>` — every commit needs a new
   `docs/frames/*.png`+`.txt` pair (batch-integrity gate).
6. Commit only green work, staging named files (never `git add -A`:
   `.progress-log/` accumulates untracked files that are not yours to
   commit). Run `python3 tools/runner/gates_batch_integrity.py
   --staged` first; add `--owner-approved` + `MDK_OWNER_APPROVED=1`
   only for ledger/protected edits under the owner's standing
   authorisation. Close ledger rows in a separate `ledger:` commit
   naming the code commit's hash.
7. The different-model adversarial validator (`ralph/validator.md`)
   could not be dispatched from the Muse session; do not claim it
   ran. If your harness can run it, do so before committing.

## 5. Remaining work, in the order to take it

### 5.1 The last 15 opcodes (`opcodes.remaining-27` row)

Verified remaining set (recount after the Effects batch):
**28, 114, 130, 131, 158, 159, 167, 171, 172, 178, 181, 184, 209,
221, 224**. None fires in levels 3–8 (probe to t30000 is clean), so
every one lands through `vm_tests` synthetics, never through golden
regeneration. `docs/OPCODE_COVERAGE.md`'s headline says "24/26
remaining" in places — it is stale; recount before quoting it:
brace-match `ScriptVm.Execute` (238 cases) against
`ScriptVm::execute` and take the set difference, remembering the
nested `case 0:` inside opcode 249 is not a dispatch.

Dependency order (smallest honest subsystem first):

| Batch | Opcodes | Oracle | C++ seams present | Missing (the real work) |
|---|---|---|---|---|
| Wind | 224 | `ScriptRuntime.WindZone` (`ScriptRuntime.cs:2300`) | Fans query pattern to imitate | Wind state + consumers in Kurt/object motion (`Wind` enum, per-arena) |
| Platform pair | 114, 209 | `GetKurtPlatform` (`:2720`), `Kurt.Firing` | `worldBounds` ported; Player platform tracking partial | Kurt platform identity + firing state reachable from the runtime |
| Rides gate | 171 | `Rides.Ridden` (`Rides.cs:40`) | `ridesRidden` field (always null) | Just the Rides slice below; 171 itself is one branch |
| Bomb pair | 167 (+165/166 realise) | `Items.Bomb`, VM case 167 | `itemsBomb` field (never assigned) | Items bomb throw/hold (§5.5); 167 then needs `StartMove` command 78 (exists) |
| Waypoint | 221 | `PickWaypoint8` (`:2338`) | DTI spots machinery (cover/advance use it) | Waypoint table selection semantics; read the C# fully first |
| Boss bar | 181 | VM `BossBar` helper | HUD draw exists | Boss-bar state (counter vs arena variable) + HUD consumer |
| Combat core | 158, 172, 178, 184 | `TouchDamage` (`:2758`), `SpawnExplosion` (`:1770`), `Items.Blast` (`Items.cs:604`) | `kill()` death path, ExplodeSound wiring partial | Blast (objects/Kurt/groups, hit type −5), explosion spawn, touch damage; also realises partials 16/56/81/195 writers |
| Sniper | 28, 130 | `SniperRounds.GuideLastMortar`, `StampBulletHole` (`:1297`), `BulletHoles.cs` | none | SniperRounds subsystem (rounds, mortars, target lock) + texture stamping; biggest single slice here |
| Spawn box | 159 | `SpawnBox` (`ScriptRuntime.cs:1590`) + `Boxes` list | `spawn()` exists | Box objects (bounds-named model) **and** their EffectsView billboards (§3) |
| Cutscene | 131 | `SpecialEvent` (`:1009`), `EndLevel.cs`, `StrikeScene.cs` | none (no EndLevel/cutscene state in C++) | End-the-level flow + cutscene camera modes; split ≤50 (EndTheLevel) from >50 (cutscenes) if needed |

Acceptance per batch: `make -C tests/scripts` (new synthetic case per
opcode incl. negative/edge operands), `bash tools/check.sh`, goldens
byte-identical (they must be — none fires in the corpus), frame pair,
commit, then update `docs/OPCODE_COVERAGE.md`'s remaining list in the
same commit. Close the ledger row only when the set is empty.

### 5.2 Partial dispatches (faithful case, dead subsystem)

Thirteen owned opcodes are partial per `docs/OPCODE_COVERAGE.md`'s
table (16, 56, 81, 135, 140, 165, 166, 174, 195, 202, 203, 215, 248).
They close as by-products of §5.1/§5.3–§5.5: e.g. combat core gives
81/195 their writers; Items gives 165/166/174 theirs; snapshot work
gives 202/203 their consumers. Do not "fix" a partial by stubbing the
consumer — the current documented no-op is worth more than a fake.

### 5.3 Rides, ropes, sniper scope (Viewer draws still missing)

`Viewer.cs:705-707` draws ropes, sniper scope, effects between the
objects and Kurt. Effects landed (§3). Ropes: object rope state
exists (`rope_points`, opcode 242); the draw is `RopeView.cs` —
port it next to EffectsView (same dynamic-mesh pattern). Sniper
scope/rounds ride on §5.1's Sniper batch. Kurt's ride pose and
`Rides.cs` (mount/vehicle, alarm writer, `hurtKurt` redirection) is
the subsystem behind 171 and the `3ds`/gameplay completeness of
riding sections.

### 5.4 Effects follow-ons

- A live-effects visual check: drive a shipped level until a wound/
  drop/bubble is on screen (or a synthetic harness frame), capture
  Linux + emulator, compare against the oracle's same-tick frame.
  Until then EffectsView is unit-pinned, not picture-pinned.
- spawn_box billboards join `EffectsView::build` with opcode 159:
  quad width = max(size.X, size.Z), texel = that / texture width,
  frame = tickCount % frameCount, dead/invisible skipped.
- Twister ribbons (`Twister.cs`, `Ribbon.cs`, texture `WMIBTEX`)
  join the same build when Items lands.

### 5.5 Items (the keystone subsystem)

`src/Mdk.Game/Scripts/Items.cs` + `Twister.cs`: pickups, the
Interesting Bomb, blasts, twisters. It unblocks 167/165/166/184/178
(writers), 174's ammo writers (with the chain-gun/sniper combat in
`Kurt` combat code), the HUD inventory writers (HUD currently must
not invent them), and §5.4's ribbons. It is the largest remaining
game slice; split it pickup → bomb → blast → twister, each with
`vm_tests` pins, and expect `Player`/Kurt combat interaction points
to need care (one health, one combat state — see the `kurt()` note
in `script_runtime.h`).

### 5.6 Enhanced stereo / lit residue (`p6.enhanced-stereo`, research)

Linux enhanced frame vs oracle: ~86–87% differing pixels against the
88.0 shrink-only ceiling (`tests/frames/reference.sha256` records
the original-look 1.240%; the enhanced figure lives in the enhanced
capture path — re-measure before quoting either number; docs cite
86.241%, tonight's notes 86.773%). Stage isolation found no single
remaining defect: it is a per-face light-distribution difference
(effective-light median +6%, an oracle shadowed-face tail). This is
a research batch, not a fix: bring a new measurement idea (per-face
light dumps from both renderers on one scene), change one stage at
a time, and never raise the ceiling. The 3DS side of p6 is
best-effort PICA analogues behind Enhanced toggles; the two that
cannot get close (sun shadows, post occlusion/glow grade) are
already recorded in `docs/OWNER_REVIEW.md` for the owner's call —
do not relitigate them in code.

### 5.7 Snapshot / save-load consumers

Several ported states lack their snapshot consumer (135's camera
consumer landed; 202 sky mode, 203 camera-track fields, script
snapshot in `ScriptRuntime.Snapshot.cs`). Port as one slice when
taken: `Snapshot.cs` is the oracle; the save format is JSON in the
C# and fidelity there is byte-level, not vibes.

## 6. Known defects and traps (do not rediscover these)

1. **3DS canvas texture addressing** (`docs/OPEN_ITEMS.md`, batch
   173, OPEN): GPU samples canvas textures ~4× too finely across the
   width; CPU texels, geometry, UV transform all proven correct;
   synthetic patterns rule out a single scale factor. Next move is
   reading/disassembling libcitro3d's `C3D_TexInitWithParams`, NOT
   a /4 UV hack (measured: it breaks the digits). The HUD ink
   verdict (25 exact-green px measured vs 134 in the reference
   capture) is downstream of this and is **owner-parked**
   (`docs/OWNER_REVIEW.md` item 1): do not loosen `HUD_INK_*`.
2. **emurun discipline** (AGENTS P7/P8): always
   `DISPLAY=:99 TMPDIR=/tmp/mdk-tmp-lane mdk_3ds/tools/emurun.sh`;
   Mandarine pin r1.2.1 md5 `0d2adab6…` (r1.3.0 segfaults — never);
   emulator-green is not hardware-pass; `/tmp/kilo` is not writable
   by this lane (captures go to /tmp). `--expect hud` currently
   exits 7 on the parked ink question while the marker verdict
   passes 121/121 — that is the known state, not a regression you
   introduced; check `git stash` behaviour against HEAD if unsure.
3. **Batch-integrity**: protected files (`SPEC.md`,
   `SPEC-COMPLETION.md`, ledger, `tools/runner/ledger_close.py`)
   need owner approval env; each commit needs a fresh
   `docs/frames/` pair; `tools/check.sh`'s integrity call is
   range-less, so run `gates_batch_integrity.py --staged` yourself.
4. **NAS/group ownership**: some files are `piagent:development`
   group-writable; `chmod` may refuse (harmless). Ledger is
   `coleshores:coleshores 0664` in this lane — the
   `tools/runner/ledger_close.py` route in old notes needs the
   `development` group; the direct TSV edit + `--owner-approved`
   staged gate is what tonight's closures used.
5. **House style** (AGENTS §10, gate-enforced): no virtual/RTTI/
   exceptions/project templates/`std::function`/`assert`. Renderer
   split is by file, never `#ifdef`. Math LUT is one path on both
   targets (`mathLutInit()` before any `fast*`). FZ/DN helpers on
   3DS thread entries are load-bearing comments and all.
6. **Source-reading harnesses** (`tests/scripts/*_test.sh`) parse
   frontend/renderer source shapes (the refused-mesh latch family
   is the sharp one): a new refusal line inside the object pass's
   block will redden `main_3ds_flow_test.sh` for shape reasons.
   New passes report from their own block (as EffectsDraw now
   does) or the harness grows deliberately in the same batch.
7. Untracked `.progress-log/*.md` files accumulate; leave them.

## 7. Owner-blocked and owner-review (not yours to decide)

- `spec.voice-pool`: SPEC §8 says 24 voices/40 ms; the oracle ships
  64/80 ms. Needs the owner to amend the spec or authorise a
  per-target cap. No code either way.
- `re.phase-2-sweep`: needs the REA MCP enabled + owner
  authorisation for a dedicated sweep phase.
- `docs/OWNER_REVIEW.md`: PICA shadow/post verdicts (owner's call),
  HUD ink floor adjudication (§6.1), the deferred font-mod consumer
  (single-row vs wrapped glyph atlas; sketched Linux-only route is
  in the register), enhanced residue (§5.6). The CIA item there is
  **stale** — closed by `ed9d653`/`2183595`; update it in your
  first docs-touching batch, don't re-do the work.

## 8. Verification cookbook (exact commands)

```bash
# Host suites (the long one; read SUITE_RC / FAIL counts yourself)
DISPLAY=:99 TMPDIR=/tmp/mdk-tmp-lane \
  MDK_FRAME_REFERENCE_DIR=/tmp/mdk-frame-reference make -C tests/scripts
make -C tests/formats            # 156,536 checks at handoff
bash tools/check.sh              # all gates; smdh PASSes while mdk_3ds/mdk.cia exists
cmake --build build-linux --target mdk -j8
DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds -j8
DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds cia
python3 tools/runner/gates_smdh.py        # CONTRACT CLEAN on the built CIA
DISPLAY=:99 TMPDIR=/tmp/mdk-tmp-lane mdk_3ds/tools/emurun.sh --expect hud
tools/publish_frame.sh <label>            # per-commit frame pair
python3 tools/runner/gates_batch_integrity.py --staged   # before every commit
python3 tools/runner/gates_completion.py --report        # ledger truth
```

Standard env for frame work: `VK_ICD_FILENAMES=/usr/share/vulkan/
icd.d/lvp_icd.json` is the lavapipe pin the frame scripts default
to. Original-look pinned frame must stay at its recorded bytes;
the enhanced ceiling may only shrink.

## 9. Hardware rows (d3 — last, owner-driven)

`hw.harness-cycle` is the only wired mode:
`make -C mdk_3ds AUTOQUIT=90` then
`mdk_3ds/tools/hw_cycle.sh --once --ip <console>` (3dslink push,
self-quit, chain-load netload stub, HTTP log pull with md5
freshness; stale log = exit 1). `--profile/--capture/--install/
--sleep` currently print "not wired yet" and exit 3 **by design**:
each is its own row (`hw.profile-og` needs an OG-clock capture —
New-3DS clocked runs never count; `hw.pixels-rgba8`, `hw.cia-boot`
installs the §2 CIA, `hw.sleep-resume` needs lid/APT evidence per
AGENTS P1/P6). Wire one mode per batch when a console is actually
in the loop; never mark a row done on emulator evidence.

## 10. Suggested first three batches

1. **Wind (224)** — smallest missing subsystem, Fans-shaped, warms
   up the whole loop (lookup → synthetic test → commit → coverage
   doc) in one sitting.
2. **Platform pair (114, 209)** — forces the Kurt-platform question
   that 209/171 also lean on; settle where platform identity lives
   before Rides makes it load-bearing.
3. **Ropes draw** (§5.3) — first visual slice; reuses the
   EffectsDraw pattern while it is fresh, and gives the emulator a
   new thing to show.

Then combat core (158/172/178/184) before Items proper, because
Blast is the writer half of four partials. Sniper (28/130) and
cutscene (131) last among opcodes: biggest surface, least corpus
pressure (nothing shipped fires them).

## 11. What "done" looks like

d2 closes when `opcodes.remaining-27` recounts to zero with tests,
`p6.enhanced-stereo` is measured down or owner-accepted as-is, and
§5.3–§5.5 draws/subsystems exist with picture-level evidence. The
project is "ready for real hardware" when every d2 row is done and
the only open rows are the five `hw.*` — at which point the owner
runs §9 with a console, on OG clocks, and P6 evidence rules apply.
Anything less is a progress report, not a completion claim.
