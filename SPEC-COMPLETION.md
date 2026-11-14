# SPEC-COMPLETION — MDK C++ port: the mission brief for the loop's builder agent

Revision: 2026-10-25. This file is the **completion mission spec**. It is
written for a fresh agent with no memory of the sessions that produced the
work. Read it fully before doing anything.

**Companion documents, and what each is for:**

| File | Role | Authority |
|---|---|---|
| `SPEC.md` | The architecture/design contract (two backends, renderer seam, phases P1–P8, risks) | Binding |
| `AGENTS.md` | The performance + language + platform contract; the gates; **§11 P7 display rule** | Binding, enforced by `tools/` |
| `docs/CONTINUATION.md` | Live state: priority order, verified tree state, what is next | Binding, updated every batch |
| `docs/OPCODE_COVERAGE.md` | The opcode audit: what is ported/partial/missing and why | Reference (audit it) |
| `docs/PROGRESS_LOG.md` | Archived batch narratives | Reference |
| `docs/DOCTRINE.md`, `docs/architecture.md`, `docs/formats.md` | Design language, tree map, format notes | Reference |

The C# source under `src/` (Mdk.Game, Mdk.Formats) is the **read-only
behavioural oracle**. For any question "is this right?", the answer is what
the C# does.

---

## 1. The mission

Port MDK (1997, Shiny) — the C# decompilation under `src/` — to C++ in the
house style of `AGENTS.md` §10 ("C with Classes": no virtual, no RTTI, no
exceptions, no project templates, no `std::function`, no `assert`), with:

- **`mdk_core/`** — shared game logic: formats, script VM, collision, audio,
  everything platform-neutral.
- **`mdk_linux/`** — SDL3 + Vulkan 1.3 frontend (original AND enhanced look).
- **`mdk_3ds/`** — libctru + Citro3D frontend. **The original 3DS is the
  performance target** (268 MHz ARM11, L2 off); the 3DS shows the original
  look only.

1:1 fidelity first: port what the C# does, do not improve it. The phase plan
and exit criteria are `SPEC.md` §13.

## 2. What has been done so far — and an explicit invitation to audit it

**Do not trust this section.** It records what previous agents *claimed*,
with the commands to re-verify each claim. Your first task is to spot-check
the claims (run the commands) and read `git log` yourself. Trust nothing you
have not run.

| Phase | Status | How to re-verify |
|---|---|---|
| P1 scaffold/build | DONE 2026-10-21 | `make -C mdk_3ds` links `mdk.3dsx`; `cmake --build build-linux --target mdk` builds the Linux frontend |
| P2 gates (`tools/runner/`) | DONE | `bash tools/check.sh` → `CHECK: ALL GATES PASSED`; each gate was required to fail on a seeded violation |
| P3 formats + unit tests | DONE | `make -C tests/formats` — 155,037 checks against the real GOG data in `./MDK` |
| P4 game logic/VM | **IN PROGRESS** — the bulk of the work so far | `make -C tests/scripts` → `All cases match the oracle` (byte-exact golden corpus, levels 3–8, to tick 30000, table AND script dumps) |
| P5–P8 renderer depth, packaging, sweep | NOT STARTED in earnest | — |

**P4 detail (verified as of 2026-10-25, iteration 17):**

