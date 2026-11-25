#!/bin/sh
# main_3ds_flow_test.sh — the 3DS frontend's collision-world invariants, gated.
#
# WHY A SOURCE CHECK EXISTS. `main_3ds.cpp` includes <3ds.h> and cannot be
# compiled on a host, so nothing else in `make -C tests/scripts` can reach what
# its frame loop DOES. `pica_matrix` and `input_3ds` were split out of that tree
# because their content was arithmetic; this is not arithmetic, it is WHERE a call
# sits, which is the same reason c3d_frame_test.sh exists.
#
# THE DEFECT THIS PINS (external review 2026-11-05, run-20261105-142911 F1, High).
# The frontend pinned Kurt's solid arena ONCE, in `spawnPlayer`, and never again.
# `ArenaSpace::setSolid` is STICKY: while `_solid` is non-empty, `move()` uses only
# it (arena_space.cpp:118) and `kurts()` returns only it (:174-181), so the
# arenas-around-the-feet fallback is bypassed for the rest of the session. The C#
# refreshes it every step (`_space.SetSolid(_scripts.SolidArenas)`, Viewer.cs:533)
# and the Linux frontend refreshes it every step too (mdk_linux/src/main.cpp:530).
# A pin set once at the spawn therefore leaves Kurt colliding with the arena he was
# born in after he walks into its neighbour: the walk probes find no floor there,
# he drops through that neighbour's floor, and `bottom()` — which DOES consider any
# arena holding his feet (:160) — reports that neighbour's bottom and fires the
# fall-out death. Walking into the next room kills and respawns him.
#
# WHAT IS PINNED, and why each one is its own check:
#   1. a `setSolid` call inside the fixed-step loop body — presence anywhere in
#      the file is not enough, that is the defect;
#   2. that call AFTER `follow.update`, because the C# evaluates SetSolid after
#      UpdateCamera and the camera's clearance can shove Kurt into another arena;
#   3. the spawn pin still present, because the loop cannot supply it: a Kurt
#      standing outside every arena gets a null `arenaAt`, and with no pin at all
#      `bottom()` resolves nothing and the fall-out death never fires;
#   4. that the in-loop refresh is GUARDED on the lookup, not unconditional: an
#      unguarded `setSolid("")` when Kurt is outside every arena CLEARS the pin,
#      which is check 3's failure reached by the opposite edit.
#
# WHAT BATCH 99 ADDED (the ScriptRuntime on the console, priority item 6d). The
# frontend now owns a script runtime, which means four more things became load-
# bearing that no compiler can see:
#   6. the step loop's pin reads the runtime's DRAWN LIST and never its single
#      current arena. The oracle hands SetSolid the scripts' drawn arenas -- current
#      plus the active second (ScriptRuntime.cs:265-282, Viewer.cs:533) -- and move()
#      sweeps only the pinned arenas, so one name drops the room behind the open door
#      Kurt came through. That is the class review-6 F1 found when batch 99 pinned a
#      currentArena_ that was, at that point, still frozen at the spawn and a door walk
#      fell through the room it opened into. Batch 134 ported the crossing and batch
#      137 ported the list (ScriptRuntime::solidArenas, and ArenaSpace::setSolid now
#      takes the NAMES the C# passes), so the check requires the list instead of
#      forbidding the single name: with a runtime the loop must call
#      solidArenas(), without one the geometric lookup, exactly one refresh per branch.
#   7. the scripts tick inside the fixed-step loop between Kurt and the camera
#      (Viewer.cs:505-535), because the runtime's own 1/30 s clock assumes it is
#      called once per 1/60 s step;
#   8. the frame mixes the pointer that follows the runtime and not the standalone
#      mixer — ScriptRuntime OWNS its mixer and every script sound is written into
#      that one (script_vm.cpp:1258 opcode 89 -> ScriptRuntime::playSound), so a
#      frontend that keeps mixing its own gets a silent level with a healthy ledger;
#   9. a respawn re-points that pointer, rebinds Kurt, and re-attaches the sink, so
#      a rebuilt runtime is not a silent level after the first death;
#  10. the runtime is constructed under the asset gate, because a runtime missing
#      its CMI, bank or sprites archive is a different random stream and not a
#      degraded oracle run;
#  11. the camera's rest pitch comes from the runtime when one exists (camera_track
#      first, then the scripts' arena), with the geometric arena kept only as the
#      no-runtime stand-in.
# Check 4 was widened from "the first setSolid site" to "every setSolid site" for
# the same reason: the loop now has two, and a check that stopped at the first
# would let an unguarded second one clear the pin while the gate printed PASS.
#
# WHAT BATCH 102 ADDED (the object pass on the console). Drawing the runtime's
# objects made four more things load-bearing that no compiler can see:
#  12. the pass sits BETWEEN the arena and Kurt's sprite. The C#'s draw order is the
#      picture (Viewer.cs:702-708); a pass that runs somewhere else in the frame
#      still runs, still costs the same, and still produces the wrong frame.
#  13. the pass is gated on the runtime existing. A level whose CMI, bank or sprites
#      archive is missing runs without one (check 10), so `runtime` is null in a
#      shipped configuration and `runtime->objects()` there is a crash in the frame
#      path — not a degraded picture.
#  14. the pass skips both the invisible AND the removed. The C#'s object list has
#      already dropped its removed objects; this port keeps them in its deque for
#      pointer stability, so walking the deque without the second skip draws what
#      the oracle has unlisted.
#  15. a respawn drops the two caches keyed by a Model address. ScriptRuntime OWNS
#     its model cache and holds the models BY VALUE (script_runtime.h:580), so
#     `runtime.reset()` frees every Model* that ObjectView::layouts_ and the shared
#     pose caches are keyed by, and the rebuilt runtime's models land on recycled
#     addresses — same level, same parse order, so the address very likely reappears
#     holding a DIFFERENT model and inherits its geometry and pose. The C# cannot
#     have this: a respawn is a fresh Viewer, whose constructor calls
#     MdkObject.ForgetPoses() (Viewer.cs:185) and builds a fresh ObjectView. This
#     check is also why the object-pass declarations sit ABOVE spawnPlayer: moving
#     them down beside the draw is a one-line edit that would silently undo the fix,
#     and it is the kind of "tidy" move a later batch makes by reflex.
#
# WHAT BATCH 105 ADDED (review-11 F1 and F7, the refused-mesh log line). The line
# that review-10 F3 added to make a refused `linearAlloc` visible could itself
# have been wrong in the same direction:
#  17. it must name the capacity the backend was REFUSED. `ObjectDraw::ensure`
#      rounds the frame's corner count up to a power of two, and it is that rounded
#      capacity `mdk_render_create_dynamic_mesh` was handed, so printing the frame's
#      480 corners reported a 9 KB refusal for an 80 KB one and disagreed with
#      renderer_c3d.cpp's own `linearAlloc(N) FAILED` line by up to 8x. A reader
#      budgeting linear memory from the guest log got a number for an allocation
#      that was never attempted.
#  18. its edge trigger must clear when a later attempt succeeds. A latch that only
#      latches prints the FIRST refusal of the run and is silent about a bigger one
#      after a recovery, which is the same invisibility review-10 F3 was raised for.
#
# WHAT BATCH 106 ADDED (review-12 R1 and R2, both about these two checks being
# weaker than the properties they name):
#  17 was on "does the refused capacity appear ANYWHERE in the printf's tail", so
#     substituting the frame's corner count for the KB multiplicand kept the first
#     figure honest and the byte figure 8x wrong and stayed green. The check now
#     parses the call's arguments and requires every derived figure to come from the
#     same source as the first one -- which is the defect: two committed numbers for
#     one refusal that disagree.
#  18 was on "does some line equal `streamFailureLogged = false;`", which pins a
#     LINE and not the GUARD: widening `if (!streamFailureLogged)` to `if (true)`
#     leaves the assignment in place and the refusal prints 60 times a second, and
#     the gate printed PASS (review-12 R1). It is now a brace-scan of the branch --
#     the clear must sit in the `else` sibling of the `allocationFailed()` test, so
#     it runs on every frame that did not refuse -- and the guard gets its own check
#     19 with its own isolating arm, because "clears when it recovers" and "prints
#     once per refusal" are two properties and one line cannot pin both.
#     (vault: 30-architecture/patterns/gate-the-failing-branch-not-the-enclosing-block
#     and 50-tooling/a-probe-must-be-able-to-fail.)
#
# WHAT BATCH 107 ADDED (review-13 D1-D4: the same three checks, a third round, each
# still weaker than the property it names -- the recurring shape is a check that reads
# ONE thing where the property is about ALL of them, or that reads a NAME where the
# property is about an ARITHMETIC or a POSITION):
#  17 read ONE printf. It resolved the first `log3dsPrintf(` after the test, so a
#     second line added to the same refusing branch was read by no check at all, and a
#     second line built from `objectCorners` put review-11 F1's 9-KB-for-an-80-KB
#     disagreement back on the console with the gate green (D1). It now reads EVERY
#     printf in the branch. And it compared the derived argument's IDENTIFIER SET to
#     {src}, which says nothing about what those identifiers are multiplied by: `* 4`
#     for the corner stride stayed green while the printed byte count disagreed with
#     the backend's (D4). It now reduces the argument to its factor set and requires it
#     to BE the factor set parsed out of `renderer_c3d.cpp`'s own
#     `linearAlloc((size_t)cap * 5 * sizeof(float))` -- one source of truth instead of
#     a rule about operands -- with the `/1024` required exactly when the line names KB.
#  18 accepted its equivalent assignment form by searching the WHOLE FILE (D2), so the
#     assignment placed inside the refusing branch -- where it sets the latch and can
#     never clear it, review-11 F7 exactly -- satisfied the check from inside the very
#     branch the clear has to live outside of. The form is now anchored: it must sit in
#     the block that contains the test, BEFORE the test, because it is only the same
#     state machine when it runs on every frame.
#  19 substring-tested the guard's condition (D3), so `if (!streamFailureLogged ||
#     verbose)` passed while printing on every frame of the refusal. The condition must
#     now BE the negative latch test (`fullmatch`), and EVERY refusal line must sit
#     inside such a block -- the half of D1 that check 17 could not see.
# Each of the four was reproduced on a mutant before it was fixed, and each mutant is
# now a named arm.
#
# WHAT BATCH 108 ADDED (review-14: the same family a fourth round. The recurring shape
# this time was a check that pins HALF a state machine, or that reads a POSITION where
# the property is REACHABILITY):
#  F1: checks 17/18/19 pinned the guard and the clear and NOTHING ELSE -- the arming
#     statement `streamFailureLogged = true;` was named by no check, no arm and no
#     probe in the repository, so deleting it left the guard always true, printed the
#     refusal on every frame of one sustained refusal (~60/s, the class review-10 F3
#     and review-12 R1 were raised for) and the suite still printed `PASS Main3dsFlow
#     (19 checks, 27 arms, 9 probes)`. Check 19 now requires the guarded block to ARM
#     the latch as a direct statement of itself. Its ORDER inside the block is
#     deliberately not pinned: arm-then-print and print-then-arm both print once, and
#     requiring one would redden a shape the port may write (review-10 F6), so there
#     is a probe for the other order.
#  F2: check 18's accepted assignment form was anchored to a POSITION ("in the
#     enclosing block, before the test") where the property is that it RUNS ON EVERY
#     FRAME. A copy nested in an inner condition (`if (objectCorners == 0) { ... }`,
#     `if (capacity != last) { ... }`) satisfied the window search, never cleared on a
#     frame that did not refuse, and silenced the second, larger refusal after a
#     recovery -- review-11 F7 with checks 18 AND 19 green and only arm bookkeeping
#     reddening, which reads as arm maintenance rather than as a defect. Both accepted
#     forms are now depth-tested: brace depth 1 from the block's own '{' is what
#     "direct statement" means and it is decidable in text.
#  F6: check 17 compared factor sets literally, so naming the stride -- the refactor
#     that makes drift IMPOSSIBLE, which is what D4 was for -- reddened it and four
#     arms. One level of local `const`/`constexpr` integer definitions now folds on
#     BOTH sides before the comparison, with a probe for the named-stride shape.
#  F7: D1's "every refusal line" was scoped to the body of the FIRST
#     `if (objectDraw.allocationFailed())`. A refusal line in a SIBLING condition
#     outside that body was read by neither check 17 nor check 19. The scope is now the
#     object pass, and family membership is decided by the line's own format string
#     (it names the refusal), which is also what keeps the capacity-change line -- a
#     SUCCESS report -- out of the family. A refusal line whose format is a variable is
#     outside the net; that is stated, not hidden.
#  F3: three arms did not compile, which this file's own standard calls a mutant of
#     nothing, and the header claimed they all did. `arm_objects_ungated` spliced one
#     character early and re-emitted the block's own `{` (`if (true) {{` -- unbalanced);
#     `arm_objects_before_arena` anchored on `text.find("if (arena) {")`, which is the
#     FIRST of three such blocks and sits above the declaration of `runtime`, so the
#     mutant named identifiers that do not exist there; `arm_before_camera` moved the
#     refresh above the declaration of its own guard. All three now build real mutants,
#     so the claim below is true rather than scoped away.
#
# WHAT REVIEW-15 (run-20261108-195620) FOUND AND THIS BATCH FIXED, each reproduced on
# the reviewer's own mutant before any edit:
#  F1: checks 18 and 19 read the clear and the guard, and neither read the INVERSE, so
#     a second `streamFailureLogged = false;` -- at the end of the refusing branch, or
#     as a plain statement after the if/else -- cleared the latch on the frame that
#     refused and the line printed ~60x/s with 17, 18 and 19 all green. Check 20 now
#     reads the whole write set: every write to the latch inside the pass must be the
#     arming inside a latch guard, the clear in the `else`, or the anchored assignment
#     form. Review-15's open question 1 ("should the family pin the absence of a clear
#     on the refusing path?") is answered YES and pinned, because check 19's failure
#     text already promised it and a defensive per-frame clear is the defect, not a
#     shape the port writes on purpose.
#  F3: batch 108 replaced batch 107's format-agnostic scan of the refusing branch with
#     a format filter over the pass and recorded it as a widening. It was both, and the
#     narrowing was the half that mattered: a line inside the refusing branch whose
#     format never spells FAILED was read by neither check. The two nets are now a
#     UNION (in the branch, whatever it says; or names the refusal, wherever it sits),
#     `arm_failure_second_line_names_the_frame_count` was re-cut with a format that
#     does NOT name the refusal so it tests the branch half instead of co-operating
#     with the filter, and a probe keeps an honest non-FAILED line inside the branch
#     green. Review-15's open question 2 is answered: the narrowing was a defect, not
#     an intent, and the record no longer calls the change a pure widening.
#  F4: `backend_mesh_atoms()` returning None was documented to make check 17 RED and
#     instead raised TypeError, killing the script before checks 18-20, every arm and
#     every probe. The absence is now reported as a red check. Verified with the
#     reviewer's skeleton tree (the script copied into a tree with no renderer_c3d.cpp
#     and the real `main_3ds.cpp` handed to it): check 17 prints its own FAIL line and
#     the arms run.
#  F5: the compile claim below had no generator behind it. It has one now --
#     `tests/scripts/arm_compile_check.sh`, which dumps the mutants from THESE lists
#     and compiles them with the flags read out of `mdk_3ds/Makefile`. Running it is
#     how the two backend arms review-15 F5's harvest found (`flushDraws`, a name that
#     exists only in a comment, and a two-argument `shaderProgramConfigure` where
#     libctru declares three) were caught and fixed.
#  Every arm and every probe compiles for the 3DS target with the Makefile's own flags.
#  The claim is re-runnable, not transcribed: `sh tests/scripts/arm_compile_check.sh`
#  dumps EVERY arm and probe of this file and of `c3d_frame_test.sh` -- it reads the
#  lists out of the two gates rather than restating them -- and syntax-checks each with
#  `arm-none-eabi-g++` and the flags read out of `mdk_3ds/Makefile`. No count is quoted
#  here on purpose: review-15 F5 raised the missing generator and review-16 R3 caught
#  the transcribed counts going stale in the same file that named the command, so the
#  command is the record and the tool prints the number. An arm that cannot compile is a
#  mutant of nothing, and a claim nobody can re-run is a claim that will rot.
#
# WHAT REVIEW-20 (run-20261108-195620) FOUND AND THIS BATCH FIXED, each reproduced on
# the reviewer's own mutant before any edit:
#  F1 (Medium, CONFIRMED): neither half of review-19 F1's fix was pinned. Deleting the
#     whole CORNER_UNIT loop, and separately deleting the whole-argument capacity rule,
#     each left `PASS Main3dsFlow (20 checks, 45 arms, 18 probes)` byte-identical to
#     shipped -- the reviewer ran both, and this batch ran both again. Three arms now
#     isolate the corner rule and one probe isolates the rule that replaced it, and the
#     four gate mutants that were invisible (corner loop out, attribution reverted,
#     literal rule reverted, whole-argument rule put back) each lose exactly the arms
#     that name them.
#  F2 (Medium, CONFIRMED): the whole-argument rule batch 113 restored reddened an
#     honest refusal line that prints ONLY the byte figure the capacity explains --
#     `"... linearAlloc FAILED, %d KB refused", kb` -- which is the shape the backend's
#     own refusal writes. It is deleted, not narrowed: the per-slot rules already require
#     every byte figure to BE the backend's capacity arithmetic and every corner figure
#     to BE the capacity, so the line-level rule could only ever refuse a line that
#     disagreed with nothing [CORRECTED BY BATCH 115 / REVIEW-21 F2: it also refused a
#     line that named no unit word at all and printed a figure that was not the refused
#     request, and deleting it accepted that; the property is restored in the narrowed
#     form], and the cheap way past it was to print a corner figure the line did not
#     need. That is review-11 F1's wrong request bought with the gate's own PASS, and
#     review-10 F6 is the same trade. A probe pins the honest line for good.
#  F4 (Low, CONFIRMED): (a) the corner rule read only the text AFTER its conversion, so
#     `corners %d, attempted %d = %d KB` printed the frame's corner count green while
#     say() claimed every corner slot; a unit word now belongs to the conversion NEAREST
#     it, which also keeps `%d corners at frame %lu` honest. (b) the limit's parenthetical
#     said the opposite of what the rule claimed. (c) `bare_literal` missed a paren, a
#     sign and a decimal point: `(81920)`, `((int64_t)81920)`, `(int64_t)(81920)`,
#     `-81920` and `81920.0f` were green in a slot naming no unit. One fixpoint `unwrap`
#     plus one pattern now covers eleven spellings, three of them new arms.
#  F3 (Medium, REBUTTED): see docs/PROGRESS_LOG.md. The quoted `PASS C3dFrame (31 checks,
#     48 arms adjudicated red, 2 probes green)` over the batch-112 gate IS reachable and
#     was reproduced verbatim; the reviewer's placement of the pre-arm between
#     `r->inFrame = true;` and `r->frame++;` broke an unrelated arm's anchor, which the
#     harness refuses to report on rather than paper over.
#
# WHAT REVIEW-21 (run-20261108-195620) FOUND AND THIS BATCH FIXED, each reproduced on
# the reviewer's own mutant before any edit:
#  F1 (Medium, CONFIRMED): check 17 died with `ValueError: min() iterable argument is
#     empty` on a refusal line whose format NAMES a unit word and carries NO conversion
#     -- `("objects: linearAlloc FAILED, bytes refused", attempted)`, the edit a batch
#     makes when it deletes a conversion and leaves the argument behind. Fail-closed,
#     but it stopped checks 18-20, scored no arm and no probe, and named no property:
#     review-15 F4's symptom again, reached because the review-17 R3 skip read `not vals
#     and not convs` when what makes a line unreadable by the figure rules is that the
#     FORMAT prints no figure, whatever it is handed. The skip now reads `not convs`, one
#     probe pins it, and an arm or probe whose adjudication raises is reported as a named
#     FAIL of its own instead of a traceback.
#  F2 (Medium, CONFIRMED): batch 114's rationale for deleting the whole-argument rule was
#     false as written. The rule did not only ever refuse a line that disagreed with
#     nothing: it also refused a refusal line that names NO unit word and prints a figure
#     that is not the refused request, and batch 114 accepted that with a full PASS, with
#     no arm and no probe recording the loss. The property is restored in the narrowed
#     form that still bites -- a refusal line whose format names no unit at all must print
#     the refused capacity in one of the slots it FILLS (a dangling argument past the last
#     conversion prints nothing and cannot vouch for the line). Every honest line
#     review-20 F2 was about names the unit its figure is in, so the byte-only line stays
#     green and `"objects: refused %d at frame %lu", objectCorners, (unsigned long)frame`
#     is red again. Two arms.
#  F3 (Low, CONFIRMED): `unwrap` was documented as a fixpoint and bounded at six passes,
#     so a figure wrapped in seven parens walked through `bare_literal` green while
#     `cap_name` failed to read a capacity wrapped that deep and reddened an honest line.
#     Each pass only removes characters, so the equality break alone IS a fixpoint; the
#     bound is gone, one arm pins the literal half and one probe the capacity half.
#
# The backend's copy of the same edge trigger (`meshAllocFailureLogged`, the line that
# actually printed 1710 times) is pinned in c3d_frame_test.sh, which is the file whose
# arms can mutate renderer_c3d.cpp; review-14's related observation asked which file
# owned it. This file reads renderer_c3d.cpp only for the stride coupling.
#
# Every check is paired with a mutation arm that must turn it red; a gate that
# prints PASS over a mutant that never ran is a pass generator.
#
# WHAT THIS DOES NOT COVER, stated plainly: it reads the source, so it pins the
# STRUCTURE and not the pixels, and it cannot show that the geometric arena under
# the feet is a good stand-in for the scripts' `SolidArenas` (it is the same
# stand-in the runtime-less Linux frontend uses, and it is narrower than the
# oracle, which also lists the active second arena). Whether any of it is correct
# on a console stays UNVERIFIED (AGENTS.md P6/P8).
#
# usage: sh main_3ds_flow_test.sh [path-to-main_3ds.cpp]
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SRC=${1:-$ROOT/mdk_3ds/src/main_3ds.cpp}
[ -f "$SRC" ] || { printf '  FAIL main_3ds flow: cannot read %s\n' "$SRC"; exit 1; }

python3 - "$SRC" "$ROOT" <<'PY'
import os
import re
import sys

PATH = sys.argv[1]
ROOT = sys.argv[2] if len(sys.argv) > 2 else os.path.dirname(os.path.dirname(PATH))
SRC = open(PATH, errors="replace").read()


def code_lines(text):
    """(index, text) for lines that are code. Comments are dropped: this tree
    documents its own rules in prose, and prose about `setSolid` is not a call
    site (the same trap audit_c_with_classes.py documents for itself)."""
    out = []
    for i, line in enumerate(text.splitlines()):
        s = line.strip()
        if s.startswith("//") or s.startswith("*") or s.startswith("/*"):
            continue
        out.append((i, line))
    return out


