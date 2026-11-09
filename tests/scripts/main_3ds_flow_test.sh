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
#   6. the step loop's ONE pin reads the arena under Kurt and never the runtime's
#      current arena — which the unported connection crossing leaves at the spawn
#      arena for the whole level (review-6 F1: batch 99 pinned to it and a door walk
#      fell through the room it opened into). This is the check that keeps the
#      reverted stand-in reverted until CrossedArena lands.
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

python3 - "$SRC" <<'PY'
import re
import sys

PATH = sys.argv[1]
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


# Casts, keywords and the unit the log line names. `value_names` removes these so an
# arithmetic argument reduces to the quantities it actually multiplies.
NON_VALUE = {"int", "long", "short", "char", "unsigned", "signed", "bool", "float",
             "double", "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t",
             "uint16_t", "uint32_t", "uint64_t", "size_t", "ptrdiff_t", "sizeof",
             "static_cast", "const", "KB"}


def value_names(expr):
    """The identifiers an expression computes with, casts and `sizeof` removed."""
    return {m.group(0) for m in re.finditer(r"[A-Za-z_][A-Za-z0-9_]*",
                                            expr.replace("objectDraw.", ""))
            if m.group(0) not in NON_VALUE}


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
    "pin-not-the-runtime":
        "the step loop pins from the arena under Kurt, never from the runtime's frozen "
        "current arena",
    "scripts-tick-order":
        "the scripts tick in the step loop, after Kurt and before the camera",
    "mixes-runtime-mixer":
        "the frame mixes the pointer that follows the runtime, not the standalone mixer",
    "respawn-reattaches":
        "a respawn re-points the mixer, rebinds Kurt, and re-attaches the sink",
    "asset-gate": "the runtime is constructed under the asset gate",
    "pitch-from-runtime": "the pitch target is camera_track, then the scripts' arena",
    "objects-drawn-in-order":
        "the object pass is drawn between the arena and Kurt's sprite (Viewer.cs:702-708)",
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
        "the frame's corner count (review-11 F1)",
    "failure-latch-re-arms-on-success":
        "the refused-mesh latch clears when a later attempt succeeds, so a second "
        "refusal after a recovery still prints (review-11 F7)",
    "failure-line-prints-once-per-refusal":
        "the refused-mesh line is edge-triggered, so one sustained refusal prints once "
        "and not on every frame of it (review-12 R1)",
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
                      r"space\.setSolid\([ \t]*\*([A-Za-z_][A-Za-z0-9_]*)[ \t]*\)", loop)
        if m:
            shape = m.group(1) == m.group(2) and re.search(
                r"arenaAt\([^;]*\);[ \t]*\n[ \t]*const[ \t]+std::string\*[ \t]*" + re.escape(m.group(1)),
                loop) is not None or bool(re.search(
                r"std::string\*[ \t]+" + re.escape(m.group(1)) + r"[ \t]*=[ \t]*\n?[ \t]*space\.arenaAt",
                loop))
    say(CHK["guard-is-the-lookup"], shape, 
        "a guarded setSolid whose guard is not the lookup can still clear the pin")

    # 6. the step loop's pin NEVER reads the runtime's current arena. Review-6 F1
    #    (High): `currentArena_` is written exactly once, on the runtime's first
    #    tick (script_runtime.cpp:362-378), because the DTI connection crossing that
    #    moves the oracle's `CurrentArena` (CrossedArena, ScriptRuntime.cs:791-803)
    #    is not ported. Pinning to it therefore freezes the collision world at the
    #    SPAWN arena for the whole level: walk through an open door and move() --
    #    which uses ONLY the pinned arena, arena_space.cpp:118 -- finds no floor in
    #    the room Kurt entered, he drops through it, and bottom() fires the fall-out
    #    death into a respawn loop. Batch 99 shipped exactly that; this check is the
    #    pin that stops it coming back, and it is the reason the claim "with a
    #    runtime the pin is the oracle's own" was deleted rather than kept quiet.
    frozen = bool(re.search(r"setSolid\([ \t]*runtime->currentArena\(", loop or ""))
    say(CHK["pin-not-the-runtime"],
        not frozen and loop_code is not None and loop_code.count("setSolid(") == 1,
        "currentArena() does not move until CrossedArena is ported, so a pin that "
        "reads it is the spawn arena forever; the loop must have exactly one refresh "
        "and it must be the geometric one")

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
    arena_at = text.find("view.draw();")
    kurt_at = text.find("kurtSprite.draw(")
    frame = frame_body_span(text)
    in_frame = (op is not None and frame is not None
                and frame[0] <= op["start"] < frame[1])
    say(CHK["objects-drawn-in-order"],
        in_frame and arena_at >= 0 and kurt_at >= 0 and arena_at < op["start"] < kurt_at,
        "C# Viewer.DrawScene draws the arena, then the objects, then Kurt; anywhere "
        "else and the order of the draws is a different picture")

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
    #     -- two committed lines, one refusal, 51 KB apart. The check is on the
    #     argument of the printf inside the `allocationFailed()` branch, so it
    #     cannot be satisfied by prose and does not care how the line is worded.
    #
    #     WHAT THE ARGUMENTS MUST AGREE ON (review-12 R2). The first version asked
    #     only whether the refused capacity appeared ANYWHERE in the call's tail, so
    #     `attempted` corners with a KB figure built from `objectCorners` passed --
    #     the corner count honest, the byte count 8x wrong, the gate green, and the
    #     exact disagreement F1 was raised about still on the console. The check now
    #     parses the call's arguments: the first must BE the refused capacity (the
    #     call itself, or a local initialised from it -- a bare rename stays legal,
    #     review-10 F6's lesson), and every later argument that does arithmetic must
    #     compute with that same quantity and nothing else. A plain extra argument is
    #     still allowed: a line that ALSO says what the frame wanted is informative,
    #     not wrong; a derived byte figure that disagrees with the corner figure is.
    fail_ok = False
    at = code.find("objectDraw.allocationFailed()")
    call = re.search(r"log3dsPrintf[ \t]*\(", code[at:]) if at >= 0 else None
    if call is not None:
        head = at + call.start()
        args = call_args(code, at + call.end() - 1)
        vals = args[1:] if args and args[0].lstrip().startswith('"') else None
        if vals:
            # Either the call itself, or a local whose initialiser is the call. The
            # second form is what the file writes, and a check that demanded the first
            # would redden a legitimate rename (review-10 F6's lesson about gates that
            # reject a shape the port may write). The window is the test's own header
            # up to its printf, so a same-named local in another branch cannot lend it
            # a source.
            sourced = set(re.findall(r"([A-Za-z_][A-Za-z0-9_]*)[ \t]*=[ \t]*objectDraw"
                                     r"\.attemptedCapacity\(\)", code[at:head]))
            first = vals[0].replace(" ", "")
            src = "attemptedCapacity" if first == "attemptedCapacity()" else (
                first if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", first) and first in sourced
                else None)
            # The byte figure: at least one derived argument must exist (that is the
            # half that has to reconcile with the backend's own `linearAlloc(N)`),
            # and each one may only compute with the refused capacity.
            derived = [v for v in vals[1:] if "*" in v or "/" in v]
            fail_ok = (src is not None and bool(derived)
                       and all(value_names(v) <= {src} for v in derived))
    say(CHK["failure-line-names-the-refused-capacity"], fail_ok,
        "the pass rounds up to a power of two and refuses THAT size; a line naming "
        "anything else reports an allocation that was never attempted")

    # 18. THE LATCH CLEARS ON A FRAME THAT DID NOT REFUSE (review-11 F7, re-pinned by
    #     review-12 R1). `streamFailureLogged` is the edge trigger that keeps a level
    #     under linear pressure from printing the same line 60 times a second, and it
    #     must clear when a later attempt succeeds or a grow to 8192 corners that
    #     fails after a success at 4096 prints nothing and the log ends on the smaller
    #     request. The first version of this check asked whether ANY LINE EQUALLED
    #     `streamFailureLogged = false;` -- which pins a line, not the guard: it was
    #     satisfied by an assignment that no longer guarded anything, and it reddened
    #     on a trailing comment. Both directions were wrong. It is now a brace-scan:
    #     the clear must sit in the `else` SIBLING of the `allocationFailed()` test,
    #     which is the branch every non-refusing frame takes, so "it clears" means
    #     "it clears when the pass succeeded" and not "the text is in the file".
    #     The equivalent assignment form (`streamFailureLogged =
    #     objectDraw.allocationFailed();` above the test) is accepted too: it is the
    #     same state machine, and a gate that reddens it is a gate the next batch
    #     learns to route around. There is a probe for it below.
    latch_ok = False
    once_ok = False
    at = code.find("objectDraw.allocationFailed()")
    if at >= 0:
        sp = block_span_from(code, code.find("{", at))
        if sp is not None:
            branch = code[sp[0]:sp[1]]
            after = code[sp[1]:]
            rest = re.match(r"[ \t\r\n]*else[ \t]*\{", after)
            if rest is not None:
                esp = block_span_from(after, rest.end() - 1)
                if esp is not None:
                    latch_ok = re.search(r"streamFailureLogged[ \t]*=[ \t]*false[ \t]*;",
                                         after[esp[0]:esp[1]]) is not None
            if latch_ok is False and re.search(
                    r"streamFailureLogged[ \t]*=[ \t]*objectDraw\.allocationFailed\("
                    r"[ \t]*\)[ \t]*;", code) is not None:
                latch_ok = True
            # 19. AND THE LINE PRINTS ONCE PER REFUSAL, not once per frame. The clear
            #     being reachable is only half of an edge trigger: with the guard
            #     widened the line still prints on every frame of a sustained refusal
            #     -- which is why it was edge-triggered at all, since the backend line
            #     it answers once printed 1710 times in a run (review-10 F3). So the
            #     printf must sit inside a branch whose CONDITION tests the latch
            #     negatively, found by containment and not by a line match.
            for g in re.finditer(r"\bif[ \t]*\(([^{}();]*)\)[ \t]*\{", branch):
                gsp = block_span_from(branch, g.end() - 1)
                if gsp is None or "log3dsPrintf(" not in branch[gsp[0]:gsp[1]]:
                    continue
                cond = g.group(1)
                if re.search(r"![ \t]*streamFailureLogged|streamFailureLogged"
                             r"[ \t]*==[ \t]*false", cond):
                    once_ok = True
    say(CHK["failure-latch-re-arms-on-success"], latch_ok,
        "a latch that can never clear is a one-shot log: the second, larger refusal "
        "is silent and the run reads as the first one")
    say(CHK["failure-line-prints-once-per-refusal"], once_ok,
        "an unguarded refusal line prints on every frame of one refusal, which is the "
        "log spam the edge trigger exists to stop")
    return r


