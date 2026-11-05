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
    say("the step loop refreshes Kurt's solid arena (review-8 F1)",
        loop_code is not None and "setSolid(" in loop_code,
        "no setSolid call in the `while (pending >= step)` body, so the pin is set "
        "wherever else it is set and never follows Kurt")

    # 2. and it is refreshed AFTER the camera step, as the C# orders them.
    ok_order = False
    if loop is not None:
        cam = call_line(loop, "follow.update")
        sol = call_line(loop, "space.setSolid")
        ok_order = cam is not None and sol is not None and sol > cam
    say("the refresh runs after follow.update, the C#'s order (Viewer.cs:517 then :533)",
        ok_order, "the camera's clearance can move Kurt, so a refresh placed before it "
                  "pins the arena he left rather than the one he is in")

    # 3. the spawn pin survives: the loop cannot supply it for a Kurt outside
    #    every arena, and without it bottom() resolves nothing.
    say("the spawn pin is still there for a Kurt outside every arena",
        spawn_body is not None and "setSolid(" in spawn_body,
        "with no pin at all bottom() answers false and the fall-out death never fires")

    # 4. the in-loop refresh is guarded, so being outside every arena keeps the
    #    last pin instead of clearing it. Depth is the load-bearing half: a call
    #    at depth 0 is unconditional whatever its argument looks like.
    guarded = False
    if loop is not None:
        depths = line_depths(loop)
        for i, line in code_lines(loop):
            if re.search(r"setSolid\s*\(", line):
                d = depths[i] if i < len(depths) else None
                guarded = d is not None and d >= 1
                break
    say("the in-loop refresh is guarded, so an empty lookup does not clear the pin",
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
    say("the guard is the arenaAt lookup itself", shape,
        "a guarded setSolid whose guard is not the lookup can still clear the pin")
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


def arm_no_refresh(text):
    """The defect itself: the in-loop refresh deleted, the spawn pin left alone."""
    sp = STEP_LOOP
    if sp is None:
        return text
    a, b = sp
    loop = text[a:b]
    m = re.search(r"(?m)^[ \t]*if[ \t]*\(solid\)[ \t]*\{[ \t]*\n"
                  r"[ \t]*space\.setSolid\([ \t]*\*solid[ \t]*\);[ \t]*\n[ \t]*\}[ \t]*\n", loop)
    if not m:
        return text
    return text[:a] + loop[:m.start()] + loop[m.end():] + text[b:]


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


ARMS = [
    ("in-loop-refresh-deleted (review-8 F1 itself)", arm_no_refresh),
    ("refresh-moved-before-the-camera-step", arm_before_camera),
    ("refresh-clears-the-pin-when-outside-every-arena", arm_clears_the_pin),
    ("spawn-pin-removed", arm_drops_the_spawn_pin),
    ("guard-replaced-by-an-unrelated-flag", arm_guard_on_something_else),
]

print("main_3ds_flow: %s — %d checks, %d mutation arms" % (PATH, real.checks, len(ARMS)))
arms_red = 0
for label, mutate in ARMS:
    mutated = mutate(SRC)
    if mutated == SRC:
        print("  FAIL main_3ds_flow arm %s: the mutation changed nothing, so the arm tested "
              "the file it was handed instead of the invariant" % label)
        continue
    after = adjudicate(mutated, quiet=True)
    if after.failed:
        arms_red += 1
        print("  ok   arm %s: turned `%s` red as required" % (label, after.broke))
    else:
        print("  FAIL main_3ds_flow arm %s: the mutation was accepted, so the check does not "
              "pin what it claims to pin" % label)

if real.failed:
    sys.exit(1)
if arms_red != len(ARMS):
    print("  FAIL main_3ds_flow: only %d of %d arms adjudicated red; a PASS here would be a "
          "reading of nothing" % (arms_red, len(ARMS)))
    sys.exit(1)
print("PASS Main3dsFlow (%d checks, %d arms adjudicated red)" % (real.checks, arms_red))
PY
exit $?