def blank_comments(text):
    """`text` with every C++ comment replaced by spaces, byte offsets preserved.

    `code_lines` above drops whole comment LINES, which is not enough: review-10 F6
    reproduced check 16 turning red because a prose line ABOVE the call named it
    (`// the only objectDraw.draw( call site; see Viewer.cs:704`). The scan is
    string-aware, because this frontend contains `romfs:/` and `sdmc:/` literals and
    blanking from a `//` inside one would delete real code. Offsets are preserved so
    a position found in the blanked text still compares against a span resolved in
    the original."""
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c in ('"', "'"):
            quote = c
            i += 1
            while i < n and text[i] != quote:
                i += 2 if text[i] == "\\" else 1
            i += 1
        elif c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                out[i] = " "
                i += 1
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            while i < n and not (text[i] == "*" and i + 1 < n and text[i + 1] == "/"):
                if text[i] != "\n":
                    out[i] = " "
                i += 1
            out[i] = " "
            if i + 1 < n:
                out[i + 1] = " "
            i += 2
        else:
            i += 1
    return "".join(out)


def block_span_from(text, pos):
    """(start, end) offsets of the brace-matched block whose '{' is at or after
    `pos`; `end` is one past the matching '}'."""
    i = text.find("{", pos)
    if i < 0:
        return None
    depth = 0
    for j in range(i, len(text)):
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
            if depth == 0:
                return (i, j + 1)
    return None


def block_span(text, pattern):
    m = re.search(pattern, text)
    if not m:
        return None
    return block_span_from(text, m.end())


def body_span(text, name):
    """Absolute (start, end) of one lambda or function body, from
    `auto name = [&]() {` or `void name(...) {`, or None. The `auto` matters:
    the frontend's spawn block is a lambda, and a pattern that required the
    identifier at the start of the line matched nothing and made its check
    unconditionally red."""
    m = re.search(r"(?m)^[ \t]*(?:auto[ \t]+)?" + re.escape(name) +
                  r"[ \t]*(?:=[ \t]*\[&\]\([ \t]*\)|\([^;{]*\))[ \t]*\{",
                  text)
    if not m:
        return None
    sp = block_span_from(text, m.end() - 1)
    return (m.start(), sp[1]) if sp else None


def step_loop_span(text):
    """Absolute (start, end) of the fixed-step loop body, resolved by brace
    matching on its own header. Absolute so the arms can splice at these
    offsets."""
    sp = block_span(text, r"while[ \t]*\([ \t]*pending[ \t]*>=[ \t]*step[ \t]*\)")
    return sp


def step_loop(text):
    sp = step_loop_span(text)
    return text[sp[0]:sp[1]] if sp else None


def object_pass(text):
    """The object pass's block: the INNERMOST brace-matched `if (...) {` block that
    actually CONTAINS `objectDraw.draw(`.

    Resolved by containment and not by "the nearest `if` before the call", because
    the pass contains a second, inner `if` (the invisible/removed skip) whose header
    sits closer to the call than the gate does -- matching on nearness returned the
    skip's block, whose header is not `if (runtime)` and whose body does not hold
    the call. And it is resolved by position rather than by a pattern over the whole
    file because the frontend has more than one `if (runtime)`: a greedy search
    lands on the camera-pitch one in the step loop.

    Returns {'start','end','header','body'} or None. `header` is the matched `if`
    header text, which is what the gate check inspects: the pass being inside SOME
    block is not the invariant; being inside the runtime's is."""
    at = text.find("objectDraw.draw(")
    if at < 0:
        return None
    best = None
    for m in re.finditer(r"\bif[ \t]*\([^\n]*?\)[ \t]*\{", text[:at]):
        sp = block_span_from(text, m.end() - 1)
        if sp is not None and sp[0] <= at < sp[1]:
            best = (m, sp)
    if best is None:
        return None
    m, sp = best
    return {"start": sp[0], "end": sp[1], "header": m.group(0),
            "body": text[sp[0]:sp[1]]}


def frame_body_span(text):
    """Absolute (start, end) of the `while (aptMainLoop())` body — the frame, as
    distinct from the fixed-step loop inside it."""
    return block_span(text, r"while[ \t]*\([ \t]*aptMainLoop[ \t]*\([ \t]*\)[ \t]*\)")


def call_line(block, name):
    """0-based line index of the first code line of `block` that is a statement
    calling `name(`, or None. Whole-line match, so a call wrapped in `if (x)` on
    the same line is not mistaken for a guarded one."""
    if block is None:
        return None
    for i, line in code_lines(block):
        if re.match(r"[ \t]*" + re.escape(name) + r"\s*\(", line.strip()):
            return i
    return None


def line_depths(block):
    """Brace depth at the START of each line of `block`, relative to the block's
    own '{'. A statement written directly in the block is depth 0; one inside an
    `if` in the block is depth 1. This is what turns "inside the loop" into
    "executed every time the loop runs"."""
    depths = []
    d = -1
    depths.append(d)
    for ch in block:
        if ch == "\n":
            depths.append(d)
        elif ch == "{":
            d += 1
        elif ch == "}":
            d -= 1
    return depths


def call_args(text, open_paren):
    """The top-level argument strings of the call whose '(' sits at `open_paren`,
    format string first, or None if the call is not closed.

    String-aware and paren-aware, because the refused-mesh printf's format string
    contains a comma ("... FAILED, no objects drawn") and its KB argument contains
    casts and a `sizeof(float)`. Splitting on commas cut the format string in half;
    stopping at the first ')' stopped inside the cast. Review-12 R2: a check that
    reads a call's arguments has to actually parse them."""
    depth = 0
    args = []
    cur = []
    i = open_paren
    n = len(text)
    while i < n:
        c = text[i]
        if c in '"\'':
            quote = c
            start = i
            i += 1
            while i < n and text[i] != quote:
                i += 2 if text[i] == "\\" else 1
            i += 1
            if depth >= 1:
                cur.append(text[start:i])
            continue
        if c == "(":
            depth += 1
            if depth > 1:
                cur.append(c)
        elif c == ")":
            depth -= 1
            if depth == 0:
                args.append("".join(cur).strip())
                return args
            cur.append(c)
        elif c == "," and depth == 1:
            args.append("".join(cur).strip())
            cur = []
        elif depth >= 1:
            cur.append(c)
        i += 1
    return None


# The refused-mesh line's byte figure is compared against the renderer's OWN
# `linearAlloc` expression (review-13 D4), so the two sides are reduced to the same
# shape: a sorted list of (operator, atom) pairs with casts and parentheses gone,
# `sizeof(T)` folded into one atom, and the capacity name folded into `CAP`.
CAST_RE = re.compile(r"\([ \t]*(?:u?int\d*_t|size_t|ptrdiff_t|int|long|short|char"
                     r"|unsigned|signed|bool|float|double)[ \t]*\)")
ATOM_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*|\d+|[*/]")


def arith_atoms(expr, cap_names):
    """The multiplicative atoms of `expr`, with every name in `cap_names` folded to
    `CAP` so two files may name the same quantity differently.

    Review-13 D4: the previous rule was `value_names(v) <= {src}`, an identifier
    test, so `attempted * 4 * sizeof(float) / 1024` passed with a stride that
    disagreed with the renderer's own `(size_t)cap * 5 * sizeof(float)`. The
    variables were right and the arithmetic was wrong, which is review-11 F1's
    symptom with the variable substitution already fixed."""
    e = expr.replace("objectDraw.", "")
    e = re.sub(r"sizeof[ \t]*\([ \t]*([A-Za-z_][A-Za-z0-9_]*)[ \t]*\)",
               r"sizeof_\1", e)
    e = CAST_RE.sub(" ", e)
    e = re.sub(r"[()]", " ", e)
    atoms, op = [], "*"
    for tok in ATOM_RE.findall(e):
        if tok in "*/":
            op = tok
            continue
        atoms.append((op, "CAP" if tok in cap_names else tok))
        op = "*"
    return sorted(atoms)


_BACKEND = {"atoms": None, "read": False}


def kb_form(atoms):
    """The backend's byte atoms with the KB unit conversion the log line names."""
    return sorted(atoms + [("/", "1024")])


# The two spellings of "print only while the latch is down". `fullmatch`, so a
# condition that merely MENTIONS the negation (`!streamFailureLogged || verbose`)
# is red: it prints on every frame of the refusal (review-13 D3).
LATCH_NEG = re.compile(r"![ \t]*streamFailureLogged"
                       r"|streamFailureLogged[ \t]*==[ \t]*false")

# A line is a REFUSAL line when its own format string says so: it names the refusal
# the backend names (review-14 F7). The capacity-change line reports a SUCCESS and
# stays out of the refusal family; a line whose format is a variable is out too,
# which is stated as a limit rather than hidden.
REFUSAL_FMT = re.compile(r"FAILED|refus", re.IGNORECASE)

# A format NAMES a figure when it carries a printf conversion (review-17 R3). `%%`
# is a literal percent, not a figure, so it is excluded. This is what separates "a
# refusal line that states no number" -- which cannot disagree with anything -- from
# "a refusal line whose format asks for a number and passes none".
#
# Review-18 F4: the old pattern was `%` + one optional flag + one letter, so `%8d`,
# `%08x`, `%-5d`, `%.1f`, `%5.1f` and `%*d` did not match at all, and the comment's
# claim that "a format with a conversion and no argument is still red" was false for
# every one of them -- a dropped argument in those spellings was skipped, and
# `log3dsPrintf` carried no printf format attribute either, so nothing checked it.
# The pattern is the conversion grammar: flags, width, precision, length modifier,
# conversion letter.
#
# Review-22 F1 closed the last gap in that claim: the SPACE flag was missing from the
# flag class, so `"% d"` read as a literal percent and every figure rule skipped a line
# that `vsnprintf` -- and, once batch 117 landed the attribute, the compiler too -- read
# as a conversion asking for an int. A flag class that reads less than the library does
# is a pass generator wearing a stated limit. The class now matches what the library
# matches, so a space-flagged conversion is adjudicated ON ITS MERITS:
# `("objects: refused % d bytes", objectCorners)` is red because a slot its own text
# calls a byte figure prints a corner count, not because the gate cannot parse it.
# STATED LIMIT: a conversion C itself does not have -- a `%' grouping flag, a `%1$'
# positional field -- is still outside the class, and the skip in check 17 REFUSES any
# percent it cannot read rather than wave it through, so a future grammar gap is a named
# failure and not an exemption.
FIGURE_FMT = re.compile(r"%(?!%)[-+ #0]*(?:\d+|\*)?(?:\.(?:\d+|\*))?[hlLqjzt]*[a-zA-Z]")

# The unit a refusal line's own format text names for the figure beside it
# (review-18 F3). This is the only textual signal that separates "the number of bytes
# the backend was refused" from "the frame number", and it is the one the port already
# writes: the committed line says `%d corners = %d KB`. A figure in a slot whose text
# names no byte unit is not a byte figure and is not adjudicated against the backend's
# byte count -- adjudicating every argument instead was what reddened an honest line
# that also named the frame it refused on.
BYTE_UNIT = re.compile(r"\b(?:KB|kB|KiB|kb|bytes|byte)\b")
KB_UNIT = re.compile(r"\b(?:KB|kB|KiB|kb)\b")

# The unit a refusal line's own format text names for the CORNER figure (review-19
# F1). The committed line writes `%d corners = %d KB`, and the two figures are the
# SAME REQUEST: the capacity the pass handed the backend. Adjudicating only the byte
# half left the corner slot free to print the frame's own corner count, which is
# review-11 F1's `480 corners = 80 KB` -- a refusal of a request that was never
# made -- with the gate green. A slot whose text names a corner quantity must print
# the refused capacity.
CORNER_UNIT = re.compile(r"\b(?:corners|corner|capacity|vertices|verts)\b")


def conv_arity(m):
    """How many arguments the conversion `m` consumes: one, plus one for each `*`
    that stands in for a width or a precision (review-19 F2). `%*d` reads its width
    from an argument, so `("%*d bytes", n)` reads a vararg that was never passed and
    prints stack garbage, while `("%*d corners", 24, attempted)` is legal and its
    `24` is a column width, not a figure. Counting one argument per conversion got
    both of those backwards.
    """
    return 1 + m.group(0).count("*")


MULTI_CAST_RE = re.compile(r"\([ \t]*(?:(?:unsigned|signed)[ \t]+(?:long|int|short|char)"
                           r"|(?:long|short)[ \t]+(?:int|long)|long[ \t]+double)[ \t]*\)")


def wrapping_paren(t):
    """True when the `(` at offset 0 closes on the LAST character, so removing the
    pair cannot mangle an expression that merely begins with a parenthesised term:
    `(a + b) * (c)` starts with a paren and does not end with its match."""
    if not t.startswith("("):
        return False
    depth = 0
    for i, ch in enumerate(t):
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                return i == len(t) - 1
    return False


def unwrap(t):
    """Strip, to a fixpoint, the things that do not change what a figure IS: a cast
    and a pair of parens around the whole expression (review-20 F4c). `(81920)`,
    `((int64_t)81920)` and `(int64_t)(81920)` are the same figure as `81920`, and a
    rule that reads one spelling of four is the rule review-19 F4 was written about,
    one spelling on. The two-word casts need their own pattern: `CAST_RE` matches one
    word.

    Review-21 F3: the loop was `for _ in range(6)`, which is a BOUND wearing a
    docstring that says fixpoint -- at seven nested parens the unwrap stopped, so
    `(((((((81920)))))))` walked through `bare_literal` green, and the same bound made
    `cap_name` fail to read a capacity wrapped that deep, a false red on an honest
    line. Every pass here only removes characters (a cast becomes a space, a matched
    pair of parens goes, and the result is stripped), so a pass that changes the text
    makes it strictly shorter and the loop cannot oscillate: the equality break alone
    is the fixpoint the comment always claimed.
    """
    while True:
        n = MULTI_CAST_RE.sub(" ", CAST_RE.sub(" ", t)).strip()
        if wrapping_paren(n):
            n = n[1:-1].strip()
        if n == t:
            return n
        t = n


# A number that stands for itself: decimal or hex, with an integer suffix, with a
# sign, as a float with an `f`, or with an exponent. Review-20 F4c: the old pattern was
# unsigned and integer-only, so `81920.0f` and `-81920` were stale figures that walked
# through the rule that exists to refuse stale figures. Review-21 open question 4: the
# exponent was the same hole one spelling on -- `8192e1` IS 81920, and the old pattern
# matched `8192` and gave up, so a stale figure written in scientific notation was
# green. Hex must carry its `0x`, or the identifier `abc` would read as a literal.
LITERAL_RE = re.compile(r"[-+]?(?:0[xX][0-9a-fA-F]+[uUlLiI]*"
                        r"|(?:\d+\.\d*|\.\d+|\d+)[eE][-+]?\d+[fF]?"
                        r"|(?:\d+\.\d*|\.\d+)[fFlL]?|\d+[uUlLiIfFlL]?)")


def bare_literal(v):
    """True when `v` IS a number rather than computed from one (review-19 F4,
    review-20 F4c). `81920`, `81920u`, `0x14000`, `0x14000UL`, `(int64_t)81920`,
    `(unsigned long)81920`, `(81920)`, `((int64_t)81920)`, `(int64_t)(81920)`,
    `-81920`, `81920.0f` and `8192e1` are ONE defect -- a figure with no source to
    disagree with -- in twelve spellings, so the test is one fixpoint unwrap and one
    pattern, not a list of spellings. Hex must carry its `0x`, or the identifier `abc`
    would read as a literal.
    """
    return bool(LITERAL_RE.fullmatch(unwrap(v)))


# The ONLY shapes in which the latch's name may appear (review-18 F1). The write net
# enumerates writes by the latch's NAME, so any indirection -- address-of, a reference
# or pointer bound to it in any of the `=`, `(...)` or `{...}` forms, or a call passing
# it to a helper that takes a reference -- is a write the net cannot see, and the
# sustained ~60x/s refusal review-12 R1 was raised about comes back with the gate
# green. A blacklist of spellings was tried twice and each review found a third
# spelling, so this is a WHITELIST: an occurrence of the name outside these shapes is
# refused whatever it spells, and a new spelling cannot be invented to slip past it.
LATCH_SHAPE = re.compile(
    r"bool[ \t]+streamFailureLogged\s*=\s*false\s*;"          # the declaration
    r"|![ \t]*streamFailureLogged"                            # the negative read
    r"|streamFailureLogged\s*==\s*false"                      # the other read
    r"|streamFailureLogged\s*=\s*(?:true|false"               # the arming, the clear,
    r"|objectDraw\.allocationFailed\(\s*\))\s*;")            # the anchored form


def unpinned_uses(text, name, shapes):
    """Offsets of every occurrence of `name` in `text` that is NOT inside one of
    `shapes`. Empty list means the name scan describes every use it can find."""
    spans = [m.span() for m in shapes.finditer(text)]
    return [m.start() for m in re.finditer(r"\b" + name + r"\b", text)
            if not any(a <= m.start() and m.end() <= b for a, b in spans)]


def cap_name(v):
    """The bare name an argument expression carries (review-18 F3):
    `objectDraw.attemptedCapacity()` and `attempted` both name the refused capacity,
    and the net must see that they are the same quantity wherever they sit in the
    argument list. Whitespace goes because an argument is often wrapped across lines,
    and parens go for the reason the literal rule takes them off: `(attempted)` IS the
    capacity. Stripping all whitespace can merge two tokens into one name, but a merged
    name only equals a `cap_names` entry when the argument was already that capacity
    with spaces or parens put into it, so the merge cannot lend a NON-capacity the
    capacity's name in any C expression that compiles (review-20 open question 3).
    """
    t = re.sub(r"\s+", "", unwrap(v)).replace("objectDraw.", "")
    return t[:-2] if t.endswith("()") else t


# One level of local integer constant, so a named stride folds to the backend's
# literal (review-14 F6). Only a plain decimal initialiser counts.
CONST_INT_RE = re.compile(r"\b(?:const|constexpr)[ 	]+(?:u?int\d*_t|size_t|int|long"
                          r"|short|unsigned)[ 	]+([A-Za-z_][A-Za-z0-9_]*)[ 	]*=[ 	]*"
                          r"(\d+)[ 	]*;")


def const_ints(window):
    """{name: digits} for the local integer constants declared in `window`."""
    return {m.group(1): m.group(2) for m in CONST_INT_RE.finditer(window)}


# One level of local initialiser, ARBITRARY right-hand side (review-17 R2). The
# byte figure a refusal line prints is often hoisted into a named local first:
# `const int32_t refusedBytes = (int32_t)((int64_t)cap * 5 * sizeof(float));`
# and then printed by name. A scan that only recognises a figure when the argument
# itself spells an operator (`"*" in v or "/" in v`) does not see such an argument
# at all, so it compares it against NOTHING -- which is how review-12 R2's
# byte-figure-from-the-corner-count came back one refactor away, green.
LOCAL_INIT_RE = re.compile(r"\b(?:const|constexpr)[ \t]+(?:u?int\d*_t|size_t|int|long"
                           r"|short|unsigned)[ \t]+([A-Za-z_][A-Za-z0-9_]*)[ \t]*=[ \t]*"
                           r"([^;]+);")


def local_inits(window):
    """{name: initialiser text} for the local constants declared in `window`,
    including ones whose initialiser is an expression rather than a literal."""
    return {m.group(1): m.group(2).strip() for m in LOCAL_INIT_RE.finditer(window)}


def fold_locals(expr, inits):
    """`expr` with every named local constant replaced by its initialiser, ONCE.
    `re.sub` makes a single pass over the ORIGINAL text, so a local initialised
    from another local is not chased: one level is the claim, and one level is
    what a reader of the source can follow without a symbol table."""
    if not inits:
        return expr
    return re.sub(r"[A-Za-z_][A-Za-z0-9_]*",
                  lambda m: "(" + inits[m.group(0)] + ")"
                  if m.group(0) in inits else m.group(0), expr)


def fold_consts(expr, consts):
    """`expr` with every named constant replaced by its literal, so the frontend's
    `attempted * kMeshStride * sizeof(float)` and the renderer's `cap * 5 *
    sizeof(float)` compare as the same arithmetic. A gate that reddens a named
    constant is a gate the next batch learns to route around (review-10 F6)."""
    if not consts:
        return expr
    return re.sub(r"[A-Za-z_][A-Za-z0-9_]*",
                  lambda m: consts.get(m.group(0), m.group(0)), expr)


def brace_depth(text, a, b):
    """Brace depth at `b` counting from `a`, string-aware (comments are already
    blanked by the caller). A statement written DIRECTLY in the block whose '{' is
    at `a` sits at depth 1; one nested inside an inner condition of that block sits
    deeper. This is what turns "placed before the test" into "runs on every frame"
    (review-14 F2), and "inside the guard" into "runs whenever the print does"
    (review-14 F1)."""
    d = 0
    i = a
    n = min(b, len(text))
    while i < n:
        c = text[i]
        if c in '"\'':
            quote = c
            i += 1
            while i < n and text[i] != quote:
                i += 2 if text[i] == "\\" else 1
        elif c == "{":
            d += 1
        elif c == "}":
            d -= 1
        i += 1
    return d


def backend_mesh_atoms():
    """The atoms of the size the renderer actually asks `linearAlloc` for a dynamic
    mesh, or None when that call cannot be read. None makes check 17 RED: a gate
    that skips on a missing input is a pass generator.

    This is the single source of truth the frontend's KB figure is compared to, so a
    stride change in the renderer moves the gate instead of drifting from it."""
    if not _BACKEND["read"]:
        _BACKEND["read"] = True
        _BACKEND["atoms"] = _read_backend_mesh_atoms()
    return _BACKEND["atoms"]


def _read_backend_mesh_atoms():
    try:
        backend = os.path.join(ROOT, "mdk_3ds", "src", "render", "renderer_c3d.cpp")
        with open(backend, errors="replace") as fh:
            text = fh.read()
        # The renderer is one translation unit assembled from c3d/*.inc
        # (the 2026-11-15 split): expand those includes so this coupling
        # reads the same text the compiler does.
        def _sub(match):
            inc = os.path.join(os.path.dirname(backend), match.group(1))
            with open(inc, errors="replace") as fh:
                return fh.read()

        text = re.sub(r'^#include "((?:c3d|vulkan)/[^"]+\.inc)"', _sub, text, flags=re.M)
    except OSError:
        return None
    m = re.search(r"mdk_render_create_dynamic_mesh[ \t]*\([^;{]*\)[ \t]*\{", text)
    if m is None:
        return None
    sp = block_span_from(text, m.end() - 1)
    if sp is None:
        return None
    call = re.search(r"linearAlloc[ \t]*\(", text[sp[0]:sp[1]])
    if call is None:
        return None
    args = call_args(text[sp[0]:sp[1]], call.end() - 1)
    if not args:
        return None
    # Review-14 F6, the same folding on the other side: a renderer that names its
    # stride is the drift-proof shape the gate is supposed to encourage, and without
    # this it would return None and make check 17 red against an unmodified frontend.
    expr = fold_consts(args[0], const_ints(text[sp[0]:sp[1]]))
    bare = {n for _, n in arith_atoms(expr, set())
            if not n.startswith("sizeof_") and not n.isdigit()}
    return arith_atoms(expr, bare) if len(bare) == 1 else None


