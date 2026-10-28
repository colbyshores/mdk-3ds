## What this batch did (iteration 62 — opcode 204, object avoidance)

- Ported opcode 204 (`avoid_objects`) from `ScriptVm.cs:1715-1729`. The
  receiver computes its world bounds once, walks the live same-arena objects
  in stable list order, and turns away from the first one whose strict world
  bounds overlap its own. Dead objects, objects flagged `FlagNotSolid` or
  `FlagNotSolid2`, and cross-arena objects are skipped. The turn is the
  oracle's wrapped `YawTo(other) + 180 - yaw`, clamped to half a turn times
  one tick, and the scan stops after the first eligible overlap.
- The implementation reuses the existing `ScriptRuntime::worldBounds`,
  `objects`, and `yawTo` seams. The local box intersection is strict on every
  axis, matching `ScriptMath.Intersects`: boxes that merely touch do not
  overlap. This is a real dispatch, not a decoded no-op or a stub.
- One synthetic `vm_tests` case, `AvoidObjectsTurnsAwayFromTheFirstSolidOverlap`,
  pins the dispatch and the self/dead/cross-arena/non-solid/touching
  exclusions, both turn directions, stable first-object order, and the clamp
  to the available angle. It also pins the six-degree-per-tick limit. The
  test was mutation-checked: accepting dead objects, accepting non-solid
  objects, making the bounds inclusive, continuing after the first overlap,
  and replacing the available-angle clamp with a fixed step each make the
  test fail.
- The newest external review file, `logs/ralph-pi/run-20261027-223315/review-5.md`,
  contains only `REVIEWER FAILED model=litellm/qwen rc=1`; it supplied no
  BLOCKER, MAJOR, MINOR, or NIT finding to address. Review-4's remaining
  documentation point was already fixed in iteration 61.
- Coverage: 200 true C++ dispatches / 238, 38 remaining. The brace-matched
  recount gives C# 238 cases and C++ 202 numeric labels / 201 unique labels;
  the extra numeric label is the nested mode arm inside opcode 249, and one
  nested label is duplicated, leaving 200 true dispatches.
- Verification on this tree:
  - `make -C tests/scripts clean && make -C tests/scripts` → 34 VM tests PASS
    and the full oracle suite ended with `All cases match the oracle.`
  - `bash tools/check.sh` → `CHECK: ALL GATES PASSED`; SMDH SKIPPED (no CIA
    built).
  - `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C
    mdk_3ds` → linked `mdk.elf`, staged 69 RomFS files (79M), built
    `mdk.3dsx`.
  - `cmake --build build-linux --target mdk` → built the Linux target.
- No golden changed; opcode 204 does not fire in the level 3–8 corpus. No
  emulator or hardware run was performed. Old-3DS timing remains unmeasured.


## What this batch did (iteration 61 — the alarm state: movement command 15 and opcode 27)

- Ported the oracle's alarm state machine. `ScriptRuntime` now has the shared
  `alarmTicks` countdown: it is decremented once at the start of every
  non-cutscene tick (`ScriptRuntime.cs:896`), movement command 15 sets it to
  ten for an alarm object in Kurt's current arena
  (`ObjectMotion.cs:374-385`), and opcode 27 (`if_alarm`) branches while it
  is positive (`ScriptVm.cs:1692-1693`). Command 15 also plays `ALERT` at
  the object's position when `TickCount & 31` is zero, preserving the
  oracle's every-32-ticks cadence and its `Mixer.PlayAt` placement at the
  object's position.
- The current-arena guard, the countdown value, its exact ten-tick lifetime,
  the true and false `if_alarm` branches, and the sound cadence are all
  pinned by one new synthetic `vm_tests` case,
  `AlarmSoundsForTenTicksAndPlaysEveryThirtyTwo`. It drives the public motion
  seam with a level-7 runtime, uses a fresh probe for the false branch so a
  stale variable cannot make the assertion pass, and reaches `TickCount` 32
  to distinguish the 32-tick sound from a sound on every tick.
- The C#'s Rides subsystem is the other oracle writer of this countdown
  (`Rides.cs:219,224`). Rides is still unported with its player/vehicle
  slice, so the riding vehicle's alarm source remains deferred; the shared
  countdown, command-15 producer, and condition are present and tested.
- Addressed review-4's MINOR documentation finding. The old handoff text
  called `release_children` (49) the one place the oracle clears an inbound
  `leader`, but command 30 clears one in two places as well. Both the
  coverage document and the iteration-60 narrative now say that 49 is the
  only OPCODE that clears one and that the chain motion's own let-go
  (`ObjectMotion.cs:470/480`) is the only other place.
- Coverage: 199 true C++ dispatches / 238, 39 remaining (recounted with the
  brace-matched method spans; the raw C++ label count is 201 with one nested
  switch arm and one duplicate label in it). Opcode 15 leaves the partial
  table; opcodes 15 and 27 are now in the complete set.
- Verification on this tree:
  - `make -C tests/scripts clean && make -C tests/scripts` → 33 VM tests PASS
    and the full oracle suite ended with `All cases match the oracle.`
  - `bash tools/check.sh` → `CHECK: ALL GATES PASSED`; SMDH SKIPPED (no CIA
    built).
  - `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C
    mdk_3ds` → linked `mdk.elf`, staged 69 RomFS files (79M), built
    `mdk.3dsx`.
  - `cmake --build build-linux --target mdk` → built the Linux target.
- No golden changed; no emulator or hardware run was performed. Old-3DS timing
  remains unmeasured.


## What this batch did (iteration 60 — the chain-link family: opcodes 29/49 and movement command 30)

- Ported the chain that hangs off an object: opcode 29 (`spawn_chain`), opcode
  49 (`release_children`) and movement command 30 (the chain's own motion,
  `ObjectMotion.cs:464`), which had no C++ counterpart. The oracle's
  `LastFollowerId` (`ScriptVm.cs:273`) and `LastChainLink` (`ScriptVm.cs:288`)
  are now `ScriptRuntime::lastFollowerId` and `ScriptRuntime::lastChainLink`,
  and `ScriptRuntime::updateChain` ports `UpdateChain` with the C#'s `Links`
  list (the live followers, ordered by instance id, in the reused member list
  the C# keeps in a field).
- The details that matter: the links are numbered after the highest instance
  id of the object's LIVE followers (a removed follower does not count, so its
  id is reused), and the oracle's loop counts down, so the FIRST link spawned
  has the HIGHEST id and the last one has id 0 — the link that drives the
  chain. The chain's motion hangs each link's reference point 0 at the
  previous link's reference point 1 and turns the links 90 degrees apart; only
  the link with id 0 drives the chain, a gap in the chain makes the later
  links let go, and a dead leader makes them let go too. `release_children`
  lets go of the HIGHEST-id chain link (same arena, movement command 30) per
  count and branches TRUE when the chain is empty. It is the only OPCODE that
  clears an inbound `leader`, and it clears exactly the links it releases; the
  chain motion's own let-go (`ObjectMotion.cs:470/480`) is the only other
  place the oracle clears one.
- Three `vm_tests` tests pin it: `SpawnChainNumbersItsLinks` (the numbering,
  the live/dead follower rule, the per-leader numbering),
  `ChainLinksHangOffTheirLeader` (the assembly, the 90-degree turns, that
  only the id-0 link drives the chain, the broken chain, the dead leader) and
  `ReleaseChildrenLetsGoOfTheChain` (the release order and the branch when
  the chain runs out). Mutation-checked: breaking the follower-id scan, the
  yaw, the release order, the gap check, the dead-leader check or the
  "only id 0 drives" rule each fails a check (the first pass of the last one
  was vacuous — re-assembling an assembled chain is a no-op — so the test now
  drives a link BEFORE the chain is assembled).
- Addressed review-3's NIT: the `link_partners` comment no longer claims that
  180/213/214/246 depend on the retained dead link — they read the field but
  reject a dead partner exactly as they reject a null one; variable source 3
  and getFlags/setFlags are what read the retained pointer.
- Recorded review-3's open question in the coverage doc: the chain-command
  readers (`Links`, `LastFollowerId`, `LastChainLink`) compare against the
  RETAINED leader pointer, so nothing may clear a link's `leader` except
  `release_children` (49) and the chain motion's own let-go.
- Coverage: 198 true C++ dispatches / 238, 40 remaining (recounted with the
  brace-matched method spans; the raw C++ label count is 200 with one nested
  switch arm and one duplicate label in it).
- The known pre-existing gap in the shared `spawn()` helper — the C#'s
  `Stats.CountEnemy(type, Created)` call is missing — applies equally to
  opcode 29 and to every other spawn opcode (86/149/156/179/206); it was
  already recorded (progress log, iterations 30 and 57) and the level
  goldens do not observe those counters. Left untouched here so the batch
  stays one subsystem.
- Verification on this tree:
  - `make -C tests/scripts clean && make -C tests/scripts` → 32 VM tests PASS
    and the full oracle suite ended with `All cases match the oracle.`
  - `bash tools/check.sh` → `CHECK: ALL GATES PASSED`; SMDH SKIPPED (no CIA
    built).
  - `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C
    mdk_3ds` → linked `mdk.elf`, staged 69 RomFS files (79M), built
    `mdk.3dsx`.
  - `cmake --build build-linux --target mdk` → built the Linux target.
- No golden changed; no emulator or hardware run was performed. Old-3DS timing
  remains unmeasured.


## What this batch did (iteration 59 — the oracle's Remove clears no inbound reference)

- Addressed the newest external review's two MINOR findings and two NITs on the
  partner/removal semantics. The review found that `ScriptRuntime::remove`
  nulled inbound `leader`, `attached` and `cow_target` pointers, while the
  oracle's `Remove` (`src/Mdk.Game/Scripts/ScriptRuntime.cs:1648-1673`) clears
  no inbound reference at all. The review could not prove the `attached` clear
  safe — the oracle's `Attached` read sites test only for null
  (`ObjectBehaviors.cs:238,267`), so nulling is NOT equivalent there — and
  asked for the divergence to be removed rather than justified.
- `ScriptRuntime::remove` now clears nothing, matching the oracle: a widow
  keeps its dead partner, a removed chute keeps its leader, a pickup keeps a
  chute removed under it. Every C++ read site was checked against its oracle
  twin — `leader` (`followAttachment`, `mayCommand`, opcode 34, command
  selector 7/8), `attached` (the pickup fall/shrink paths) and `cow_target`
  (no C++ read yet; the oracle's is `is { Dead: false }`) — and each reads a
  retained dead pointer the same way the oracle's retained managed reference
  reads. The removed object's deque slot is permanent, so the retained pointer
  stays valid; the object-list loops skip removed objects by `dead`, which
  `remove` alone sets.
- Corrected the header comment that claimed `kill` leaves an object "dead but
  PRESENT": `kill` leaves the object present with `health == 0` and
  `dead == false`; `dead` is written in exactly one place in each language —
  `remove`, immediately before the object leaves the list.
- Corrected the `link_partners` comment: admitting a dead link to the loose
  list is unobservable for the pairing itself (a widow is skipped as `i` and
  as `j`, since both loops skip every non-null link), so the comment no longer
  claims the two steps matter. The clause is kept verbatim because it IS the
  oracle's filter, and the retained link it admits is what the other `linked`
  consumers (variable source 3, 180, 213/214, 246) read.
- The `vm_tests` link_partners fixture no longer fabricates any state: the
  widow's pair is built by a real `link_partners` run (the only two BEAMS in
  the arena), the cross-type already-linked pair by `spawn_partner` (which
  links its receiver to the spawned partner whatever the type), and the
  keep/preserve links by `spawn_partner` as well; the redundant hand-set
  re-links in the clear section were deleted because the pair program above
  already produced that state.
