# Phase 2 continuation checkpoint

Last updated: 2026-10-27 (iteration 60: the chain-link family — opcodes 29/49
and movement command 30).
Current coverage: 198 true C++ dispatches / 238, 40 remaining. Continue one
opcode family per batch via `vm_tests`; renderer work waits for P5.0.

This is persistent handoff. Keep it under ~150 lines; batch narrative goes in
`docs/PROGRESS_LOG.md`.

## PRIORITY ORDER (owner directive 2026-10-25)

1. **DONE — mixer call-sites** (`ObjectMotion.cs:133`, `ObjectBehaviors.cs:90-115`).
2. **DONE — `Debris.Shatter` + opcodes 137/138/139**, dump line and oracle
   goldens. BreakUp/Trail/Updraft await Effects.
3. Continue opcode conversion subsystem by subsystem; tests/goldens in the same
   batch. Opcode 208 (reachability probe), harness + 13, parts 32/129,
   rope-swing 226/242, spawn 156/206, push-hit-dir 81, stats 217, vision 243,
   value slots 199/210, Kurt/screen 215/240/244, and no-ops 239/77 are
   implemented and pinned. Newer slices include 85 (`set_rolling`), 74
   (`attach_to`), the 213/214 partner condition pair, the 179/180/228
   partner-link family, and the 29/49 chain-link family (with movement
   command 30, the chain's own motion). All three oracle `Linked` writers
   (245/179/228) are now ported, so partner pairs can feed the 213/214
   conditions. 132/136 still need Effects; frame-blend integration remains a
   separate future slice. Remaining 40 are listed in
   `docs/OPCODE_COVERAGE.md`.
4. **P5.0 — frame-golden harness** (`SPEC-COMPLETION.md` §3 item 4) before
   renderer batches. Owner must select headless C# vs original-EXE reference;
   no renderer batch until the harness gate is green.

Deferred: alarm/move 15 fires zero times in current corpus; 248 `push_kurt` is
an honest no-op pending Kurt knock-back. Old-3DS timing remains UNMEASURED.

## Current tree state (VERIFIED)

- Iteration 60 ports the chain-link family: opcode 29 (`spawn_chain`) spawns the
  links that hang off an object, numbered after the highest instance id of its
  LIVE followers (the oracle's loop counts down, so the first link spawned has
  the highest id and the last has id 0 — the link that drives the chain);
  opcode 49 (`release_children`) lets go of the highest-id chain link per count
  and branches TRUE when the chain is empty; and movement command 30 (the
  chain's own motion, `ObjectMotion.cs:464`) hangs each link's reference point
  0 at the previous link's reference point 1, turns the links 90 degrees apart,
  lets a link go when the chain has a gap or the leader is dead, and moves the
  chain only from the id-0 link. The oracle's `LastFollowerId`/`LastChainLink`/
  `Links` are now `ScriptRuntime::lastFollowerId`, `lastChainLink` and the
  reused `chainLinks_` list. Three `vm_tests` cases pin it (numbering, the
  live/dead follower rule, the assembly and its turns, the broken chain, the
  dead leader, the release order and the branch). Mutation-checked: breaking
  the id scan, the yaw, the release order, the gap check, the dead-leader check
  or the "only id 0 drives" rule each fails a check. Review-3's NIT is
  addressed (the `link_partners` comment no longer claims 180/213/214/246 read
  the retained dead link; they reject a dead partner as they reject a null
  one), and its open question is recorded: the chain readers compare against
  the RETAINED leader pointer, so only `release_children` and the chain
  motion's let-go may clear a link's `leader`. Coverage is now 198 true
  dispatches / 238 with 40 remaining. `make -C tests/scripts clean && make -C
  tests/scripts` passed 32 VM tests and the complete oracle suite (`All cases
  match the oracle.`); `bash tools/check.sh` passed all contract gates (SMDH
  skipped, no CIA); `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM
  make -C mdk_3ds` built `mdk.3dsx` after staging 69 RomFS files (79M), and
  `cmake --build build-linux --target mdk` built the Linux target. No golden
  changed; no emulator/hardware run. Old-3DS timing remains unmeasured.

- Iteration 59 addresses review-2's removal-semantics findings. The C# `Remove`
  clears NO inbound reference (not `Linked`, `Leader`, `CowTarget`, `Attached`);
  the C++ nulled inbound `leader`/`attached`/`cow_target` pointers, which the
  oracle never does — and which is not equivalent for `Attached`, whose oracle
  read sites test only for null. `ScriptRuntime::remove` now clears nothing,
  matching the oracle: every C++ read site twins its oracle guard
  (null-or-dead for `linked`/`leader`, null-only for `attached`, otherwise a
  comparison against a live object), so a retained pointer to the permanent
  deque slot reads the same way it does in C#. The header comment's claim that
  `kill` leaves an object "dead but PRESENT" is corrected (`kill` leaves
  `health == 0` and `dead == false`; `dead` is set only by `remove`, in both
  languages), and the `link_partners` comment no longer claims the dead-link
  admission is observable. The `vm_tests` fixture hand-builds no link at all
  now: the widow pair comes from a real `link_partners` run, the cross-type
  pair and the keep/preserve links from `spawn_partner`. A new
  `RemoveKeepsInboundReferences` case pins the retained leader (via
  `attach_to`) and the dead-leader reading of `if_no_leader`. Mutation-checked:
  restoring the clears fails the new case, restoring the `linked` clear fails
  11 checks. Coverage was 196 true dispatches / 238, 42 remaining; the gates
  (tests, contract checks, 3DS and Linux builds) passed, with no golden
  changed and no emulator/hardware run.

- Iteration 58 addresses review-1's partner-widow findings.
  `ScriptRuntime::remove` now retains the oracle's inbound `Linked` pointer
  instead of nulling it, so removing one member of a pair leaves the survivor
  linked to its dead partner; `link_partners` consequently skips that widow
  exactly as the C# does, while variable source 3 still reads and writes the
  removed partner's persistent variable array. The `vm_tests` fixture creates
  a real pair and calls `runtime.remove(partner)` instead of fabricating an
  unreachable `dead` flag; it pins the retained link, no re-pairing, and the
  removed partner's variable write.
- Iteration 57 ports the partner-link family: 179 (`spawn_partner`) spawns
  the named type at the receiver's position with yaw zero and links both
  objects; 180 (`snap_to_partner`) copies a live partner's position and then
  applies its optional offset; 228 (`link_partners`) pairs live same-arena
  objects of the named type in stable list order when their squared distance
  is below 10000, while its list form with a zero second value clears only
  the receiver's link. Three `vm_tests` cases pin both link directions,
  dead/no-link behavior, case-insensitive type matching, dead/cross-arena/
  already-linked exclusions, the strict 100-unit boundary, list order and
  the asymmetric clear. All three oracle `Linked` writers are ported,
  removing the producer caveat from 213/214.
- Next: continue opcode conversion from the remaining 40 in
  `docs/OPCODE_COVERAGE.md`; select the next testable subsystem slice. The
  chain readers (`LastFollowerId`, `LastChainLink`, `Links`) compare against
  the RETAINED leader pointer — do not clear a link's `leader` outside
  `release_children` (49) and the chain motion's own let-go.

## Testability and standing open items

- Oracle/goldens compare fixed table + script fields, not velocity,
  `alarm_ticks`, `Items.Bomb`/`Decoy`, or all combat/effect state. Use `vm_tests`
  for otherwise unreachable opcode behavior; don't port without a test. The
  tick corpus matches byte-for-byte through t30000 on levels 3–8.
- Oracle driver: `/tmp/kilo/oracle/Driver/Program.cs`; run with
  `DOTNET_ROOT=/tmp/kilo/dotnet10`, `MDK_DATA_DIR=<repo>/MDK`, driver args
  `L T 1 script`. Source oracle `src/Mdk.Game/**` is read-only.
- Dynamic scene refresh exists at scene level; no GameEngine exists to wire
  refresh on opcode 140. Do not fake call-site wiring.
- Known divergences: incomplete decoy/`AlienTarget` target selection; opcode
  207's `MathF.Sign(NaN)` exception is accepted because C++ exceptions banned;
  degenerate-path guards return 0 rather than C# out-of-range behavior.
- C3D depth state still differs from C#: depth test disabled in C3D versus
  LESS_OR_EQUAL depth test/write in C#. Resolution needs projection remap,
  `C3D_DepthMap`, test function and clear value plus capture. DepthLayer/
  DepthPull are unported and depend on textured renderer path.
- Old 3DS hardware timing UNMEASURED. Never claim OG readiness without actual
  Old 3DS capture. Emulator GUI runs must use explicit `DISPLAY=:99` and save
  captures under `/tmp/kilo`.
- Documentation drift noted earlier: `AGENTS.md`/`SPEC.md` cite absent
  `docs/DOCTRINE.md`; SPEC §9.4 names an unwritten `gates_romfs`. Flag to owner,
  do not invent policy/gates unilaterally.

## Verification commands

```
make -C tests/scripts
bash tools/check.sh
DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds
```

Tests do not track header dependencies: run `make -C tests/scripts clean` after
header changes. Keep the monolithic `ScriptVm::execute()` switch. Never weaken a
gate or test.