def enclosing_block(text, pos):
    """The innermost brace-matched block containing `pos`, found by walking backwards
    with a brace balance (so it stays linear in the file). None if `pos` is outside
    every block."""
    depth = 0
    for i in range(pos - 1, -1, -1):
        c = text[i]
        if c == "}":
            depth += 1
        elif c == "{":
            if depth == 0:
                return block_span_from(text, i)
            depth -= 1
    return None


class Result:
    def __init__(self):
        self.checks = 0
        self.failed = False
        self.broke = ""

    def check(self, label, ok, detail=""):
        self.checks += 1
        if ok:
            print("  ok   %s" % label)
        else:
            self.failed = True
            if not self.broke:
                self.broke = label
            print("  FAIL %s%s" % (label, (" — " + detail) if detail else ""))


# Every check has ONE label, defined here and used by both the check and the arm
# table below. Review-7 F2: the arms used to accept "any red verdict" as success and
# printed whichever check broke first, so an arm written for one check could be
# satisfied by an earlier one and the check it claimed to pin had no mutation that
# isolated it -- check 6 was exactly that, and stubbing its condition to True kept
# the whole suite printing PASS. An arm now names the check it must break, and the
# comparison is exact equality against these strings.
CHK = {
    "refresh-in-loop": "the step loop refreshes Kurt's solid arena (review-8 F1)",
    "refresh-after-camera":
        "the refresh runs after follow.update, the C#'s order (Viewer.cs:517 then :533)",
    "spawn-pin": "the spawn pin is still there for a Kurt outside every arena",
    "refresh-guarded":
        "every in-loop refresh is guarded, so an empty lookup does not clear the pin",
    "guard-is-the-lookup": "the guard is the arenaAt lookup itself",
    "pin-is-the-drawn-list":
        "the step loop pins the runtime's DRAWN LIST (current + active second) and "
        "never its single current arena, which is one name where the oracle has two",
    "scripts-tick-order":
        "the scripts tick in the step loop, after Kurt and before the camera",
    "mixes-runtime-mixer":
        "the frame mixes the pointer that follows the runtime, not the standalone mixer",
    "respawn-reattaches":
        "a respawn re-points the mixer, rebinds Kurt, and re-attaches the sink",
    "music-follows-kurts-arena":
        "the level's music is made on the runtime's mixer, dropped before the runtime is, "
        "entered and faded every step after the camera, and the loading arena's track is "
        "prefetched after the views are synced to the pair (arena streaming Phase 5)",
    "asset-gate": "the runtime is constructed under the asset gate",
    "pitch-from-runtime": "the pitch target is camera_track, then the scripts' arena",
    "objects-drawn-in-order":
        "the object pass is drawn between the arena and Kurt's sprite (Viewer.cs:702-708)",
    "arenas-follow-the-scripts":
        "the views are synced before the frame on the runtime's drawn arenas, and the same "
        "list is drawn (arena streaming Phase 2)",
    "objects-gated-on-runtime":
        "the object pass runs only when there is a runtime to take its objects from",
    "objects-skip-invisible-and-removed":
        "the pass skips the invisible and the removed, as C# Viewer.DrawObjects does",
    "objects-drawn-exactly-once":
        "the object pass has exactly one draw call site in the frame (Viewer.cs:704)",
    "respawn-drops-the-model-caches":
        "a respawn drops the two caches keyed by a Model address, because rebuilding "
        "the runtime frees the models they are keyed by",
    "failure-line-names-the-refused-capacity":
        "the refused-mesh log line prints the capacity the backend was refused, not "
        "the frame's corner count (review-11 F1): every figure the format's own unit "
        "words assign to a byte figure must be the backend's capacity x stride x "
        "sizeof(float) (a KB slot divides by 1024), every figure they assign to a "
        "corner figure must BE that capacity, a line whose format names no unit at all "
        "must still print that capacity in one of the slots it FILLS (review-21 F2), a "
        "format carrying a percent this gate cannot read as a conversion is refused and not "
        "skipped (review-22 F1), and "
        "no figure may be baked into the "
        "format text at all, because a figure with no argument behind it has no source "
        "to disagree with (review-18 F2/F3, review-19 F1/F2, review-20 F1/F2/F4, "
        "review-21 F1/F2, review-22 F1)",
    "failure-latch-re-arms-on-success":
        "the refused-mesh latch clears when a later attempt succeeds, so a second "
        "refusal after a recovery still prints (review-11 F7)",
    "failure-line-prints-once-per-refusal":
        "the refused-mesh line is edge-triggered, so one sustained refusal prints once "
        "and not on every frame of it (review-12 R1)",
    "failure-latch-writes-are-the-pinned-three":
        "the only writes to the refused-mesh latch are the arming inside its guard, "
        "the clear in the `else`, and the anchored assignment form -- a fourth write "
        "clears the latch on a refusing frame (review-15 F1)",
}