real = adjudicate(SRC)

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
    both. Deleting only the geometric one — which is what this arm used to do,
    and what it still has to do to stay honest — leaves the runtime's call behind,
    so the mutant would keep refreshing the pin and the arm would test nothing."""
    out = re.sub(r"(?m)^[ \t]*if[ \t]*\(solid\)[ \t]*\{[ \t]*\n"
                 r"[ \t]*space\.setSolid\([ \t]*\*solid[ \t]*\);[ \t]*\n[ \t]*\}[ \t]*\n",
                 "", loop)
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
    """Refresh moved above follow.update: the arena the step STARTED in."""
    sp = STEP_LOOP
    if sp is None:
        return text
    a, b = sp
    loop = text[a:b]
    m = re.search(r"(?m)^[ \t]*if[ \t]*\(solid\)[ \t]*\{[ \t]*\n"
                  r"[ \t]*space\.setSolid\([ \t]*\*solid[ \t]*\);[ \t]*\n[ \t]*\}[ \t]*\n", loop)
    if not m:
        return text
    block = m.group(0)
    rest = loop[:m.start()] + loop[m.end():]
    p = re.search(r"(?m)^[ \t]*follow\.update\(", rest)
    if p is None:
        return text
    return text[:a] + rest[:p.start()] + block + rest[p.start():] + text[b:]


def arm_clears_the_pin(text):
    """The tidy-up that clears the pin when Kurt is outside every arena."""
    sp = STEP_LOOP
    if sp is None:
        return text
    a, b = sp
    loop = text[a:b]
    m = re.search(r"(?m)^[ \t]*if[ \t]*\(solid\)[ \t]*\{[ \t]*\n"
                  r"[ \t]*space\.setSolid\([ \t]*\*solid[ \t]*\);[ \t]*\n[ \t]*\}[ \t]*\n", loop)
    if not m:
        return text
    return text[:a] + loop[:m.start()] + \
        "                space.setSolid(solid ? *solid : std::string());\n" + \
        loop[m.end():] + text[b:]


def arm_drops_the_spawn_pin(text):
    """The spawn pin removed, leaving only the per-step refresh."""
    sp = body_span(text, "spawnPlayer")
    if sp is None:
        return text
    a, b = sp
    body = text[a:b]
    m = re.search(r"(?m)^[ \t]*if[ \t]*\(here\)[ \t]*\{[ \t]*\n"
                  r"[ \t]*space\.setSolid\([ \t]*\*here[ \t]*\);[ \t]*\n[ \t]*\}[ \t]*\n", body)
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
                  r"[ \t]*space\.setSolid\([ \t]*\*solid[ \t]*\);[ \t]*\n[ \t]*\}[ \t]*\n", loop)
    if not m:
        return text
    return text[:a] + loop[:m.start()] + \
        "                if (playerReady) {\n" \
        "                    space.setSolid(*solid);\n" \
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


def arm_no_asset_gate(text):
    """The gate replaced by `if (true)`: the runtime is built whatever the level
    loaded, which is the different-random-stream case the header forbids."""
    mutated, n = re.subn(r"if[ \t]*\([ \t]*scriptAssetsReady[ \t]*\)[ \t]*\{",
                         "if (true) {", text)
    return mutated if n else text


def arm_pin_from_the_runtime(text):
    """Batch 99's frozen pin, put back: the step loop reads the runtime's current
    arena instead of the arena under Kurt. `currentArena_` is written once, on the
    first tick, so this is the spawn arena for the whole level and a door walk falls
    through the room it opened into (review-6 F1)."""
    mutated, n = re.subn(
        r"(?m)^[ \t]*const[ \t]+std::string\*[ \t]+solid[ \t]*=[ \t]*\n"
        r"[ \t]*space\.arenaAt\([^;]*;[ \t]*\n"
        r"[ \t]*if[ \t]*\([ \t]*solid[ \t]*\)[ \t]*\{[ \t]*\n"
        r"[ \t]*space\.setSolid\([ \t]*\*solid[ \t]*\);[ \t]*\n[ \t]*\}[ \t]*\n",
        "                if (!runtime->currentArena().empty()) {\n"
        "                    space.setSolid(runtime->currentArena());\n"
        "                }\n", text)
    return mutated if n else text


def arm_frozen_pin_added(text):
    """Batch 99's frozen pin put back ALONGSIDE the geometric refresh, and placed
    AFTER it, so it is the last writer and the pin ends up the spawn arena whatever
    the geometric probe found. Review-7 F2: the arm above, which REPLACES the
    geometric block, trips check 5 first (the guarded `arenaAt` shape is simply
    gone), so check 6 had no mutation that isolated it and could be stubbed to a
    pass generator without the suite noticing. This mutant leaves check 5's shape
    intact, so only check 6 can catch it."""
    sp = STEP_LOOP
    if sp is None:
        return text
    a, b = sp
    loop = text[a:b]
    m = re.search(r"(?m)^[ \t]*if[ \t]*\(solid\)[ \t]*\{[ \t]*\n"
                  r"[ \t]*space\.setSolid\([ \t]*\*solid[ \t]*\);[ \t]*\n[ \t]*\}[ \t]*\n", loop)
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
def arm_objects_before_arena(text):
    """Move the whole object pass above the arena draw. The pass still runs and
    still skips what it should, so only the ORDER check can catch it -- which is
    the point: the C#'s order is the picture, not an implementation detail."""
    op = object_pass(text)
    if op is None:
        return text
    block = text[op["start"]:op["end"]]
    at = text.find("if (arena) {")
    if at < 0 or at > op["start"]:
        return text
    return text[:at] + block + "\n        " + text[at:op["start"]] + text[op["end"]:]