- Added `RemoveKeepsInboundReferences`: `attach_to` (74) makes one object the
  leader of another, the leader is removed, and the follower still points at
  the dead leader (the oracle's field state), while `if_no_leader` (34) still
  reads it as no leader and clears the move command. Mutation-checked: putting
  the inbound clears back fails this case (`follower->leader == leader`), and
  putting the `linked` clear back fails 11 checks in the link_partners case.
- No opcode count changed: 196 true dispatches / 238, 42 remaining.
- Verification: `make -C tests/scripts clean && make -C tests/scripts` passed
  29 VM tests and the complete oracle suite (`All cases match the oracle.`);
  `bash tools/check.sh` passed all contract gates (SMDH skipped, no CIA);
  `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds`
  built `mdk.3dsx` after staging 69 RomFS files (79M), and `cmake --build
  build-linux --target mdk` built the Linux target. No golden changed; no
  emulator/hardware run. Old-3DS timing remains unmeasured.

## What this batch did (iteration 58 — preserve dead partner links)

- Addressed the newest external review's MAJOR and MINOR findings on the
  partner-link family. The C# `ScriptRuntime.Remove` marks the object dead and
  removes it from `Objects`, but deliberately leaves inbound `Linked`
  references intact. The C++ runtime had normalized those references to null,
  making a surviving partner look unlinked; `link_partners` then re-paired it,
  unlike the oracle.
- `ScriptRuntime::remove` now preserves the dead partner pointer. This is
  safe because `std::deque` object slots are stable and removed slots remain
  allocated, matching the managed object's continued readability in C#. The
  existing live-partner guards on opcodes 180/213/214/246 and command
  selector 9 remain in force; variable source 3 deliberately reaches the
  removed partner's persistent variables, as in the oracle.
- Rebuilt the opcode-228 synthetic fixture around a real pair and a real
  `ScriptRuntime::remove(partner)` call. It now pins the retained dead link,
  the widow's exclusion from re-pairing, and a source-3 variable write into
  the removed partner's array. The previous hand-set `dead` flags represented
  states the runtime could not produce and were removed.
- The redundant `isRemoved` term in the live-candidate filter was dropped;
  removal always sets `dead`, exactly as the C# filter's `!other.Dead`
  requires. No opcode count changed: 196 true dispatches / 238, 42 remaining.
- Verification: `make -C tests/scripts clean && make -C tests/scripts` passed
  28 VM tests and the complete oracle suite (`All cases match the oracle.`);
  `bash tools/check.sh` passed all contract gates (SMDH skipped, no CIA);
  `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds`
  built `mdk.3dsx` after staging 69 RomFS files (79M), and `cmake --build
  build-linux --target mdk` built the Linux target. No golden changed; no
  emulator/hardware run. Old-3DS timing remains unmeasured.

## What this batch did (iteration 57 — partner spawn/snap/link family)

- Ported the partner-link family from `ScriptVm.cs:1451-1488`: opcode 179
  (`spawn_partner`) spawns the named type at the receiver's position with
  yaw zero and links the new object and receiver in both directions; opcode
  180 (`snap_to_partner`) copies a live partner's position and then adds the
  optional three-float offset; opcode 228 (`link_partners`) pairs live,
  same-arena objects of a named type whose squared distance is below 10000,
  while its list form with a zero second value clears only the receiver's
  link.
- `ScriptRuntime::linkPartners` preserves the oracle's exact two-stage
  filter: the candidate snapshot admits a null link or a link to a dead
  object, then the pairing loop skips every non-null link. It follows stable
  object-list order (the first eligible later object, not the geometrically
  nearest), uses case-insensitive exact type matching, excludes dead and
  cross-arena objects, and leaves existing links untouched when no pair is
  made.
- Added three synthetic `vm_tests` cases. They pin both directions of the
  179 link, spawn type/arena/position/yaw, 180's null/dead/live and offset
  paths, and 228's case folding, type/arena/dead/already-linked exclusions,
  dead-link rule, list order, strict 100-unit boundary, mutual links, and
  asymmetric receiver-only clear. The opcode-pair family adds three to the
  per-opcode test count (28 opcodes total).
- Recounted the brace-matched method spans: C# 238 cases; C++ 198 numeric
  labels / 197 unique labels, of which 196 are true dispatches; 42 remain.
  `docs/OPCODE_COVERAGE.md` updates the remaining set, subsystem grouping,
  owned-case classification and per-opcode test count. All three oracle
  `Linked` writers (245/179/228) are now ported, so partner pairs created
  by the new opcodes can feed conditions 213/214.
- The existing `ScriptRuntime::spawn` omission of the C#'s
  `Stats.CountEnemy` call remains unchanged and applies equally to every
  spawn opcode; the level goldens do not observe those counters.
- A local mutation probe over the new paths killed five variants: a one-way
  179 link, an ignored 180 offset, a clear-on-any 228 list operand, an
  inclusive 100-unit range boundary, and a case-sensitive type match.
- Verification: `make -C tests/scripts clean && make -C tests/scripts` passed
  28 VM tests and the complete oracle suite (`All cases match the oracle.`);
  `bash tools/check.sh` passed all contract gates (SMDH skipped, no CIA);
  `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds`
  built `mdk.3dsx` after staging 69 RomFS files (79M), and `cmake --build
  build-linux --target mdk` built the Linux target. No golden changed; no
  emulator/hardware run. Old-3DS timing remains unmeasured.

## What this batch did (iteration 56 — review-3 partner-condition fixture hardening)

- Addressed external review-3's MINOR/NIT findings on opcodes 213/214. The
  synthetic fixture now gives the receiver a non-origin position and a
  non-zero yaw, makes the partner differ in z, separates the receiver from
  Kurt, and uses the oracle's approximate-equality comparison. It now pins
  the full 3D distance, the receiver-relative distance origin, the floor of
  one, a live due-east angle of zero, and
  `wrap(receiverYaw - bearingToPartner)` (the negated turn required to face
  the partner), in addition to null/dead-partner behavior.
- A local mutation probe rebuilt the four implementations review-3 found
  previously unconstrained: 2D distance, no angle wrap, ignored receiver yaw,
  and a Kurt-relative distance origin. All four now fail the fixture.
- Standardized the angle description in the test and documentation to
  `wrap(receiverYaw - bearingToPartner)`, the negated turn required to face
  the partner. `docs/OPCODE_COVERAGE.md` now also records that 213/214 are
  complete over the existing `linked` field but that only the `find_object`
  writer (245) is ported: the oracle's `spawn_partner` (179) and
  `link_partners` (228) writers remain in the remaining 45, so those paths
  cannot yet feed the conditions. The stale reachability count was corrected
  from 66 to the current 45.
- Runtime behavior and oracle goldens are unchanged.
- Verification: `make -C tests/scripts clean && make -C tests/scripts` passed
  25 VM tests and the complete oracle suite (`All cases match the oracle.`);
  `bash tools/check.sh` passed all contract gates (SMDH skipped, no CIA);
  `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds`
  built `mdk.3dsx` after staging 69 RomFS files (79M). No golden changed; no
  emulator/hardware run. Old-3DS timing remains unmeasured.

## What this batch did (iteration 55 — partner conditions 213/214 and review-2 fixture isolation)

- Ported opcode 213 (`if_partner_distance`) and 214 (`if_partner_angle`) from
  `ScriptVm.cs:1441-1449`. Both read the existing `MdkObject::linked` field: a
  null or dead partner gives distance/angle zero; a live partner gives the full
  3D distance floored at one (`mathMax`, the C#'s `MathF.Max`) or
  `wrap(receiverYaw - bearingToPartner)`, the negated turn required to face
  the partner. No subsystem was invented;
  `find_object` (245) already writes the link.
- Added a synthetic `vm_tests` case, `PartnerConditionsUseOnlyLiveLinkedObject`,
  covering no partner, a dead partner, the distance floor, the 3D distance, and
  the signed angle (due north is -90, not 0 or +90). The opcode-pair addition
  moves coverage to 193 true dispatches / 238, with 45 remaining; the audit
  counts, remaining list, grouping notes, owned-case classification and the
  per-opcode test count were updated consistently.
- Addressed external review-2's MINOR/NIT findings on the opcode-241 fixture:
  `testIfPlayerHealthComparesKurtHealth` now constructs a dedicated
  `ScriptRuntime` (so `hurtKurt(1)` cannot leak into later tests), the receiver
  health assignment comments now say the default/defensive rationale, and the
  `OPCODE_COVERAGE.md` causal wording no longer credits the explicit pin with
  branch-discriminating strength. Runtime behavior and oracle goldens are
  unchanged.
- Verification: `make -C tests/scripts clean && make -C tests/scripts` passed
  25 VM tests and the complete oracle suite (`All cases match the oracle.`);
  `bash tools/check.sh` passed all contract gates (SMDH skipped, no CIA);
  `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds`
  built `mdk.3dsx` after staging 69 RomFS files (79M); `cmake --build
  build-linux --target mdk` built the Linux target. No golden changed; no
  emulator/hardware run. Old-3DS timing remains unmeasured.

## What this batch did (iteration 54 — correct opcode 241 fixture claims)

- Addressed the newest external review's mutation finding. `MdkObject::health`
  already defaults to 100, so iteration 53's two explicit assignments did not
  add branch-discriminating strength: iteration 52's true/false assertions
  already rejected an implementation that read the receiver instead of Kurt.
  Retain the assignments as defensive pinning against a future default change,
  and correct the iteration, coverage, and continuation claims accordingly.
  Runtime behavior and oracle goldens are unchanged.
- Verification: `make -C tests/scripts clean && make -C tests/scripts` passed
  24 VM tests and the complete oracle suite (`All cases match the oracle.`);
  `bash tools/check.sh` passed all contract gates (SMDH skipped, no CIA);
  `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds`
  built `mdk.3dsx` after staging 69 RomFS files (79M). No golden changed; no
  emulator/hardware run. Old-3DS timing remains unmeasured.

## What this batch did (iteration 53 — explicitly pin opcode 241 health fixture)

- Explicitly pinned the receiver object's own health to 100 while Kurt is
  damaged to 99. The later mutation review established that this added no
  branch-discriminating strength: `MdkObject::health` already defaulted to 100,
  and iteration 52's true/false assertions already rejected a receiver-health
  read. The explicit pin is retained as protection against a future default
  change. Runtime behavior and oracle goldens are unchanged.
- Verification: `make -C tests/scripts clean && make -C tests/scripts` passed
  24 VM tests and the complete oracle suite (`All cases match the oracle.`);
  `bash tools/check.sh` passed all contract gates (SMDH skipped, no CIA); the
  3DS build staged 69 RomFS files (79M) and produced `mdk.3dsx`. No golden
  changed; no emulator/hardware run. Old-3DS timing remains unmeasured.

## What this batch did (iteration 52 — opcode 241 `if_player_health`)

- Ported the C# comparison against `runtime.Kurt.Health` in the C++ VM. Added
  synthetic coverage for a true (`100 > 99`) and false (`100 < 99`) branch and
  confirmed opcode 241 leaves the unimplemented bucket. Updated the opcode
  audit: 191/238 true dispatches, 47 remaining. No level golden changes.
- Verification: `make -C tests/scripts clean && make -C tests/scripts` passed
  24 VM tests and all oracle cases (`All cases match the oracle.`); `bash
  tools/check.sh` passed all contract gates (SMDH skipped, no CIA); the 3DS
  build staged 69 RomFS files (79M) and produced `mdk.3dsx`. No emulator or
  hardware run; Old-3DS timing remains unmeasured.

## What this batch did (iteration 51 — exact `attach_to` type matching)

- Addressed review-10's remaining fixture gap: a live same-arena `BOLT_1000`
  candidate collides numerically with `BEAMS_1000`. The existing `BEAMS_1000`
  request must still bind to `second`, rejecting a resolver that ignores the
  type component. The test also asserts the candidate's fixture identity.
  Updated `OPCODE_COVERAGE.md` to list this discrimination alongside the
  existing exact-ID near matches. Runtime and oracle goldens are unchanged.
- Verification: `make -C tests/scripts clean && make -C tests/scripts` passed
  23 VM tests and the full oracle suite (`All cases match the oracle.`);
  `bash tools/check.sh` passed all gates (SMDH skipped, no CIA); the 3DS build
  staged 69 RomFS files (79M) and produced `mdk.3dsx`. `git diff --check`
  passed. No golden changed, no emulator/hardware run. Old-3DS timing remains
  unmeasured.

## What this batch did (iteration 50 — signed opcode 217 and percent truncation)

- Strengthened the opcode 217 `GameStats` VM test with a negative S32 operand
  that subtracts from `headShots`, plus a `Percent(2, 3) == 66` check to pin
  integer truncation. The disabled-counting assertion now verifies it leaves the
  resulting zero unchanged. Runtime implementation and goldens are unchanged.
- Verification: `make -C tests/scripts clean && make -C tests/scripts` passed
  all 23 VM tests and all oracle cases (`All cases match the oracle.`);
  `bash tools/check.sh` reported all gates passed with SMDH skipped (no CIA);
  `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds`
  staged 69 RomFS files (79M) and produced `mdk.3dsx`. No golden changed; no
  emulator/hardware run. Old-3DS timing remains unmeasured.

## What this batch did (iteration 49 — prove exact `attach_to` ID matching)

- Addressed review-9's finding that the prior fixture rejected suffix-ignoring
  logic but did not force exact equality. Added live same-arena IDs 999 and
  10001 after the valid candidates: a lower-bound/range resolver and a
  canonical-name prefix resolver now select a near-match last and fail the
  exact ID assertions. Runtime code and oracle goldens are unchanged.
- Verification: `make -C tests/scripts clean && make -C tests/scripts` passed
  all 23 VM tests and all oracle cases (`All cases match the oracle.`);
  `bash tools/check.sh` reported all gates passed with SMDH skipped (no CIA);
  `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds`
  staged 69 RomFS files (79M) and produced `mdk.3dsx`; `git diff --check`
  passed. No golden changed; no emulator/hardware run. Old-3DS timing remains
  unmeasured.

## What this batch did (iteration 48 — pin `attach_to` ID matching)

- Addressed review-8's test gap: every matching candidate previously used
  instance ID 1000, so ignoring the requested `TYPE_ID` suffix could still pass.
  Added a live same-arena `BEAMS_1001` candidate. The `BEAMS_1000` request must
  select the last eligible ID-1000 object, while a separate `BEAMS_1001` request
  must select the ID-1001 object and update both attach points.
- Verification: `make -C tests/scripts clean && make -C tests/scripts` passed all
  23 VM tests and all oracle cases (`All cases match the oracle.`).
  `bash tools/check.sh` reported all gates passed; `gates_smdh` was skipped
  because no CIA exists. `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM
  make -C mdk_3ds` staged 69 RomFS files (79M) and built `mdk.3dsx` (the
  existing `mdk.elf` was up to date). `git diff --check` passed. No golden
  changed; no emulator or hardware run. Old-3DS timing remains unmeasured.

## What this batch did (iteration 47 — strengthen `attach_to` exclusion test)

- Addressed review-7's finding that the dead/cross-arena exclusion assertions
  were masked by later valid matches. The test now inserts the valid candidates,
  self, then dead and cross-arena matches, in that order. Since `attach_to`
  retains the last eligible match, removing either exclusion independently
  makes that excluded object the selected leader and fails the assertion.
- Verification: `make -C tests/scripts clean && make -C tests/scripts` passed all
  23 VM tests and all oracle cases (`All cases match the oracle.`). `bash
  tools/check.sh` reported all gates passed; `gates_smdh` was skipped because no
  CIA exists. `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM
  make -C mdk_3ds` staged 69 RomFS files (79M) and built `mdk.3dsx` (the
  existing `mdk.elf` was up to date). No golden changed. No emulator or
  hardware run; Old-3DS timing remains unmeasured.

## What this batch did (iteration 46 — opcode 74 `attach_to`)

- Ported opcode 74 from `ScriptVm.cs:968`: scan live objects in stable runtime
  order, excluding self and removed/dead objects, require same arena, compare
  formatted `TYPE_ID` case-insensitively, and keep the last match. On a match,
  assign leader, both attach-point indices, and movement command 74; on a miss,
  preserve all object state. Existing movement-command 74 behavior follows the
  attachment and copies the leader's reference-point position and orientation.
- External review-6 found that C# auto IDs begin at 1000 while C++ began at 0,
  and that the original test inferred its operand from C++ state and did not
  actually prove duplicate-ID selection or state preservation. Set
  `nextInstance_` to 1000 and use an isolated-runtime test with oracle operand
  `BEAMS_1000`, duplicate explicit IDs, dead/cross-arena/self exclusions, case
  folding, and seeded leader/point/move state on a miss. Restored the displaced
  `findObject` source comment to its function.
- Verification: `make -C tests/scripts clean && make -C tests/scripts` passed
  all 23 VM tests and all oracle cases (`All cases match the oracle.`). The
  first test run caught a harness mistake (the second receiver was not in the
  runtime object collection); corrected it by executing the program on the
  spawned self object, then reran the full command successfully. `bash
  tools/check.sh` reported all gates passed; `gates_smdh` was SKIPPED because no
  CIA exists. `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM
  make -C mdk_3ds` linked `mdk.elf`, staged 69 RomFS files (79M), and built
  `mdk.3dsx`. `git diff --check` passed. No golden changed. No emulator or
  hardware run; Old-3DS timing remains unmeasured.

## What this batch did (iteration 45 — finish deferred object inventory review)

- Followed up on external review-4 of iteration 44's `object.h` behavior list.
  The list now records `PoseParts` as unported; sound positioning is implemented
  in `SoundMixer::voicePosition`, and distance behavior is already inlined at
  landed call sites. Clarified that `PartCenter` is a rest-pose box centre,
  unlike current-pose `part_bounds()` / `pose_bounds()`.
- Documentation/comments only; no runtime behavior, oracle fixtures, or goldens
  changed.
- Verification: `make -C tests/scripts clean && make -C tests/scripts` passed
  all 22 VM tests and all oracle cases (`All cases match the oracle.`);
  `bash tools/check.sh` reported `CHECK: ALL GATES PASSED` (`gates_smdh`
  skipped because no `.cia` exists); `DEVKITPRO=/opt/devkitpro
  DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds` linked `mdk.elf`, staged
  69 RomFS files (79M), and built `mdk.3dsx`. No emulator/hardware run; Old-3DS
  timing remains unmeasured.

## What this batch did (iteration 44 — reconcile deferred object behavior)

- Addressed external review-3's documentation finding in `mdk_core/src/scripts/object.h`:
  reconciled the object behavior status against the current tree. The deferred
  list excludes implemented `rotation()`, `playAnimation()` / `advanceAnimation()`,
  `poseBounds()`, `partCenter()`, and runtime `yawTo()`; it retains unported
  Footing, frame-blend/transform reads, and pose/part baking. Review-4 found that
  the then-current comment still incorrectly listed sound-position and distance
  as deferred and omitted `PoseParts`; iteration 45 corrects that inventory.
  Documentation-only; no runtime behavior or goldens changed.
- Verification: `make -C tests/scripts clean && make -C tests/scripts` passed
  all 22 VM tests and all oracle cases (`All cases match the oracle.`);
  `bash tools/check.sh` reported all gates passed (`gates_smdh` skipped because
  no `.cia` exists); `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM
  make -C mdk_3ds` linked `mdk.elf`, staged 69 RomFS files (79M), and built
  `mdk.3dsx`. No hardware or emulator run was needed/performed. This is a
  documentation-only correction and does not claim Old-3DS readiness.

## What this batch did (iteration 40 — opcode 77 `debug_msg` no-op)

- Ported opcode 77 as an explicit no-op in `ScriptVm::execute`, matching the
  C# `case 77` at `ScriptVm.cs:1713`. The C# deliberately ignores debug output;
  no logging subsystem or side effect is implied. Decoder continues to consume
  the U8 and Pascal-string operands before dispatch.
- Added a synthetic `vm_tests` program encoding mode 3 and `"debug text"`, then
  a `set_var` instruction. It verifies opcode 77 is not in the unimplemented
  bucket and that execution advances and performs the following write.
- No level golden changes: opcode 77 does not fire in levels 3-8. Coverage is
  now 188 true dispatches / 238, with 50 remaining.
- Verification: `make -C tests/scripts clean && make -C tests/scripts` passed
  all 21 VM unit tests and `All cases match the oracle.`; `bash tools/check.sh`
  reported all gates passed (SMDH gate skipped because no `.cia` exists);
  `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds`
  linked `mdk.elf` and built `mdk.3dsx`; `git diff --check` passed. The
  adversarial reviewer was unavailable (review-37 is a prior quota failure and
  no reviewer response was available for this iteration); self-review compared
  behavior and decoder operand order against the read-only C# oracle.


## What this batch did (iteration 11 — pickup/chute/death lifecycle slice)

- **`ScriptRuntime::kill()` now mirrors C# `Kill` (ScriptRuntime.cs:1678)**:
  `health=0`, then either the death-script switch (move_command=0,
  FlagNotTarget, restart/wait_resume = death_script, wait_time=0,
  death_script=0) or the Explode branch. The Explode branch's EFFECTS
  (PlaySoundAt/FlashAt/BreakUp/SpawnExplosion — mixer, flash, debris, explosion
  model) remain unported, but its REMOVAL ACCOUNTING now lands: C# `Explode`
  calls `Remove(obj)` (cs:1717), so the C++ branch calls `remove(obj)` instead
  of the old `dead=true` placeholder (which left the object counted in
  `objectCount()` — found by the adversarial validator). No golden to t30000
  exercises the Explode branch (every corpus death is a death-script death),
  so the missing effects stay the documented gap.
- **`ScriptRuntime::remove()` now mirrors C# `Remove` (cs:1648)**: a live
  `Attached` (a pickup's chute) is removed FIRST (before the parent is marked
  dead), with an `isRemoved()` early-return guard for idempotence (the C++
  `removed_.push_back` is not idempotent, unlike C# `List.Remove`). A pickup
  that falls out (`fallOut` → `remove`) takes its chute with it. The stale
  `fallOut` NOTE (calling kill() a placeholder) was corrected.
- **The frontier moved from ~t400-900 to the end of the corpus.** Probed tick
  by tick against the oracle (table AND script dumps): L3 matches to t30000
  (11 live objects), L8 to t30000 (10), L6 to t30000 (5), L7 to t30000 (7),
  L4 to t10000 (0 objects — the arena empties at the same ticks as the oracle;
  the old "L4 empties past t600" divergence was the kill() placeholder). L5
  matches to t2302; t2303 is the new blocker (BOLT spawn, priority 1 above).
- 36 new golden cases, 72 golden files, all generated from the oracle driver
  (`dotnet mdkoracle.dll <level> <ticks> 1 [script]`). The temp `RALPH_DUMP`
  probe used for the probing was removed from `tick1.cpp` before committing.
- Adversarial validator run over the diff: one MAJOR (Explode-branch count
  accounting) found and fixed as above; the death-script branch field-for-field
  and the Attached cascade order were independently confirmed against the C#.
- Call-site audit vs the C#: C# `Kill` has three call sites (ScriptVm.cs:494/505
  = opcodes 16/56, ported; cs:1554 = opcode 184 `explode`, in the remaining-77
  set, gated behind the unported `Items.Blast`). C# `Remove` call sites map to
  the C++ ones (opcode 110, fallOut, chute-shrink, the Attached cascade); the
  arena-unload removal (cs:677) and the collected-shrink removal (cs:2610)
  belong to unported slices (arena streaming, collected shrink) and do not fire
  in the corpus.


## What this batch did (iteration 8 — deep-tick golden frontier)

- Extended the golden corpus to each level's honest deep-tick frontier, generated
  from the oracle driver (table + script dumps), all matching byte for byte:
  **L3 t360/t420** (pickups/chutes resolve, objects drop 14→12), **L5 t660** (the
  XGEN region past t600), **L6 t660/720/780/840/900** (the XD keeps walking — the
  table genuinely cycles, not a frozen steady state: object 002's pos/yaw/restart
  change across the range), **L7 t660/t900** (a steady arena bracketed to prove no
  slow drift). 10 new cases, 20 new golden files.
- **The frontier is the unported-slice boundary, confirmed empirically, not
  assumed.** Probed deeper ticks against the oracle and committed only the matching
  ones; the first divergent tick per level is the unported lifecycle: **L8 diverges
  at t420** (SW_H01 + SW_CHUTE removed between t360 and t420 — the pickup/chute
  removal slice), **L3 at t480**, **L5 at t720**, and **L4 empties to 0 objects
  past t600** (degenerate, not committed). None of the divergent goldens were
  committed (they would fail).
- **Independently re-verified the testability map's blocker** (so the next context
  does not re-litigate it): `grep` over every golden shows `move=15/` appears **0
  times** — movement command 15 (the only `AlarmTicks` setter, ObjectMotion.cs:385)
  never fires in levels 3-8 to t600, so porting the alarm-ticks state machine +
  dumping `alarm_ticks` would yield an all-zeros column that exercises nothing.
  The four reachable unimplemented opcodes (109/138/175/249) fire but their effects
  are not in the dumped field set (Kurt health, Kurt inventory, audio) or need an
  unported subsystem, so they remain blind-port-forbidden.
- **Corpus cannot widen by level**: only `MDK/TRAVERSE/LEVEL3..8` data is present
  (no level 1/2/9+), so the only axis left to densify is ticks, and that axis is
  now bounded by the unported lifecycle slices above.
- Test-only change; no `mdk_core` code touched, so both targets are unaffected.
  Verified: `make -C tests/scripts` (clean) → `All cases match the oracle.`;
  `bash tools/check.sh` → `CHECK: ALL GATES PASSED` (`gates_smdh` SKIPs: no `.cia`);
  `make -C mdk_3ds` → links `mdk.3dsx`; `cmake --build build-linux --target mdk` →
  `Built target mdk`.


## What this batch did (iteration 7 — target-tracking goldens)

- Executed the testability map's recommended unblock (the "extend BOTH the oracle
  driver's `script` dump and `tick1.cpp` `dumpScript`" move), scoped to the
  runtime fields the C++ **already** maintains so the change is a pure
  regression-lock with no behaviour change: `target_pos`, `target_yaw`,
  `alien_target`. These are `ScriptRuntime`-level state (C# `TargetPosition`/
  `TargetYaw`/`AlienTarget`, ScriptRuntime.cs:199-208), reset to Kurt each tick
  (cs:887-889) and overridable per object by `SelectTarget` (cs:418-441); the C++
  mirrors this in `selectTarget` (script_runtime.cpp:2192) and the constructor.
- **Measured before committing**: extended both dumps, ran the C++ against the
  OLD goldens (117 cases differ — the SCRIPT line changed), then confirmed the
  C++ `got` SCRIPT lines equal the oracle's for L3_t300/L5_t600/L8_t300/L4_t600
  byte for byte. Regenerated all 114 script goldens from the oracle → all match.
- **Sensitivity proven**: a `+0.0001f` perturbation of the dumped `target_yaw`
  makes the script checks FAIL (then reverted) — the new fields are genuinely
  compared, not ignored.
- `alien_target` is `-1` in every current case (opcode 251 does not fire in
  levels 3-8 to t600), so the index mapping is correct-but-unexercised; it is
  computed over the same removed-skipped enumeration as the OBJ lines, so the
  two views agree when it does fire.
- **`alarm_ticks` deliberately NOT added**: the C++ has no `alarm_ticks` field
  (the alarm-ticks state machine — the per-tick decrement cs:896 + the setters in
  movement command 15 / Rides — is unported), so dumping it would diverge and
  force a blind port. It stays the next field to add, paired with the alarm
  subsystem slice. `Items.Bomb`/`Decoy` also stay out: they live on a separate
  `Items` subsystem not reachable from the driver, and the C++ `itemsBomb` is
  always null (matches the C# when no bomb is in play).
- **Oracle-driver provenance** (the driver is `/tmp/kilo/oracle/Driver/Program.cs`,
  not in the repo — same as every existing golden): the `script` dump mode's
  SCRIPT line gained `target_pos={X:F4},{Y:F4},{Z:F4} target_yaw={Yaw:F4}
  alien_target={idx}`, where `idx` is `AlienTarget`'s index in `scripts.Objects`
  or `-1`. Regenerate with
  `dotnet mdkoracle.dll <level> <ticks> 1 script > golden/L<level>_t<ticks>_script.txt`.
- Verified: `make -C tests/scripts` (clean) → `All cases match the oracle.`;
  `bash tools/check.sh` → `CHECK: ALL GATES PASSED` (`gates_smdh` SKIPs: no
  `.cia`); `make -C mdk_3ds` → links `mdk.3dsx`; `cmake --build build-linux
  --target mdk` → links `mdk`. Test-only + oracle-harness change; no `mdk_core`
  code touched, so both targets are unaffected.


## What this batch did (iteration 6 — verification, review, doc correction)

- **Tree re-verified green** on a clean rebuild (iteration 5 timed out mid-final-
  verification after committing the scene code `003f9a3` but before committing its
  `CONTINUATION.md` update):
  - `make -C tests/scripts` (after `clean`) → `All cases match the oracle.` (all
    table + script + `L{n}_dti` + the three scene checks PASS).
  - `bash tools/check.sh` → `CHECK: ALL GATES PASSED` (`gates_smdh` SKIPs: no built
    `.cia`).
  - `make -C mdk_3ds` → stages RomFS (69 files, 79M) and links `mdk.3dsx`.
- Committed iteration 5's dangling `CONTINUATION.md` update (the dynamic-scene
  batch note) as its own commit — the code was already committed, only the doc was
  left in the working tree.
- **Adversarially reviewed the scene commit `003f9a3`** against the C# `LevelView`
  (the four external reviews all timed out at rc=124 mid-verification and produced
  no findings). Checked: the run-break at group boundaries (a batch is single-group,
  matching the C#'s per-group meshes batched by surface); the orphaned-vertex path
  when a not-drawn triangle splits a same-material/same-group run (the split triangle's
  3 vertices are pushed but referenced by no batch — wasted buffer, never drawn, and
  the neighbouring batches' `first`/`count` stay correct); `arenaBatchVisible`
  (group 0 has no stored state → always drawn); `LevelView::refresh()` (rebuilds the
  whole arena mesh for a `group_set_texture`, the C# per-group `Rebuild` produces the
  same picture; the texture/flat-colour caches grow monotonically, matching the C#
  "old mesh lives until the level ends"). **No bug found.**
- **Corrected a real mis-classification** in `docs/OPCODE_COVERAGE.md`: opcode **27**
  was grouped under "Audio (no mixer port)". It is `if_alarm`, a branch on
  `runtime.AlarmTicks > 0` (ScriptVm.cs:1693) — gated behind the **alarm-ticks state
  machine** (a runtime `alarm_ticks` field + the per-tick decrement
  ScriptRuntime.cs:896 + the setters in movement command 15 ObjectMotion.cs:385 and
  Rides Rides.cs:219), none of which exists in the C++. 107/233/249 are the real
  audio opcodes. 27 does not fire in levels 3-8 to t600, so there is no golden to
  exercise it — it stays deferred, now with the correct gate named.


## What this batch did (dynamic scene updates — open item 1)

- Open item 1 ("a mid-play `group_state`/`group_set_texture` change does not
  change what is drawn until the scene is rebuilt") is **closed at the scene
  level**, faithful to the C# `LevelView`, which checks Hidden at DRAW time
  (LevelView.cs:126) and rebuilds a group's mesh on a material change (the
  `Changed` event → `Rebuild`, LevelView.cs:251).
- `ArenaBatch` gained a `group` field, and `buildArenaScene` now breaks a run at
  a **group boundary** as well as a material change, so every batch is single-
  group. The build-time Hidden skip is **gone**: hidden groups' geometry stays in
  the mesh and `arenaBatchVisible(arena, batch)` (a new free function) decides
  per batch at draw time — so `group_set_state` hide/show needs **no rebuild**.
  `LevelView::draw()` skips a batch whose group is Hidden now.
- `LevelView::refresh()` is the `group_set_texture` path (the C# `Rebuild`): it
  re-runs `buildArenaScene` against the arena's current group materials and
  rebuilds the GPU mesh (release + create, like the C# creating a new mesh). It
  is the integration hook a `group_set_texture` calls between frames; there is no
  `GameEngine` yet to call it, so the mechanism is in place and tested at the
  scene level, and the per-opcode wiring lands with the engine. A hide/show does
  NOT call it (draw reads hidden live).
- **Regression test** (`tests/scripts/tick1.cpp`, scene section): over **all 104
  arenas** of levels 3-8 it checks (a) the batches partition the drawn triangles
  by group — a batch spanning two groups breaks this equality, so this proves the
  run-break; **880 cross-group same-material adjacencies** exercise it (removing
  the run-break makes it FAIL on HMO_2/HMO_3/MEAT_*/…, confirmed); (b) hiding a
  group after the build makes its batches not-visible while others stay visible;
  (c) `group_set_texture(-1)` on a textured group makes its batches flat on a
  rebuild. `arena_scene.cpp` + `material_resolver.cpp` added to the test Makefile.
- Verified: `make -C tests/scripts` (clean) → `All cases match the oracle.` (new
  scene PASSes); `bash tools/check.sh` → `CHECK: ALL GATES PASSED`; `make -C
  mdk_3ds` → links `mdk.3dsx`; `cmake --build build-linux --target mdk` → links
  `mdk` (both targets build the changed core).


## What this batch did (DTI record discrepancy — reproduced and closed)

- Open item 4's "C++/oracle DTI record discrepancy" is **CLOSED with a concrete
  reproduction**, not an argument (the prior batch had only argued the layouts
  match field-for-field).
- Reproduction: dumped every arena's object records for levels 3-8 from BOTH the
  C# oracle (`Dti.ParseRecords`) and the C++ `Dti::parseRecords`, printing every
  float (angle, position[3], boxEnd[3], arena pitch) as **raw IEEE bits** so a
  one-ULP difference cannot hide behind `%.4f`. Result: **byte-identical across
  all 6 levels, 19 arenas each, every record** — type/id/angle/position/boxEnd
  (the three-floats-then-`skip(-12)`-then-name(12) rewind quirk) and name. The
  claim was unsubstantiated; there is no DTI record discrepancy.
- Locked it with a **permanent regression test**: `tests/scripts/tick1.cpp` now
  dumps the same raw-bit record view and compares to committed goldens
  `tests/scripts/golden/L{3..8}_dti.txt` (generated from the oracle). The test is
  sensitive — a probe aliasing bug during authoring made the C++ dump differ, and
  the check caught it, confirming it fails on a real divergence.
- Reproduce/regenerate: the oracle driver gained a `dti` dump mode
  (`/tmp/kilo/oracle/Driver/Program.cs`, 4th arg `dti`) that prints the records
  in this format; the goldens are that output. (The driver is the oracle harness,
  not in the repo — same as the existing goldens' provenance.)
- Test-only change; no `mdk_core` code touched, so both targets are unaffected.
  Verified: `make -C tests/scripts` (clean) → `All cases match the oracle.` (six
  `L{n}_dti` PASS); `bash tools/check.sh` → `CHECK: ALL GATES PASSED`;
  `make -C mdk_3ds` → links `mdk.3dsx`.


## What this batch did (owned-opcode classification — Task 2 remainder)

- Open item 2 (classify the 161 owned cases) is **done**. The classification is
  in `docs/OPCODE_COVERAGE.md` §"Owned-case classification", produced by a
  reproducible brace-matched extraction of the numeric `case` labels from the C#
  `ScriptVm.Execute` and the C++ `ScriptVm::execute` (re-confirmed C# 238 / C++
  161 / remaining 77; the C++ owns no case the C# lacks).
- Result: **0 stubs**, **11 partial** (faithful dispatch whose gameplay effect
  is gated behind a partial/unported subsystem), **150 complete**. Each partial
  is verified against the code, not asserted:
  - 6, 15 — set `move_command` 6/15 but `ScriptRuntime::updateCommand`
    (script_runtime.cpp:1561) has no case for them → no motion.
  - 16, 56 — call `kill()`, a death-slice placeholder (script_runtime.cpp:2451
    sets only `dead`; death script / debris / drops unported).
  - 24, 151 — store sound names whose playback needs the unported mixer.
  - 89, 248 — honest no-ops (mixer / Kurt knock-back unported; no state the
    goldens read).
  - 140 — sets group material VM state; renderer ignores it (open item 1).
  - 165, 166 — read `itemsBomb`, never assigned (script_runtime.h:226) → always
    "no bomb" until the Items slice.
- Test-coverage axis recorded separately: all 161 owned cases are covered only by
  the end-to-end (level, tick) goldens; there are no per-opcode unit tests.
- Doc-only change; no `mdk_core` code touched, so both targets are unaffected.
  Verified: `make -C tests/scripts` → `All cases match the oracle.`;
  `bash tools/check.sh` → `CHECK: ALL GATES PASSED`; `make -C mdk_3ds` → links
  `mdk.3dsx`.


## What the prior batch did (L5 deep-tick goldens — divergence closed)

- The open "L5 `XG[004]` golden divergence ~tick 577 (root cause unknown)" is
  **resolved**. Reproduced against the oracle: at t600 the oracle's object count
  oscillates 8→9→8 (the XGEN spawner's 5th XG opens ~t585 and dies ~t595) and
  object 004 takes a `move_near_target` (opcode 43) at ~t577. The current C++
  matches the oracle **byte for byte** (table AND script state) at t577/585/590/
  595/600. The old divergence was the same degree/radian constant-fold ULP class
  the left-associative spelling already resolves (the stale `/tmp/kilo/cpp_L5_t600_*.txt`
  dumps predated that fix and misled the earlier note).
- Added golden cases `{5,577},{5,585},{5,590},{5,595},{5,600}` (table + script)
  to `tests/scripts/tick1.cpp`, generated from the oracle. Verified:
  - `make -C tests/scripts` → `All cases match the oracle.` (new cases PASS,
    table + script).
  - `bash tools/check.sh` → `CHECK: ALL GATES PASSED`.
  - `make -C mdk_3ds` → links `mdk.3dsx`.
  - Test-only change; no `mdk_core` code touched, so both targets are unaffected.


## What the previous batch did (all verified against the C# oracle)

- `TriangleGroups.SetState` semantics: stored per-group state on `Arena`
  (`groupState`/`setGroupState`/`hasGroup`), full 1..255 group numbers, the
  0/2/3/4/5/other operation table, write-through to every triangle of the
  group. `ScriptRuntime::setGroupState` mirrors it; the old per-triangle
  derivation and the ≤16 clamp are gone.
- Load-time `Disable`: `Arena::applyDisabledTriangles()` runs in the
  `ScriptRuntime` constructor (after clip flags), not in `Arena::parse` —
  LEVEL8 GUNT_2's diagonal wall is now non-solid like the original.
- `raycastGroup` mirrors `raycastPoint` exactly (`kSegmentAny`,
  `kClipPassesThrough`, second arena unconditional, all-arena fallback,
  nearest-hit by squared distance, same `>= best` skip);
  `getKurtFloorGroup` uses it (+1 to −10 under Kurt).
- `HitGroup` implemented, including the scratch-object run path: the arena's
  `hit_scripts` `MdkObject` gets hit_type=weapon, hit_event=0, wait_time=0,
  cleared gosub stacks, restart=script, then `vm_.run()`.
- Group materials: `Arena::groupMaterial(group, int32_t* out)` returns
  presence (negative materials like −1 are valid; the old −1 sentinel was a
  bug the validator caught). `setGroupTexture` (opcode 140) wired with the
  existence check.
- Renderer build-time skips: clip/disabled triangles and hidden groups.
- Opcodes audited per Task 1.7: 245 `find_object` (factored into a runtime
  helper), 246, 250 `if_in_box` (six-value zero-filled array + sender), 87
  `kurtLooksAt` (constants match ScriptRuntime.cs:2701), 6/15, 62, 207.
- VM group cases 98/99/140/162/163/168/194 match C# operand layout.


## Iteration 14 — Kurt health/inventory state + opcodes 109/175 (commit 4bdcba6)

- New `mdk_core/src/game/kurt.h/.cpp`: the Hurt/Damage half of `Kurt.Combat.cs`
  (health 0x574324, red flash clamp 75..180 at 25/damage, invulnerability
  0x573bd4, knock damage 0x573b20, the Dead/Knocked/GetUp/HardLand gate, the
  difficulty scaling 2/3-easy-min-1 / 2x-hard) and the `Inventory.cs` slot list
  (5 slots, the Item enum). The movement state machine and pickup collection are
  later slices, so in the corpus the state moves only through the scripts.
- `ScriptRuntime::hurtKurt` mirrors C# `HurtKurt` (ScriptRuntime.cs:2676)
  including the `Rides.TakesHits` gate (ride slice unported → nothing ridden →
  Kurt takes every hit, as the C# reads). VM: case 109 → hurtKurt(I(o[0]));
  case 175 → countItem + compareOps(count, o[1], o[2], o[3] ?? 0).
- The oracle driver's script dump gained a `KURT` line
  (health/hurt_flash/invulnerable/knock/state/slots); all 170 script goldens
  regenerated from the oracle and matched byte for byte. L3 t480 pins health 96
  / flash 150 / knock 4 (two hurts, flash clamp); L5 pins Kurt dead (health 0,
  knock 101) by t2303; L4/L6/L7/L8 pin him untouched (100/0/0). state=0 and
  slots=0 everywhere confirm push_kurt never knocks him and nothing is collected
  in the corpus — the gates are exercised at their false branch.
- Gates: `audit_r11_idiv` flagged the easy-difficulty `/3`; the constants are
  now `constexpr` (the audit trusts constexpr divisors; house style in object.h
  already uses them). `std::max` needed `(int32_t)` literals on arm-none-eabi
  (int32_t is `long` there).
- Verified: tests 341 PASS/0 SKIP, check.sh ALL GATES PASSED (smdh SKIP, no
  .cia), mdk_3ds links mdk.3dsx, linux `Built target mdk`.

## Iteration 17 — the audio slice: mixer, sound bank, device control plane; 89/107/233/249

- In-flight from iterations 15-16 (which died before committing); this iteration
  verified, fixed, and banked it. New `mdk_core/src/audio/`: `sound_bank.{h,cpp}`
  (SNI archive lookup, newest-archive-wins, cached-missing, WAV-header metadata
  only — samples belong to the platform backend), `sound_mixer.{h,cpp}` (the
  voice table, Start modes New/Restart/Once, 2D/point/source placement, Doppler,
  scope listening — a 1:1 port of SoundMixer.cs; the spare-Voice pool and the
  Func-follow overload are documented non-ports), and `sound_device.{h,cpp}`
  (the AudioDevice control plane: ids from 1, the 64-voice cap that drops a new
  sound, per-voice gain/pitch/pan, the frame-consumption lifecycle the muted
  device would run). The C# `ISoundSource` virtual is replaced by the two fields
  that answer it (the object's position and yaw pointers into the never-vacated
  object deque) per AGENTS.md §10.
- VM: case 89 → playSound(str(o[2]), I(o[0]), o[1]) with the flags decode
  (mode 0-3, Flat/Following/AtReference/AtPoint/Tracked); case 107 →
  setLoopSound; case 233 → isSoundPlaying(upper(name)); case 249 → the
  TrackedVoice/TrackedSound branch. ScriptRuntime::remove now stops the loop,
  tracked, and following voices as the C# Remove does.
- The oracle driver's script dump gained the `AUDIO voices=N list=id:NAME,...`
  line (reflection on `_voices`) and per-object `tsnd/tvoice/tplay/lsnd/lvoice/
  lplay`; all 170 script goldens regenerated from the oracle. L5 pins nonzero
  tracked voices with tplay=1 (249's true branch fires); L3's table fills to the
  64-voice cap with RICO1 on both sides (the headless harness never calls
  mixer.update, on either side). Six goldens re-diffed against a freshly built
  oracle binary: byte-identical.
- Fixes this iteration: the R6 audit flagged `rotatedZ`'s sinf/cosf — pinned with
  the established oracle-fidelity R6 pin (glibc matches MathF.SinCos bit-for-bit;
  the LUT would move a voice's pan). The R11 screen flagged the WAV frame-count
  division in sound_bank.cpp — the geometry moved to `Wav::frames()` (formats/,
  the contract's cold-parsing scope) instead of pinning a cold site with a
  hot-only rule (§5). A stray extra printf argument in tick1.cpp.
- `docs/OPCODE_COVERAGE.md` recounted with its own brace-matched method: C# 238 /
  C++ 168 / remaining 71 (it had also been stale on 115, ported at iter 11);
  partials 11→10 (89 complete; 24/151 re-gated to their now-unblocked call-sites).
- Verified: tests 341 PASS/0 SKIP, check.sh ALL GATES PASSED (smdh SKIP, no
  .cia), mdk_3ds links mdk.3dsx, linux `Built target mdk`.

## Mixer call-sites + Muse swarm migration (2026-10-25)

- Wired the last two mixer call-sites: the motion frame-sound trigger
  (`ObjectMotion.cs:133`) and the door state machine's `DoorSounds`
  (`ObjectBehaviors.cs:90-115`, `Play` at 418-424). All three gates pass
  with no golden change; adversarial review found no semantic divergence
  (observability MAJOR rebutted in `docs/CONTINUATION.md`).
- The swarm moved from Kilo/litellm to the Muse subscription: `ralph.sh`
  now drives `muse exec` (builder `muse-spark-1.3`, reviewer
  `muse-wonder-1.0` with `--disable-write`), prompts live in
  `ralph/builder.md` + `ralph/validator.md`, and `.kilo/` is removed.

## Debris slice: opcodes 137/138/139 (2026-10-25)

- `mdk_core/src/scripts/debris.{h,cpp}`: 1:1 port of `Debris.cs`
  (Shatter/Spark/Update, 600-piece pool, seeded-RNG call order, banker's
  rounding for `MathF.Round`, .NET's double-derived degrees→radians
  factor — the float spelling was 1 ULP off in ~46% of draws, proven by
  the adversarial review). Runtime: `ShatterPoint`/`ShatterDirection`,
  Ray/GroupFacets delegates wired; Trail/Updraft wait for Effects/Fans.
- Oracle harness fix: the driver never registered its `TriangleGroups`
  (only the viewer's `LevelView` calls `Add`), so Shatter was a no-op in
  every golden. It now registers all arenas; L5's 138 (t482) yields 134
  pieces. All 340 goldens regenerated from the oracle; the script dump
  gained a `DEBRIS` line on both sides.
- Verified: tests/scripts "All cases match the oracle", check.sh ALL
  GATES PASSED (one R6 pin for spawn-time spin trig), mdk_3ds links,
  Linux builds. Coverage 171/238, remaining 68. Adversarial review: no
  BLOCKER/MAJOR; the 1-ULP MINOR above was fixed.
- Note: the runtime half of this slice (constructor/rayIn/groupFacets)
  was banked inside the preceding audio commit by a staging overlap;
  the tree at HEAD is consistent, that one intermediate commit is not
  bisect-clean on its own.

## Synthetic opcode harness + if_cheat_key (2026-10-26)

- The corpus is spent for opcodes (nothing unported fires in levels 3-8 to
  t30000), so the second route in is a synthetic harness:
  `tests/scripts/vm_tests.cpp` is a 1:1 C++ twin of
  `tests/Mdk.Game.Tests/ScriptVmTests.cs` — same `Code` builder bytes (gap,
  FileBase-4 targets), same level-7 runtime shape, same 7 programs and
  assertions — wired as a second binary in `tests/scripts/Makefile` that runs
  before the goldens in `make -C tests/scripts`. Both binaries link the same
  core objects (shared `COMMON_SOURCES` list).
- Opcode 13 (`if_cheat_key`) ports through it: an explicit
  `return branch(obj, ins, false)` mirroring `ScriptVm.cs:1709`, instead of
  falling into `noteUnimplemented`. The twin pins both the value (goto not
  taken, `variables[0] == 1`) and the clean `unimplemented()` bucket (the C#
  owns 13 explicitly, so it never touches its `Unimplemented` dict either).
- Authentic failure observed: pre-port, 6/7 twin tests passed and only the new
  `!saw13` assertion failed; post-port all 7 pass
  ("All VM unit tests match the oracle."). Goldens unchanged, as predicted
  (13 never fires in the corpus).
- Verified: `make -C tests/scripts` → twin 7/7 + "All cases match the oracle.";
  `bash tools/check.sh` → ALL GATES PASSED (smdh SKIP, no .cia);
  `make -C mdk_3ds` (devkitARM env) → links `mdk.elf`, builds `mdk.3dsx`.
- Coverage: brace-match recount → C# 238 / C++ 173 / remaining 66; fixed the
  doc's stale 171/68 classification numbers and the 11-vs-8 partials slip.
- Gap (environmental, not code): the C# `ScriptVmTests` suite could not run in
  this sandbox — AF_UNIX sockets are denied (`socket()` → EPERM, probed) and
  .NET's MSBuild/test servers require them; no prebuilt test dll exists. The
  twin was verified against the in-repo C# file line by line instead, and the
  6/7 first-run pass corroborates the program bytes. Re-run the C# suite
  outside the sandbox when convenient.
- Next: unfired opcodes land through the harness, one family per batch — the
  parts bucket (32 `clear_parts_mask`, 129/132/136) is the natural next slice.

## 2026-10-26 — iteration 24: opcodes 32 + 129 (parts pair) via vm_tests (coverage 175/238)

- Opcode 32 (`clear_parts_mask`) is a one-liner through the existing
  `partsMask` helper with `hide=false`; opcode 129 (`blow_off_parts`)
  validates the gore mode (`mode > 2`, null model, mode-2-without-gore break),
  resolves names via `findPart`, and ORs the mask into `locked_parts` and
  `hidden_parts` (`ScriptVm.cs:1523-1524,1746-1769`). No new subsystem: all
  fields and helpers already existed for opcode 31.
- 132/136 (`spawn_effect`/`spawn_debris`) stay with the Effects slice: both
  draw RNG and delegate to `Effects.SpawnBubble`/`SpawnDrop` (effect list,
  textures, frame counts, per-tick update) — a dump-line subsystem slice, not
  a dispatch.
- Tests: `pstr` builder + synthetic 3-part model fixture (`partsModel`) in
  `vm_tests.cpp`; three tests pin hide/show-by-name, ALL-vs-locked,
  gore on/off, mode 1 vs 3, unknown names, and the 129→32 interaction, each
  with the `unimplemented()`-bucket check. No C# counterpart exists to twin
  (that suite covers 11 opcodes), so they pin the oracle's ScriptVm.cs
  directly with identical program bytes — noted in the file header.
- Authentic failure observed: with the port stashed, 14 checks fail (state +
  bucket assertions); restored, all 10 tests pass
  ("All VM unit tests match the oracle.").
- Verified: `make -C tests/scripts` → 10/10 + "All cases match the oracle."
  (goldens unchanged — 32/129 never fire in the corpus);
  `bash tools/check.sh` → ALL GATES PASSED (smdh SKIP, no .cia);
  `make -C mdk_3ds` (DEVKITPRO/DEVKITARM) → `mdk.elf` (fresh link, newer than
  the port) + `mdk.3dsx`.
- Coverage: brace-matched recount → C# 238 / C++ 175 / remaining 64 (exact set
  difference). Noted in the doc: the C++ label count carries a nested-switch
  artifact (the `case 0:` mode arm inside opcode 249, present since the mixer
  slice), so true dispatches are 174 and 174 + 64 = 238.
- Next: unfired opcodes land through the harness, one family per batch —
  132/136 need the Effects slice; the Kurt/items, arena, movement and
  camera buckets in OPCODE_COVERAGE.md keep their subsystem gates.

## Iteration 28 — rope-swing pair: opcodes 226 `jump_to` + 242 `set_2d0_block`

- 226 starts the swing below the pivot (`swing_pivot`/`speed`/`gain`/`yaw`/
  `length` + `FlagSwinging`); 242 sets the rope lines (colour 1, mask 0xFF,
  four points) or clears the mask on count zero. Pure object-state over
  fields the C++ already owns (`object.h`); 242 guards an empty block like
  the existing `partsMask` helper. 85 (`set_rolling`) stays out: it snapshots
  the full `Rotation` matrix and `rotation()` is deferred to FrameBlend.
- Tests first: two `vm_tests` tests failed authentically in the
  `unimplemented()` bucket (20 checks), then passed after the dispatch.
  Pin `ScriptVm.cs:1139-1166` with identical program bytes (F32×5 for 226;
  u8 mode + 12 F32 for 242).
- Gates: `vm_tests` 12/12; goldens "All cases match the oracle" (unchanged —
  neither opcode fires in the corpus); `tools/check.sh` ALL PASSED;
  `mdk.3dsx` links. Coverage: brace-matched recount → C# 238 / C++ 177 /
  remaining 62 (true dispatches 176; `case 0:` artifact inside 249 noted).
- Next: one opcode family per batch through the harness; 132/136 still need
  the Effects slice; 85 waits on the FrameBlend `rotation()` slice.

## Iteration 29 — spawn slice: opcode 156 `spawn_at_point` + review-28 closed (coverage 178/238)

- 156 is `runtime_.spawn` at `obj->reference_point`, operand layout `[U8, Pstr, Code32]`
  (`ScriptVm.cs:1573`, `MdkObject.cs:574-582`). Both helpers already existed for the
  earlier spawn cases, so the dispatch is three lines with no new subsystem. R11/R4/R6
  clean (no division, no double, no libm in the dispatch).
- Test pins the oracle directly (no C# counterpart exists): BEAMS is a model of the
  DANT_1 arena (LEVEL7O.MTO) with no init script (no `DANT_1$BEAMS` key in the CMI
  object scripts — verified in the binary), so the spawn runs no nested script.
  Asserts type/arena/position (refpoint + position), spawn_position, yaw 0,
  instance sequencing (-1 takes next id twice), the out-of-range fallback to the
  position, and the clean `unimplemented()` bucket.
- Review-28 MINOR closed: 242 now requires `block.size() >= 13` before the draw loop
  (decoder-unreachable short blocks clear the mask instead of UB — commented as a
  deliberate deviation). NIT closed: the 242 test asserts all four points and
  colour/point preservation on the zero path.
- Verified: `vm_tests` 13/13; `make -C tests/scripts` → "All cases match the oracle."
  (goldens unchanged — 156 never fires in the corpus); `bash tools/check.sh` →
  ALL GATES PASSED (smdh SKIP, no .cia); `make -C mdk_3ds` → `mdk.3dsx` links.
  Brace-matched recount: C# 238 / C++ 178 / remaining 61 (true dispatches 177).
- Known pre-existing gap (out of scope, noted): C++ `spawn()` omits the C#'s
  `Stats.CountEnemy` call — affects all spawn opcodes equally, goldens don't observe it.
- Next: one opcode family per batch through the harness; 132/136 still need the
  Effects slice; 85 waits on the FrameBlend `rotation()` slice.

## Iteration 30 — review-29 findings closed: 156 test hardening + 242 comment scope (no new opcode)

- Review-29 (VERDICT: INCOMPLETE, no BLOCKER/MAJOR) left two MINOR test-strength
  gaps on the iteration-29 spawn slice, both closed here; coverage unchanged
  (C++ 178/238, remaining 61).
- Gap 1 (rotation/scale unpinned): the 156 test ran at yaw 0 / scale 1, so a
  sign error in `reference_point`'s RotZ or a missing `* scale` passed. Added a
  third spawn: parent yaw 90, scale 2, ref point (3,1,0.5) → hand-computed
  (8,26,6), asserted with 1e-3 tolerance (cosf(pi/2) ≈ -4.4e-8, not 0 —
  verified with libm before writing the test). Each wrong shape lands
  elsewhere (no scale: (9,23,5.5); no rotation: (16,22,6); transposed sign:
  (12,14,6)). Also pins `previous_position == position` and nonzero `restart`.
- Gap 2 (`restart` plumbing unpinned): the test never asserted the `code32`
  script operand reaches the object. Now asserts `created.restart != 0` AND
  that it points at the trailing kEnd byte of the rebuilt program
  (`readOffset` adds FileBase back, so the stored `label-4` decodes to the
  exact label offset — a wrong operand index would leave restart 0 and fail).
- Open question closed: the Beta dialect has no 242 mapping
  (`scriptBetaOpcodeInfo` returns null → `decodeBeta` rejects the instruction),
  so the "decoder-unreachable short block" hardening holds on both dialects;
  the `script_vm.cpp` comment now says so. No behavior change.
- Verified: `vm_tests` 13/13 PASS, zero FAIL; `make -C tests/scripts` →
  "All cases match the oracle." (goldens unchanged); `bash tools/check.sh` →
  ALL GATES PASSED (smdh SKIP, no .cia); `make -C mdk_3ds` → `mdk.3dsx` built.
- Next: one opcode family per batch through the harness; 132/136 still need the
  Effects slice; 85 waits on the FrameBlend `rotation()` slice.

## Iteration 31 — review-30 findings closed + opcode 81 push_hit_dir (179/238, remaining 60)

- Review-30 (VERDICT: INCOMPLETE, one MINOR + one NIT, no code bug) both closed
  in `vm_tests.cpp`: each 156 spawn now places a separator `set_var` between
  the spawn and its label and asserts `restart == labelOffset` exactly, so a
  fall-through (`ins.next`) regression fails (next points at the separator, not
  the label); the range guard is a single `restartInRange` boolean, no
  out-of-bounds read on a failed check. Applied to all three spawns
  (spawned/landed/go).
- Opcode 81 (`push_hit_dir`, ScriptVm.cs:601-607): `value(o[1])` (literal or
  variable) times `Dt` in mode 1 along `hit_direction`, x/y only, z untouched.
  Operands `[U8, Value]` confirmed against the opcode table (offset 123).
  Port sits after case 80 with a `see push_forward` R6 pin (cosf/sinf match the
  C# SinCos bit-for-bit per the probed exemption). New `vm_tests` test pins
  literal mode-0 (exact), literal mode-1 (30*Dt within 1e-6 of 1), and variable
  speed at heading 90 (+y within 1e-4). No C# counterpart exists to twin.
  81 never fires in levels 3-8, so goldens are unchanged by construction.
- Recount-verified: C# 238 / C++ 179 / remaining 60 (true dispatches 178; the
  179th label is the nested `case 0:` inside 249).
- Verified: `vm_tests` 14/14 PASS; `make -C tests/scripts` → "All cases match
  the oracle."; `bash tools/check.sh` → ALL GATES PASSED (smdh SKIP, no .cia);
  `make -C mdk_3ds` (DEVKITPRO/DEVKITARM from /opt/devkitpro) → `mdk.3dsx`.
- Next: one opcode family per batch through the harness; 132/136 still need the
  Effects slice; 85 waits on the FrameBlend `rotation()` slice.

## Iteration 32 — opcode 206 spawn_along_path (180/238, remaining 59) + review-31 doc fix

- Review-31 (VERDICT: INCOMPLETE, one MINOR + one NIT, no code bug) addressed:
  OPCODE_COVERAGE no longer claims 81's inputs are "all already ported" — only
  the `hit_direction` FIELD is ported; its writers (ChainGunHit, the mortar
  path, Twister, sniper rounds, TouchDamage) have no C++ counterpart yet, so in
  integrated play 81 always pushes due east until those slices land, and each of
  those slices' acceptance checks must re-verify 81 end-to-end. The R6 shorthand
  pin stays (consistent with cases 80/201/126; the gate-grammar question is
  flagged for the owner, not unilaterally changed).
- Opcode 206 (`spawn_along_path`, ScriptVm.cs:440-448): from time 0 to before
  the path's last key, one flagged spawn per step at `pathPosition(path, time)`,
  yaw 0, instance -1. Operands `[Data32, F32, Pstr, Code32]` confirmed against
  the opcode table (offset 425). The float accumulation is oracle-exact by
  design (an integer-count rewrite would shift spawn positions by ulps); a zero
  step hangs the oracle identically, so this matches it. New `vm_tests` test on
  LEVEL7.CMI path 53729 (3 keys, frames [0,5,10], step 5 → times 0 and 5):
  count, exact positions, flagged flags, instance chain, restart operand, clean
  unimplemented bucket. Mutation check: 15 checks fail with the port reverted,
  green with it applied.
- Recount-verified: C# 238 / C++ 180 / remaining 59 (true dispatches 179; the
  180th label is the nested `case 0:` inside 249); the remaining set matches the
  doc list exactly.
- Verified: `vm_tests` 15/15 PASS; `make -C tests/scripts` → "All cases match
  the oracle." (goldens unchanged — 206 never fires in the corpus);
  `bash tools/check.sh` → ALL GATES PASSED (smdh SKIP, no .cia);
  `make -C mdk_3ds` → `mdk.3dsx`.
- Next: one opcode family per batch through the harness; 132/136 still need the
  Effects slice; 85 waits on the FrameBlend `rotation()` slice.

## Iteration 33 — review-32 hardening batch (180/238, remaining 59, gates green)

- Review-32 (VERDICT: INCOMPLETE, 1 MAJOR + 3 MINOR + 1 inherited, no code
  bug in the 206 port) addressed as one hardening batch (no new opcode):
- MAJOR (float-accumulation pin gap): `testSpawnAlongPathSpawnsEveryStep`
  gains a second run with non-exact step 0.1f to end 10 — 100 spawns, each
  position bit-compared against `pathPosition` at the accumulated time, plus
  a `differ > 0` guard proving the case discriminates accumulation from
  `i*step`. Independent oracle corroboration: a throwaway .NET 10 program
  (outside the repo, `/tmp/kilo/acc`, SDK at `/tmp/kilo/dotnet10`) running
  the exact C# loop (`for (var time = 0f; time < end; time += step)`,
  ScriptVm.cs:445) prints `count=100 last=9.900002 firstDiff=7
  acc7=3F333334 mul7=3F333333` — the count and the first-divergence hex
  pinned in the test comment come from the oracle toolchain, not the port.
- MINOR (absolute positions): the exact-step case now asserts literal
  `x/y/z == 0` for t=0 and `z == 6.0f` for t=5, so a Hermite regression
  moving both sides together fails here instead of passing silently.
- MINOR (T2 pin grammar): NOT added as a pin — proven by direct test that
  `EXEMPT_RE` in `tools/runner/gates_vfp.py` accepts R-rules only, so a
  `/* @vfp-exempt T2 ... */` pin would trip a MUST "invalid @vfp-exempt
  grammar" finding and break `tools/check.sh`. The 206 loop keeps a prose
  rationale that names T2 and records the owner-grammar decision, consistent
  with the review-31 precedent. The gate author (owner) owns the grammar
  extension; the port does not change gates unilaterally.
- MINOR (doc): `docs/OPCODE_COVERAGE.md` movement/AI grouping no longer lists
  206 as remaining (it landed in iter 32; the remaining-59 set already
  excluded it).
- Inherited (degenerate path): recorded in CONTINUATION open item 3 —
  `pathKeyCount`/`pathKeyFrame` return 0 on invalid paths where the C#
  throws (span reads); 206 inherits the divergence, unreachable in shipped
  scripts (206 never fires in the corpus).
- Verified: `vm_tests` 15/15 PASS ("All VM unit tests match the oracle.",
  incl. hardened 206 test); `make -C tests/scripts` exit 0, 376 PASS lines,
  0 FAIL, "All cases match the oracle." (goldens unchanged); `bash
  tools/check.sh` → ALL GATES PASSED (smdh SKIP, no .cia); `make -C mdk_3ds`
  → `mdk.3dsx` (118 MB, rebuilt 2026-10-26).
- Next: one opcode family per batch through the harness; 132/136 still need
  the Effects slice; 85 waits on the FrameBlend `rotation()` slice.

## Iteration 35 — stats slice: opcode 217 via new GameStats (181/238, remaining 58, gates green)

- Ported 217 (`global_573c4c_add`) with a new header-only
  `mdk_core/src/scripts/game_stats.h`: a 1:1 port of
  `src/Mdk.Game/Scripts/GameStats.cs` — the exact 33-entry enemy table
  (order-verified against GameStats.cs:14-17), `isEnemyType`
  (`ToUpperInvariant` → `toUpperAscii`, ASCII-identical on these names),
  `countEnemy` (Created/Killed split), `percent` (truncating shape,
  0 without shots). Header-only on purpose: no object file, so no
  `notdir` collision in the 3DS build (same reason as
  `formats/ascii_case.h`). `GameStats.IndexOf` deliberately NOT ported —
  it is `GameState` order (save-game/Viewer slice), nothing ported reads it.
- `ScriptRuntime` gains a `stats` member (zero-initialised, which is what a
  fresh level has); `case 217` writes `headShots` only when the first
  operand is 1, exactly `ScriptVm.cs:1606-1611`. Other writers (chain-gun
  fire, sniper rounds, kill path, town flags) land with their own slices.
- Pinned by `HeadShotsAddOnlyWhenEnabled` in `vm_tests` (no C#
  `ScriptVmTests` counterpart exists — the test pins the oracle directly:
  +3/+2 accumulate, first-operand 0 counts nothing, plus `isEnemyType`
  (`xb1` yes / `KURT` no), `countEnemy` created/killed separation, and
  `percent` truncation incl. 0/0). Goldens unchanged — the counters are
  not dumped (dump-field template does not apply).
- Mid-iteration gate fix (no gate relaxed): the R11 screen flagged
  `hits * 100 / shots` via file-level loop inference from `isEnemyType`'s
  table scan; pinned `/* @vfp-exempt R11 ... */` with a level-end-only
  rationale (Score-o-matic math runs at most once per level, never per
  frame; float math would change the truncation) — same surface as
  `renderer_c3d.cpp:1130`.
- Recount-verified: C# 238 / C++ 181 / remaining 58 (true dispatches 180;
  the 181st label is the nested `case 0:` in 249); remaining set matches
  the doc list exactly (217 removed).
- Iteration mechanics: the iter-35 worker was killed by subscription quota
  exhaustion (API 429) AFTER the two code commits landed but BEFORE the
  state commit — iter 36 inherited a dirty tree (`docs/CONTINUATION.md`
  only), re-verified all three gates on the final tree itself, appended
  this narrative, and committed the state. No code changed in iter 36.
- Verified (iter 36, on the final tree): `vm_tests` 16/16 PASS ("All VM
  unit tests match the oracle.", incl. the new 217 test); `make -C
  tests/scripts` (after `clean`) exit 0, "All cases match the oracle."
  (goldens unchanged); `bash tools/check.sh` → ALL GATES PASSED (smdh
  SKIP, no .cia); `make -C mdk_3ds` (with
  `DEVKITARM/DEVKITPRO=/opt/devkitpro/...`) → `mdk.3dsx`.
- Next: one opcode family per batch through the harness; 132/136 still need
  the Effects slice; 85 waits on the FrameBlend `rotation()` slice.

## Iteration 37 — vision-condition slice: opcode 243 if_point_sees_player (182/238, remaining 57)
- `case 243` in `ScriptVm::execute` (script_vm.cpp): `reference_point(ival(o[0]))`
  within `fval(o[1])` of `kurtPosition()` (feet) AND `raycastClear(point, chest)`
  with chest = feet + 4z — exactly `ScriptVm.cs:1703-1708`. The *u16* range rides
  an int operand on both sides (ScriptDecoder.cs:217), so fval's int arm is the
  C# F int arm, exact. Single `&&` expression keeps the C# short-circuit (NaN
  distance is false, never a raycast). sqrtf matches the established bit-for-bit
  glibc pattern; 243 never fires in the corpus (cold), no pin needed.
- Pinned by `testPointSeesPlayerNeedsRangeAndAClearLine` in `vm_tests` (no C#
  `ScriptVmTests` counterpart — pins the oracle directly): at-feet/range-1000
  sees (feet->chest clear at the L7 spawn), 5000-east/range-100 misses (distance
  gate, geometry-free), ref-0-5000-east with body at feet misses (pins o[0] as
  ref index, geometry-free), 100-below/range-1000 misses (segment crosses the
  ground — pins the raycast-false path). New `Code::u16` helper (little-endian,
  matching binU16). `OPCODE_COVERAGE.md` recounted: C# 238 / C++ 182 / 57
  (true dispatches 181); 243 removed from the camera/cutscene group (it never
  needed the camera — only reference_point/kurtPosition/raycastClear).
- Review-36 (stats slice) was VERDICT COMPLETE: MINOR house-wide int-overflow
  note (no action — unreachable magnitudes), NIT negative-add coverage gap
  (deferred to next touch of that test). Nothing to address.
- Verified on the final tree: `vm_tests` 17/17 PASS ("All VM unit tests match
  the oracle."); `make -C tests/scripts` exit 0, "All cases match the oracle."
  (goldens unchanged); `bash tools/check.sh` → ALL GATES PASSED (smdh SKIP,
  no .cia); `make -C mdk_3ds` → `mdk.elf` + `mdk.3dsx`.
- Next: one opcode family per batch through the harness; 132/136 still need
  the Effects slice; 85 waits on the FrameBlend `rotation()` slice.

## Iteration 38 — value-slot slice: opcodes 199 set_104 and 210 set_2c0 (184/238, remaining 55)
- Inherited a half-batched tree: `vm_tests.cpp` already carried
  `testUnidentifiedValueSlotsStoreTheirOperand` (uncommitted), but neither
  dispatch case nor the main() registration existed. Finished exactly that:
  `case 210` / `case 199` in `ScriptVm::execute` (script_vm.cpp) —
  `obj->value_2c0 = value(obj, op(ins, 0))` / `obj->value_104 = ...`,
  exactly `ScriptVm.cs:1663-1668` (`obj.Value2c0/Value104 = Value(obj, o[0])`).
  The C# itself labels both "not identified": the write IS the oracle
  behaviour, both fields were already on `MdkObject` (object.h:136-137), and
  no C++ code reads them, so there is nothing to diverge — same shape as the
  ported 205 (`set_21f`) / 218 (`set_f13c`). No new pin needed (no float→int,
  no loop, no transcendental; plain stores).
- Test (twin of nothing — no C# `ScriptVmTests` counterpart, pins the oracle
  directly): literal operand lands bit-for-bit in its own slot (7.5f /
  -2.25f), variable operand resolves through the shared `Value()` helper
  (own var 0 feeds both slots), and the `unimplemented()` bucket stays clean
  for both opcodes. Registered in main() after the 243 test.
- Review-37 was an environment failure (reviewer quota exhausted, 429), no
  findings to address.
- `OPCODE_COVERAGE.md` recounted with the brace-match script: C# 238 / C++
  184 / remaining 55 (true dispatches 183; the 184th label is still the
  nested `case 0:` in 249). 199/210 removed from the remaining list and from
  the "Not identified (no safe port exists)" group — the group note now
  records that porting the write is exact and only a future *reader* for the
  slots would need reclassification.
- Verified on the final tree: `vm_tests` 18/18 PASS ("All VM unit tests match
  the oracle."); `make -C tests/scripts` exit 0, "All cases match the oracle."
  (goldens unchanged — neither opcode fires in levels 3-8); `bash
  tools/check.sh` → ALL GATES PASSED (smdh SKIP, no .cia); `make -C mdk_3ds`
  (DEVKITPRO/DEVKITARM=/opt/devkitpro/...) → `mdk.elf` + `mdk.3dsx`.
- Tooling note: the `commit` tool silently produced no commit twice (HEAD
  unchanged after "commit attempt finished"); landed the batch with a direct
  `git add -A && git commit` (cc09097).
- Next: one opcode family per batch through the harness; 132/136 still need
  the Effects slice; 85 waits on the FrameBlend `rotation()` slice.


## Iteration 39 — opcode 239 `set_27c` explicit no-op

- Ported opcode 239 in `mdk_core/src/scripts/script_vm.cpp` as an explicit
  no-op, matching `src/Mdk.Game/Scripts/ScriptVm.cs:1783`. The decoder's
  generated layout already consumes six F32 operands; no behavior was inferred
  from the unidentified field name. The `vm_tests` synthetic program gives it
  six floats, then writes a second object variable. It verifies the no-op,
  clean `unimplemented()` bucket, and correct instruction alignment by checking
  that the following write executes.
- C++ dispatch coverage recalculated against brace-matched method spans:
  C# has 238 unique cases; C++ has 189 numeric labels, 188 unique labels, of
  which 187 are dispatches (the nested `case 0` in opcode 249 isn't an opcode).
  Thus 51 remain. Added 239 to the completed list and adjusted the deferred
  dispatch count; this is a one-opcode batch, no golden changes.
- Review-37 was unavailable (reviewer quota 429); no newer adversarial review
  was present. Self-review confirmed the C# no-op and decoder's six-F32 record.
- Verification on this tree:
  - `make -C tests/scripts` → VM tests 20/20 PASS; full suite ended with
    `All cases match the oracle.`
  - `bash tools/check.sh` → `CHECK: ALL GATES PASSED`; SMDH check SKIPPED
    (no CIA built).
  - Bare `make -C mdk_3ds` failed because `DEVKITARM` was unset. Retried as
    `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C
    mdk_3ds` → linked `mdk.elf`, staged 69 RomFS files (79M), built
    `mdk.3dsx`.
- No Original 3DS or emulator run was performed; this opcode has no rendered
  output and hardware performance remains unmeasured.

## What this batch did (iteration 42 — opcode 85 oracle-rounding review fixes)

- Addressed the external review's two MAJOR findings: `rotation()` now uses the .NET `float.DegreesToRadians` evaluation order (multiply by pi, then divide by 180), and matrix composition uses System.Numerics' first product followed by three fused multiply-adds in `Vector4.Transform` order.
- Replaced the loose orientation tolerance assertions with a full 16-element exact-bit fixture for roll=30, pitch=20, yaw=40, generated by the .NET 10 C# oracle. Removed stale comments claiming opcode 85 and `rotation()` were deferred.
- Verification: `make -C tests/scripts clean && make -C tests/scripts` → all VM cases, goldens, and supporting checks PASS, ending `All cases match the oracle.`; `bash tools/check.sh` → `CHECK: ALL GATES PASSED` (SMDH SKIPPED: no `.cia`); `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds` → `mdk.elf` linked, 69 RomFS files staged, `mdk.3dsx` built. `git diff --check` clean.
- Toolchain check: devkitARM GCC 15.2.0; ARM object disassembly references `fmaf` for the matrix FMA sequence. This does not establish hardware behavior or performance: no Original 3DS or emulator run was performed; Old-3DS readiness remains unmeasured.

## What this batch did (iteration 41 — opcode 85 `set_rolling`)

- Added `MdkObject::rotation()` using row-vector `Mat4` composition matching the C# `RotationOf(roll, -pitch, yaw)` order, plus `set_rolling(bool)` preserving the transition semantics: enabling captures only on off→on; disabling clears the flag without changing the saved basis.
- Added opcode 85 dispatch and a synthetic VM test for a 90° yaw basis, no recapture while already rolling, basis preservation on stop, and recapture on the next start. The oracle case is `MdkObject.SetRolling` / `Rotation` (`src/Mdk.Game/Objects/MdkObject.cs:279-285,327-341`); no level golden changes because opcode 85 is unreachable in the level 3-8 corpus.
- Raw trig in the new gameplay orientation path is R6-pinned because using the approximate LUT would perturb the saved gameplay basis. The first `tools/check.sh` run caught the missing pin; the pin was added and the complete gate rerun passed.
- Updated opcode counts: 189 true C++ dispatches / 238, 49 remaining; opcode coverage now includes a direct VM test for 85.
- Verification after the final code/doc edits: `make -C tests/scripts` → VM tests PASS including `SetRollingCapturesRotationOnTransition`; all table/script, DTI, scene checks PASS and `All cases match the oracle.`; `bash tools/check.sh` → `CHECK: ALL GATES PASSED` (SMDH gate skipped: no built `.cia`); `DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM make -C mdk_3ds` → linked `mdk.elf`, staged 69 RomFS files and built `mdk.3dsx`; `git diff --check` clean. No OG hardware capture was performed. Latest external review remains unavailable: review-37 records quota exhaustion; this iteration self-reviewed the C# matrix order and transition behavior.