def adjudicate(text, quiet=False):
    """Run every check over `text`. Returns the tally so the arms can reuse it."""
    r = Result()
    loop = step_loop(text)
    spawn = body_span(text, "spawnPlayer")
    spawn_body = text[spawn[0]:spawn[1]] if spawn else None

    def say(label, ok, detail=""):
        if quiet:
            r.checks += 1
            if not ok:
                r.failed = True
                if not r.broke:
                    r.broke = label
            return
        r.check(label, ok, detail)

    # 1. the collision world is refreshed INSIDE the fixed-step loop.
    #    Over CODE lines, not the raw block: this tree documents its own rules in
    #    prose, and a comment inside the loop naming `setSolid(` must not satisfy a
    #    check whose docstring says prose is not a call site (review-2 F5).
    loop_code = "".join(line + "\n" for _, line in code_lines(loop)) if loop is not None else None
    say(CHK["refresh-in-loop"],
        loop_code is not None and "setSolid(" in loop_code,
        "no setSolid call in the `while (pending >= step)` body, so the pin is set "
        "wherever else it is set and never follows Kurt")

    # 2. and it is refreshed AFTER the camera step, as the C# orders them.
    ok_order = False
    if loop is not None:
        cam = call_line(loop, "follow.update")
        sol = call_line(loop, "space.setSolid")
        ok_order = cam is not None and sol is not None and sol > cam
    say(CHK["refresh-after-camera"],
        ok_order, "the camera's clearance can move Kurt, so a refresh placed before it "
                  "pins the arena he left rather than the one he is in")

    # 3. the spawn pin survives: the loop cannot supply it for a Kurt outside
    #    every arena, and without it bottom() resolves nothing.
    say(CHK["spawn-pin"],
        spawn_body is not None and "setSolid(" in spawn_body,
        "with no pin at all bottom() answers false and the fall-out death never fires")

    # 4. the in-loop refresh is guarded, so being outside every arena keeps the
    #    last pin instead of clearing it. Depth is the load-bearing half: a call
    #    at depth 0 is unconditional whatever its argument looks like. EVERY call
    #    site is tested, not the first: a second site has been added before and an
    #    unguarded one would clear the pin while the gate printed PASS.
    depths = line_depths(loop) if loop is not None else []
    sites = [(i, line) for i, line in code_lines(loop)
             if re.search(r"setSolid\s*\(", line)] if loop is not None else []
    guarded = bool(sites) and all(
        (depths[i] if i < len(depths) else 0) >= 1 for i, _ in sites)
    say(CHK["refresh-guarded"],
        guarded, "an unconditional setSolid with an empty name clears the pin, which is "
                 "check 3's failure reached from the other side")

    # 5. the guard is the arena lookup, not some other condition that happens to
    #    wrap it. `if (solid) space.setSolid(*solid)` is the shape; a guard on an
    #    unrelated flag would pass check 4 while clearing the pin.
    shape = False
    if loop is not None:
        m = re.search(r"if[ \t]*\(([A-Za-z_][A-Za-z0-9_]*)\)[ \t]*\{[ \t]*\n[ \t]*"
                      r"space\.setSolid\([ \t]*\{[ \t]*\*([A-Za-z_][A-Za-z0-9_]*)[ \t]*\}", loop)
        if m:
            shape = m.group(1) == m.group(2) and re.search(
                r"arenaAt\([^;]*\);[ \t]*\n[ \t]*const[ \t]+std::string\*[ \t]*" + re.escape(m.group(1)),
                loop) is not None or bool(re.search(
                r"std::string\*[ \t]+" + re.escape(m.group(1)) + r"[ \t]*=[ \t]*\n?[ \t]*space\.arenaAt",
                loop))
    say(CHK["guard-is-the-lookup"], shape, 
        "a guarded setSolid whose guard is not the lookup can still clear the pin")

    # 6. the step loop pins the runtime's DRAWN LIST, and never its single current
    #    arena. The oracle hands SetSolid the scripts' drawn arenas -- current plus
    #    the active second (ScriptRuntime.cs:265-282, handed over at Viewer.cs:533) --
    #    and move() sweeps ONLY the pinned arenas, so one name drops the room behind
    #    the open door Kurt came through: no floor there, he drops through it, and
    #    bottom() fires the fall-out death into a respawn loop. Batch 99 shipped
    #    exactly that (review-6 F1); the geometric stand-in that replaced it was right
    #    to stop the fall and wrong to stop at one name. Batch 137 ported the list
    #    (ScriptRuntime::solidArenas, ArenaSpace::setSolid taking the names the C#
    #    passes), so this check now REQUIRES it: solidArenas() with a runtime, the
    #    lookup without one, exactly one refresh per branch, and never currentArena().
    #    The value itself is executed by vm_tests'
    #    SolidArenasAreTheDrawnListAndThePinTakesBoth; this is the shape.
    frozen = bool(re.search(r"setSolid\([ \t]*runtime->currentArena\(", loop or ""))
    drawn = bool(re.search(r"setSolid\([ \t]*runtime->solidArenas\([ \t]*\)[ \t]*\)",
                           loop or ""))
    say(CHK["pin-is-the-drawn-list"],
        drawn and not frozen and loop_code is not None and loop_code.count("setSolid(") == 2,
        "the oracle pins the scripts' DRAWN list (current + active second), so a pin "
        "that reads currentArena() drops the room behind the door; the loop must have "
        "exactly one refresh per branch and the runtime's must be solidArenas()")

    # 7. the scripts tick INSIDE the fixed-step loop, between Kurt and the camera.
    #    The C#'s Viewer order is UpdateKurt, _scripts.Update, UpdateCamera
    #    (Viewer.cs:505-535); a runtime updated after the camera feeds the camera a
    #    tick behind, and one updated outside the loop runs at the frame rate while
    #    its own 1/30 s clock assumes it is called every 1/60 s step.
    ok_scripts = False
    if loop is not None:
        kurt = call_line(loop, "player.update")
        scr = call_line(loop, "runtime->update")
        cam = call_line(loop, "follow.update")
        ok_scripts = kurt is not None and scr is not None and cam is not None \
            and kurt < scr < cam
    say(CHK["scripts-tick-order"], ok_scripts,
        "Viewer.cs:505-535 is Kurt, scripts, camera; outside the loop the runtime's 1/30 s "
        "clock is driven by frames, not by steps")

    # 8. the frame mixes the RUNTIME's mixer. ScriptRuntime owns its SoundMixer and
    #    every script sound is written into that one (script_vm.cpp:1258 opcode 89
    #    -> ScriptRuntime::playSound -> mixer_), so a frontend that keeps mixing its
    #    own standalone mixer attaches the sink to a device the scripts never write
    #    to: the level would run its scripts and be silent, with a healthy ledger.
    #    (Reads `text`, the adjudicate argument, and not the module's SRC: a check
    #    that read SRC would adjudicate the unmutated file for every arm and turn
    #    its own arm green, which is the pass-generator shape this file exists to
    #    avoid — and the arm for this check caught it on its first run.)
    mix_ok = all(re.search(p, text) for p in (
        r"mix->listenFromCamera\(camera\);", r"mix->update\(elapsed\);",
        r"mix->device\(\)\.update\(elapsed\);")) and not re.search(
        r"(?m)^[ \t]*mixer\.(listenFromCamera|update|device\(\)\.update)[ \t]*\(", text)
    say(CHK["mixes-runtime-mixer"],
        mix_ok, "the scripts write to the runtime's mixer; mixing another one is a silent "
                "level that still drains its audio queue")

    # 9. a respawn re-points the mixer AND re-attaches the sink. The runtime owns
    #    its mixer and its SoundDevice, so a rebuilt runtime is a new device: the
    #    sink attached to the old one leaves every level after the first death
    #    silent, which is the kind of bug that only shows up after a minute of play.
    spawn_mix = (spawn_body is not None
                 and re.search(r"mix[ \t]*=[ \t]*&runtime->mixer\(\)[ \t]*;", spawn_body) is not None
                 and re.search(r"player\.setMixer\(&runtime->mixer\(\)\)", spawn_body) is not None
                 and re.search(r"mix->device\(\)[ \t]*\.setSink\(", spawn_body) is not None)
    say(CHK["respawn-reattaches"], spawn_mix,
        "the rebuilt runtime owns a new SoundDevice; the old sink is a device nothing reads")

    # 10. the runtime is built only when its three assets are present. A runtime
    #     with a hole in one of them is a different random stream, not a degraded
    #     oracle run (script_runtime.h's constructor note), so the gate has to be
    #     the reason it is not built.
    gate_ok = (spawn_body is not None
               and re.search(r"if[ \t]*\([ \t]*scriptAssetsReady[ \t]*\)[ \t]*\{", spawn_body)
               is not None
               and re.search(r"runtime\.reset\([ \t]*new[ \t]+mdk::scripts::ScriptRuntime",
                             spawn_body) is not None)
    say(CHK["asset-gate"], gate_ok,
        "a runtime without its CMI, bank or sprites archive plays a different game")

    # 11. the camera's rest pitch comes from the runtime when there is one: a live
    #     camera_track request first, then the scripts' current arena (the C#'s two
    #     sources, Viewer.cs:746-747). The geometric arena is the no-runtime
    #     stand-in and nothing more.
    pitch_ok = (loop is not None
                and "runtime->cameraTrackTicks" in loop
                and "runtime->cameraTrackPitch" in loop
                and "level.arenaPitch(runtime->currentArena())" in loop)
    say(CHK["pitch-from-runtime"], pitch_ok,
        "the pitch the camera rests at is the scripts' arena's, not the geometric one")

    # 12. the object pass is drawn, and BETWEEN the arena and Kurt's sprite. The C#
    #     orders arena, objects, Kurt (Viewer.cs:702-708) and the order is the
    #     picture: objects drawn before the arena are painted over, and objects
    #     drawn after Kurt paint over him. Presence anywhere in the file is not the
    #     invariant, which is the same reason check 1 exists.
    op = object_pass(text)
    # The arena draw is the scripts' drawn list (Viewer.cs:702, LevelView.Draw(
    # DrawnArenas)) since arena streaming Phase 2; it was one `view.draw()` of the
    # level's start arena before.
    arena_at = text.find("arenaViews.draw(*frameDrawn);")
    kurt_at = text.find("kurtSprite.draw(")
    frame = frame_body_span(text)
    in_frame = (op is not None and frame is not None
                and frame[0] <= op["start"] < frame[1])
    say(CHK["objects-drawn-in-order"],
        in_frame and arena_at >= 0 and kurt_at >= 0 and arena_at < op["start"] < kurt_at,
        "C# Viewer.DrawScene draws the arena, then the objects, then Kurt; anywhere "
        "else and the order of the draws is a different picture")

    # 12b. THE ARENAS FOLLOW THE SCRIPTS, IN THIS ORDER (arena streaming Phase 2):
    #      the texture stream is stepped (ArenaStream::update), THEN the views are
    #      synced (a view built this frame finds its textures' pixels in), both
    #      BEFORE the frame begins (they make and release GPU resources), and what is
    #      drawn is the runtime's drawn list -- not the start arena, which is all
    #      this frontend drew until 2026-11-18.
    #      ONE drawn list feeds all three (frameDrawn: the runtime's drawnArenas, or
    #      Kurt's arena before the first tick). The scripts snapshot that list in the
    #      middle of their tick and the current/second pair is its end, so a stream
    #      or a view fed from the pair alone lets go of an arena the frame still
    #      draws after a scripted teleport (review, 2026-11-18).
    #      THE STREAM IS GONE (4fea3380, 2026-11-22, owner-directed): ArenaStream's
    #      paced 32 KiB SD reads were the transition stutter, so it was retired for a
    #      whole-MTO pixel fallback and the views' own surface pump. Its half of this
    #      check (`stream.update` before the sync, and its `secondActive` flag) went
    #      with it; what survives is the list, the sync before begin_frame (the sync
    #      makes and releases GPU resources), and the draw on the same list. Repaired
    #      2026-11-24 -- the check had been red on every correct build since the
    #      retirement, which cascaded into every arm below reporting this check.
    sync_at = text.find("arenaViews.sync(streamCurrent, streamSecond, *frameDrawn, bubbleHints);")
    begin_at = text.find("mdk_render_begin_frame(renderer, &params);")
    list_at = text.find("frameDrawn = scriptsDrew ? &runtime->drawnArenas() : &startDrawn;")
    say(CHK["arenas-follow-the-scripts"],
        frame is not None
        and frame[0] <= list_at < sync_at < begin_at < arena_at < frame[1]
        and "runtime->secondArena()" in text[frame[0]:sync_at],
        "frameDrawn (the runtime's drawnArenas) -> arenaViews.sync -> begin_frame -> "
        "arenaViews.draw, both on the one list; any other order or a second list draws an "
        "arena whose textures are not in, or lets go of one that is drawn")

    # 12c. THE LEVEL'S MUSIC (arena streaming Phase 5; audio/level_music.h).
    #     - It holds voices of the RUNTIME's mixer, so it is dropped BEFORE the
    #       runtime is destroyed (a LevelMusic that outlives its mixer stops a voice
    #       through a freed pointer on the next step) and it is built on that mixer
    #       and no other: built on the standalone one it would play into a device
    #       nothing mixes once there is a runtime.
    #     - The C# enters Kurt's arena and advances the fades EVERY STEP, after the
    #       camera and before SetSolid (Viewer.cs:521-525, :533): enter, then update.
    #     - The bank's stepped prefetch of the loading arena's track runs once per
    #       FRAME, after the views have been synced to the pair (it was "after the
    #       stream" until the stream's retirement, 4fea3380) and before the frame
    #       begins.
    music_ok = False
    if spawn_body is not None and loop is not None and frame is not None:
        drop = spawn_body.find("music.reset();")
        kill = spawn_body.find("runtime.reset();")
        made = re.search(r"music\.reset\(new mdk::audio::LevelMusic\(&runtime->mixer\(\),",
                         spawn_body)
        cam = call_line(loop, "follow.update")
        enter = call_line(loop, "music->enter")
        fade = call_line(loop, "music->update")
        solid = call_line(loop, "space.setSolid")
        prefetch_at = text.find("bank.prefetch(track)")
        step_at = text.find("bank.prefetchStep()")
        # The tracks given back once a second are every one BUT the playing one,
        # the fading one and the LOADING arena's: without the third the track a
        # prefetch has just landed is released within the second, and Kurt walks
        # into a stall the prefetch existed to prevent. (The list grew a fourth,
        # the door bubble's track, review M2; the count handed to the release must
        # be the list's own, or a kept track is read past or never read.)
        keep = re.search(r"const std::string keep\[(\d+)\] = \{(.*?)\};", text, re.S)
        keep_ok = (keep is not None
                   and "music->playing()" in keep.group(2)
                   and "music->fading()" in keep.group(2)
                   and "music->trackOf(streamSecond)" in keep.group(2)
                   and ("bank.releaseMusicExcept(keep, %s)" % keep.group(1))
                   in text[frame[0]:begin_at])
        music_ok = (0 <= drop < kill and made is not None and keep_ok
                    and "music->enter(runtime->currentArena());" in loop
                    and "music->update(step);" in loop
                    and None not in (cam, enter, fade, solid) and cam < enter < fade < solid
                    and frame[0] <= sync_at < prefetch_at < step_at < begin_at < frame[1])
    say(CHK["music-follows-kurts-arena"], music_ok,
        "music.reset() before runtime.reset(); LevelMusic(&runtime->mixer(), ...); in the "
        "step loop follow.update -> music->enter(currentArena) -> music->update(step) -> "
        "setSolid; in the frame arenaViews.sync -> bank.prefetch -> bank.prefetchStep -> "
        "begin_frame; releaseMusicExcept keeps the playing, the fading and the loading "
        "arena's track")

    # 13. the pass is gated on the runtime. `runtime->objects()` on a null runtime
    #     is a null dereference in the frame path, and the level runs without scripts
    #     whenever its CMI, bank or sprites archive is missing (check 10), so the
    #     null case is a shipped configuration and not a hypothetical.
    say(CHK["objects-gated-on-runtime"],
        op is not None and re.fullmatch(
            r"if[ \t]*\([ \t]*runtime[ \t]*\)[ \t]*\{", op["header"]) is not None,
        "a level with no runtime has no objects, and walking a null one is a crash in "
        "the frame path")

    # 14. the pass skips the invisible and the removed. C# Viewer.DrawObjects skips
    #     `!obj.Visible`, and the C#'s list has already dropped its removed objects
    #     while this port keeps them in the deque for pointer stability — so a pass
    #     that walks the deque without skipping them draws corpses and dead pickups.
    body = op["body"] if op else ""
    say(CHK["objects-skip-invisible-and-removed"],
        bool(re.search(r"![ \t]*obj\.visible", body))
        and bool(re.search(r"isRemoved\(&obj\)", body)),
        "the deque holds removed objects the C# has already unlisted; without both "
        "skips the pass draws what the oracle does not")

    # 15. a respawn drops the two caches keyed by a Model address. ScriptRuntime
    #     OWNS its model cache (script_runtime.h:580, models held BY VALUE), so
    #     `runtime.reset()` frees every Model* that ObjectView::layouts_ and the
    #     shared pose caches are keyed by — and the new runtime's models land on
    #     recycled addresses, same level and same parse order, so the address very
    #     likely reappears holding a DIFFERENT model. The C# cannot have this: a
    #     respawn is a fresh Viewer, whose constructor calls MdkObject.ForgetPoses()
    #     (Viewer.cs:185) and builds a fresh ObjectView. This is why the object-pass
    #     declarations sit above spawnPlayer and not beside the draw: the drop has to
    #     be reachable from it, and moving them down would silently undo the fix.
    caches_dropped = (spawn_body is not None
                      and re.search(r"MdkObject::forget_poses\(\)", spawn_body) is not None
                      and re.search(r"objectView\.clear\(\)", spawn_body) is not None)
    say(CHK["respawn-drops-the-model-caches"], caches_dropped,
        "rebuilding the runtime frees the models both caches key on; a recycled address "
        "would otherwise draw the previous model's geometry and pose")

    # 16. EXACTLY ONE object-pass call site (review-9 F2). Check 12 resolves the pass
    #     from the FIRST `objectDraw.draw(` and asserts only that that one sits between
    #     the arena and Kurt, so a SECOND copy of the pass after Kurt is invisible to
    #     every check in the file: the reviewer inserted `if (runtime) {
    #     objectDraw.draw(...); }` just before mdk_render_end_frame and all 15 checks
    #     stayed green. That mutant is a picture difference — the frame's stream is
    #     uploaded and drawn twice, the objects are painted over Kurt, and the pass's
    #     depth and blend transitions are taken twice — and C# Viewer.DrawScene calls
    #     DrawObjects once (Viewer.cs:704). Counting the call sites is the whole check;
    #     the position of the one allowed site is check 12's job, not this one's.
    #     The count runs over a comment-blanked copy (review-10 F6): prose about the
    #     call site is not a call site, and the reviewer's one-line comment naming it
    #     reddened this check when it counted the raw file. The check pins the call
    #     INSIDE the frame, which is the shape the oracle's single DrawObjects has;
    #     moving the pass into a helper would move checks 12, 13 and 16 together, and
    #     that refactor is not this port's — review-10 F6's second half is deferred
    #     with that cause rather than silently unhandled.
    code = blank_comments(text)
    calls = [m.start() for m in re.finditer(r"objectDraw\.draw\(", code)]
    say(CHK["objects-drawn-exactly-once"],
        len(calls) == 1 and op is not None and frame is not None
        and frame[0] <= calls[0] < frame[1],
        "one DrawObjects per frame is the oracle's; a second call site replays the "
        "stream over whatever the first one already painted")

    # 17. THE REFUSED-MESH LINE NAMES THE REFUSED CAPACITY (review-11 F1). The line
    #     exists because review-10 F3 found a refused `linearAlloc` was invisible;
    #     it fails the same way if it prints a size that was never requested.
    #     `ObjectDraw::ensure` rounds the frame's corner count up to a power of two
    #     and it is that rounded capacity the backend was handed, so a line built
    #     from `objectCorners` printed a 9 KB refusal for an 80 KB one and
    #     disagreed with renderer_c3d.cpp's own `linearAlloc(N) FAILED` by up to 8x
    #     -- two committed lines, one refusal, 51 KB apart.
    #
    #     WHAT THE ARGUMENTS MUST AGREE ON (review-12 R2, review-13 D1 and D4, and
    #     review-15 F3). Four weakenings were found in this check, each one a gate that
    #     could not fail at the thing it claimed to pin:
    #       * it read only the FIRST printf in the branch, so a second, unguarded
    #         line built from `objectCorners` was read by neither check 17 nor
    #         check 19 (D1). It now reads EVERY `log3dsPrintf(` in the branch.
    #       * it compared the derived argument's IDENTIFIER SET to {src}, so the
    #         multiplication factors were unchecked and `* 4` for the corner stride
    #         stayed green while disagreeing with the backend's byte count (D4). It
    #         now reduces the argument to its atoms and requires them to BE the
    #         backend's own `(size_t)cap * 5 * sizeof(float)`, optionally divided by
    #         1024 for the KB the line names -- one source of truth, not a rule about
    #         operands.
    #       * the first argument may still be the call itself or a local initialised
    #         from it, so a bare rename stays legal (review-10 F6's lesson: a gate
    #         that reddens a shape the port may write is a gate that gets routed
    #         around). A plain extra argument is still allowed; what is not allowed is
    #         a derived byte figure that disagrees with the corner figure.
    #       * review-14 F7 widened the scope from the first refusing branch to the
    #         whole object pass but decided family membership by FORMAT, which read
    #         fewer lines inside the branch than review-13 D1 did (review-15 F3). The
    #         net is now the union of both, and it is the union that both findings
    #         asked for: see the comment on the loop below.
    # 18. THE LATCH CLEARS ON A FRAME THAT DID NOT REFUSE (review-11 F7, re-pinned by
    #     review-12 R1 and review-13 D2). `streamFailureLogged` is the edge trigger
    #     that keeps a level under linear pressure from printing the same line 60
    #     times a second, and it must clear when a later attempt succeeds or a grow
    #     to 8192 corners that fails after a success at 4096 prints nothing and the
    #     log ends on the smaller request. The first version asked whether ANY LINE
    #     EQUALLED `streamFailureLogged = false;` -- which pins a line, not the guard
    #     (review-12 R1). It is a brace-scan: the clear must sit in the `else` SIBLING
    #     of the `allocationFailed()` test, the branch every non-refusing frame takes.
    #     The equivalent assignment form is accepted too, but ANCHORED (review-13 D2):
    #     it used to be searched for over the whole file, so the assignment placed
    #     INSIDE the refusing branch -- which sets the latch and can never clear it,
    #     the exact F7 defect -- satisfied the check from inside the very branch the
    #     clear has to live outside of. The form is only the same state machine when it
    #     runs on every frame, so it must sit in the enclosing block BEFORE the test.
    # 19. AND EVERY REFUSAL LINE PRINTS ONCE PER REFUSAL, not once per frame (review-
    #     12 R1, review-13 D1 and D3). The clear being reachable is only half of an
    #     edge trigger: with the guard widened the line still prints on every frame of
    #     a sustained refusal -- which is why it was edge-triggered at all, since the
    #     backend line it answers once printed 1710 times in a run (review-10 F3).
    #     Two properties, both on EVERY printf in the branch: it must sit inside a
    #     block (containment, not a line match), and that block's condition must BE the
    #     negative latch test -- `fullmatch`, not `search`, because a condition that
    #     merely MENTIONS `!streamFailureLogged` (`!streamFailureLogged || verbose`)
    #     prints on every frame of the refusal and was green (D3).
    # 20. AND NOTHING ELSE MAY WRITE THE LATCH (review-15 F1). Checks 18 and 19 pin
    #     the clear and the guard, and both stayed green while a SECOND
    #     `streamFailureLogged = false;` was added at the end of the refusing branch,
    #     or as a plain statement after the if/else: either one clears the latch on
    #     the frame that refused, so the guard is true again next frame and the line
    #     prints ~60x/s -- review-12 R1's spam, one line away, with every other check
    #     honest. A clear that is reachable is only half of an edge trigger; the other
    #     half is that NO FRAME WHICH REFUSED CLEARS IT. So the net is the whole write
    #     set of the latch inside the pass, and every write must be one of exactly
    #     three shapes: `= true` as a direct statement of a latch-guarded block (the
    #     arming), `= false` as a direct statement of the `else` sibling (the clear),
    #     or `= objectDraw.allocationFailed()` as a direct statement of the block
    #     enclosing the test (the equivalent form check 18 accepts). A fourth write is
    #     a write that runs on a refusing frame, and that is the defect.
    fail_ok = False
    latch_ok = False
    once_ok = False
    writes_ok = False
    # Anchored on the IF HEADER, not on the first textual `allocationFailed()`: the
    # accepted assignment form writes the call twice, and a plain `code.find` landed on
    # the assignment and made its own window empty.
    test = re.search(r"\bif[ \t]*\([ \t]*objectDraw\.allocationFailed\([ \t]*\)[ \t]*\)",
                     code)
    at = test.start() if test else -1
    branch_sp = block_span_from(code, test.end()) if test else None
    # Review-14 F7: the property is about EVERY refusal line, so the scope cannot be
    # the body of the first `if (allocationFailed())`. A second refusal line in a
    # SIBLING condition -- a first-pass diagnostic, a "log it again while the capacity
    # is still zero" -- sat outside that body and was read by neither check 17 nor
    # check 19, which is review-11 F1's two-numbers-on-one-refusal back on the console
    # with the gate green. The scope is now the object pass, and a line belongs to the
    # family when its OWN FORMAT NAMES the refusal (REFUSAL_FMT); that is also what
    # keeps the capacity-change line, which reports a SUCCESS, out of the family.
    # A refusal line whose format is a variable, or that never says so, is outside
    # this net: stated as a limit, not hidden.
    op_sp = object_pass(code)
    scope = code[op_sp["start"]:op_sp["end"]] if op_sp else ""
    off = op_sp["start"] if op_sp else 0
    backend = backend_mesh_atoms()
    prints = []
    for p in re.finditer(r"log3dsPrintf[ \t]*\(", scope):
        args = call_args(scope, p.end() - 1)
        if not args or not args[0].lstrip().startswith('"'):
            continue
        # Review-15 F3: TWO nets, not one. Batch 108 replaced batch 107's
        # format-agnostic scan of the refusing branch with a format filter over the
        # pass, and wrote "widened" in the record; the format filter is NARROWER on
        # the branch, so `log3dsPrintf("objects: %d corners this frame",
        # objectCorners)` inside `if (allocationFailed())` -- review-11 F1's wrong
        # number, printed on every refusing frame -- was read by neither check and
        # went green. A line belongs to the refusal family when it sits IN THE
        # REFUSING BRANCH (whatever it says: inside that branch every line the pass
        # prints is a line about the refusal) OR when its own format names the
        # refusal (review-14 F7's sibling-condition line, which is outside the
        # branch). The union is what both D1 and F7 asked for; neither half alone
        # closes the other's mutant.
        in_branch = (branch_sp is not None
                     and branch_sp[0] <= off + p.start() < branch_sp[1])
        if not (in_branch or REFUSAL_FMT.search(args[0])):
            continue
        prints.append((p, args))
    # Review-15 F4: `backend is None` must make this check RED, not raise. The
    # docstring of backend_mesh_atoms() promises exactly that, and the loop below
    # called kb_form(None) unconditionally, so a rewritten `linearAlloc` expression
    # killed the script with a TypeError after 16 lines: no check 18, no check 19,
    # no arm and no probe run, and an unattributed traceback where a named failure
    # should have been. The claim was true only of the FIRST use of `backend`.
    fail_ok = bool(prints) and backend is not None
    for p, args in (prints if backend is not None else []):
        fmt, vals = args[0], args[1:]
        convs = list(FIGURE_FMT.finditer(fmt))
        # Review-18 F2: a figure BAKED INTO THE FORMAT TEXT has no source at all.
        # Batch 115 skipped a line with no arguments and no conversion, so
        # `log3dsPrintf("objects: refused 81920 bytes")` -- review-12 R2's stale byte
        # figure with the conversion taken out, which is what a fix for "that number is
        # wrong" looks like when nobody asks where the number comes from -- was skipped
        # by every figure rule while still printing a byte count nobody recomputes.
        # Conversions are stripped first, so a `%8d` width is not read as a figure.
        if re.search(r"\d", FIGURE_FMT.sub(" ", fmt)):
            fail_ok = False
            break
        # Review-17 R3, widened by review-21 F1: what puts a line outside the figure
        # rules is that it PRINTS no figure, and that is a property of the FORMAT, not of
        # the argument list. A refusal line that states no figure -- the natural companion
        # to the backend's own `linearAlloc(%u) FAILED`, e.g. "objects: linearAlloc FAILED
        # for the stream mesh" -- disagrees with nothing, so it cannot fail a rule about
        # disagreement. Reddening it was the gate refusing a shape the port may write
        # (review-10 F6), and the failure text even described a defect the line did not
        # have. But the skip read `not vals and not convs`, so the SAME format with a
        # dangling argument -- `("objects: linearAlloc FAILED, bytes refused", attempted)`,
        # which is what a refusal line looks like after a batch deletes a conversion and
        # forgets the argument -- fell through into a slot loop over zero conversions, and
        # `min([])` killed the script with a ValueError after 16 checks: no check 18, no
        # check 19, no arm and no probe scored, and the unattributed traceback review-15 F4
        # was written about. A format that asks for nothing has no slot to adjudicate.
        #
        # Review-22 F1: "the gate found no conversion" is NOT "the format asks for no
        # figure", and the skip was keyed on the first. `FIGURE_FMT` does not read the space
        # flag -- the file says so in its own stated limit above -- so
        # `("objects: refused % d bytes", objectCorners)` had zero convs, was skipped by
        # EVERY rule below including the byte-unit backstop, and printed ` 480 bytes` for an
        # 80 KB request (review-11 F1's defect wearing a flag the tool cannot parse) with a
        # full `PASS Main3dsFlow`. Batch 113 reddened that line and batch 114 crashed on it:
        # both failed closed, and a skip that turns a fail-closed into an exemption is a pass
        # generator. A format carrying a percent that is not `%%` and is not a readable
        # conversion is a format this check cannot adjudicate, so it is refused by name.
        if not convs:
            if "%" in fmt.replace("%%", ""):
                fail_ok = False
                break
            continue
        # Review-18 F4: a conversion with no argument is undefined behaviour, and this
        # comment always claimed it was red; with the old one-letter FIGURE_FMT it was
        # red only for the spellings that pattern matched. Now the claim is true of the
        # whole conversion grammar.
        #
        # Batch 117 gave `mdk::log3dsPrintf` a `format (printf, 1, 2)` attribute, so the
        # compiler diagnoses a dropped argument now too -- and this rule STAYS, for two
        # reasons. A warning is not a gate: nothing fails a build on it, and this loop
        # stops on `tools/check.sh` and the suites, not on a notice in a build log. And
        # the slot loop below indexes `vals` through the conversions, so without this line
        # a dropped argument is a Python IndexError that kills the suite instead of one
        # named FAIL -- review-15 F4's class. What the compiler owns and this check never
        # did is the TYPE of the argument at every one of the tree's 72 log call sites:
        # `tests/scripts/arm_compile_check.sh` compiles the shipped sources with
        # -Werror=format, which is where that half lives now.
        #
        # Review-19 F2: a conversion does not consume ONE argument. `%*d` reads a width
        # and then the value, so `("objects: refused %*d bytes", bytes)` asks for two
        # arguments, supplies one, and prints stack garbage -- and the old
        # `len(convs) > len(vals)` test called that balanced. The count is now the
        # arguments the conversions CONSUME, star fields included.
        if sum(conv_arity(c) for c in convs) > len(vals):
            fail_ok = False
            break
        # Every name in `cap_names` IS the refused capacity: the call itself, or a
        # local whose initialiser is the call. The window is the pass up to THIS
        # printf, so a same-named local declared after it cannot lend it a source.
        cap_names = set(re.findall(r"([A-Za-z_][A-Za-z0-9_]*)[ \t]*=[ \t]*objectDraw"
                                   r"\.attemptedCapacity\(\)", scope[:p.start()]))
        cap_names.add("attemptedCapacity")
        # Review-14 F6: a named stride is the same arithmetic written differently, so
        # one level of local integer constants folds before the comparison. Reddening
        # it would be review-10 F6's failure again -- a gate that rejects a shape the
        # port may legitimately write is a gate the next batch routes around.
        consts = const_ints(scope[:p.start()])
        # One level of local initialiser, so `refusedBytes` printed by name is
        # compared as the arithmetic it hides (review-17 R2). Folded AFTER the
        # integer constants, because the initialiser may itself name a stride.
        inits = local_inits(scope[:p.start()])
        # Review-18 F3: WHICH arguments are byte figures is decided by the format's
        # own unit words, not by "is not the first argument". Adjudicating every
        # argument reddened an honest line that also named a non-byte quantity --
        # "objects: refused %d corners at frame %lu", attempted, (unsigned long)frame
        # -- which is review-10 F6's failure mode again: a gate that rejects legal
        # code gets routed around, and the route-around is worse than the defect. The
        # slot of a conversion is the format text around it up to its neighbouring
        # conversions, which is where the port already writes `corners` and `KB`.
        #
        # Review-19 F2: the argument a conversion PRINTS is the LAST of the ones it
        # consumes, because a star width or precision is read before the value. With
        # `("... %*d corners = %d KB ...", 24, attempted, bytes)` the old `vals[i]`
        # mapping handed the width `24` to the first conversion and adjudicated a
        # column width as a byte figure, which reddened a legal line while the UB line
        # stayed green.
        #
        # STATED LIMIT (review-19 F4, review-20 F2/F4, review-21 F2): a slot that the
        # format's own unit words assign NEITHER a byte nor a corner unit is not
        # adjudicated against the backend's arithmetic, UNLESS its argument is a literal --
        # a literal figure has no source whatever unit is named, so `81920`, `81920u`,
        # `0x14000`, `(int64_t)81920`, `(81920)`, `-81920` and `81920.0f` are all refused
        # wherever they sit, while `"... at frame %lu", attempted, frame` is not. What a
        # unit-less slot cannot do is be the line's only claim about the refusal: when the
        # format names no unit word ANYWHERE the refused capacity must be one of the
        # arguments the line FILLS (review-21 F2). And an argument past the LAST conversion
        # is never looked at: the format cannot ask for it, so it prints nothing.
        #
        # Review-22 F2: FILLS is the word that carries the weight, so it has an arm. A gate
        # mutant that reads the argument list instead of the filled slots passed 54/54 arms
        # and 22/22 probes while accepting `"objects: refused %d at frame %lu", objectCorners,
        # (unsigned long)frame, attempted` -- the capacity present in the call and printed by
        # nothing, which is review-11 F1 again with the argument parked where no conversion
        # can reach it.
        #
        # Review-22 F7, stated as a limit rather than hidden: the same rule refuses a line
        # that prints ONLY a frame number -- `("objects: linearAlloc FAILED at frame %lu",
        # frame)` is red. That is deliberate. The gate cannot tell a unit-less figure that
        # makes a claim about the refusal from one that does not, which is exactly why the
        # rule exists, and the only widening that keeps review-21 F2's shape red -- accept a
        # line whose EVERY filled slot is the frame counter -- needs a new "is this the frame
        # counter" predicate with arms and probes of its own, for a shape the port does not
        # write today. The honest edit a batch reaches for is to print the refused capacity
        # alongside the frame, and that is green.
        #
        # Review-20 F2, NARROWED rather than deleted by review-21 F2: a line that names a
        # unit word is NOT separately required to print the capacity as an argument of its
        # own. The per-slot rules below already say that every figure the format calls a
        # byte figure IS the backend's own capacity arithmetic and every figure it calls a
        # corner figure IS the capacity, so a line printing only the KB figure -- the shape
        # `renderer_c3d.cpp`'s own refusal writes, and the shape the next batch will reach
        # for -- explains its one figure and is about the refusal. Batch 113's
        # whole-argument rule reddened it, and the cheap way out was to print a corner
        # figure the line did not need, which buys back review-11 F1's wrong request as the
        # price of satisfying the gate. Review-10 F6 is the same trade and this family has
        # now made it twice. Batch 114 read that as "the line-level rule can only refuse a
        # line that disagrees with nothing" and deleted the rule outright; review-21 F2
        # proved it also refused a line naming NO unit word at all, so what survives is the
        # one form that cannot be satisfied by inventing a figure -- see below.
        slots = []
        used = 0
        for i in range(len(convs)):
            take = conv_arity(convs[i])
            hi = convs[i + 1].start() if i + 1 < len(convs) else len(fmt)
            slots.append((fmt[convs[i - 1].end() if i else 0:hi], vals[used + take - 1]))
            used += take
        # WHICH conversion a unit word belongs to: the NEAREST one (review-20 F4a,
        # open question 2). The text between two conversions names a unit for one of
        # them and "nearest" is the reading a person uses: in `%d corners at frame %lu`
        # the corners are the first slot's and the frame is the second's, and in
        # `corners %d = %d KB` the corners belong to the conversion they precede.
        # Batch 113 read only the text AFTER a conversion, because the wide window gave
        # `corners` to the KB slot in `%d corners = %d KB` and demanded the capacity
        # for a byte figure and a byte figure for the corner slot at once; nearest wins
        # both without a second window, and it refuses `corners %d, attempted %d = %d
        # KB` printed from the frame's corner count instead of accepting review-11 F1
        # on a spelling that only moves a word. A slot naming a byte unit is a BYTE
        # slot whatever else it names: a byte figure IS the capacity's byte count, so
        # demanding the bare capacity name there would refuse honest arithmetic.
        claims = [[] for _ in convs]
        for rx, kind in ((BYTE_UNIT, "byte"), (CORNER_UNIT, "corner")):
            for m in rx.finditer(fmt):
                gap = [max(0, c.start() - m.end(), m.start() - c.end()) for c in convs]
                claims[gap.index(min(gap))].append((kind, m.group(0)))
        figs = []
        for i, (ctx, v) in enumerate(slots):
            if v.lstrip().startswith('"'):
                continue                       # a string is not a quantity
            if any(k == "byte" for k, _ in claims[i]) or bare_literal(v):
                figs.append((any(k == "byte" and KB_UNIT.fullmatch(t)
                                for k, t in claims[i]),
                             arith_atoms(fold_consts(fold_locals(v, inits), consts),
                                         cap_names)))
        # Review-19 F1: a slot the format labels a CORNER figure must print the refused
        # capacity. The byte half was adjudicated and the corner half was not, so the
        # two figures of one line could disagree about which request was refused.
        for i, (ctx, v) in enumerate(slots):
            if any(k == "byte" for k, _ in claims[i]):
                continue
            if any(k == "corner" for k, _ in claims[i]) and cap_name(v) not in cap_names:
                fail_ok = False
                break
        # Review-21 F2: the whole-argument property, in the form that still bites. A line
        # whose format names a unit word is adjudicated slot by slot above. A line whose
        # format names NO unit word anywhere has no slot to adjudicate, and batch 114's
        # deletion of the line-level rule let it print anything: `"objects: refused %d at
        # frame %lu", objectCorners, (unsigned long)frame` is review-11 F1's 9-KB refusal
        # of an 80-KB request with the unit word taken out, and it went green with a full
        # `PASS Main3dsFlow`. What survives asks only that the capacity the backend was
        # refused be one of the arguments the line FILLS -- a dangling argument past the
        # last conversion prints nothing and cannot vouch for the line -- and it stays out
        # of the way of every honest line review-20 F2 was about, because each of those
        # names the unit its own figure is in.
        if not BYTE_UNIT.search(fmt) and not CORNER_UNIT.search(fmt):
            printed = [v for _, v in slots if not v.lstrip().startswith('"')]
            if printed and not any(cap_name(v) in cap_names for v in printed):
                fail_ok = False
                break
        # A line that NAMES a byte unit and ASKS FOR a figure must print a byte figure:
        # saying KB while printing the raw byte count is 1024x wrong, the same two-numbers
        # defect one unit out. STATED REACH (review-22 F4): the `convs` term is true at this
        # point for every line that gets here -- the skip above either returns (a format with
        # no percent at all) or fails the check (a percent it cannot read) -- so since batch
        # 115 this rule has NOT adjudicated a figure-less
        # format, and `"objects: linearAlloc FAILED, bytes refused"` (a byte word in prose,
        # no conversion) is green by design and pinned green by a probe. The reviewer's
        # suggested `if figs and BYTE_UNIT...` would be vacuous, since `figs` non-empty and
        # `not figs` cannot both hold. What the rule still refuses is a format that asks for
        # figures and derives none of them from the capacity, e.g. `("%s bytes", name)`.
        if convs and BYTE_UNIT.search(fmt) and not figs:
            fail_ok = False
        for kb, atoms in figs:
            if atoms != (kb_form(backend) if kb else backend):
                fail_ok = False
                break
        if not fail_ok:
            break

    else_sp = None
    enc = None
    if branch_sp is not None:
        after = code[branch_sp[1]:]
        rest = re.match(r"[ \t\r\n]*else[ \t]*\{", after)
        if rest is not None:
            esp = block_span_from(after, rest.end() - 1)
            if esp is not None:
                # Absolute, so check 20 can compare it against writes found anywhere
                # in the pass and not only inside this `else`.
                else_sp = (branch_sp[1] + esp[0], branch_sp[1] + esp[1])
                cl = re.search(r"streamFailureLogged[ \t]*=[ \t]*false[ \t]*;",
                               after[esp[0]:esp[1]])
                # The clear must be a DIRECT statement of the `else` block. One
                # wrapped in an inner condition does not run on every non-refusing
                # frame, which is review-14 F2's hazard on this half of the check.
                latch_ok = (cl is not None
                            and brace_depth(after, esp[0], esp[0] + cl.start()) == 1)
        enc = enclosing_block(code, at)
        if not latch_ok and enc is not None:
            m = re.search(
                r"streamFailureLogged[ \t]*=[ \t]*objectDraw\.allocationFailed\("
                r"[ \t]*\)[ \t]*;", code[enc[0]:at])
            # Review-14 F2: POSITION is not REACHABILITY. The accepted assignment
            # form is the same state machine only when it runs on EVERY frame, and
            # "somewhere in the enclosing block, before the test" does not say
            # that: a copy nested inside an inner condition (`if (objectCorners ==
            # 0) { ... }`, `if (capacity != last) { ... }`) satisfies a window
            # search, never clears on a frame that did not refuse, and silences
            # the second, larger refusal after a recovery -- review-11 F7 again,
            # with checks 18 and 19 both green. Depth 1 from the block's own '{'
            # is decidable in text and is what "runs on every frame" means here.
            latch_ok = (m is not None
                        and brace_depth(code, enc[0], enc[0] + m.start()) == 1)

    guarded = [block_span_from(scope, g.end() - 1)
               for g in re.finditer(r"\bif[ \t]*\(([^{}();]*)\)[ \t]*\{", scope)
               if LATCH_NEG.fullmatch(g.group(1).strip())]
    guarded = [sp for sp in guarded if sp is not None]
    once_ok = bool(prints) and all(
        any(sp[0] <= p.start() < sp[1] for sp in guarded) for p, _ in prints)
    # Review-14 F1: the guard is only HALF an edge trigger. Delete the arming
    # statement and the guard is ALWAYS true, so the line prints on every frame of a
    # sustained refusal -- the ~60/s spam review-10 F3 and review-12 R1 were raised
    # for -- while check 17 reads honest numbers and check 18 reads an untouched
    # clear. Nothing in the repository pinned the SET; the guard and the clear were
    # gated and the arming was not. So the block that guards a refusal line must also
    # ARM the latch, as a direct statement of that block. Its ORDER inside the block
    # is deliberately NOT pinned: print-then-arm and arm-then-print both print once,
    # and requiring one of them would redden a shape the port may write (review-10 F6).
    if once_ok:
        for p, _ in prints:
            armed = False
            for sp in guarded:
                if sp[0] <= p.start() < sp[1]:
                    s = re.search(r"streamFailureLogged[ \t]*=[ \t]*true[ \t]*;",
                                  scope[sp[0]:sp[1]])
                    armed = (s is not None and brace_depth(scope, sp[0],
                                                           sp[0] + s.start()) == 1)
                    break
            if not armed:
                once_ok = False
                break
    # Review-15 F1: the write SET. Checks 18 and 19 read the clear and the guard;
    # neither reads a THIRD write, so a second `streamFailureLogged = false;` --
    # one at the end of the refusing branch, one as a plain statement after the
    # if/else, one at the top of the frame loop -- clears the latch on the frame
    # that refused, the guard is true again next frame, and the refusal prints
    # ~60x/s with 17, 18 and 19 all green. Every write to the latch in the frame
    # must therefore be one of exactly the three shapes the state machine needs,
    # each a DIRECT statement (depth 1) of the block that makes it run when it
    # should:
    #   `= true`   in a latch-guarded block          -- the arming (check 19)
    #   `= false`  in the `else` sibling of the test -- the clear  (check 18)
    #   `= allocationFailed()` in the block enclosing the test, before it
    #                                              -- check 18's accepted form
    # A fourth write runs on a refusing frame by construction, and that is the
    # defect. The scope of the scan is therefore THE FRAME, not the pass: review-16
    # R1 found that a `streamFailureLogged = false;` written as the FIRST STATEMENT
    # of `while (aptMainLoop())` -- the natural "reset the diagnostic latch at frame
    # start so it cannot carry over" -- sat outside `object_pass()`, compiled clean
    # for armv6k, and printed one sustained refusal ~60x/s with all 20 checks, 32
    # arms and 12 probes green. The comment here used to justify the pass scope with
    # "writes outside the pass are not on the every-frame path", which is simply
    # false: the frame body IS the every-frame path, and the latch is declared in
    # `main()` before it, so every write in that body runs on the frames that
    # refused. The pass's own span is now just one of the regions the three pinned
    # shapes are allowed to sit in. A write outside the frame (in a respawn, say)
    # is still outside the net, and that half is still true: it does not run once a
    # frame. The pattern is newline-tolerant around `=` for the same reason: a write
    # wrapped as `streamFailureLogged\n    = false;` is the same statement, and a
    # scan that cannot cross the newline does not see it (review-16 R1's mutant b).
    # `(?!=)` keeps a COMPARISON out of the write set: `if (streamFailureLogged ==
    # false)` is the second spelling LATCH_NEG accepts, not a write, and reading it
    # as one reddened correct code -- the review-10 F6 failure, on this side.
    frame_sp = frame_body_span(code)
    wscope = code[frame_sp[0]:frame_sp[1]] if frame_sp else scope
    woff = frame_sp[0] if frame_sp else off
    # Review-16 open question 2, closed on this side too. The net enumerates writes by
    # the latch's NAME, so a reference alias -- `bool& latchAlias = streamFailureLogged;
    # latchAlias = false;` at the top of the frame -- is the same per-frame reset and is
    # invisible here, exactly as on the backend. Taking a reference (or a pointer) to a
    # field written in two places is a normal refactor, not an exotic attack, so the net
    # refuses to certify a write set it cannot enumerate instead of passing it. Both
    # spellings count: taking the address, and declaring a reference/pointer initialised
    # from the latch (the reviewer's mutant is the second, where the `&` sits on the
    # alias's name and never touches the field's).
    #
    # Review-17 R1: the SCOPE of this search is the whole file, not the frame body. The
    # alias was searched in `wscope`, which is the frame body, while the property this
    # half states is about the latch's name ANYWHERE -- and an alias is normally
    # declared where the field is, one line above the frame loop, and written inside it.
    # Batch 110's scan therefore missed the shape it existed for: an alias declared at
    # `main_3ds.cpp:666` and written as the frame's first statement printed one
    # sustained refusal ~60x/s with every check, arm and probe in this file green.
    # The c3d twin already scanned the whole file; the two nets now agree. (No count is
    # quoted here on purpose -- review-16 R3 and review-17 R4 are both about transcribed
    # counts going stale in the file that names them.)
    #
    # Review-18 F1: this was a BLACKLIST of two spellings -- `&latch` and `T& a = latch`
    # -- and each review has found a third: `bool& a(latch);` binds the same reference by
    # direct initialisation, and `clearRefusalLatch(streamFailureLogged);` writes it from
    # a helper without ever naming an alias at all, so no scan for alias SPELLINGS can
    # see that write. It is now a WHITELIST (LATCH_SHAPE): the latch's name may appear
    # only in its own declaration, the two reads the guard accepts, and the three pinned
    # writes, and any other occurrence reddens whatever it spells. That is deliberately
    # stricter than the defect -- a read-only `const bool&` view of the latch is refused
    # too, and the failure text says so, because the net cannot enumerate a write set
    # through a name it does not track.
    unenumerated = unpinned_uses(code, "streamFailureLogged", LATCH_SHAPE)
    guarded_abs = [(off + s, off + e) for (s, e) in guarded]
    writes_ok = True
    for w in re.finditer(r"\bstreamFailureLogged\s*=(?!=)\s*([^;]+);", wscope):
        rhs = re.sub(r"\s+", "", w.group(1))
        pos = woff + w.start()
        if rhs == "true":
            ok = any(ga <= pos < gb
                     and brace_depth(code, ga, pos) == 1 for ga, gb in guarded_abs)
        elif rhs == "false":
            ok = (else_sp is not None and else_sp[0] <= pos < else_sp[1]
                  and brace_depth(code, else_sp[0], pos) == 1)
        elif rhs == "objectDraw.allocationFailed()":
            ok = (enc is not None and enc[0] <= pos < at
                  and brace_depth(code, enc[0], pos) == 1)
        else:
            ok = False
        if not ok:
            writes_ok = False
            break
    if unenumerated:
        writes_ok = False
    if not writes_ok or not prints:
        writes_ok = False
    say(CHK["failure-line-names-the-refused-capacity"], fail_ok,
        "the pass rounds up to a power of two and refuses THAT size; the refused "
        "capacity must be an argument of the line, every slot its own format calls a "
        "corner figure must print THAT capacity and not the frame's corner count, "
        "every slot its own format calls a byte figure must print the backend's own "
        "capacity x stride x sizeof(float) (a KB slot divides by 1024), and no figure "
        "may be baked into the format text at all, because a figure with no argument "
        "behind it has no source to disagree with (review-18 F2/F3, review-19 F1/F2)")
    say(CHK["failure-latch-re-arms-on-success"], latch_ok,
        "a latch that can never clear is a one-shot log: the second, larger refusal "
        "is silent and the run reads as the first one")
    say(CHK["failure-line-prints-once-per-refusal"], once_ok,
        "an unguarded refusal line prints on every frame of one refusal, which is the "
        "log spam the edge trigger exists to stop")
    say(CHK["failure-latch-writes-are-the-pinned-three"], writes_ok,
        "a write to the latch beyond the arming, the `else` clear and the anchored "
        "assignment form clears the edge trigger on a frame that refused, and the "
        "refusal prints on every frame of it (review-15 F1). The net enumerates writes "
        "by the latch's NAME, so the name is refused outside those shapes ON PURPOSE: a "
        "reference or pointer to the latch -- even a read-only `const bool&` view that "
        "writes nothing today -- is a write this gate cannot see, and a per-frame reset "
        "through one prints the refusal ~60x/s (review-18 F1/F6)")
    return r