def arm_objects_ungated(text):
    """Ungate the pass. `if (true)` keeps the block, its skips and its order, so
    only the gate check can see it -- and a level whose scripts never started
    (check 10's asset gate) is exactly the shipped case where runtime is null."""
    op = object_pass(text)
    if op is None or not re.fullmatch(r"if[ \t]*\([ \t]*runtime[ \t]*\)[ \t]*\{",
                                      op["header"]):
        return text
    return text[:op["start"] - len(op["header"])] + "if (true) {" + text[op["start"]:]


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
    ("runtime-built-without-the-asset-gate", arm_no_asset_gate, CHK["asset-gate"]),
    # The replacement form trips check 5 first: with the geometric block gone there
    # is no guarded arenaAt lookup left to check. arm_frozen_pin_added is the arm
    # that isolates check 6.
    ("pin-taken-from-the-runtime's-frozen-current-arena (review-6 F1)",
     arm_pin_from_the_runtime, CHK["guard-is-the-lookup"]),
    ("frozen-runtime-pin-added-alongside-the-geometric-one (review-7 F2)",
     arm_frozen_pin_added, CHK["pin-not-the-runtime"]),
    ("pitch-ignores-the-runtime", arm_pitch_ignores_runtime, CHK["pitch-from-runtime"]),
    ("object pass moved above the arena draw", arm_objects_before_arena,
     CHK["objects-drawn-in-order"]),
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
    # The other half of review-12 R1's brittleness: the clear line carrying a comment
    # is ordinary code, and the exact-text check used to answer "no such line".
    ("the re-arm line carries a trailing comment (check 18 reads the branch, not a line)",
     lambda s: s.replace("                streamFailureLogged = false;",
                         "                streamFailureLogged = false;  // re-arm the "
                         "edge trigger (review-11 F7)", 1)),
]

print("main_3ds_flow: %s — %d checks, %d mutation arms, %d green probes"
      % (PATH, real.checks, len(ARMS), len(PROBES)))
arms_red = 0
for label, mutate, expected in ARMS:
    mutated = mutate(SRC)
    if mutated == SRC:
        print("  FAIL main_3ds_flow arm %s: the mutation changed nothing, so the arm tested "
              "the file it was handed instead of the invariant" % label)
        continue
    after = adjudicate(mutated, quiet=True)
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
    after = adjudicate(mutated, quiet=True)
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