- Script VM: of the C#'s 238 `Execute` cases, the C++ owns **168**; **71**
  remain; **11** of the owned set are partial (stub-dependent — see
  `docs/OPCODE_COVERAGE.md` for the exact list and each one's blocker).
- Ported subsystems so far: formats (DTI/MTO/ARENA/BSP/texture/palette/WAV),
  collision (Bsp sweep/raycast), triangle groups (show/hide/solidity/destructible
  + per-group material), Kurt health/inventory state, **audio** (sound bank,
  mixer voice table, device control plane; opcodes 89/107/233/249), object
  death/removal cascade, deep-tick lifecycle (pickups/chutes), targeting.
- The golden corpus is **byte-exact against the C# oracle** to t30000 on every
  level present (L3–L8; the GOG tree here has no L1/2/9+). The divergences
  that used to exist (L5 XG[004] at t577, the DTI record-layout mismatch) are
  closed and pinned by goldens.
- The oracle driver binary lives OUTSIDE the repo at
  `/tmp/kilo/oracle/Driver` (see §5) — `/tmp` may have been wiped; rebuild it
  from the in-repo C# if the goldens need regenerating.
- 76 commits since 2026-10-20. Audit the interesting ones: `git log --oneline`,
  especially the `scripts:` and `audio:` batches.

**Known weak spots to check first (a reviewer already flagged some):**

1. The 11 partial opcodes — a dispatch case that calls a stub is NOT
   implemented. `docs/OPCODE_COVERAGE.md` is the inventory.
2. The two unwired mixer call-sites (see §4, item 1) — the mixer exists but
   two C# playback sites still no-op in C++.
3. Renderers: `mdk_3ds` renders a static scene (group hide/retexture now
   update it, but the stereo/enhanced paths of P5–P8 are untouched), and the
   Linux Vulkan backend is headless-tested only (no DRI3 on this box).
4. `docs/CONTINUATION.md` claims should be re-verified by running its three
   gate commands on a clean tree before you believe them.

## 3. What remains (priority order)

From `docs/CONTINUATION.md` (owner directive): port code; goldens follow in
the same batch. One subsystem slice per batch.

1. **Wire the mixer's remaining call-sites**: the motion frame-sound trigger
   (`ObjectMotion.cs:133`; C++ mirror clears `frame_sound` without playing at
   `script_runtime.cpp:~289`) and the door state machine's `DoorSounds`
   (`ObjectBehaviors.cs:90-115`; marked at `script_runtime.cpp:~2008/2019`).
   Completes partials 24 and 151; observable in the AUDIO golden line.
2. **`Debris.Shatter` + opcode 138** (fires in L5 — the last reachable gap):
   C# `src/Mdk.Game/Scripts/Debris.cs`, the `ShatterPoint`/`ShatterDirection`
   runtime state, effects. Port the subsystem, dump what makes it
   observable, implement 138, regenerate.
3. **The remaining 71 opcodes**, subsystem by subsystem (buckets with
   evidence are in `docs/OPCODE_COVERAGE.md`): camera/cutscene,
   combat/effects, items/partners, HUD, movement. Same rule each time: port
   the subsystem, make its state observable in the golden dump, implement the
   opcodes, regenerate. **Never blind-port** an opcode whose behavior cannot
   be exercised and verified by a golden.
4. **P5.0 — the frame-golden harness (owner directive 2026-10-26: build it
   BEFORE any P5–P7 renderer work).** The VM corridor is safe to run
   unsupervised because the golden dump proves every batch; the renderer has
   no equivalent proof, and a reviewer's opinion is not a frame oracle.
   Deliverable: a deterministic headless render of a pinned level + scripted
   input, frozen reference frames (or a hash manifest), a tolerance
   comparator (byte-exact where deterministic, perceptual elsewhere), and a
   gate wired into `tools/check.sh` that fails on regression — proven to fail
   on a seeded visual regression before it is trusted. Same rule as the
   goldens: a reference frame changes only from the reference source, never
   from the implementation's output.
   **Reference source — DECIDED (owner directive 2026-10-28): the C# itself.**
   Findings that decided it: the C# is a complete SDL3/SDL_GPU implementation
   whose Linux backend is Vulkan, `lvp_icd.json` (lavapipe) is installed here,
   the game's data files are present in the gitignored `./MDK/` GOG install,
   and the C# already ships a deterministic capture mode — `--screenshot=file
   --wait=seconds --level=N --gpu=vulkan --snapshot=NAME` (the last prints a
   state hash). So the oracle is the C# rendered headlessly here; captures of
   the original `MDKD3D.EXE` (via the bundled nGlide wrapper) are an
   OPTIONAL secondary fidelity cross-check, dev-time only. Reference frames
   derived from the original art are ROM-derived pixels: they live under a
   gitignored path with a regeneration script, never in the repo.
5. **P5**: golden-frame tests for the Vulkan renderer (original look within
   tolerance vs the C#). Requires P5.0.
6. **P6**: enhanced look, stereo, mods (Linux-only). Requires P5.0.
   **Sub-batches (owner itemization 2026-11-14; each lands with an oracle
   pin and a shrinking ceiling in `tests/scripts/frame_gate_test.sh`):**
   (a) lighting rig — DONE (`mdk_core/src/game/level_light`, bit-exact in
   `tests/formats`); (b) colour textures (RGBA premultiplied expansion +
   mip chains + per-frame layers, `Renderer.Colours.cs`); (c) lit pipeline
   (Lit/Sprite, no shadow yet); (d) sun shadow pass (2048², `SunShadow.cs`
   biases); (e) camera-depth + occlusion + post glow (`post.hlsl`);
   (f) filtered canvas + enhanced sky; (g) mods / HD-texture path; then
   the row closes via `ledger_close.py`. Until then `--enhanced` renders
   the original look and says so through the refusal log.
   **3DS direction (owner directive 2026-11-14):** the original look stays
   the 3DS baseline; enhanced fidelity on the 3DS is best-effort against
   the PC render, built hardware-first — TexEnv combiner stages, ProcTex /
   maestro extensions, fragment-lighting LUTs, hardware blend modes —
   with CPU work only where the PICA200 structurally cannot (the palette
   expansion in SPEC.md §7.3 is the documented case). References to work
   from when each feature lands: `../t2k/t2k_3ds/src/rendering/c3d/`
   (vector-glow billboards, `06_builders.inc`), the vault's
   `GX-to-PICA200/` notes (`20-tev-to-combiners.md`, `22-textures.md`),
   devkitPro's citro3d TexEnv/ProcTex/lightlut headers and examples.
   **ETC textures for the 3DS build path (owner directive 2026-11-14,**
   following `../forsaken`): bake HD/enhanced textures offline with
   `tex3ds` (`-f etc1` opaque, `-f etc1a4` alpha, `-q high -m gaussian`
   mips) into `.t3x` RomFS packs, load them at runtime through the tex3ds
   library (`GPU_ETC1`/`GPU_ETC1A4`, native decompression) — the
   `../forsaken/regen_hd_textures_*.sh` + `render_c3d.c` method. ETC is
   for the enhanced/HD layer; the original look keeps its lossless
   RGBA8 expansion (SPEC.md §7.3). Sizes: ETC1 is 8× smaller than RGBA8,
   which is what makes enhanced textures fit the 3DS's VRAM at all.
7. **P7**: the Citro3D renderer depth: platform integration (aptMainLoop,
   sleep/resume, worker sync — `AGENTS.md` §11 P1–P6), audio sink, RomFS
   streaming; the A/B checklist run in mandarine. Requires P5.0.
   **Enhanced Mode toggles (owner directive 2026-11-14, SPEC.md 7.9):**
   the five GPU-only feature toggles, their `config.ini` persistence
   (`sdmc:/3ds/mdk/config.ini`), and the runtime setter seam are
   implemented; each 3DS enhanced feature (item 6's hardware-first
   effects) lands behind its toggle. **The Video/Graphics settings
   menu page is a separate later item**: the menu subsystem was cut
   (2026-11-05), so the menu arrives with whatever batch resurrects a
   settings surface, and it only has to call
   `mdk_render_set_enhanced_toggles` + rewrite the same `config.ini`.
8. **P8**: `.cia` packaging — **`makerom` is NOT installed and `libmxml` is
   absent**; `mdk_3ds/tools/build_cia.sh` exits non-zero with an explanation
   instead of shipping a broken title. Installing makerom is an owner
   decision; do not fake it. Then: gates green, final adversarial sweep.
9. **Phase 2 — the post-process gap sweep (owner directive 2026-10-28).**
   **Sequence: only AFTER the C#-driven work above is exhausted.** The C#
   is the preferred intermediate representation and stays the oracle
   wherever it implements the behavior. What the C# itself does not implement
   (the deferred-with-cause set: the Rides writer behind the alarm countdown,
   Kurt's touch writer, the projectile and item-chain writers, and anything
   else the C# leaves unidentified) is lifted in a SEPARATE sweep from the
   original `./MDK/MDKD3D.EXE` using the REA MCP server (Ghidra 12.1.4 at
   `~/re-tools/ghidra_12.1.4_PUBLIC`, `rea mcp`; disabled in
   `~/.config/kilo/kilo.jsonc` — enable it when the sweep starts). RE-derived
   behavior is documented as the oracle of record for that item, exactly as
   the C#-identified slots (199/210) are documented today. No RE work enters
   a normal porting batch.


**Hardware-only (never claim done):** original Old-3DS timing/OG profile. A
devkitARM build or a mandarine run is NOT hardware validation. This stays
open until measured on actual Old-3DS hardware.

**Deferred with evidence (do not re-litigate without new data):** alarm
(move_command 15 never fires in the corpus — verified 0 occurrences);
240/241/244 (Kurt state exists but none fire in L3–L8); 248 `push_kurt`
(honest no-op, `state=0` in every golden).

## 4. Build and test — the exact commands

```sh
# The three gates (run after EVERY batch; all must pass):
make -C tests/scripts        # golden corpus: "All cases match the oracle"
bash tools/check.sh          # the six contract gates (see tools/check.sh --help)
make -C mdk_3ds              # devkitARM target; needs:
                             #   DEVKITPRO=/opt/devkitpro
                             #   DEVKITARM=/opt/devkitpro/devkitARM

# The Linux target (same core, different frontend):
cmake --build build-linux --target mdk

# Formats unit tests:
make -C tests/formats
```

- devkitARM lives at `/opt/devkitpro/devkitARM` (verified:
  `/opt/devkitpro/devkitARM/bin/arm-none-eabi-gcc`).
- The 3DS build outputs `mdk_3ds/mdk.3dsx` (118 MB with the GOG data staged
  into its RomFS; staging via `mdk_3ds/tools/stage_romfs.sh`). `.cia` is
  blocked on `makerom` (§3 item 7).
- The Linux build uses SDL3 (`/usr/local/include/SDL3`), **vendored** Vulkan
  headers + volk (`mdk_linux/third_party/{vulkan,volk}` — CMake fails loudly
  if they are missing), and glslang at
  `~/.local/opt/glslang/bin/glslangValidator`. There is **no DRI3 on this
  box**, so windowed Vulkan presenting is impossible here — the Linux build
  has a `--headless` mode (also what golden-frame harnesses should use).
- The game data is the GOG tree at `./MDK` (`MDK_DATA_DIR`).

**Gate discipline:** a failing gate is a root-cause bug, never a gate to
relax. Goldens are oracle output: a golden may only change when the new value
came from the oracle, never from your implementation.

## 5. The C# oracle — how the goldens are produced

The goldens under `tests/scripts/golden/` are dumps of a driver that runs the
**C#** engine (the real oracle) and the C++ engine side by side and prints
object tables + VM state in an identical format. Byte-identical output = the
C++ matches the C#.

- Driver source: `/tmp/kilo/oracle/Driver/Program.cs`, binary at
  `/tmp/kilo/oracle/Driver/bin/Debug/net10.0/mdkoracle`, run with
  `DOTNET_ROOT=/tmp/kilo/dotnet10` and `MDK_DATA_DIR=<repo>/MDK`
  (e.g. `mdkoracle L T 1 script` = level L, tick T, seed 1, script dump).
- **If `/tmp` was wiped**, the driver must be rebuilt from the in-repo C#
  (`src/`) before any golden regeneration is possible — treat a missing
  oracle as a blocker, not as license to trust the C++ output.
- The C++ side of the harness is `tests/scripts/tick1.cpp`; the test runner
  and case list are in `tests/scripts/Makefile`.

## 6. Emulator testing — FRAMEBUFFER ONLY, never the operator's screen 1

This is **AGENTS.md §11 P7** and it is a contract violation, not a nuisance,
to get it wrong:

- **Every emulator run (mandarine or any GUI harness) runs on the dedicated
  headless X display `:99`, NEVER on the operator's desktop (screen 1).**
- Set `DISPLAY=:99` **explicitly on every invocation** — never inherit an
  ambient `DISPLAY`.
- Start the server if it is not listening:
  `Xvfb :99 -screen 0 1280x1024x24 &`
  It is shared; do not kill a server another run is using.
- Capture to files under `/tmp/kilo` (ImageMagick `import`/`convert` are
  installed) and READ THE FILES. Never open a viewer on the operator's
  display. Record the display used in the evidence — a capture with no
  display recorded does not count.

### Where mandarine is, and its traps

- **Binary**: `/mnt/nas/Development/mandarine.AppImage` — a symlink into a
  versioned extract directory, so version changes are one `ln -sfn` with zero
  harness edits. Harnesses take it from an env var:
  `APPIMAGE="${MANDARINE:-/mnt/nas/Development/mandarine.AppImage}"`.
- **Version pinning (critical)**: **r1.2.1** (`20250128-418c25c`) is known
  good — boots, loads a level, runs a harness sweep. **r1.3.0**
  (`20260415-3f023d0`) deterministically segfaults during level load. Use
  r1.2.1. The full writeup is the Obsidian note
  `50-tooling/mandarine-emulator-location-and-version-pinning.md` — read it
  before your first emulator session; it includes the cleanup contract (the
  `pkill -f mandarine.AppImage` self-kill trap: exclude by PID, never by
  pattern) and the stale-log trap (delete/timestamp-check logs before reading
  them; a no-output run means "did it start?", not "what does the leftover
  say?").
- Config/virtual SD: `~/.config/mandarine-emu`, `~/.local/share/mandarine-emu`
  (the guest's `sdmc`). The 3DS-side log channel is `output_string` /
  the log file under the config dir; raise `log_filter` to see guest debug
  output. A previous session ran the `.3dsx` at 60 Hz with data embedded and
  captured screenshots from `:99`.

## 7. MCP servers available to you

Available to harness sessions in this workspace (the Obsidian and Cognee
servers below are wired as MCP tools):

| Server | Endpoint / notes | Status |
|---|---|---|
| **Obsidian** | `https://obsidian.lab.home/mcp` — the vault is at `~/Documents/port-docs` | Working (verified 2026-10-25) |
| **Cognee** | `https://cognee.lab.home/mcp` — knowledge-graph memory of past ports | **ERRORING** (recall fails as of 2026-10-25) — attempt a recall once; if it errors, proceed without it and say so. Setup history: runbook `20-runbooks/cognee-mcp-through-kilo-the-patches-that-made-it-work.md` |
| **reverify** | `https://reverify.lab.home/mcp` — verification inbox pattern | See runbook `20-runbooks/using-the-reverify-mcp-from-any-session-the-inbox-pattern.md` |
| **codebase-memory** | local nvm binary `codebase-memory-mcp` | See runbook `20-runbooks/codebase-memory-mcp-through-kilo-auto-index-and-the-one-reload-you-cant-script.md` |
| **Vaultwarden** (`vault-mcp`) | `https://vault-mcp.lab.home/sse` | **FORBIDDEN** for this swarm. Do not call `vault_*` tools; if credentials are needed, say so and use non-sensitive mock data. |

### Obsidian notes that are actually useful for this port

- `GX-to-PICA200/` — the GX→PICA200 hardware model and porting playbook
  (`00-index.md`, `02-pica200-hardware-model.md`, `20-tev-to-combiners.md`,
  `22-textures.md`, `30-lighting.md`, `31-fog.md`,
  `33-viewport-scissor-efb-mapping.md`, `43-stereo-3d.md`,
  `50-porting-playbook.md`).
- `10-projects/forsaken/findings/citro3d-renderer-architecture-and-vertex-pipeline.md`
  — the citro3d frame lifecycle, vertex conversion, texture format/Morton
  tiling rules. The closest thing to a Citro3D manual on this machine.
- `10-projects/forsaken/findings/citro3d-native-renderer-rosetta-stone.md`
  and the sibling `citro3d-*` findings (stereo replay, per-group scissors,
  hybrid vertex lighting, proctex noise) — D3D→PICA200 conversion precedent;
  MDK is a Direct3D game, so they apply nearly verbatim.
- `10-projects/forsaken/findings/dsp-adpcm-music-and-sfx-audio.md` — 3DS
  audio; directly relevant now that the mixer is ported.
- `10-projects/forsaken/findings/hardware-only-failure-classes.md` — what an
  emulator can never prove; read before claiming hardware readiness.
- `50-tooling/mandarine-emulator-location-and-version-pinning.md` — §6 above.
- `20-runbooks/the-experiment-harness-one-runner-and-its-guarantees.md` — the
  experiment-harness discipline (liveness by PID, no stale logs).
- Filesystem rosetta stone (not MCP): `../forsaken/3DS_PORT_NOTES.md` — the
  D3D→PICA200 depth-range and cull-enum conversions, the hard-float ABI trap,
  unaligned access, colourkey-before-gamma. `../t2k/` — the sibling port whose
  house style, build system and gates this project inherits.

## 8. Agent swarm and adversarial validation (REQUIRED)

The owner requires a swarm with adversarial checks, running on the Pi harness
(`ralph-pi.sh`) against the local LiteLLM router. The pieces in this repo:

- **Builder**: `ralph/builder.md` (`litellm/qwen`, fallbacks defined only in
  `ralph-pi.sh`) — the per-iteration implementer with
  the banking discipline baked in. `ralph-pi.sh` feeds it to a fresh
  `pi` run each iteration.
- **Adversarial validator**: `ralph/validator.md` (default
  `litellm/deepseek-v4.1-flash`, **write-disabled by design**) —
  falsifies instead of praising: fidelity to the C# oracle first (name the
  C# method and diff semantically), then the 3DS traps, the performance
  contract, the language contract, memory/lifetime, and both-target
  compilation. A finding without evidence is an opinion and must be labeled
  UNPROVEN.
- **In-session validation**: when working interactively instead of through
  the loop, run the same `ralph/validator.md` instructions as a read-only
  review over the current diff (a separate write-disabled reviewer run,
  or a read-only subagent given that file). Never let the builder
  grade its own batch.
- **The ralph loop**: `./ralph.sh` (§9) runs the swarm mechanically:
  builder per iteration + an external stronger-model review of each batch +
  independent gate runs the worker cannot influence.

Swarm rules (from the owner's standing directive):

1. **Sequential, never concurrent writers.** One agent edits the tree at a
   time. Reviewers read a stable snapshot; two builders on one tree is
   corruption, not parallelism.
2. **A review is mandatory after every logically complete batch** — the
   builder's self-review plus the external validator over the diff. An
   aborted/empty reviewer result is a FAILED review, not a pass; retry once,
   then record it as failed.
3. **Fix confirmed BLOCKER/MAJOR findings, rerun the affected gates, then
   commit.** An `UNPROVEN` finding may be rebutted with evidence in
   `docs/CONTINUATION.md`.
4. **Never substitute a reviewer silently.** Record the model used. The
   builder is `litellm/qwen` and the reviewer is `litellm/deepseek-v4.1-flash`;
   `ralph-pi.sh` is the source of truth for both chains and their fallbacks.
   The reviewer must stay a different model family from the builder — identity
   is the resolved upstream, never the alias — and a reviewer that resolves to
   the builder's family is skipped rather than dispatched. If no eligible
   reviewer resolves, the loop records BLOCKED and stops; it never reviews with
   the builder's own model.
5. **Completion is never a worker's claim.** Gates + goldens re-run on the
   final tree by the harness itself, plus a full-tree adversarial review.

**Current harness (owner directive 2026-11-05):** the loop is
`./ralph-pi.sh` on the Pi harness — builder `litellm/qwen` with
`litellm/deepseek-v4.1-flash-th` as its only fallback, reviewer
`litellm/deepseek-v4.1-flash`, gates run independently in the script.
This is the only harness this project runs; the retired Muse loop lives at
`tools/retired/ralph-muse.sh` and must not be restarted. `logs/ralph/disabled`
stays in place as the guard that blocks it. Never run two loops on one tree.

## 9. The ralph loop (the machinery — use it or replace it deliberately)

`./ralph-pi.sh` is the autonomous loop. Per iteration: a FRESH builder context
(`pi --model <worker>`, prompt = `ralph/builder.md` + `ralph/PROMPT.md` + a
machine status block), then the loop itself independently runs the three gates,
detects progress/stall, and dispatches the adversarial reviewer as a separate
write-disabled `pi` run on a different model family, given
`ralph/validator.md`. The loop runs as user `piagent`; the worker and reviewer
resolve their models through the local LiteLLM router.

```sh
./ralph.sh start [--max N]   # detached, pid file logs/ralph/loop.pid
./ralph.sh status             # state + last summary lines
./ralph.sh stop               # stop loop AND kill orphaned workers
./ralph.sh once | smoke       # one foreground iteration | mechanics test
```

- State/logs: `logs/ralph/` (summary.log, iter-N.*, review-N.md). Worker
  reviews land in `logs/ralph/review-*.md` and are fed to the next builder.
- A cron watchdog restarts the loop after a silent death (it has died
  silently twice — cause never proven, likely memory pressure: this box also
  runs another project's loop; `free -h` before you schedule anything heavy).
- **Never start a second writer on this tree.** `ralph-pi.sh` refuses to start
  while another loop's pid file is alive, and `logs/ralph/disabled` keeps the
  retired Muse watchdog from resurrecting the old loop.
- Budgets: worker iteration 3600 s, reviewer 3600 s, 30 iterations per start,
  stall/failure stops are in the script header.

## 10. Non-negotiables

1. **Never weaken a gate, a test, or a golden to obtain a pass.**
2. Goldens change only from oracle output.
3. No `#ifdef` renderer forks — backend split is by file.
4. C with Classes (`AGENTS.md` §10) — the `tools/runner/audit_*` gates
   enforce the zero-tolerance constructs.
5. The performance contract (`AGENTS.md` §§1–4, R1–R11, T1/T2) — the 3DS is
   the target, not a desktop.
6. `AGENTS.md` §11 P1–P7 platform requirements, including the display rule.
7. `src/` (C#) is read-only.
8. Old-3DS hardware timing is never claimed without Old-3DS hardware.
9. Report honestly: a claim of "passed" must name the command you ran.
10. Commit per verified batch; never leave finished work uncommitted
    (a killed iteration loses everything it did not bank).

## 11. Completion criteria and the final report

Done means, with evidence for each: every SPEC.md §13 exit criterion met
(§13/§14 items that are hardware- or owner-gated are explicitly dispositioned,
not silently skipped); the three gates green on the final clean tree; the
golden corpus byte-exact; the full-tree adversarial review has no open
BLOCKER/MAJOR findings (UNPROVEN items either proven or rebutted in writing);
`docs/CONTINUATION.md` and `docs/OPCODE_COVERAGE.md` reconciled with reality;
and a final engineering report covering: architectural changes and rationale,
files/subsystems affected, the opcode coverage disposition, tests/gates run
with real results, both target build results, performance/size deltas where
measured, adversarial findings and their resolutions, and remaining
unverified/hardware-only items. Distinguish verified results from assumptions
at every step — "Do not claim a test passed unless you ran it."