def adjudicate_named(label, text, quiet=False):
    """`adjudicate` with a crash turned into a NAMED failure (review-21 F1, review-22 F6).
    A check that raises IS fail-closed, but it tells nobody anything: it stops the checks
    after it, scores nothing, and the report is a traceback naming no property --
    review-15 F4's symptom, which this file has now been told about three times. Batch 115
    wrapped the mutants and left the shipped-source call bare, so the same class over the
    real file was still an unattributed traceback with no tally; the naming now covers
    every call, and the tally stays short either way. Returns None when it raised."""
    try:
        return adjudicate(text, quiet=quiet)
    except Exception as exc:  # noqa: the point is to NAME it, not to handle it
        print("  FAIL main_3ds flow %s: adjudicating it raised %s: %s, which names no "
              "property and leaves every later check unscored (review-21 F1, review-22 F6)"
              % (label, type(exc).__name__, exc))
        return None


real = adjudicate_named("the shipped source", SRC)
if real is None:
    sys.exit(1)

# --- mutation arms ----------------------------------------------------------
# Each reintroduces one of the defects the checks claim to prevent. An arm that
# cannot build its mutant is a FAIL, never a skip.

STEP_LOOP = step_loop_span(SRC)


def in_loop(mutate):
    """Splice a mutated loop body back into the file at the loop's own offsets."""
    def arm(text):
        sp = step_loop_span(text)
        if sp is None:
            return text
        a, b = sp
        return text[:a] + mutate(text[a:b]) + text[b:]
    return arm


def strip_all_set_solid(loop):
    """Remove every in-loop solid-pin site, runtime branch and geometric branch
    both. Deleting only one leaves the other refreshing the pin, so the mutant would
    test nothing."""
    out = re.sub(r"(?m)^[ \t]*if[ \t]*\([ \t]*runtime[ \t]*\)[ \t]*\{[ \t]*\n"
                 r"[ \t]*space\.setSolid\([ \t]*runtime->solidArenas\([ \t]*\)[ \t]*\);"
                 r"[ \t]*\n", "", loop)
    out = re.sub(r"(?m)^[ \t]*if[ \t]*\(solid\)[ \t]*\{[ \t]*\n"
                 r"[ \t]*space\.setSolid\([ \t]*\{[ \t]*\*solid[ \t]*\}[ \t]*\);[ \t]*\n[ \t]*\}[ \t]*\n",
                 "", out)
    out = re.sub(r"(?m)^[ \t]*if[ \t]*\([ \t]*![ \t]*runtime->currentArena\([ \t]*\)"
                 r"\.empty\([ \t]*\)[ \t]*\)[ \t]*\{[ \t]*\n"
                 r"[ \t]*space\.setSolid\([ \t]*runtime->currentArena\([ \t]*\)[ \t]*\);[ \t]*\n"
                 r"[ \t]*\}[ \t]*\n", "", out)
    return out


def arm_no_refresh(text):
    """The defect itself: the in-loop refresh deleted, the spawn pin left alone."""
    sp = STEP_LOOP
    if sp is None:
        return text
    a, b = sp
    loop = text[a:b]
    stripped = strip_all_set_solid(loop)
    if stripped == loop:
        return text
    return text[:a] + stripped + text[b:]


def arm_before_camera(text):
    """Refresh moved above follow.update: the arena the step STARTED in.

    Review-14 F3: the arm used to move only the `if (solid) { ... }` block, which
    leaves it above the `const std::string* solid = space.arenaAt(...)` that declares
    its own guard -- `'solid' was not declared in this scope`, a mutant that cannot
    compile and so proves nothing. The LOOKUP moves with the refresh, because that is
    the real regression: probing the arena before the camera's clearance instead of
    after it. The mutant now compiles and still places the pin before `follow.update`.
    It also has to move the block's leading comment, or the comment ends up describing
    a call that is no longer there."""
    sp = STEP_LOOP
    if sp is None:
        return text
    a, b = sp
    loop = text[a:b]
    m = re.search(r"(?m)^[ \t]*const[ \t]+std::string\*[ \t]+solid[ \t]*=[ \t]*\n"
                  r"[ \t]*space\.arenaAt\([^;]*;[ \t]*\n"
                  r"[ \t]*if[ \t]*\(solid\)[ \t]*\{[ \t]*\n"
                  r"[ \t]*space\.setSolid\([ \t]*\{[ \t]*\*solid[ \t]*\}[ \t]*\);[ \t]*\n[ \t]*\}[ \t]*\n",
                  loop)
    if not m:
        return text
    block = m.group(0)
    rest = loop[:m.start()] + loop[m.end():]
    p = re.search(r"(?m)^[ \t]*follow\.update\(", rest)
    if p is None:
        return text
    return text[:a] + rest[:p.start()] + block + rest[p.start():] + text[b:]


def arm_clears_the_pin(text):
    """The tidy-up that clears the pin when the drawn list resolves to nothing (no
    runtime has ticked yet, or Kurt is outside every arena): the same refresh hoisted
    out of its guard and placed before it, unguarded. It ADDS a site rather than
    replacing one, because replacing the guarded pair removes the shapes the other
    checks read and one of those would catch the mutant first -- which is the
    pass-generator shape this file's own arm table forbids."""
    sp = STEP_LOOP
    if sp is None:
        return text
    a, b = sp
    loop = text[a:b]
    m = re.search(r"(?m)^[ \t]*if[ \t]*\([ \t]*runtime[ \t]*\)[ \t]*\{[ \t]*\n"
                  r"[ \t]*space\.setSolid\([ \t]*runtime->solidArenas\([ \t]*\)[ \t]*\);",
                  loop)
    if not m:
        return text
    return text[:a] + loop[:m.start()] + \
        "                space.setSolid(runtime->solidArenas());\n" + \
        loop[m.start():] + text[b:]


def arm_drops_the_spawn_pin(text):
    """The spawn pin removed, leaving only the per-step refresh."""
    sp = body_span(text, "spawnPlayer")
    if sp is None:
        return text
    a, b = sp
    body = text[a:b]
    m = re.search(r"(?m)^[ \t]*if[ \t]*\(here\)[ \t]*\{[ \t]*\n"
                  r"[ \t]*space\.setSolid\([ \t]*\{[ \t]*\*here[ \t]*\}[ \t]*\);[ \t]*\n[ \t]*\}[ \t]*\n", body)
    if not m:
        return text
    return text[:a] + body[:m.start()] + body[m.end():] + text[b:]


def arm_guard_on_something_else(text):
    """The guard kept, but on a flag instead of on the lookup."""
    sp = STEP_LOOP
    if sp is None:
        return text
    a, b = sp
    loop = text[a:b]
    m = re.search(r"(?m)^[ \t]*if[ \t]*\(solid\)[ \t]*\{[ \t]*\n"
                  r"[ \t]*space\.setSolid\([ \t]*\{[ \t]*\*solid[ \t]*\}[ \t]*\);[ \t]*\n[ \t]*\}[ \t]*\n", loop)
    if not m:
        return text
    return text[:a] + loop[:m.start()] + \
        "                if (playerReady) {\n" \
        "                    space.setSolid({*solid});\n" \
        "                }\n" + loop[m.end():] + text[b:]


def arm_scripts_tick_deleted(text):
    """The runtime is still built and its sounds still reach the mixer, but nothing
    ever ticks it: the level's objects never spawn and its scripts never run."""
    mutated, n = re.subn(r"(?m)^[ \t]*runtime->update\(step\);[ \t]*\n", "", text)
    return mutated if n else text


def arm_scripts_tick_after_camera(text):
    """The tick kept, but placed after the camera step: the camera is driven by the
    previous tick's arena, camera_track request and shake."""
    m = re.search(r"(?m)^([ \t]*)(if[ \t]*\([ \t]*runtime[ \t]*\)[ \t]*\{[ \t]*\n"
                  r"[ \t]*runtime->update\(step\);[ \t]*\n[ \t]*\}[ \t]*\n)", text)
    if m is None:
        return text
    rest = text[:m.start()] + text[m.end():]
    p = re.search(r"(?m)^[ \t]*follow\.update\([^;]*;[ \t]*\n", rest)
    if p is None:
        return text
    return rest[:p.end()] + m.group(2) + rest[p.end():]


def arm_mixes_standalone(text):
    """The C#'s one-mixer shape abandoned: the frame goes back to the standalone
    mixer, so every script sound is written into a device no sink reads."""
    mutated = text.replace("mix->listenFromCamera(camera);", "mixer.listenFromCamera(camera);")
    mutated = mutated.replace("mix->update(elapsed);", "mixer.update(elapsed);")
    mutated = mutated.replace("mix->device().update(elapsed);", "mixer.device().update(elapsed);")
    return mutated if mutated != text else text


def arm_sink_not_reattached(text):
    """The respawn rebuilds the runtime — and with it the mixer and its device — but
    leaves the sink on the device the old runtime owned."""
    sp = body_span(text, "spawnPlayer")
    if sp is None:
        return text
    a, b = sp
    body = text[a:b]
    mutated, n = re.subn(r"(?m)^[ \t]*mix->device\(\)[ \t]*\.setSink\([^;]*;[ \t]*\n", "", body)
    if not n:
        return text
    return text[:a] + mutated + text[b:]


def arm_music_outlives_the_runtime(text):
    """The respawn destroys the runtime -- and its mixer -- while the LevelMusic that
    holds two of that mixer's voices is still alive."""
    sp = body_span(text, "spawnPlayer")
    if sp is None:
        return text
    a, b = sp
    mutated, n = re.subn(r"(?m)^[ \t]*music\.reset\(\);[^\n]*\n", "", text[a:b], count=1)
    if not n:
        return text
    return text[:a] + mutated + text[b:]


def arm_music_on_the_standalone_mixer(text):
    """The music is built on the frontend's own mixer, which nothing mixes once the
    runtime exists: the level's scripts are heard and its music is not."""
    return text.replace("new mdk::audio::LevelMusic(&runtime->mixer(),",
                        "new mdk::audio::LevelMusic(&mixer,", 1)


def arm_music_fades_before_it_enters(text):
    """update() before enter(): the fade of a frame is applied to the track of the
    arena Kurt has just left, and a new track starts a step late."""
    enter = "                    music->enter(runtime->currentArena());\n"
    fade = "                    music->update(step);\n"
    if enter not in text or fade not in text:
        return text
    text = text.replace(fade, "", 1)
    return text.replace(enter, fade + enter, 1)


def arm_music_never_prefetched(text):
    """The loading arena's track is asked for, and the bank is never stepped: every
    arena's music is then read whole on the step Kurt walks in (a stall each)."""
    return text.replace("const bool more = bank.prefetchStep();",
                        "const bool more = false;", 1)


def arm_loading_arenas_track_is_released(text):
    """The once-a-second release keeps the playing and the fading track and forgets
    the one that was prefetched for the arena Kurt is walking towards."""
    return text.replace(
        "streamSecond.empty() ? std::string() : music->trackOf(streamSecond),",
        "std::string(),", 1)


def arm_no_asset_gate(text):
    """The gate replaced by `if (true)`: the runtime is built whatever the level
    loaded, which is the different-random-stream case the header forbids."""
    mutated, n = re.subn(r"if[ \t]*\([ \t]*scriptAssetsReady[ \t]*\)[ \t]*\{",
                         "if (true) {", text)
    return mutated if n else text


def arm_pin_from_the_runtime(text):
    """Batch 99's runtime pin, put back: the step loop reads the runtime's current
    arena instead of the arena under Kurt. The crossing is ported now, so this is no
    longer the spawn arena forever -- but it is still ONE name where the oracle pins
    the drawn LIST (current + active second), so the room behind the open door is not
    solid and a door walk falls through it (review-6 F1)."""
    mutated, n = re.subn(
        r"(?m)^[ \t]*const[ \t]+std::string\*[ \t]+solid[ \t]*=[ \t]*\n"
        r"[ \t]*space\.arenaAt\([^;]*;[ \t]*\n"
        r"[ \t]*if[ \t]*\([ \t]*solid[ \t]*\)[ \t]*\{[ \t]*\n"
        r"[ \t]*space\.setSolid\([ \t]*\{[ \t]*\*solid[ \t]*\}[ \t]*\);[ \t]*\n[ \t]*\}[ \t]*\n",
        "                if (!runtime->currentArena().empty()) {\n"
        "                    space.setSolid(runtime->currentArena());\n"
        "                }\n", text)
    return mutated if n else text


def arm_frozen_pin_added(text):
    """Batch 99's runtime pin put back ALONGSIDE the geometric refresh, and placed
    AFTER it, so it is the last writer and the pin ends up the runtime's single name
    whatever the geometric probe found -- which drops the oracle's active second.
    Review-7 F2: the arm above, which REPLACES the geometric block, trips check 5
    first (the guarded `arenaAt` shape is simply gone), so check 6 had no mutation
    that isolated it and could be stubbed to a pass generator without the suite
    noticing. This mutant leaves check 5's shape intact, so only check 6 can catch
    it."""
    sp = STEP_LOOP
    if sp is None:
        return text
    a, b = sp
    loop = text[a:b]
    m = re.search(r"(?m)^[ \t]*if[ \t]*\(solid\)[ \t]*\{[ \t]*\n"
                  r"[ \t]*space\.setSolid\([ \t]*\{[ \t]*\*solid[ \t]*\}[ \t]*\);[ \t]*\n[ \t]*\}[ \t]*\n", loop)
    if not m:
        return text
    return text[:a] + loop[:m.end()] + \
        "                if (!runtime->currentArena().empty()) {\n" \
        "                    space.setSolid(runtime->currentArena());\n" \
        "                }\n" + loop[m.end():] + text[b:]


def arm_pitch_ignores_runtime(text):
    """The pitch target back to the geometric arena even when a runtime exists: the
    scripts' camera_track request and their arena both ignored."""
    sp = STEP_LOOP
    if sp is None:
        return text
    a, b = sp
    loop = text[a:b]
    mutated, n = re.subn(
        r"if[ \t]*\(runtime\)[ \t]*\{[ \t]*\n[ \t]*target[ \t]*=[ \t]*\n?[ \t]*runtime->cameraTrackTicks"
        r"[^;]*;[ \t]*\n[ \t]*\}[ \t]*(?:\n[ \t]*)?else[ \t]*\{", "if (false) {", loop)
    if not n:
        return text
    return text[:a] + mutated + text[b:]


# --- the object pass's arms -------------------------------------------------
# RETIRED 2026-11-24: "views synced before the stream is stepped". ArenaStream was
# removed by design (4fea3380, 2026-11-22: its paced SD reads were the transition
# stutter), so there is no stream step left for the sync to precede. The two arms
# below take its place on what the check still pins: the sync before the frame, and
# the sync on the drawn list.
def arm_views_synced_inside_the_frame(text):
    """The views are synced after mdk_render_begin_frame: the sync makes and releases
    GPU resources (a view's meshes, its textures), so inside the frame it frees what
    the previous frame's commands may still read and builds under the GPU (AGENTS.md
    P3). Written to compile: the pair is re-read in the moved block, and the counter
    the frame logs is left at zero."""
    old = ("            const int32_t builtViews =\n"
           "                arenaViews.sync(streamCurrent, streamSecond, *frameDrawn, "
           "bubbleHints);\n")
    begin = "        mdk_render_begin_frame(renderer, &params);\n"
    if old not in text or begin not in text:
        return text
    text = text.replace(old, "            const int32_t builtViews = 0;\n", 1)
    return text.replace(
        begin, begin +
        "        if (arena) {\n"
        "            const std::string& streamCurrent =\n"
        "                (runtime && !runtime->currentArena().empty()) ? "
        "runtime->currentArena()\n"
        "                                                              : startArenaName;\n"
        "            const std::string streamSecond = runtime ? runtime->secondArena() "
        ": std::string();\n"
        "            arenaViews.sync(streamCurrent, streamSecond, *frameDrawn, bubbleHints);\n"
        "        }\n", 1)


def arm_views_synced_on_a_second_list(text):
    """The views are synced on the start arena's list while the frame draws the
    scripts' list: a view the frame draws is released, or never built, after a
    scripted teleport (the "second list" the check names)."""
    return text.replace(
        "arenaViews.sync(streamCurrent, streamSecond, *frameDrawn, bubbleHints);",
        "arenaViews.sync(streamCurrent, streamSecond, startDrawn, bubbleHints);", 1)


def arm_drawn_list_is_the_start_arena(text):
    """The frame's list is Kurt's start arena whatever the scripts say: the
    pre-Phase-2 frontend, where nothing past the first door was ever drawn. The
    stream and the views still run in order, so only the list's source can tell."""
    return text.replace("frameDrawn = scriptsDrew ? &runtime->drawnArenas() : &startDrawn;",
                        "frameDrawn = &startDrawn;", 1)


def arm_objects_before_arena(text):
    """Move the whole object pass above the arena draw. The pass still runs and
    still skips what it should, so only the ORDER check can catch it -- which is
    the point: the C#'s order is the picture, not an implementation detail.

    Review-14 F3: this arm used `text.find("if (arena) {")`, which is the FIRST of the
    three `if (arena)` blocks in the file -- the one at :265, above the declaration of
    `runtime` -- so the mutant moved the pass into a scope where its own identifiers
    do not exist and did not compile. It now anchors on the arena block INSIDE the
    frame, and moves the pass's header with its body: the old splice left `if
    (runtime)` glued to whatever followed the pass, which is not a program either."""
    op = object_pass(text)
    if op is None:
        return text
    frame = frame_body_span(text)
    if frame is None or not (frame[0] <= op["start"] < frame[1]):
        return text
    hdr_at = op["start"] + 1 - len(op["header"])
    block = text[hdr_at:op["end"]]
    arena_at = text.rfind("if (arena) {", frame[0], hdr_at)
    if arena_at < 0:
        return text
    rest = text[:hdr_at] + text[op["end"]:]
    return rest[:arena_at] + block + "\n\n        " + rest[arena_at:]


def arm_objects_ungated(text):
    """Ungate the pass. `if (true)` keeps the block, its skips and its order, so
    only the gate check can see it -- and a level whose scripts never started
    (check 10's asset gate) is exactly the shipped case where runtime is null.

    Review-14 F3: the splice used to cut one character too early and re-emit the
    block's own `{`, so the mutant was `if (true) {{` -- unbalanced braces, an
    `expected '}' at end of input`, and a mutant of nothing by this file's own
    standard. The header now spans [start - len(header) + 1, start], because `start`
    IS the `{` the header ends with."""
    op = object_pass(text)
    if op is None or not re.fullmatch(r"if[ \t]*\([ \t]*runtime[ \t]*\)[ \t]*\{",
                                      op["header"]):
        return text
    hdr = op["start"] + 1 - len(op["header"])
    return text[:hdr] + "if (true) {" + text[op["start"] + 1:]


def arm_objects_draws_removed(text):
    """Drop the skip, so the deque's removed objects are drawn. The C# has already
    unlisted them; this port keeps them in place for pointer stability."""
    mutated, n = re.subn(
        r"if[ \t]*\([ \t]*![ \t]*obj\.visible[ \t]*\|\|[ \t]*runtime->isRemoved"
        r"\(&obj\)[ \t]*\)[ \t]*\{[ \t]*\n[ \t]*continue[ \t]*;[ \t]*\n[ \t]*\}[ \t]*\n",
        "", text)
    return mutated if n else text


def arm_objects_drawn_twice(text):
    """Draw the frame's object stream a SECOND time, after Kurt (review-9 F2). The
    first call site keeps its place, its gate and its skips, so only a count can see
    the copy -- this is the reviewer's own mutant, which the 15-check file left green.
    It is inserted before the frame end so it is inside the frame body, where a real
    'draw the other arena too' edit would go."""
    op = object_pass(text)
    if op is None:
        return text
    at = text.find("mdk_render_end_frame(renderer);")
    if at < 0 or at < op["end"]:
        return text
    copy = ("\n        if (runtime) {\n            objectDraw.draw(renderer, &surfaces, "
            "objectStream);\n        }\n")
    return text[:at] + copy + text[at:]


def arm_failure_corner_slot_names_the_frame_count(text):
    """Substitute ONLY the corner slot: the line still prints the refused capacity's
    byte figure, so the byte half is honest, but the `%d corners` slot prints the
    frame's own corner count (review-19 F1). Batch 112's capacity rule accepted the
    capacity appearing as an atom INSIDE a byte figure, which every honest KB figure
    satisfies, so the corner slot was not adjudicated at all and this line read
    `480 corners = 80 KB` -- a refusal of a request that was never made -- with
    `PASS Main3dsFlow`. The reviewer's mutant; no existing arm covers this shape,
    because every other arm that changes the corner figure also changes the byte
    figure and so reds through the byte half instead."""
    return text.replace("""                                      (int)attempted,
                                      (int)((int64_t)attempted * 5 * sizeof(float) /""",
                        """                                      (int)objectCorners,
                                      (int)((int64_t)attempted * 5 * sizeof(float) /""",
                        1)


def arm_failure_corner_slot_names_a_second_frame_count(text):
    """Keep the refused capacity as a whole argument, ADD a slot, and give that slot
    the frame's corner count (review-20 F1). Batch 113's whole-argument rule was
    satisfied by the first slot and its byte rule was satisfied by the third, so the
    only thing that can refuse this is the corner rule -- and review-20 proved no arm
    isolated it: deleting the corner loop left `PASS Main3dsFlow` byte-identical to
    shipped. Reproduced here before the fix, on the reviewer's own shape."""
    return text.replace('mdk::log3dsPrintf("objects: stream mesh of %d corners = %d KB "',
                        'mdk::log3dsPrintf("objects: stream mesh refused %d of %d '
                        'corners = %d KB "', 1).replace("""                                      (int)attempted,
                                      (int)((int64_t)attempted * 5 * sizeof(float) /""",
                        """                                      (int)attempted, (int)objectCorners,
                                      (int)((int64_t)attempted * 5 * sizeof(float) /""", 1)


def arm_failure_corner_unit_written_before_its_conversion(text):
    """`corners %d` instead of `%d corners`, with the frame's corner count in that slot
    (review-20 F4a, open question 2). The unit sits BEFORE its conversion, which is
    the one spelling batch 113's corner rule said it did not claim -- and while it
    said so, review-11 F1's refusal of a request that was never made was legal again
    on a spelling that only moves a word. The rule now attributes a unit word to the
    conversion nearest it, so this is refused and the honest version of the same
    spelling is a probe."""
    return text.replace('mdk::log3dsPrintf("objects: stream mesh of %d corners = %d KB "',
                        'mdk::log3dsPrintf("objects: stream mesh refused: corners %d, '
                        'attempted %d = %d KB "', 1).replace("""                                      (int)attempted,
                                      (int)((int64_t)attempted * 5 * sizeof(float) /""",
                        """                                      (int)objectCorners, (int)attempted,
                                      (int)((int64_t)attempted * 5 * sizeof(float) /""", 1)


def probe_failure_corner_unit_written_before_its_conversion(text):
    """The same spelling, honest: the slot the format labels `corners` prints the
    refused capacity (review-20 F4a). Claiming a unit word wherever it sits must not
    mean refusing the line that names the unit before its conversion and prints the
    right number there -- that is review-10 F6's failure, a gate that rejects a shape
    the port may write and so gets routed around."""
    return text.replace("""                                            1024));
                }""",
                        """                                            1024));
                    mdk::log3dsPrintf("objects: refused corners %d = %d KB linear",
                                      attempted,
                                      (int)((int64_t)attempted * 5 * sizeof(float) /
                                            1024));
                }""", 1)


def probe_failure_line_prints_only_the_byte_figure(text):
    """A refusal line that prints ONLY the byte figure derived from the refused
    capacity, with no corner figure at all (review-20 F2). This is the shape the
    backend's own `linearAlloc(%u) FAILED for a dynamic mesh of %d corners` writes,
    and the capacity explains its one figure as an atom of it, so the line IS about
    the refusal. Batch 113's whole-argument rule reddened it; the cheapest way to
    satisfy that rule was to print a corner figure the line never needed, which is
    review-11 F1's wrong request bought with the gate's own PASS. Reproduced on the
    reviewer's own mutant before the rule was deleted."""
    return text.replace("""                                            1024));
                }""",
                        """                                            1024));
                    mdk::log3dsPrintf("objects: stream mesh linearAlloc FAILED, %d KB "
                                      "refused",
                                      (int)((int64_t)attempted * 5 * sizeof(float) /
                                            1024));
                }""", 1)


def probe_failure_line_names_a_unit_in_prose_but_prints_no_figure(text):
    """A refusal line whose FORMAT spells a byte unit while printing no figure at all,
    with an argument left behind (review-21 F1): the edit a batch makes when it deletes a
    conversion and forgets the argument. The line prints no number, so it disagrees with
    nothing, and a format that asks for no argument has no slot to adjudicate -- the old
    code fell through to a slot loop over zero conversions and killed the script with
    `min() iterable argument is empty` after 16 checks, scoring no arm, no probe and no
    property. This probe is what pins the skip: delete it and the probe dies."""
    return text.replace("""                                            1024));
                }""",
                        """                                            1024));
                    mdk::log3dsPrintf("objects: linearAlloc FAILED, bytes refused",
                                      attempted);
                }""", 1)


def probe_failure_capacity_wrapped_deeply_in_parens(text):
    """The refused capacity wrapped in more parens than the old `for _ in range(6)` could
    unwrap (review-21 F3). `(((((((attempted))))))))` IS the capacity at any depth, and a
    bound dressed up as a fixpoint reddened this honest line while letting a stale figure
    wrapped that deep walk through the literal rule. Contrived spelling, real property:
    `bare_literal` and `cap_name` read the same unwrap."""
    return text.replace("""                                            1024));
                }""",
                        """                                            1024));
                    mdk::log3dsPrintf("objects: refused request was %d corners",
                                      (((((((attempted))))))));
                }""", 1)


def probe_failure_unit_less_line_prints_the_capacity(text):
    """The ACCEPT half of the review-21 F2 rule (review-22 F3): a refusal line whose format
    names no unit word anywhere and prints the refused capacity in the one slot it has. It
    disagrees with nothing -- the number is the capacity the backend was actually handed --
    so a gate that reddens it is review-10 F6's failure again. Nothing pinned this half:
    a mutant that refused EVERY unit-less line printing a figure kept 54/54 arms and 22/22
    probes while reddening exactly this line."""
    return text.replace("""                                            1024));
                }""",
                        """                                            1024));
                    mdk::log3dsPrintf("objects: linearAlloc FAILED, %d refused",
                                      attempted);
                }""", 1)


def arm_failure_star_width_names_no_argument(text):
    """A `%*d` conversion the call cannot supply (review-19 F2). The star reads a
    width from an argument, so this call asks for two and passes one: the line prints
    stack garbage and `log3dsPrintf` carries no format attribute to catch it. The
    old `len(convs) > len(vals)` test counted one conversion and one argument and
    called that balanced."""
    anchor = "                                            1024));\n"
    if anchor not in text:
        return text
    return text.replace(anchor, anchor +
                        '                mdk::log3dsPrintf("objects: refused %*d bytes",\n'
                        '                                          (int)((int64_t)attempted\n'
                        '                                                * 5 * sizeof(float)));\n',
                        1)


def probe_failure_star_width_supplies_its_width(text):
    """The same conversion, legal: the width IS supplied, so `%*d` consumes two
    arguments and the value it prints is the second (review-19 F2). Adjudicating
    `24` as a byte figure would be review-10 F6's failure -- a gate that rejects a
    shape the port may write gets routed around, and a column width is not a
    quantity."""
    return text.replace('mdk::log3dsPrintf("objects: stream mesh of %d corners = %d KB "',
                        'mdk::log3dsPrintf("objects: stream mesh of %*d corners = %d KB "',
                        1).replace("""                                      (int)attempted,
                                      (int)((int64_t)attempted * 5 * sizeof(float) /""",
                        """                                      24, (int)attempted,
                                      (int)((int64_t)attempted * 5 * sizeof(float) /""",
                        1)


def arm_stale_literal(spelling):
    """A stale figure in ONE spelling, in a slot whose format names no unit at all
    (review-19 F4, review-20 F4c). Each spelling is its own arm because "the rule
    catches the spellings" is precisely the claim both findings falsified: the first
    version caught `81920` alone, batch 113 added the suffix, the hex and the cast,
    and review-20 F4c caught the paren, the sign and the float. A figure with no
    source is one defect, so the test is one unwrap to a fixpoint, and one arm per
    spelling is what keeps that honest rather than aspirational."""
    def arm(text):
        return text.replace("""                                            1024));
                }""",
                            """                                            1024));
                    mdk::log3dsPrintf("objects: refused %d corners at frame %lu",
                                      attempted, SPELLING);
                }""".replace("SPELLING", spelling), 1)
    return arm


arm_failure_suffixed_literal_names_no_source = arm_stale_literal("81920u")

# Review-21 F3: the same defect at a depth the old `for _ in range(6)` could not reach.
# Seven nested parens is not a new defect, it is the old one wearing a seventh paren, and
# the bound was invisible until someone counted.
arm_failure_deeply_parenthesised_literal_names_no_source = arm_stale_literal(
    "(((((((81920)))))))")


def arm_failure_unit_less_line_names_the_frame_count(text):
    """A refusal line that names NO unit word anywhere and prints the frame's corner
    count (review-21 F2). Batch 114 deleted the whole-argument rule to let the honest
    byte-only line through, and the deletion also let THIS through: no slot of
    `"objects: refused %d at frame %lu"` names a byte or a corner unit, so the slot rules
    had nothing to say about the 480 in the first slot and the line printed a 9 KB refusal
    of an 80 KB request with `PASS Main3dsFlow (20 checks, 50 arms, 20 probes)`. The
    reviewer's mutant, reproduced over the shipped gate before the rule was restored in
    its narrowed form."""
    return text.replace("""                                            1024));
                }""",
                        """                                            1024));
                    mdk::log3dsPrintf("objects: refused %d at frame %lu",
                                      objectCorners, (unsigned long)frame);
                }""", 1)


def arm_failure_unit_less_line_names_no_capacity(text):
    """The same defect with the frame slot taken off: one slot, no unit word, and the
    frame's corner count where the refused capacity belongs (review-21 F2, the reviewer's
    second mutant). `"... stream mesh refused %d", objectCorners` names neither a byte
    nor a corner unit, so only the line-level rule can refuse it, and it is the shape a
    batch writes when it wants to say the request was refused and never asks which
    request."""
    return text.replace("""                                            1024));
                }""",
                        """                                            1024));
                    mdk::log3dsPrintf("objects: stream mesh refused %d",
                                      objectCorners);
                }""", 1)


def arm_failure_unit_less_line_hides_the_capacity_past_the_last_conversion(text):
    """The refused capacity is in the call and printed by NOTHING (review-22 F2). Same
    unit-less format as review-21 F2's defect, with `attempted` parked after the last
    conversion, where no conversion can reach it and the line still prints only the frame's
    corner count. The shipped gate refuses it because the rule reads the slots the format
    FILLS; a gate mutant that reads the argument list instead saw the dangling `attempted`,
    called the line honest and passed 54/54 arms and 22/22 probes -- the clause batch 115's
    record said carried the weight was pinned by nothing, which is this file's own
    review-21 F1 failure one round later."""
    return text.replace("""                                            1024));
                }""",
                        """                                            1024));
                    mdk::log3dsPrintf("objects: refused %d at frame %lu",
                                      objectCorners, (unsigned long)frame, attempted);
                }""", 1)


def arm_failure_space_flag_conversion_names_a_byte_unit(text):
    """A space-flagged conversion in a slot its own text calls a byte figure
    (review-22 F1). `FIGURE_FMT` did not match the space flag -- its own stated limit --
    so `("objects: refused % d bytes", objectCorners)` looked like a format that asks for
    nothing and was skipped by EVERY figure rule, the byte-unit backstop included. But
    `vsnprintf` DOES print the space flag (log_3ds.cpp:79), so the line really does report a
    9 KB refusal of an 80 KB request with `PASS Main3dsFlow`. Batch 113 reddened it and batch
    114 crashed on it.

    Batch 117 put the space flag in the flag class, so this is now refused because a byte
    slot prints a corner count -- on its merits, the way every other wrong-figure line is
    refused -- and not because the gate cannot parse it. The accept side of the same flag
    is a probe (`a refusal line prints the refused capacity's byte figure with a space
    flag`): the flag class is pinned from both sides, and the unreadable-percent refusal is
    what is left behind it for a conversion neither C nor this gate has."""
    return text.replace("""                                            1024));
                }""",
                        """                                            1024));
                    mdk::log3dsPrintf("objects: refused % d bytes", objectCorners);
                }""", 1)


def arm_failure_line_names_the_frame_count(text):
    """Print the frame's corner count on the refused-mesh line (review-11 F1). The
    line still prints, still says FAILED, and still names KB -- it just names the
    480 corners the frame held instead of the 4096 the backend was handed, which is
    the defect: an allocation that was never attempted."""
    return text.replace("objectDraw.attemptedCapacity()", "objectCorners")


def arm_failure_kb_names_the_frame_count(text):
    """Keep the corner figure honest and build the BYTE figure from the frame's corner
    count (review-12 R2). The first version of check 17 stayed green on exactly this:
    the refused capacity still appeared in the line, so the check read it and passed
    while the KB disagreed with renderer_c3d's own byte count by 8x -- one refusal,
    two committed numbers, and a gate that could not see the disagreement."""
    mutated, n = re.subn(r"\(int64_t\)[ \t]*attempted[ \t]*\*[ \t]*5",
                         "(int64_t)objectCorners * 5", text)
    return mutated if n else text


def arm_failure_line_prints_every_frame(text):
    """Widen the edge trigger to nothing (review-12 R1). The clear assignment stays
    exactly where it is, so a check that only looks for the assignment -- which is
    what check 18 used to be -- stays green while the refusal line prints on every
    frame of a sustained refusal, which is the ~60/s log spam the trigger exists to
    stop. Only a check on the GUARD can see this, so it has its own."""
    mutated, n = re.subn(r"\bif[ \t]*\([ \t]*![ \t]*streamFailureLogged[ \t]*\)[ \t]*\{",
                         "if (true) {", text)
    return mutated if n else text


def arm_failure_guard_compound_condition(text):
    """Make the guard MENTION the latch instead of testing it (review-13 D3). A later
    batch adds a debug disjunct that defaults true; the refusal prints on every frame
    again and a substring test on the condition cannot see it, because the substring it
    looks for is still there. Only a check that requires the condition to BE the
    negative latch test can see this one."""
    mutated, n = re.subn(r"\bif[ \t]*\([ \t]*![ \t]*streamFailureLogged[ \t]*\)[ \t]*\{",
                         "if (!streamFailureLogged || true) {", text)
    return mutated if n else text


def arm_failure_second_line_names_the_frame_count(text):
    """Add a SECOND refusal line to the branch, built from the frame's corner count
    (review-13 D1). The guarded line stays exactly as committed, so a check that reads
    only the first `log3dsPrintf(` after the test reads that one and passes while this
    line puts review-11 F1's 9-KB-for-an-80-KB disagreement back on the console.

    Review-15 F3: the format deliberately does NOT spell FAILED or "refus". Batch
    108 scoped the family by format string, so a copy of this arm whose format named
    the refusal was caught by the family filter and proved nothing about the
    branch-level rule it claimed to test; this one is only visible to a check that
    reads every printf inside the refusing branch, whatever it says.
    """
    at = text.find("if (objectDraw.allocationFailed()) {")
    if at < 0:
        return text
    sp = block_span_from(text, text.index("{", at))
    copy = ('                mdk::log3dsPrintf("objects: stream mesh of %d corners "\n'
            '                                  "= %d KB linear",\n'
            '                                  objectCorners,\n'
            '                                  (int)((int64_t)objectCorners * 5\n'
            '                                        * sizeof(float) / 1024));\n')
    return text[:sp[1] - 1] + copy + text[sp[1] - 1:]


def arm_failure_second_line_unguarded(text):
    """Add a second refusal line that names the refused capacity CORRECTLY but sits
    outside the edge trigger (review-13 D1, the half that isolates check 19). Check 17
    is satisfied by both lines, so only a check that requires EVERY printf in the
    refusing branch to sit inside the latch-guarded block sees the ~60/s spam. It
    compiles: `attempted` is scoped to the guarded block, so this one re-reads the
    accessor."""
    at = text.find("if (objectDraw.allocationFailed()) {")
    if at < 0:
        return text
    sp = block_span_from(text, text.index("{", at))
    copy = ('                mdk::log3dsPrintf("objects: refused mesh of %d corners "\n'
            '                                  "= %d KB linear — linearAlloc FAILED",\n'
            '                                  objectDraw.attemptedCapacity(),\n'
            '                                  (int)((int64_t)objectDraw.attemptedCapacity()\n'
            '                                        * 5 * sizeof(float) / 1024));\n')
    return text[:sp[1] - 1] + copy + text[sp[1] - 1:]


def arm_failure_kb_wrong_stride(text):
    """Get the corner stride wrong by one (review-13 D4). `* 4` instead of `* 5`: the
    identifiers are all still the refused capacity's, so the identifier rule batch 106
    added is satisfied and the byte figure still disagrees with the backend's own
    `linearAlloc(N)` -- 64 KB printed for an 80 KB refusal. Only comparing the FACTORS
    to the renderer's expression sees it."""
    mutated, n = re.subn(r"attempted[ \t]*\*[ \t]*5", "attempted * 4", text)
    return mutated if n else text


def arm_failure_latch_set_inside_branch(text):
    """Re-arm the latch from INSIDE the refusing branch (review-13 D2). The accepted
    assignment form is the same state machine only when it runs on every frame; placed
    here it sets the latch on a refusal and can never clear it, so a second, larger
    refusal after a recovery is silent -- review-11 F7 exactly. A check that searches
    the whole file for the assignment accepts this; only an anchored one refuses it."""
    mutated, n = re.subn(r"\}[ \t]*else[ \t]*\{[ \t]*\n[ \t]*streamFailureLogged"
                         r"[ \t]*=[ \t]*false;[ \t]*(?://[^\n]*)?\n[ \t]*\}[ \t]*\n", "}\n",
                         text)
    if not n:
        return text
    mutated, n2 = re.subn(r"(if \(objectDraw\.allocationFailed\(\)\) \{\n)",
                          r"\1                streamFailureLogged = "
                          r"objectDraw.allocationFailed();\n", mutated)
    return mutated if n2 else text


def arm_failure_latch_never_rearms(text):
    """Latch the refused-mesh line for the whole run (review-11 F7). The first
    refusal still prints exactly once, so nothing about the first failure changes;
    what disappears is every refusal after a recovery.

    The clear is matched with its optional trailing comment, because the arm that
    cannot build its mutant is a FAIL and a comment on that line is legitimate code
    (review-12 R1's other half: the exact-text version reddened the suite for a
    comment, which is a gate rejecting a shape the port may write)."""
    mutated, n = re.subn(r"\}[ \t]*else[ \t]*\{[ \t]*\n[ \t]*streamFailureLogged"
                         r"[ \t]*=[ \t]*false;[ \t]*(?://[^\n]*)?\n[ \t]*\}[ \t]*\n", "}\n",
                         text)
    return mutated if n else text


def arm_failure_latch_never_arms(text):
    """Delete the arming statement (review-14 F1). The guard `if
    (!streamFailureLogged)` is then ALWAYS true, so one sustained refusal prints on
    every frame -- the ~60/s spam review-10 F3 was raised about -- while the printed
    numbers stay honest (check 17 green) and the clear stays exactly where it is
    (check 18 green). Only a check that reads the SET can see this, and until
    review-14 nothing in the repository did."""
    mutated, n = re.subn(r"[ \t]*streamFailureLogged[ \t]*=[ \t]*true;[ \t]*\n", "", text)
    return mutated if n else text


def arm_failure_latch_clear_unreachable(text):
    """Re-arm the latch from inside an inner condition (review-14 F2). The accepted
    assignment form is placed before the test, so a window search over the enclosing
    block accepts it -- but it is nested in `if (objectCorners == 0)`, and
    `objectCorners` is nonzero on every frame that draws objects, so the latch
    NEVER clears and the second, larger refusal after a recovery is silent: review-11
    F7 exactly. Position is not reachability; only a depth test separates this from
    the probe that writes the same statement as a direct member of the block."""
    mutated, n = re.subn(r"\}[ \t]*else[ \t]*\{[ \t]*\n[ \t]*streamFailureLogged"
                         r"[ \t]*=[ \t]*false;[ \t]*(?://[^\n]*)?\n[ \t]*\}[ \t]*\n", "}\n",
                         text)
    if not n:
        return text
    mutated, n2 = re.subn(r"(if \(objectDraw\.allocationFailed\(\)\) \{\n)",
                          r"\1                if (objectCorners == 0) {\n"
                          r"                    streamFailureLogged = "
                          r"objectDraw.allocationFailed();\n                }\n", mutated)
    return mutated if n2 else text


def arm_failure_second_line_sibling_branch(text):
    """Add a refusal line in a SIBLING condition outside the refusing branch
    (review-14 F7). Its numbers are honest -- the refused capacity, the backend's own
    factors, KB named -- so check 17 passes it, and the committed guarded line is
    untouched, so only a check that reads EVERY refusal line in the pass can see that
    this one sits outside the edge trigger and prints every frame of a refusal. It
    compiles: it re-reads the accessor because `attempted` is scoped to the guarded
    block.

    Review-15 F3: this arm is the ISOLATING mutation for the format half of the
    family -- it sits outside the refusing branch, so only a line whose own format
    names the refusal reaches it. The branch half is isolated by
    `arm_failure_second_line_names_the_frame_count`, whose format deliberately does
    NOT name the refusal.
    """
    marker = ("            } else {\n                streamFailureLogged = false;\n"
              "            }\n")
    at = text.find(marker)
    if at < 0:
        return text
    end = at + len(marker)
    copy = ('            if (objectDraw.allocationFailed() && streamCapacityLogged == 0) {\n'
            '                mdk::log3dsPrintf("objects: first refusal of %d corners "\n'
            '                                  "= %d KB linear — linearAlloc FAILED",\n'
            '                                  objectDraw.attemptedCapacity(),\n'
            '                                  (int)((int64_t)objectDraw.attemptedCapacity()\n'
            '                                        * 5 * sizeof(float) / 1024));\n'
            '            }\n')
    return text[:end] + copy + text[end:]


def arm_failure_clear_at_the_end_of_the_refusing_branch(text):
    """Clear the latch on the frame that refused, at the end of the refusing branch
    (review-15 F1 mutant a). The committed guard, the committed arming and the
    committed `else` clear are all untouched, so checks 17, 18 and 19 are each
    satisfied by the code they read -- and the line prints on every frame of one
    refusal, because the frame that printed also cleared the flag that was supposed
    to stop the next one. Only a check that reads the whole write set of the latch
    can see this, and until review-15 nothing in the repository did.
    """
    at = text.find("if (objectDraw.allocationFailed()) {")
    if at < 0:
        return text
    sp = block_span_from(text, text.index("{", at))
    return (text[:sp[1] - 1]
            + "                streamFailureLogged = false;\n"
            + text[sp[1] - 1:])


def arm_failure_clear_once_per_frame(text):
    """Clear the latch as a plain statement after the if/else, i.e. once on EVERY
    frame including the refusing ones (review-15 F1 mutant b). The obvious defensive
    line -- "reset the diagnostic latch each frame so it cannot carry over" -- is the
    defect: the guard is true again next frame and one sustained refusal prints ~60x/s
    with checks 17, 18 and 19 all green. It is a direct statement of the block that
    encloses the test, so a window search that accepts the anchored assignment form
    without testing WHERE it sits accepts this too.
    """
    marker = ("            } else {\n                streamFailureLogged = false;\n"
              "            }\n")
    at = text.find(marker)
    if at < 0:
        return text
    end = at + len(marker)
    return text[:end] + "            streamFailureLogged = false;\n" + text[end:]


def arm_failure_clear_at_the_top_of_the_frame(text):
    """Clear the latch as the FIRST statement of `while (aptMainLoop())` (review-16
    R1). This is review-15 F1's mutant (b) one scope up, and it is the more natural
    edit of the two: "reset the diagnostic latch at frame start so it cannot carry
    over" reads as hygiene, not as a bug. The latch is declared in `main()` before
    the loop, so the write is in scope and runs on every frame INCLUDING the ones
    that refused, so a sustained refusal at 4096 corners prints ~60x/s again with
    checks 17, 18 and 19 each satisfied by the code they read. Batch 109's write net
    scanned `object_pass()`, which is inside this loop, so it read none of it.
    """
    sp = frame_body_span(text)
    if sp is None:
        return text
    return text[:sp[0] + 1] + "\n        streamFailureLogged = false;" + text[sp[0] + 1:]


def arm_failure_clear_wrapped_across_lines(text):
    """The same per-frame clear as `arm_failure_clear_once_per_frame`, but with the
    `=` wrapped onto the next line (review-16 R1, mutant b). Semantically identical
    to the committed clear's own statement shape; the ONLY reason it survived batch
    109's net was that the scan's `[ \\t]*=` could not cross a newline. An arm for
    it exists so that the newline tolerance is pinned and not an accident."
    """
    marker = ("            } else {\n                streamFailureLogged = false;\n"
              "            }\n")
    at = text.find(marker)
    if at < 0:
        return text
    end = at + len(marker)
    return (text[:end]
            + "            streamFailureLogged\n"
              "                = false;\n"
            + text[end:])


def arm_failure_alias_declared_outside_the_frame(text):
    """Declare the reference alias BESIDE the latch and write through it inside the
    frame (review-17 R1). Batch 110's alias guard scanned the frame body, so the
    shape it existed for -- an alias, which is normally declared where the field is,
    one line above the frame loop -- was invisible to it: `bool& latchAlias =
    streamFailureLogged;` at the declaration plus `latchAlias = false;` as the
    frame's first statement printed one sustained refusal ~60x/s with every check,
    arm and probe in this file green. This is that mutant, and it is the arm that
    keeps the scan's SCOPE honest: an alias declared inside the loop body (the arm
    batch 109 added) tests the pattern, not the scope.
    """
    decl = "    bool streamFailureLogged = false;\n"
    if decl not in text:
        return text
    out = text.replace(decl, decl + "    bool& latchAlias = streamFailureLogged;\n", 1)
    loop = "    while (aptMainLoop()) {\n"
    if loop not in out:
        return text
    return out.replace(loop, loop + "        latchAlias = false;\n", 1)


def arm_failure_byte_figure_hoisted_from_the_frame_count(text):
    """Put the byte figure's arithmetic in a local and print it BY NAME, with the
    count taken from the frame's corners (review-17 R2). Review-12 R2 is the defect:
    one refusal, two committed numbers that disagree by up to 8x. Batch 110's check
    read only arguments that spell an operator inline (`"*" in v or "/" in v`), so an
    argument that is a bare local name was compared against NOTHING and the defect
    passed one refactor away from the printf. The arithmetic is identical to
    `arm_failure_kb_names_the_frame_count`'s; only its POSITION moved, which is the
    whole point of the arm.
    """
    anchor = "                                            1024));\n"
    if anchor not in text:
        return text
    return text.replace(anchor, anchor +
                        '                const int32_t refusedBytes =\n'
                        '                    (int32_t)((int64_t)objectCorners * 5 * sizeof(float));\n'
                        '                mdk::log3dsPrintf("objects: refused %d corners = %d bytes",\n'
                        '                                  objectDraw.attemptedCapacity(),\n'
                        '                                  refusedBytes);\n', 1)


def arm_failure_byte_figure_is_a_stale_literal(text):
    """Print a byte figure that is a literal nobody recomputes (review-17 R2, the
    other half). `81920` is what 4096 corners cost TODAY; the day the stride or the
    rounding changes it is a number the backend was never asked for, and it carries no
    operator for a `"*" in v` filter to find. A figure with no source is a figure with
    no guarantee.
    """
    anchor = "                                            1024));\n"
    if anchor not in text:
        return text
    return text.replace(anchor, anchor +
                        '                mdk::log3dsPrintf("objects: refused %d corners = %d bytes",\n'
                        '                                  objectDraw.attemptedCapacity(), 81920);\n',
                        1)


def arm_failure_clear_through_a_direct_initialised_reference(text):
    """Bind the alias by DIRECT initialisation instead of `=` (review-18 F1):
    `bool& latchAlias(streamFailureLogged);` beside the latch, written as the frame's
    first statement. Batch 111's net matched two spellings of aliasing -- the address-of
    and `T& a = latch --` and this is the third, which contains neither a `&` beside the
    field nor an `=` after the alias's name. It is the same per-frame reset and prints
    the same sustained refusal ~60x/s. A blacklist of spellings is a race the next
    reviewer keeps winning; the whitelist refuses the name wherever it is not one of the
    pinned shapes.
    """
    decl = "    bool streamFailureLogged = false;\n"
    if decl not in text:
        return text
    out = text.replace(decl, decl + "    bool& latchAlias(streamFailureLogged);\n", 1)
    loop = "    while (aptMainLoop()) {\n"
    if loop not in out:
        return text
    return out.replace(loop, loop + "        latchAlias = false;\n", 1)


def arm_failure_clear_through_a_by_reference_helper(text):
    """Move the per-frame reset into a helper that takes the latch by reference
    (review-18 F1): `clearRefusalLatch(streamFailureLogged);` as the frame's first
    statement, with the write inside the helper. No alias is ever declared and no `&`
    ever touches the latch's name, so there is no SPELLING left for a scan to look for
    -- the write exists only under the parameter's name. The only net that can catch it
    is one that refuses a use of the latch's name it cannot enumerate, which is what
    LATCH_SHAPE now does.
    """
    anchor = "int main(int argc, char** argv) {"
    if anchor not in text:
        return text
    out = text.replace(anchor,
                       "// review-18 F1 mutant: the reset moved out of the frame.\n"
                       "static void clearRefusalLatch(bool& latch) { latch = false; }\n\n"
                       + anchor, 1)
    loop = "    while (aptMainLoop()) {\n"
    if loop not in out:
        return text
    return out.replace(loop, loop + "        clearRefusalLatch(streamFailureLogged);\n", 1)


def arm_failure_byte_figure_baked_into_the_format(text):
    """State the byte figure IN THE FORMAT STRING, with no conversion and no argument
    (review-18 F2): `log3dsPrintf("objects: refused 81920 bytes")`. Batch 115 skipped a
    line with no arguments and no conversion, so every figure rule walked past this one
    while it printed a byte count nobody recomputes -- and it is exactly the shape a
    developer reaches for after a review says the printed number is wrong: stop
    computing it, write the right one in. `81920` is what 4096 corners cost today; a
    figure with no argument behind it has no source to disagree with.
    """
    anchor = "                                            1024));\n"
    if anchor not in text:
        return text
    return text.replace(anchor, anchor +
                        '                mdk::log3dsPrintf("objects: refused 81920 bytes");\n',
                        1)


def arm_failure_conversion_names_a_figure_with_no_argument(text):
    """A conversion the call never supplies an argument for, in a spelling batch 115's
    one-letter pattern did not match (review-18 F4): `%8d`. Reading an argument that was
    never passed is undefined behaviour and the line prints the stack, and
    `log3dsPrintf` carries no `format(printf)` attribute, so the compiler does not see
    it either. The comment here has always claimed "a format with a conversion and no
    argument is still red"; with `%` + one flag + one letter that was true only of `%d`.
    """
    anchor = "                                            1024));\n"
    if anchor not in text:
        return text
    return text.replace(anchor, anchor +
                        '                mdk::log3dsPrintf("objects: refused %8d corners");\n',
                        1)


def probe_failure_line_names_no_figure_at_all(text):
    """Add a refusal line that states NO number (review-17 R3): the natural companion
    to the backend's own `linearAlloc(%u) FAILED`. It disagrees with nothing, so a
    rule about disagreement has nothing to say about it -- and reddening it is the
    review-10 F6 failure, a gate that rejects a shape the port may write and so gets
    routed around.
    """
    anchor = "                    streamFailureLogged = true;\n"
    if anchor not in text:
        return text
    return text.replace(anchor, anchor +
                        '                    mdk::log3dsPrintf("objects: linearAlloc FAILED for "\n'
                        '                                      "the stream mesh");\n', 1)


def arm_respawn_keeps_the_caches(text):
    """Keep the layout and pose caches across a runtime rebuild. This is the defect
    the object pass would have shipped: the rebuilt runtime frees the models both
    caches are keyed by, and a recycled address inherits the old model's geometry."""
    spawn = body_span(text, "spawnPlayer")
    if spawn is None:
        return text
    head, body, rest = text[:spawn[0]], text[spawn[0]:spawn[1]], text[spawn[1]:]
    mutated, n = re.subn(
        r"[ \t]*(?:mdk::scripts::)?MdkObject::forget_poses\(\)[ \t]*;[ \t]*\n"
        r"[ \t]*objectView\.clear\(\)[ \t]*;[ \t]*\n", "\n", body)
    if not n:
        return text
    return head + mutated + rest


ARMS = [
    # Each arm names the ONE check it must turn red (review-7 F2). The attribution is
    # exact equality, so an arm that an earlier check happens to catch first is a
    # FAIL until either the arm or the check is fixed -- which is what keeps a check
    # from being certified by a mutation that never reached it.
    ("in-loop-refresh-deleted (review-8 F1 itself)", arm_no_refresh,
     CHK["refresh-in-loop"]),
    ("refresh-moved-before-the-camera-step", arm_before_camera, CHK["refresh-after-camera"]),
    ("refresh-clears-the-pin-when-outside-every-arena", arm_clears_the_pin,
     CHK["refresh-guarded"]),
    ("spawn-pin-removed", arm_drops_the_spawn_pin, CHK["spawn-pin"]),
    ("guard-replaced-by-an-unrelated-flag", arm_guard_on_something_else,
     CHK["guard-is-the-lookup"]),
    ("scripts-tick-deleted", arm_scripts_tick_deleted, CHK["scripts-tick-order"]),
    ("scripts-tick-moved-after-the-camera", arm_scripts_tick_after_camera,
     CHK["scripts-tick-order"]),
    ("frame-mixes-the-standalone-mixer", arm_mixes_standalone, CHK["mixes-runtime-mixer"]),
    ("respawn-does-not-re-attach-the-sink", arm_sink_not_reattached, CHK["respawn-reattaches"]),
    ("the music outlives the runtime it plays through", arm_music_outlives_the_runtime,
     CHK["music-follows-kurts-arena"]),
    ("the music is built on the standalone mixer", arm_music_on_the_standalone_mixer,
     CHK["music-follows-kurts-arena"]),
    ("the music fades before it enters", arm_music_fades_before_it_enters,
     CHK["music-follows-kurts-arena"]),
    ("the loading arena's track is never stepped", arm_music_never_prefetched,
     CHK["music-follows-kurts-arena"]),
    ("the loading arena's track is released under Kurt",
     arm_loading_arenas_track_is_released, CHK["music-follows-kurts-arena"]),
    ("runtime-built-without-the-asset-gate", arm_no_asset_gate, CHK["asset-gate"]),
    # The replacement form trips check 5 first: with the geometric block gone there
    # is no guarded arenaAt lookup left to check. arm_frozen_pin_added is the arm
    # that isolates check 6.
    ("pin-taken-from-the-runtime's-frozen-current-arena (review-6 F1)",
     arm_pin_from_the_runtime, CHK["guard-is-the-lookup"]),
    ("frozen-runtime-pin-added-alongside-the-drawn-list-one (review-7 F2)",
     arm_frozen_pin_added, CHK["pin-is-the-drawn-list"]),
    ("pitch-ignores-the-runtime", arm_pitch_ignores_runtime, CHK["pitch-from-runtime"]),
    ("object pass moved above the arena draw", arm_objects_before_arena,
     CHK["objects-drawn-in-order"]),
    ("views synced inside the frame, after begin_frame", arm_views_synced_inside_the_frame,
     CHK["arenas-follow-the-scripts"]),
    ("views synced on a second list, not the drawn one", arm_views_synced_on_a_second_list,
     CHK["arenas-follow-the-scripts"]),
    ("the frame's drawn list is the start arena", arm_drawn_list_is_the_start_arena,
     CHK["arenas-follow-the-scripts"]),
    ("object pass ungated on a null runtime", arm_objects_ungated,
     CHK["objects-gated-on-runtime"]),
    ("object pass draws the removed", arm_objects_draws_removed,
     CHK["objects-skip-invisible-and-removed"]),
    ("respawn keeps the Model-address-keyed caches", arm_respawn_keeps_the_caches,
     CHK["respawn-drops-the-model-caches"]),
    ("object pass drawn a second time after Kurt (review-9 F2)",
     arm_objects_drawn_twice, CHK["objects-drawn-exactly-once"]),
    ("refused-mesh line prints the frame's corner count (review-11 F1)",
     arm_failure_line_names_the_frame_count,
     CHK["failure-line-names-the-refused-capacity"]),
    ("refused-mesh line's KB figure comes from the frame's corner count (review-12 R2)",
     arm_failure_kb_names_the_frame_count,
     CHK["failure-line-names-the-refused-capacity"]),
    ("refused-mesh latch never re-arms after a recovery (review-11 F7)",
     arm_failure_latch_never_rearms, CHK["failure-latch-re-arms-on-success"]),
    ("refused-mesh line prints on every frame of one refusal (review-12 R1)",
     arm_failure_line_prints_every_frame,
     CHK["failure-line-prints-once-per-refusal"]),
    ("refused-mesh guard mentions the latch instead of testing it (review-13 D3)",
     arm_failure_guard_compound_condition,
     CHK["failure-line-prints-once-per-refusal"]),
    ("a second refused-mesh line in the branch names the frame's corner count "
     "(review-13 D1)",
     arm_failure_second_line_names_the_frame_count,
     CHK["failure-line-names-the-refused-capacity"]),
    ("a second refused-mesh line in the branch sits outside the edge trigger "
     "(review-13 D1)",
     arm_failure_second_line_unguarded,
     CHK["failure-line-prints-once-per-refusal"]),
    ("refused-mesh KB figure uses the wrong corner stride (review-13 D4)",
     arm_failure_kb_wrong_stride,
     CHK["failure-line-names-the-refused-capacity"]),
    ("refused-mesh latch is re-armed from inside the refusing branch (review-13 D2)",
     arm_failure_latch_set_inside_branch,
     CHK["failure-latch-re-arms-on-success"]),
    ("refused-mesh latch is never armed, so the guard is always true (review-14 F1)",
     arm_failure_latch_never_arms, CHK["failure-line-prints-once-per-refusal"]),
    ("refused-mesh latch is re-armed only inside an inner condition (review-14 F2)",
     arm_failure_latch_clear_unreachable, CHK["failure-latch-re-arms-on-success"]),
    ("a refusal line in a sibling branch of the object pass sits outside the edge "
     "trigger (review-14 F7)",
     arm_failure_second_line_sibling_branch,
     CHK["failure-line-prints-once-per-refusal"]),
    ("the refusing branch clears the refused-mesh latch on the frame that refused "
     "(review-15 F1)",
     arm_failure_clear_at_the_end_of_the_refusing_branch,
     CHK["failure-latch-writes-are-the-pinned-three"]),
    ("the refused-mesh latch is cleared once per frame after the if/else "
     "(review-15 F1)",
     arm_failure_clear_once_per_frame,
     CHK["failure-latch-writes-are-the-pinned-three"]),
    # Review-16 R1: the two mutants batch 109's pass-scoped net read as clean code.
    # The first is the defect itself one scope up; the second is the same defect
    # spelled with the `=` on its own line, which is only a mutant because the scan
    # is newline-tolerant -- so both are arms now, and both must redden check 20.
    ("the refused-mesh latch is cleared once per frame through a reference alias "
     "(review-16 open question 2)",
     lambda s: s.replace("    while (aptMainLoop()) {",
                         "    while (aptMainLoop()) {\n"
                         "        bool& latchAlias = streamFailureLogged;\n"
                         "        latchAlias = false;", 1),
     CHK["failure-latch-writes-are-the-pinned-three"]),
    ("the refused-mesh latch is cleared at the top of the frame loop (review-16 R1)",
     arm_failure_clear_at_the_top_of_the_frame,
     CHK["failure-latch-writes-are-the-pinned-three"]),
    ("the refused-mesh latch is cleared once per frame with the `=` wrapped onto the "
     "next line (review-16 R1)",
     arm_failure_clear_wrapped_across_lines,
     CHK["failure-latch-writes-are-the-pinned-three"]),
    # Review-17 R1: the alias arm above declares the reference INSIDE the loop body,
    # so it tests the alias PATTERN while the scan could still be scoped to the body.
    # This one declares it where a reference is normally declared -- beside the field,
    # outside the frame -- which is the shape the property is about and the shape
    # batch 110 shipped green.
    ("the refused-mesh latch is written through an alias declared OUTSIDE the frame "
     "body (review-17 R1)",
     arm_failure_alias_declared_outside_the_frame,
     CHK["failure-latch-writes-are-the-pinned-three"]),
    # Review-17 R2: the two shapes batch 110's `"*" in v or "/" in v` filter could not
    # see. Both are review-12 R2's disagreement between the frontend's byte figure and
    # renderer_c3d's own `linearAlloc(N) FAILED`, with the arithmetic moved out of the
    # argument list.
    ("the refused-mesh byte figure is hoisted into a local built from the frame's "
     "corner count (review-17 R2)",
     arm_failure_byte_figure_hoisted_from_the_frame_count,
     CHK["failure-line-names-the-refused-capacity"]),
    ("the refused-mesh byte figure is a stale literal (review-17 R2)",
     arm_failure_byte_figure_is_a_stale_literal,
     CHK["failure-line-names-the-refused-capacity"]),
    # Review-18 F1: the two alias spellings batch 111's blacklist could not see, because
    # neither contains an `&` beside the field or an `=` after the alias's name. The
    # second does not declare an alias at all.
    ("the refused-mesh latch is cleared every frame through a reference bound by "
     "direct initialisation (review-18 F1)",
     arm_failure_clear_through_a_direct_initialised_reference,
     CHK["failure-latch-writes-are-the-pinned-three"]),
    ("the refused-mesh latch is cleared every frame by a helper that takes it by "
     "reference (review-18 F1)",
     arm_failure_clear_through_a_by_reference_helper,
     CHK["failure-latch-writes-are-the-pinned-three"]),
    # Review-18 F2/F4: the two ways a refusal line can state a figure the check never
    # looked at -- one baked into the format text, one asked for by a conversion the
    # call does not supply.
    ("the refused-mesh byte figure is baked into the format string with no conversion "
     "(review-18 F2)",
     arm_failure_byte_figure_baked_into_the_format,
     CHK["failure-line-names-the-refused-capacity"]),
    ("a refusal line's %8d conversion has no argument (review-18 F4)",
     arm_failure_conversion_names_a_figure_with_no_argument,
     CHK["failure-line-names-the-refused-capacity"]),
    # Review-19 F1/F2/F4: three shapes the batch-112 figure rules let through, each
    # isolating one half of what this batch re-derived. The first is the reviewer's
    # Critical: an honest byte figure used to be enough to certify the whole line.
    ("only the corner slot is substituted with the frame's corner count (review-19 F1)",
     arm_failure_corner_slot_names_the_frame_count,
     CHK["failure-line-names-the-refused-capacity"]),
    ("a refusal line's %*d conversion never supplies its width (review-19 F2)",
     arm_failure_star_width_names_no_argument,
     CHK["failure-line-names-the-refused-capacity"]),
    ("a refusal line's stale figure is a SUFFIXED literal in a slot naming no unit "
     "(review-19 F4)",
     arm_failure_suffixed_literal_names_no_source,
     CHK["failure-line-names-the-refused-capacity"]),
    # Review-20 F1/F4: three shapes the shipped gate let through, each isolating one
    # clause batch 113's record claimed was pinned. The first two are the corner rule
    # from two sides -- a SECOND corner slot, and a corner unit written before its
    # conversion -- and the last three are the stale figure one paren, one sign and
    # one decimal point away from the spelling the existing arm covers.
    ("a second corner slot prints the frame's corner count while the capacity stays "
     "an argument of its own (review-20 F1)",
     arm_failure_corner_slot_names_a_second_frame_count,
     CHK["failure-line-names-the-refused-capacity"]),
    ("the corner unit sits before its conversion and that slot prints the frame's "
     "corner count (review-20 F4a)",
     arm_failure_corner_unit_written_before_its_conversion,
     CHK["failure-line-names-the-refused-capacity"]),
    ("a stale figure wrapped in one paren sits in a slot naming no unit "
     "(review-20 F4c)",
     arm_stale_literal("(81920)"),
     CHK["failure-line-names-the-refused-capacity"]),
    ("a stale figure's SIGN walks through the literal rule (review-20 F4c)",
     arm_stale_literal("-81920"),
     CHK["failure-line-names-the-refused-capacity"]),
    ("a stale figure written as a FLOAT walks through the literal rule "
     "(review-20 F4c)",
     arm_stale_literal("81920.0f"),
     CHK["failure-line-names-the-refused-capacity"]),
    # Review-21 F2/F3: the two shapes the shipped gate let through with a full PASS. The
    # first two are the unit-less refusal line -- with a frame slot and without -- and
    # they isolate the line-level rule batch 114 deleted along with the rule review-20 F2
    # was about. The third is the stale figure at a depth the old bounded unwrap could not
    # reach, which is the only thing that pins `unwrap` to a real fixpoint.
    ("a refusal line naming no unit prints the frame's corner count (review-21 F2)",
     arm_failure_unit_less_line_names_the_frame_count,
     CHK["failure-line-names-the-refused-capacity"]),
    ("a refusal line naming no unit never mentions the refused capacity (review-21 F2)",
     arm_failure_unit_less_line_names_no_capacity,
     CHK["failure-line-names-the-refused-capacity"]),
    ("a stale figure wrapped in SEVEN parens walks through the bounded unwrap "
     "(review-21 F3)",
     arm_failure_deeply_parenthesised_literal_names_no_source,
     CHK["failure-line-names-the-refused-capacity"]),
    # Review-21 open question 4: the exponent was the same stale figure with the decimal
    # point moved into the notation, and the old pattern matched `8192` and stopped.
    ("a stale figure written in scientific notation walks through the literal rule "
     "(review-21 open question 4)",
     arm_stale_literal("8192e1"),
     CHK["failure-line-names-the-refused-capacity"]),
    # Review-22 F1 and F2: the conversion the gate cannot read, and the capacity parked past
    # the last conversion. The first is the shape batch 115's skip exempted; the second is
    # the clause that skip's own record said carried the weight and that no arm had ever
    # touched.
    ("a space-flag conversion the gate cannot parse names a byte unit it does not print "
     "(review-22 F1)",
     arm_failure_space_flag_conversion_names_a_byte_unit,
     CHK["failure-line-names-the-refused-capacity"]),
    ("a refusal line parks the capacity past its last conversion and prints the frame's "
     "corner count (review-22 F2)",
     arm_failure_unit_less_line_hides_the_capacity_past_the_last_conversion,
     CHK["failure-line-names-the-refused-capacity"]),
]

# Correct code each check must accept. A gate that reddens a legitimate edit is a
# gate the next batch learns to route around, and review-10 F6 is exactly that
# failure: a comment naming the call site was counted as a second call site.
PROBES = [
    ("a comment line names the object-pass call site (review-10 F6)",
     lambda s: s.replace("            objectDraw.draw(renderer, &surfaces, objectStream);",
                         "            // the only objectDraw.draw( call site; see "
                         "Viewer.cs:704\n"
                         "            objectDraw.draw(renderer, &surfaces, objectStream);",
                         1)),
    ("the call line carries a trailing comment naming it (review-10 F6)",
     lambda s: s.replace("            objectDraw.draw(renderer, &surfaces, objectStream);",
                         "            objectDraw.draw(renderer, &surfaces, objectStream);  "
                         "// objectDraw.draw( is DrawObjects, Viewer.cs:704", 1)),
    ("a string literal holding a // path sits before the call on its line",
     lambda s: s.replace("            objectDraw.draw(renderer, &surfaces, objectStream);",
                         '            (void)"sdmc://not-a-comment"; objectDraw.draw('
                         "renderer, &surfaces, objectStream);", 1)),
    # Check 17 reads the argument of the refused-mesh printf, so renaming the local
    # that carries the refused capacity is a legitimate edit it must accept.
    ("the refused-capacity local is renamed (check 17 reads the source, not a name)",
     lambda s: re.sub(r"\battempted\b", "refused", s)),
    # Check 18 pins the clear to the `else` sibling because that is where the port
    # writes it, but the assignment form below is the same state machine: the latch
    # holds exactly while the pass is refusing and clears the frame after it stops.
    # A check that reddened it would be the review-10 F6 failure again.
    ("the latch is re-armed by an assignment from the flag instead of an else",
     lambda s: s.replace(
         "            } else {\n                streamFailureLogged = false;\n            }\n",
         "            }\n", 1).replace(
         "            if (objectDraw.allocationFailed()) {",
         "            streamFailureLogged = objectDraw.allocationFailed();\n"
         "            if (objectDraw.allocationFailed()) {", 1)),
    # The re-arm line carrying a comment is ordinary code, and the exact-text check used
    # to answer "no such line".
    ("the re-arm line carries a trailing comment (check 18 reads the branch, not a line)",
     lambda s: s.replace("                streamFailureLogged = false;",
                         "                streamFailureLogged = false;  // re-arm the "
                         "edge trigger (review-11 F7)", 1)),
    # Check 17 now compares FACTOR SETS with the renderer's own expression, so the two
    # shapes below are the same arithmetic written differently and must stay green.
    ("the refused-mesh KB figure writes its factors in another order (review-13 D4 "
     "compares factors, not text)",
     lambda s: s.replace("(int)((int64_t)attempted * 5 * sizeof(float) /",
                         "(int)((int64_t)sizeof(float) * 5 * attempted /", 1)),
    ("the refused-mesh line names bytes instead of KB (no /1024)",
     lambda s: re.sub(
         r"\(int\)\(\(int64_t\)attempted \* 5 \* sizeof\(float\) /[ \t]*\n[ \t]*1024\)",
         "(int)((int64_t)attempted * 5 * sizeof(float))", s, count=1
     ).replace("%d KB ", "%d bytes ", 1)),
    # Check 19 requires EVERY refusal line to be edge-triggered, not that there be only
    # one, so a second guarded line that also names the refused capacity is legal.
    ("a second refused-mesh line sits inside the edge trigger too (review-13 D1 asks "
     "every line to be guarded, not that there be one)",
     lambda s: s.replace("""                                      (int)((int64_t)attempted * 5 * sizeof(float) /
                                            1024));
""",
                         """                                      (int)((int64_t)attempted * 5 * sizeof(float) /
                                            1024));
                    mdk::log3dsPrintf("objects: refused request was %d corners "
                                      "(%d bytes)", attempted,
                                      (int)((int64_t)attempted * 5 * sizeof(float)));
""", 1)),
    # Review-16 R2's twin on this side. `LATCH_NEG` has always accepted the
    # `streamFailureLogged == false` spelling, but check 20's write scan read the
    # `==` as a write and reddened the guard -- a gate rejecting a shape its own
    # condition regex accepts, which is review-10 F6's failure with the two halves
    # swapped. The scan now requires `=` not followed by `=`.
    ("the refused-mesh guard is written as a comparison, not a negation (review-16 "
     "R2: the second LATCH_NEG spelling is a read, not a write)",
     lambda s: s.replace("                if (!streamFailureLogged) {",
                         "                if (streamFailureLogged == false) {", 1)),
    # Review-16 open question 3, pinned. A line inside the refusing branch that names
    # the refused capacity and prints NO byte figure disagrees with nothing, so the
    # re-shaped check 17 must accept it. Reddening it was the gate refusing a shape the
    # port may write; review-10 F6 again. The half that stays red is a line naming a
    # unit it cannot derive -- see `arm_failure_kb_names_the_frame_count`, which prints
    # "%d KB" built from the frame's corner count. (Review-17 R7: this comment used to
    # point at a name, `oq3_kb`, that exists nowhere in the repository.)
    ("a second line in the refusing branch names the refused capacity and no byte "
     "figure (review-16 open question 3)",
     lambda s: s.replace("""                                      (int)((int64_t)attempted * 5 * sizeof(float) /
                                            1024));
""",
                         """                                      (int)((int64_t)attempted * 5 * sizeof(float) /
                                            1024));
                    mdk::log3dsPrintf("objects: refused request was %d corners",
                                      attempted);
""", 1)),
    # Review-15 F3 restored the rule that EVERY printf inside the refusing branch is a
    # refusal line whatever its wording. A net that reads every line must also ACCEPT
    # every honest one: this line never spells FAILED or "refus", names the refused
    # capacity, and sits inside the edge trigger, so reddening it would be review-10
    # F6's failure again -- a gate that rejects a shape the port may write is a gate
    # the next batch routes around.
    ("a second line in the refusing branch names the refused capacity without the "
     "word FAILED (review-15 F3: the branch net must accept an honest line)",
     lambda s: s.replace("""                                      (int)((int64_t)attempted * 5 * sizeof(float) /
                                            1024));
""",
                         """                                      (int)((int64_t)attempted * 5 * sizeof(float) /
                                            1024));
                    mdk::log3dsPrintf("objects: the request was %d corners "
                                      "(%d bytes)", attempted,
                                      (int)((int64_t)attempted * 5 * sizeof(float)));
""", 1)),
    # Check 17 compares FACTOR SETS after folding one level of local integer
    # constants (review-14 F6), because naming the stride is the drift-proof refactor a
    # later batch should write, not a defect: a gate that reddens it is a gate that
    # gets routed around.
    ("the refused-mesh KB figure names the corner stride instead of writing 5 "
     "(review-14 F6)",
     lambda s: s.replace(
         "                    const int32_t attempted = objectDraw.attemptedCapacity();",
         "                    const int32_t kMeshStride = 5;\n"
         "                    const int32_t attempted = objectDraw.attemptedCapacity();",
         1).replace("(int)((int64_t)attempted * 5 * sizeof(float) /",
                    "(int)((int64_t)attempted * kMeshStride * sizeof(float) /", 1)),
    # Check 19 now requires the guarded block to ARM the latch, so moving the arming
    # after the print -- which is the same state machine, since the guard is tested
    # once per frame -- must stay green.
    ("the refused-mesh latch is armed after the line it suppresses (review-14 F1 asks "
    "the guard to arm the latch, not for a particular order)",
     lambda s: s.replace("""                if (!streamFailureLogged) {
                    streamFailureLogged = true;
""", """                if (!streamFailureLogged) {
""", 1).replace("""                                            1024));
                }""", """                                            1024));
                    streamFailureLogged = true;
                }""", 1)),
    # Review-17 R3: the half of "no refusal line may disagree" that batch 110 left red
    # by accident. A line stating no number cannot disagree with anything, so the
    # figure rules are conditional on the format NAMING a figure. This is the honest
    # companion line to the backend's own `linearAlloc(%u) FAILED`.
    ("a refusal line states no figure at all (review-17 R3)",
     probe_failure_line_names_no_figure_at_all),
    # Review-18 F3: the other half of batch 115's over-reach. A refusal line may name a
    # quantity that is NOT a byte figure -- the frame it refused on, or a name -- and
    # adjudicating EVERY argument against the backend's byte count reddened both, which
    # is review-10 F6's failure again: legal code gets routed around. The unit words in
    # the line's own format are what say which slots are byte figures.
    ("a refusal line names the refused capacity AND the frame it happened on "
     "(review-18 F3)",
     lambda s: s.replace("""                                            1024));
                }""", """                                            1024));
                    mdk::log3dsPrintf("objects: refused %d corners at frame %lu",
                                      attempted, (unsigned long)frame);
                }""", 1)),
    # Review-22 F1's ACCEPT side, and the pin for the space flag in FIGURE_FMT's flag
    # class. The gate now reads `% d` the way `vsnprintf` and the compiler do, so a
    # space-flagged conversion is adjudicated on its merits: this line puts the refused
    # capacity's OWN byte figure in the slot its text calls a byte figure, so it agrees
    # with `renderer_c3d.cpp` and must stay green. Take the flag out of the class and this
    # probe is the thing that says so -- the format then carries a percent the gate cannot
    # read and the skip refuses it. That is why the flag class needs both halves pinned and
    # not only the arm (review-22 F3's shape, one rule on).
    ("a refusal line prints the refused capacity's byte figure with a space flag "
     "(review-22 F1: the flag is read, not refused)",
     lambda s: s.replace("""                                      (int)((int64_t)attempted * 5 * sizeof(float) /
                                            1024));
""", """                                      (int)((int64_t)attempted * 5 * sizeof(float) /
                                            1024));
                    mdk::log3dsPrintf("objects: refused % d bytes",
                                      (int)((int64_t)attempted * 5 * sizeof(float)));
""", 1)),
    ("a refusal line puts a name in front of the refused capacity (review-18 F3)",
     lambda s: s.replace("""                                            1024));
                }""", """                                            1024));
                    mdk::log3dsPrintf("objects: refused on %s: %d corners",
                                      "stream", attempted);
                }""", 1)),
    # Review-19 F2: the false-positive half. `%*d` is a legal conversion whose width
    # comes from an argument, and a gate that counted one argument per conversion
    # adjudicated the WIDTH as a byte figure and reddened the line. Refusing legal
    # code is the review-10 F6 failure, and the route-around is worse than the defect.
    ("a refusal line pads a column with %*d and supplies the width (review-19 F2)",
     probe_failure_star_width_supplies_its_width),
    # Review-20 F2 and F4a: the two honest shapes the batch-113 rules refused. The
    # first is the reason the whole-argument rule is gone rather than narrowed: a
    # refusal line may print ONLY the byte figure the capacity explains, which is what
    # the backend's own refusal line does, and a gate that reddens it gets routed
    # around into the corner figure it does not need.
    ("a refusal line prints only the byte figure the capacity explains, and no corner "
     "figure (review-20 F2)",
     probe_failure_line_prints_only_the_byte_figure),
    ("a refusal line names the corner unit BEFORE its conversion and prints the "
     "capacity there (review-20 F4a)",
     probe_failure_corner_unit_written_before_its_conversion),
    # Review-21 F1 and F3: the two shapes the gate must keep accepting. The first is the
    # no-conversion skip that batch 114's fall-through crashed on -- delete the skip and
    # this probe dies instead of the suite. The second is the capacity at a depth the
    # bounded unwrap could not read, which is the false-red half of the same finding.
    ("a refusal line spells a unit in prose and prints no figure at all "
     "(review-21 F1)",
     probe_failure_line_names_a_unit_in_prose_but_prints_no_figure),
    ("the refused capacity is wrapped in seven pairs of parens (review-21 F3)",
     probe_failure_capacity_wrapped_deeply_in_parens),
    # Review-22 F3: the accept half of the unit-less rule, which nothing pinned.
    ("a refusal line naming no unit prints the refused capacity (review-22 F3)",
     probe_failure_unit_less_line_prints_the_capacity),
]
# --- mutant dump (review-15 F5) ------------------------------------------------
# `arm_compile_check.sh` needs the mutants as FILES so it can hand them to the
# cross compiler. They are written HERE, beside the lists the gate adjudicates, so
# what gets compiled is exactly what gets adjudicated -- a second extractor would
# be a second implementation, and the one that rots. Unset by default, so the
# suite's behaviour is unchanged.
DUMP = os.environ.get("MDK_MUTANT_DUMP")
if DUMP:
    import os as _os
    _os.makedirs(DUMP, exist_ok=True)
    with open(_os.path.join(DUMP, "00_parent.cpp"), "w") as fh:
        fh.write(SRC)
    for _i, _e in enumerate(ARMS):
        with open(_os.path.join(DUMP, "arm%02d.cpp" % _i), "w") as fh:
            fh.write(_e[1](SRC))
    for _i, _e in enumerate(PROBES):
        with open(_os.path.join(DUMP, "probe%02d.cpp" % _i), "w") as fh:
            fh.write(_e[1](SRC))
    print("dumped %d arms and %d probes to %s" % (len(ARMS), len(PROBES), DUMP))
    sys.exit(0)

print("main_3ds_flow: %s — %d checks, %d mutation arms, %d green probes"
      % (PATH, real.checks, len(ARMS), len(PROBES)))


arms_red = 0
for label, mutate, expected in ARMS:
    mutated = mutate(SRC)
    if mutated == SRC:
        print("  FAIL main_3ds_flow arm %s: the mutation changed nothing, so the arm tested "
              "the file it was handed instead of the invariant" % label)
        continue
    after = adjudicate_named("arm " + label, mutated, quiet=True)
    if after is None:
        continue
    if not after.failed:
        print("  FAIL main_3ds_flow arm %s: the mutation was accepted, so the check does not "
              "pin what it claims to pin" % label)
        continue
    if after.broke != expected:
        print("  FAIL main_3ds_flow arm %s: it turned `%s` red, but the check it claims to "
              "pin is `%s` — so that check has no mutation that isolates it and the "
              "PASS would be a reading of a different check" % (label, after.broke, expected))
        continue
    arms_red += 1
    print("  ok   arm %s: turned `%s` red as required" % (label, after.broke))

if real.failed:
    sys.exit(1)
if arms_red != len(ARMS):
    print("  FAIL main_3ds_flow: only %d of %d arms adjudicated red; a PASS here would be a "
          "reading of nothing" % (arms_red, len(ARMS)))
    sys.exit(1)
probes_green = 0
for label, mutate in PROBES:
    mutated = mutate(SRC)
    if mutated == SRC:
        print("  FAIL main_3ds_flow probe %s: the mutation changed nothing" % label)
        continue
    after = adjudicate_named("probe " + label, mutated, quiet=True)
    if after is None:
        continue
    if after.failed:
        print("  FAIL main_3ds_flow probe %s: correct code turned `%s` red, so the check "
              "rejects a shape the port may legitimately write" % (label, after.broke))
        continue
    probes_green += 1
    print("  ok   probe %s stayed green" % label)
if probes_green != len(PROBES):
    print("  FAIL main_3ds_flow: only %d of %d probes stayed green" % (probes_green,
                                                                       len(PROBES)))
    sys.exit(1)
print("PASS Main3dsFlow (%d checks, %d arms adjudicated red, %d probes green)"
      % (real.checks, arms_red, probes_green))
PY
exit $?
