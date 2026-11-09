#!/bin/sh
# c3d_frame_test.sh — the Citro3D frame's draw-order invariants, gated.
#
# WHY A SOURCE CHECK EXISTS (review-1 F3/F4). `renderer_c3d.cpp` includes
# <3ds.h> and cannot be compiled on a host, so nothing else in
# `make -C tests/scripts` can reach the frame's draw order. That is the same
# reason the matrix math and the depth rule were split into
# `pica_matrix.{h,cpp}` — but the two defects this file pins are not arithmetic
# that can be moved out of the backend, they are WHERE a call sits:
#
#   * the diagnostic triangle drawn from the per-draw path, which cost two
#     effect-block-dirtying C3D_DepthTest calls and one extra draw for every
#     real draw, per eye (review-1 F3) — and had already caused a worse defect
#     once (batch 28: drawn BEFORE the real draw it replaced the context's
#     single BufInfo and the mesh read the triangle's floats, i.e. invisible
#     geometry);
#   * a canvas/inset seam flushing the queue mid-frame, which emits the frame's
#     records into the PREVIOUS frame's target and empties the queue so the eye
#     loop replays nothing (review-1 F4).
#
# Both fixes are one call site each, and both sat under a green suite. A revert
# of either is a one-line change no other gate would notice, so the shape is
# pinned here.
#
# PLACEMENT, NOT MERELY PRESENCE (review-2 B/C). The first version of this file
# asked "is `drawDiagnostic(r)` somewhere inside `mdk_render_end_frame`" and
# "is `replayQueue(r)` the only replay". Both were satisfiable while the claimed
# behaviour was absent: a call placed AFTER the eye loop (a triangle in one eye
# and a hole in the other) and a call placed BEFORE the replay (batch 28's
# invisible geometry) each left every check green, and a second replay spelled
# `replayQueue(asC3d(rr))` was invisible to a literal `replayQueue(r)` match.
# So the checks below resolve the eye loop by brace matching and assert the call
# is inside it and after the replay, and the replay match is by function name
# with any argument, anywhere in the file. Three arms reproduce exactly those
# three mutants, so the gate cannot print PASS over any of them.
#
# UNCONDITIONAL, AND THE EMITTERS ENUMERATED (review-3 F1/F2/F3). "Inside the
# eye loop" is NOT the same claim as "executed once per eye". A call the loop
# runs only for `eye == 0`, and a `queue.reset()` added inside the loop, each
# left every review-2 check green — the guarded call is still exactly one call
# site and still inside the loop, and the reset is what makes the SECOND eye
# replay an empty queue, which is the defect this file exists to prevent.
# `installEye`, the call whose misplacement WAS batch 30's black screen, was
# pinned by nothing. And the seam list named the five canvas/inset seams but
# not `mdk_render_canvas_quad`, the one the live HUD path calls
# (mdk_core/src/game/hud_view.cpp:162,198,255), so an immediate draw there was
# accepted green. So the checks now pin the loop BODY's statement set — each of
# installEye / replayQueue / drawDiagnostic must be a depth-0 statement of the
# loop body, installEye first, and nothing resets the queue inside it — they
# pin canvas_quad with the other seams, and they CLOSE the set of places that
# may emit at all: every C3D_DrawArrays/C3D_DrawElements site in the file is
# inside drawRecorded or drawDiagnostic, and drawRecorded is called only by
# replayQueue. "The only emission point is the per-eye loop" is then a checked
# property rather than a reading of the file.
#
# THE LOOP RUNS EVERY PASS, AND THE QUEUE SURVIVES THE RUN (review-4 F2/F3/F5).
# "Every statement of the loop body is unconditional" is NOT the same claim as
# "the loop runs once per eye". `if (eye == 0) break;` leaves all three
# statements at depth 0 and still draws one eye; a bound written as a literal 1
# does the same thing without touching the body at all; and a `queue.reset()`
# placed BEFORE the loop — or inside `replayQueue`, which the loop calls — empties
# the queue exactly as thoroughly as the in-loop reset the previous batch
# refused, from a placement that check could not see. All four are plausible
# tidy-ups on a 268 MHz ARM11 and all four left every review-3 check green. The
# same review also found that `install-eye-per-eye-target` could not tell WHICH
# target each eye gets, nor the SIGN of the offset (F4): `eye == 0 ? targetRight
# : targetLeft` and `-r->eyes.offset[eye]` are both inside-out stereo, and both
# passed. So the bound, the initialiser, the absence of an early exit, the reset's
# position, the target ORDER and the offset SIGN are now each their own check with
# their own arm.
#
# WHAT THIS DOES NOT COVER, stated plainly: it reads the source, so it pins the
# STRUCTURE and not the pixels. Whether the triangle appears on a screen, whether
# the depth state is right, and what any of it costs on a 268 MHz ARM11 stay
# UNVERIFIED (AGENTS.md P6 — no 3DS emulator and no Old-3DS on this host). A gate
# that printed PASS over arms that never ran is a pass generator, so every check
# is paired with a mutation arm that must turn it red, and the adjudicated count
# has to equal the declared count.
#
# usage: sh c3d_frame_test.sh [path-to-renderer_c3d.cpp]
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SRC=${1:-$ROOT/mdk_3ds/src/render/renderer_c3d.cpp}
[ -f "$SRC" ] || { printf '  FAIL c3d_frame: cannot read %s\n' "$SRC"; exit 1; }

python3 - "$SRC" <<'PY'
import os
import re
import sys

PATH = sys.argv[1]
SRC = open(PATH, errors="replace").read()

# The per-eye loop inside mdk_render_end_frame. Named once here so the checks and
# the mutation arms all resolve the same block.
EYE_LOOP = r"for[ \t]*\([ \t]*int[ \t]+eye"

# The seams a caller may drive between mdk_render_begin_frame and
# mdk_render_end_frame. canvas_quad is here because hud_view.cpp calls it and
# because "the seams do not flush" was never the whole rule: a seam may not draw
# by itself either (review-3 F1).
SEAMS = ("mdk_render_canvas_begin", "mdk_render_canvas_quad", "mdk_render_canvas_end",
         "mdk_render_present", "mdk_render_begin_inset", "mdk_render_end_inset")

# Anything that puts work in front of the GPU.
EMIT_ANY = (r"flushDraws\s*\(|replayQueue\s*\(|drawRecorded\s*\(|"
            r"C3D_DrawArrays\s*\(|C3D_DrawElements\s*\(")
EMIT_DRAW = r"C3D_DrawArrays\s*\(|C3D_DrawElements\s*\("

# Anything that empties the frame's record list. It has three places it can do
# damage — inside the eye loop, before it, or inside a function the loop calls —
# and a check that looks in only one of them is a check with two blind spots
# (review-4 F2).
RESET = r"queue\.(?:reset|clear)\s*\("


def code_lines(text):
    """(index, text) for lines that are code. Comments are dropped, because this
    file documents its own rules in prose and a naive match would read the prose
    as a call site."""
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
    """The block that follows `pattern`, or None if the pattern is absent."""
    m = re.search(pattern, text)
    if not m:
        return None
    return block_span_from(text, m.end())


def blank_comments(text):
    """`text` with every C++ comment replaced by spaces, byte offsets preserved.
    The latch checks below scan brace depth, and an apostrophe in prose -- "the
    emulator's bigger heap" is a real comment in this file -- would otherwise open a
    string literal and swallow the braces that follow it. Same helper, same rule as
    main_3ds_flow_test.sh: prose is not code, and offsets must survive so a position
    found in the blanked copy still compares against the original."""
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


def brace_depth(text, a, b):
    """Brace depth at `b` counting from `a`, string-aware. A statement written
    DIRECTLY in the block whose '{' is at `a` sits at depth 1; one nested inside an
    inner condition of that block sits deeper. This is what turns "the flag is
    assigned somewhere" into "the flag is assigned whenever this path runs"
    (main_3ds_flow_test.sh review-14 F1/F2, applied to the backend's copy)."""
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


def printf_format(text, paren):
    """The format expression of the printf whose '(' sits at `paren`: everything to
    the first top-level comma, string-aware, so a format split across concatenated
    literals is read whole. `call_args` here takes a line and a function NAME, which
    is the wrong shape for a position found by scanning."""
    depth = 0
    i = paren
    n = len(text)
    while i < n:
        c = text[i]
        if c in '"\'':
            quote = c
            i += 1
            while i < n and text[i] != quote:
                i += 2 if text[i] == "\\" else 1
        elif c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return text[paren + 1:i]
        elif c == "," and depth == 1:
            return text[paren + 1:i]
        i += 1
    return ""


def fn_body_span(text, name):
    """(start, end) of a function whose return type is anything, through its closing
    brace. `body_span` requires `void`, and the mesh factory returns MdkMeshId."""
    m = re.search(r"(?m)^[ \t]*[A-Za-z_][A-Za-z0-9_:* \t]*[ \t]+" + re.escape(name) +
                  r"[ \t]*\([^;{]*\)[ \t]*\{", text)
    if not m:
        return None
    sp = block_span_from(text, m.end() - 1)
    return (m.start(), sp[1]) if sp else None


def body_span(text, name):
    """Absolute (start, end) of one function, from its signature through its
    closing brace, or None.

    The `{` is part of the match on purpose: without it the pattern also matches
    the forward declaration `static void name(...);`, which comes first in this
    file, and the body of an unrelated block gets handed back as the function."""
    m = re.search(r"(?m)^[ \t]*(?:static[ \t]+)?void[ \t]+" + re.escape(name) +
                  r"[ \t]*\([^;{]*\)[ \t]*\{", text)
    if not m:
        return None
    sp = block_span_from(text, m.end() - 1)
    return (m.start(), sp[1]) if sp else None


def body(text, name):
    """One function's source text, or None."""
    sp = body_span(text, name)
    return text[sp[0]:sp[1]] if sp else None


# A log line is a REFUSAL line when its own format says so. The frontend gate
# (main_3ds_flow_test.sh) uses the same test for the same reason: the property is
# about EVERY refusal line, and a first-pass diagnostic that reports the failed
# allocation without spelling "FAILED" is still a refusal line and still spams.
REFUSAL_FMT = re.compile(r"FAILED|refus", re.IGNORECASE)

# The two spellings of "print only while the latch is down", matched with
# `fullmatch` so a condition that merely MENTIONS the negation is not a guard. The
# optional `name->` is there because the backend reaches the field through the
# renderer pointer and the local's name is not part of the invariant -- pinning
# `renderer->` would redden a rename of the local, which is review-10 F6's failure
# (a gate that rejects a shape the port may write gets routed around).
#
# Review-16 R2: the prefix used to sit INSIDE the first alternative only, so the
# `== false` spelling this comment promised was accepted could only match a bare
# local -- a shape this backend never writes, since the field is reached through the
# struct pointer. `if (renderer->meshAllocFailureLogged == false) {`, which is the
# SAME state machine, therefore failed `fullmatch`, the guard stopped counting as a
# latch guard, and the gate printed `FAIL ... 1 refusal printf(s), all inside a
# latch-guarded block that arms it: no` over correct code. The prefix is now a
# separate optional group so it applies to both spellings, and a probe pins the
# `== false` form so the fix cannot rot back.
_LATCH_PREFIX = r"(?:[A-Za-z_][A-Za-z0-9_]*[ \t]*->[ \t]*)?"
LATCH_NEG = re.compile(r"![ \t]*" + _LATCH_PREFIX + r"meshAllocFailureLogged"
                       r"|" + _LATCH_PREFIX + r"meshAllocFailureLogged"
                       r"[ \t]*==[ \t]*false")


def eye_loop_span(text):
    """Absolute (start, end) of the per-eye loop of mdk_render_end_frame, or
    None. Searched inside the function body, so a `for (int eye` anywhere else
    cannot satisfy a check. Absolute, because the mutation arms splice the file
    at these offsets and a check slices the file with them."""
    bs = body_span(text, "mdk_render_end_frame")
    if bs is None:
        return None
    sp = block_span(text[bs[0]:bs[1]], EYE_LOOP)
    if sp is None:
        return None
    return (bs[0] + sp[0], bs[0] + sp[1])


def first_call_line(block, name):
    """Line index of the first code line of `block` that calls `name(`, or None."""
    if block is None:
        return None
    for i, line in code_lines(block):
        if re.search(re.escape(name) + r"\s*\(", line):
            return i
    return None


def line_depths(block):
    """Brace depth at the START of each line of `block`, relative to the block's
    own '{': a statement written directly in the block is depth 0, one inside an
    `if` in the block is depth 1.

    This is what turns "inside the loop" into "executed every time the loop
    runs" (review-3 F2): `if (eye == 0) { drawDiagnostic(r); }` keeps exactly
    one call site inside the loop, but the call sits at depth 1."""
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


def statement_at(block, name):
    """(line index, depth) of the first code line of `block` that IS a statement
    calling `name(` — the whole line, so a call wrapped in `if (eye == 0)` or
    `for (...)` on the same line is not a match — or None.

    Depth is the second element and is the load-bearing half: a call on its own
    line one level deeper is a guarded call, not an unconditional one."""
    if block is None:
        return None
    depths = line_depths(block)
    for i, line in code_lines(block):
        if re.match(re.escape(name) + r"\s*\(", line.strip()):
            return (i, depths[i] if i < len(depths) else None)
    return None


def first_statement_line(block):
    """Line index of the first depth-0 code line of `block`, or None. The
    block's own `{` line is depth -1 by construction and comments are dropped,
    so this is the first statement the block actually runs."""
    if block is None:
        return None
    depths = line_depths(block)
    for i, line in code_lines(block):
        if i < len(depths) and depths[i] == 0 and line.strip():
            return i
    return None


def eye_loop_header(text):
    """The eye loop's own header — `for` through the brace that opens its body —
    or None. The bound and the initialiser live HERE, not in the body span, so a
    check about how many times the loop runs has to read this (review-4 F5)."""
    bs = body_span(text, "mdk_render_end_frame")
    if bs is None:
        return None
    seg = text[bs[0]:bs[1]]
    m = re.search(EYE_LOOP, seg)
    if m is None:
        return None
    brace = seg.find("{", m.end())
    return seg[m.start():brace] if brace >= 0 else None


def span_lines(text, sp):
    """1-based (first, last) lines actually OCCUPIED by a span, inclusive at both
    ends. `line_range` is one short at the end (it counts newlines before a
    position), which is fine for membership and wrong for "is this call after
    that brace" — hence a second helper rather than a change to the first."""
    if sp is None:
        return None
    return (text.count("\n", 0, sp[0]) + 1, text.count("\n", 0, sp[1] - 1) + 1)


def call_args(line, name):
    """The top-level argument strings of the first `name(...)` on `line`, or
    None. Splitting on commas by hand is the point: the second argument of the
    real installEye call is a ternary, and a check that wants to know which
    branch the FIRST eye takes cannot get that from a substring test
    (review-4 F4)."""
    m = re.search(re.escape(name) + r"\s*\(", line)
    if not m:
        return None
    i = m.end() - 1
    depth = 0
    cur = ""
    args = []
    for j in range(i, len(line)):
        ch = line[j]
        if ch in "([{":
            depth += 1
            if depth == 1:
                continue
        elif ch in ")]}":
            depth -= 1
            if depth == 0:
                args.append(cur.strip())
                return args
        if ch == "," and depth == 1:
            args.append(cur.strip())
            cur = ""
            continue
        if depth >= 1:
            cur += ch
    return None


def line_range(text, sp):
    """1-based inclusive (first line, last line) of an absolute (start, end)
    span, so a file-wide call site can be tested for membership in a body."""
    if sp is None:
        return None
    return (text.count("\n", 0, sp[0]) + 1, text.count("\n", 0, sp[1]))


def sites(text, pattern):
    """Call sites of a regex over the whole file, comments excluded.

    Separate from calls() because that helper builds a definition-skip out of
    the pattern, and an alternation breaks it: `A|B` would make every line
    containing B look like a definition."""
    return [(i + 1, line.strip()) for i, line in code_lines(text)
            if re.search(pattern, line)]


def calls(text, pattern):
    """Call sites of a regex pattern: comment lines and the pattern's own
    definition line are excluded."""
    hits = []
    for i, line in code_lines(text):
        if re.search(r"\bvoid[ \t]+" + pattern + r"[ \t]*\(", line):
            continue
        if re.search(pattern, line):
            hits.append((i + 1, line.strip()))
    return hits


def call_sites(text, name):
    """Call sites of a named function, ANY argument list. A literal `name(r)`
    match is not enough: the same call written `name(asC3d(rr))` is the same
    defect (review-2 C). A declaration or definition line is not a call site."""
    hits = []
    for i, line in code_lines(text):
        if re.search(r"\b(?:static[ \t]+)?(?:void|bool|int)[ \t]+" + re.escape(name) +
                     r"\s*\(", line):
            continue
        if re.search(re.escape(name) + r"\s*\(", line):
            hits.append((i + 1, line.strip()))
    return hits


class Report:
    def __init__(self, quiet=False):
        self.checks = 0
        self.failed = 0
        self.quiet = quiet
        self.broke = None

    def check(self, name, ok, detail):
        self.checks += 1
        if ok:
            if not self.quiet:
                print("  ok   %s: %s" % (name, detail))
            return
        self.failed += 1
        if self.broke is None:
            self.broke = name
        if not self.quiet:
            print("  FAIL c3d_frame %s: %s" % (name, detail))


def adjudicate(text, quiet=False):
    """The invariants, over whatever text is handed in."""
    rep = Report(quiet)
    diag_body = body(text, "drawDiagnostic")
    sp = eye_loop_span(text)
    loop = text[sp[0]:sp[1]] if sp else None
    replay_at = first_call_line(loop, "replayQueue")
    diag_at = first_call_line(loop, "drawDiagnostic")

    replay_stmt = statement_at(loop, "replayQueue")
    diag_stmt = statement_at(loop, "drawDiagnostic")
    install_stmt = statement_at(loop, "installEye")

    begin_range = span_lines(text, body_span(text, "mdk_render_begin_frame"))
    end_body = body(text, "mdk_render_end_frame")
    end_range = span_lines(text, body_span(text, "mdk_render_end_frame"))
    loop_lines = span_lines(text, sp)

    diag = calls(text, r"C3D_DrawArrays\(GPU_TRIANGLES, 0, 3\)")
    rep.check("diagnostic-drawn-once",
              len(diag) == 1 and diag_body is not None and
              "C3D_DrawArrays(GPU_TRIANGLES, 0, 3)" in diag_body,
              "%d diagnostic draw(s); inside drawDiagnostic: %s (per-draw it cost 2 depth "
              "writes + 1 draw per real draw, per eye)"
              % (len(diag), "yes" if diag_body and
                 "C3D_DrawArrays(GPU_TRIANGLES, 0, 3)" in diag_body else "no"))

    dcall = call_sites(text, "drawDiagnostic")
    rep.check("diagnostic-called-in-the-eye-loop",
              len(dcall) == 1 and diag_at is not None,
              "%d call(s); inside the eye loop: %s (once AFTER the loop is a triangle in one "
              "eye and a hole in the other, and both readings pass an in-end_frame test)"
              % (len(dcall), "yes" if diag_at is not None else "no"))

    rep.check("diagnostic-drawn-after-its-replay",
              diag_at is not None and replay_at is not None and replay_at < diag_at,
              "loop line %s replays, loop line %s draws the diagnostic (drawn BEFORE the "
              "replay it is batch 28: the triangle's floats read as the mesh's, i.e. nothing)"
              % (replay_at, diag_at))

    replay = call_sites(text, "replayQueue")
    rep.check("replay-only-at-frame-end",
              len(replay) == 1 and replay_at is not None,
              "%d replay call site(s) in the file, any argument; inside the eye loop: %s "
              "(a second one emits the frame's records an extra time into whichever target "
              "installEye last ran; a flushDraws-shaped one also empties the queue, so the "
              "eyes after it replay nothing)"
              % (len(replay), "yes" if replay_at is not None else "no"))

    # The loop body's statement set (review-3 F2). Each of the three calls must
    # be a whole statement at depth 0 of the loop body: one call site inside the
    # loop is not enough, because `if (eye == 0) { drawDiagnostic(r); }` is one
    # call site inside the loop and draws the triangle in one eye only.
    guarded = []
    for label, stmt in (("installEye", install_stmt), ("replayQueue", replay_stmt),
                        ("drawDiagnostic", diag_stmt)):
        if stmt is None:
            guarded.append(label + "(not a bare statement of the loop body)")
        elif stmt[1] != 0:
            guarded.append("%s(depth %s)" % (label, stmt[1]))
    rep.check("eye-loop-statements-unconditional", not guarded,
              "installEye/replayQueue/drawDiagnostic are each a depth-0 statement of the "
              "loop body: %s (a call guarded to one eye is a triangle in one eye and a hole "
              "in the other, and it is still exactly one call site inside the loop)"
              % ("clean" if not guarded else ", ".join(guarded)))

    # Nothing may empty the queue between the two eyes (review-3 F2): the second
    # eye replays the SAME records, so a reset inside the loop hands eye 2 an
    # empty queue, which is the black screen this file exists to prevent.
    in_loop_reset = sites(loop or "", RESET)
    rep.check("no-queue-reset-in-the-eye-loop", not in_loop_reset,
              "%d queue reset(s) inside the eye loop; the reset belongs after the LAST eye "
              "(reset inside it and the second eye replays nothing)"
              % len(in_loop_reset))

    # "Inside the loop" was only one of the three places a reset can empty the
    # queue between two eyes (review-4 F2). One placed BEFORE the loop empties it
    # before the first eye ever replays, and one at the end of `replayQueue`
    # — a function the loop calls, so the loop's own text never mentions it —
    # empties it after the first eye. Both reproduce the exact black screen the
    # check above exists to refuse, and both left all twelve review-3 checks green.
    late_reset = []
    for ln, loc in sites(text, RESET):
        if end_range and end_range[0] <= ln <= end_range[1]:
            if loop_lines is None or ln <= loop_lines[1]:
                late_reset.append("%d:%s" % (ln, loc))
    rep.check("frame-end-reset-after-the-eye-loop",
              not late_reset and end_range is not None and loop_lines is not None,
              "%d queue reset(s) inside mdk_render_end_frame at or before the loop's closing "
              "brace: %s (before the loop and BOTH eyes replay an empty queue; the reset after "
              "the last eye is the only position that leaves every eye its records)"
              % (len(late_reset), "none" if not late_reset else "; ".join(late_reset)))

    per_eye_reset = []
    for fn in ("installEye", "replayQueue", "drawRecorded", "drawDiagnostic"):
        b = body(text, fn)
        if b is None:
            per_eye_reset.append(fn + "(missing)")
            continue
        per_eye_reset.extend("%s:%s" % (fn, loc) for _, loc in sites(b, RESET))
    rep.check("per-eye-functions-do-not-empty-the-queue", not per_eye_reset,
              "none of installEye/replayQueue/drawRecorded/drawDiagnostic empties the queue: %s (the eye "
              "loop calls these, so a reset in any of them is a reset between the two eyes seen "
              "from inside a function the loop's own text cannot see)"
              % ("clean" if not per_eye_reset else ", ".join(per_eye_reset)))

    frame_resets = [loc for ln, loc in sites(text, RESET)
                    if (begin_range and begin_range[0] <= ln <= begin_range[1])
                    or (end_range and end_range[0] <= ln <= end_range[1])]
    rep.check("queue-reset-happens-in-the-frame",
              bool(frame_resets) and begin_range is not None and end_range is not None,
              "%d queue reset(s) across begin_frame and end_frame: %s (the queue keeps its "
              "capacity, so if nothing clears it at a frame boundary every frame replays every "
              "earlier frame's records forever)"
              % (len(frame_resets), "yes" if frame_resets else "none"))

    # THE CLEAR COLOUR IS CONVERTED BEFORE THE PICA SEES IT. The seam carries
    # 0xAARRGGBB (what the Vulkan backend unpacks, so what both frontends write);
    # C3D_RenderTargetClear's fill pattern for an RGBA8 buffer is 0xRRGGBBAA. A
    # pass-through makes the ALPHA byte the red channel, which is measured, not
    # theorised: 0xFF102030 filled the panel with #ff1020 on Mandarine r1.2.1.
    # The conversion's own arithmetic is pinned on the host by
    # pica_matrix_tests.cpp; what only a source check can pin is that the backend
    # calls it on the way to the clear.
    begin_body = body(text, "mdk_render_begin_frame") or ""
    clear_calls = re.findall(r"C3D_RenderTargetClear\(", begin_body)
    clear_conv = re.search(r"\bclear\s*=[^;]*pica::clearColour\s*\(", begin_body) is not None
    clear_raw = bool(re.search(r"C3D_RenderTargetClear\([^)]*clear_colour", begin_body))
    rep.check("clear-colour-is-converted-for-the-pica",
              bool(clear_calls) and clear_conv and not clear_raw,
              "%d clear call(s); the value is converted: %s; a raw seam field reaches the PICA: "
              "%s (the seam is 0xAARRGGBB and the PICA's fill is 0xRRGGBBAA, so a pass-through "
              "paints the frame with the alpha byte as red — the red clear this backend shipped "
              "with for its whole history)"
              % (len(clear_calls), "yes" if clear_conv else "no", "yes" if clear_raw else "no"))

    # The loop must RUN once per eye, not merely CONTAIN the calls (review-4 F3).
    # `if (eye == 0) break;` keeps every statement at depth 0 and every call count
    # at 1, and still draws one eye; `return` there is worse still, because it
    # also skips C3D_FrameEnd, so the frame is never submitted at all.
    escapes = sites(loop or "", r"\b(?:break|continue|return|goto)\b")
    rep.check("eye-loop-runs-every-pass",
              sp is not None and not escapes,
              "%d early exit(s) inside the loop body: %s (a guarded break is the tidy-up that "
              "\"saves an eye\" on a 268 MHz ARM11: the panel then shows one drawn eye and one "
              "clear-colour eye, and every count and depth this file checks is still clean)"
              % (len(escapes), "none" if not escapes
                 else "; ".join(loc for _, loc in escapes)))

    header = eye_loop_header(text)
    rep.check("eye-loop-bound-is-the-eye-plan",
              header is not None and
              re.search(r"eye\s*<\s*[A-Za-z_]\w*\s*->\s*eyes\.passes\b", header) is not None,
              "the loop header is `%s` (bounded by eyePlan's pass count, so a closed slider "
              "runs one pass and an open one two; a literal bound draws one eye whatever the "
              "slider says and the other buffer keeps the clear colour)" % (header or "(none)"))

    exact_plan_loop = (header is not None and re.fullmatch(
        r"\s*for\s*\(\s*int\s+eye\s*=\s*0\s*;\s*eye\s*<\s*"
        r"[A-Za-z_]\w*\s*->\s*eyes\s*\.\s*passes\s*;\s*eye\s*\+\+\s*\)\s*",
        header) is not None)
    rep.check("eye-loop-uses-the-unchanged-plan-count",
              exact_plan_loop,
              "the whole loop header is `%s` (the plan count must be used unchanged: "
              "clamping it, narrowing the condition, or changing the increment skips eyes)"
              % (header or "(none)"))

    # The eye count is captured from eyePlan in begin_frame and is immutable
    # until this replay completes. A write in end_frame can make a structurally
    # correct loop silently draw only one eye (review-5 F3).
    pass_writes = sites(end_body or "",
                        r"(?:->|\.)\s*eyes\s*(?:\.\s*passes)?\s*(?:=(?!=)|\+=|-=|\+\+|--)(?=[^\w]|$)")
    rep.check("eye-plan-count-not-written-in-end-frame",
              not pass_writes,
              "%d eye-plan write(s) in mdk_render_end_frame: %s (eyePlan's pass count "
              "must survive unchanged from begin_frame through the per-eye replay)"
              % (len(pass_writes), "none" if not pass_writes else "; ".join(
                  "%d:%s" % (ln, loc) for ln, loc in pass_writes)))

    rep.check("eye-loop-starts-at-the-first-eye",
              header is not None and re.search(r"int\s+eye\s*=\s*0\s*;", header) is not None,
              "the loop initialises eye to 0: `%s` (starting at 1 skips the buffer the panel "
              "shows when the slider is closed)" % (header or "(none)"))

    # installEye is the call batch 30 was written to place (review-3 F3). Hoisted
    # above the loop it is the pre-batch-30 shape: both eyes replay into the last
    # target installed, and the panel shows the other one.
    install = call_sites(text, "installEye")
    install_line = install[0][1] if len(install) == 1 else ""
    first_stmt = first_statement_line(loop)
    rep.check("install-eye-first-in-loop",
              len(install) == 1 and install_stmt is not None and
              install_stmt[1] == 0 and first_stmt is not None and
              install_stmt[0] == first_stmt,
              "%d installEye call(s); inside the eye loop: %s at depth %s, first statement: "
              "%s (hoisted above the loop, every eye draws into the last target installed)"
              % (len(install), "yes" if install_stmt is not None else "no",
                 install_stmt[1] if install_stmt else "-",
                 "yes" if install_stmt and first_stmt is not None and
                 install_stmt[0] == first_stmt else "no"))

    rep.check("install-eye-per-eye-target",
              len(install) == 1 and "targetLeft" in install_line and
              "targetRight" in install_line and
              re.search(r"eyes\.offset\s*\[\s*eye\s*\]", install_line) is not None,
              "the one installEye call selects the target BY EYE and passes the per-eye "
              "offset: %s (a fixed target sends both eyes to one buffer)"
              % (install_line if install_line else "no single call"))

    # Which branch the FIRST eye takes, and the sign of the offset (review-4 F4).
    # The check above is a substring test, so `eye == 0 ? targetRight : targetLeft`
    # and `-r->eyes.offset[eye]` both pass it, and both are inside-out stereo: a
    # pseudoscopic picture a player takes the glasses off to escape, with every
    # count in this file still at 1. eyePlan already gives the LEFT eye the
    # negative half (pica_matrix.h), so the plan's value must reach the matrix
    # un-negated and indexed by the loop variable.
    iargs = call_args(install_line, "installEye") if install_line else None
    first_branch = other_branch = None
    if iargs and len(iargs) == 3:
        tern = re.match(r"^(.*?)\s*\?\s*(.*?)\s*:\s*(.*)$", iargs[1])
        if tern:
            cond, tbr, fbr = tern.groups()
            cond = cond.strip()
            if re.search(r"eye\s*==\s*0", cond):
                first_branch, other_branch = tbr, fbr
            elif cond == "eye" or re.search(r"eye\s*(?:!=\s*0|>\s*0)", cond):
                first_branch, other_branch = fbr, tbr
    rep.check("install-eye-target-order",
              first_branch is not None and "targetLeft" in first_branch and
              "targetRight" not in first_branch and "targetRight" in other_branch,
              "the FIRST eye's branch names targetLeft and the other names targetRight: %s "
              "(swapping the branches is pseudoscopic stereo, and it changes no count anywhere)"
              % (iargs[1] if iargs and len(iargs) == 3 else "no single three-argument call"))

    offset_arg = iargs[2].strip() if iargs and len(iargs) == 3 else ""
    rep.check("install-eye-offset-sign",
              re.fullmatch(r"[A-Za-z_]\w*\s*->\s*eyes\.offset\s*\[\s*eye\s*\]", offset_arg)
              is not None,
              "the offset argument is the plan's own value, un-negated and indexed by eye: %s "
              "(eyePlan already gives the left eye the NEGATIVE half, so a leading minus here "
              "flips the stereo inside out)" % (offset_arg or "(none)"))

    leaking = []
    for seam in SEAMS:
        b = body(text, seam)
        if b is None:
            leaking.append(seam + "(missing)")
            continue
        for i, line in code_lines(b):
            if re.search(EMIT_ANY, line):
                leaking.append("%s:%d" % (seam, i + 1))
    rep.check("no-mid-frame-flush", not leaking,
              "the canvas (including canvas_quad, the seam hud_view.cpp drives), inset and "
              "present seams emit nothing at all: %s"
              % ("clean" if not leaking else ", ".join(leaking)))

    # The emission set is CLOSED (review-3 F1): a seam that does not call
    # replayQueue can still draw by itself, and the queue is not the only thing
    # that can reach the GPU. Every draw call in the file must live in one of the
    # two functions the per-eye loop reaches, and drawRecorded must be reachable
    # only from the replay.
    allowed = [line_range(text, body_span(text, n))
               for n in ("drawRecorded", "drawDiagnostic")]
    stray = [loc for ln, loc in sites(text, EMIT_DRAW)
             if not any(r and r[0] <= ln <= r[1] for r in allowed)]
    rep.check("emission-sites-closed",
              not stray and all(allowed),
              "%d draw call(s) outside drawRecorded/drawDiagnostic: %s (an immediate draw "
              "anywhere else is outside the per-eye replay, so it lands in one target or "
              "none)" % (len(stray), "none" if not stray else "; ".join(stray)))

    rec = call_sites(text, "drawRecorded")
    rec_range = line_range(text, body_span(text, "replayQueue"))
    stray_rec = [loc for ln, loc in rec
                 if not (rec_range and rec_range[0] <= ln <= rec_range[1])]
    rep.check("recorded-draws-only-from-the-replay",
              len(rec) >= 1 and not stray_rec,
              "%d drawRecorded call(s), %d outside replayQueue: %s (a record drawn from a "
              "seam skips the eye loop)"
              % (len(rec), len(stray_rec), "none" if not stray_rec else "; ".join(stray_rec)))

    depth = calls(text, r"C3D_DepthTest\(")
    rep.check("depth-state-single-writer", len(depth) == 1,
              "%d C3D_DepthTest call site(s); the cache is the only writer, so a second one "
              "writes behind its back" % len(depth))

    # THE BLEND AND ALPHA-TEST STATE, SAME HAZARD, SAME SHAPE (review-3 F3).
    # Batch 83 put a per-pass blend cache and a frame-wide alpha test beside the
    # depth cache, and the depth cache already had this check for exactly the
    # reason they now have it: a second call site writes behind the cache's
    # back, so `r->blendPass/r->blendMode` stops describing the register and the
    # opaque and blended passes start alternating wrongly. Nothing else can see
    # it either — the start arena is 154 all-Solid batches, and batch 84
    # measured that the whole blend change moves 0 pixels of the 400x240 panel
    # (docs/evidence/2026-11-06-L7-blend-ab.txt), so no runtime capture covers
    # these two calls at all.
    blend = calls(text, r"C3D_AlphaBlend\(")
    rep.check("alpha-blend-single-writer", len(blend) == 1,
              "%d C3D_AlphaBlend call site(s); installBlendState and its cache are the "
              "only writer, so a second one writes behind its back: %s"
              % (len(blend), "none" if not blend else "; ".join(loc for _, loc in blend)))

    # The alpha test is pinned three ways: ONE writer, INSIDE installEye (so
    # both eyes get it — the same placement rule that made batch 30's black
    # screen a checked property), and spelled with the oracle's own predicate.
    # A reference moved to 5, or GPU_ALWAYS, or a `false`, is invisible in every
    # scene this frontend can draw today, so the spelling is the pin.
    atest = calls(text, r"C3D_AlphaTest\(")
    eye_install = line_range(text, body_span(text, "installEye"))
    atest_stray = [loc for ln, loc in atest
                   if not (eye_install and eye_install[0] <= ln <= eye_install[1])]
    rep.check("alpha-test-single-writer-in-install-eye",
              len(atest) == 1 and eye_install and not atest_stray,
              "%d C3D_AlphaTest call(s), %d outside installEye: %s (frame state one eye "
              "does not get is a hole in that eye)"
              % (len(atest), len(atest_stray),
                 "none" if not atest_stray else "; ".join(atest_stray)))

    discard = [loc.strip() for _, loc in code_lines(text)
               if re.search(r"C3D_AlphaTest\(\s*true\s*,\s*GPU_GREATER\s*,\s*"
                            r"mdk::pica::kAlphaTestReference\s*\)", loc)]
    rep.check("alpha-test-is-the-oracles-discard", len(discard) == 1,
              "%d C3D_AlphaTest written as (true, GPU_GREATER, kAlphaTestReference): %s "
              "(GPU_GREATER against 0 IS `if (index == 0) discard;`, because the palette "
              "expansion encodes index 0 as alpha 0 and every other index as 255; any "
              "other spelling is a different predicate and a discarded fragment does not "
              "write depth)" % (len(discard), discard or ["none"]))

    # THE MICROCODE IS UPLOADED FROM INSIDE THE FRAME (review-6 F6, external review
    # 2026-11-05). Batch 66 hung the guest between `shaderProgramSetVsh ok` and the
    # `backend:` token; batch 67 found the cause by DELETING the create-time
    # `shaderProgramConfigure` and watching the guest boot. That is the whole
    # functional fix of a batch, and nothing in any gate could see it: the shipped
    # citro3d sends the configure itself from `C3Di_UpdateContext`, right after the
    # bind, so a second send at create is GPUCMD_Add into whatever command buffer
    # happened to be current before the first frame existed. A one-line "restore
    # symmetry with the sibling port" edit re-opens a hang that costs an emulator
    # run to find, so the shape is pinned here with the upload and the bind it goes
    # with — otherwise the check would be satisfied by deleting the shader setup.
    conf = call_sites(text, "shaderProgramConfigure")
    rep.check("shader-configure-never-sent-by-hand",
              not conf,
              "%d shaderProgramConfigure call site(s): %s (C3Di_UpdateContext sends it "
              "after the bind, inside the frame; a hand-written one at create is a "
              "GPUCMD_Add outside any C3D_FrameBegin, which is what stopped the guest "
              "before `backend:` in batch 66)"
              % (len(conf), "none" if not conf else "; ".join(loc for _, loc in conf)))

    # The span must start BEFORE the opening brace: block_span_from looks for the
    # first `{` at or after the match end, so a pattern that swallows the brace
    # hands back the first inner block and every line of the real function reads as
    # "outside" it — which is a check that fails on the good file, the only kind of
    # failure a source gate can honestly have.
    create_span = block_span(text, r"MdkRenderer mdk_render_create\(const MdkRenderDesc\* desc\)")
    create_range = line_range(text, create_span)
    vsh = call_sites(text, "shaderProgramSetVsh")
    vsh_stray = [loc for ln, loc in vsh
                 if not (create_range and create_range[0] <= ln <= create_range[1])]
    rep.check("shader-uploaded-exactly-once-at-create",
              len(vsh) == 1 and not vsh_stray and create_range,
              "%d shaderProgramSetVsh call(s), %d outside mdk_render_create: %s "
              "(setVsh builds the instance the configure later uploads; zero means the "
              "program has no microcode, two means something rebinds it mid-life)"
              % (len(vsh), len(vsh_stray),
                 "none" if not vsh_stray else "; ".join(vsh_stray)))

    eye_range = line_range(text, body_span(text, "installEye"))
    bind = call_sites(text, "C3D_BindProgram")
    bind_stray = [loc for ln, loc in bind
                  if not (eye_range and eye_range[0] <= ln <= eye_range[1])]
    rep.check("program-bound-only-where-an-eye-is-installed",
              len(bind) == 1 and not bind_stray and eye_range,
              "%d C3D_BindProgram call(s), %d outside installEye: %s (the bind is what "
              "marks the shader dirty so UpdateContext configures it once per frame; a "
              "bind outside the eye install runs outside the frame contract)"
              % (len(bind), len(bind_stray),
                 "none" if not bind_stray else "; ".join(bind_stray)))

    # THE BACKEND'S OWN REFUSED-MESH LINE IS AN EDGE TRIGGER TOO. Review-14's related
    # observation: the frontend's copy of this latch is pinned three times over and
    # its counterpart here was pinned zero times, and THIS is the line that actually
    # printed 1710 times in one run (review-10 F3). The frontend line answers this
    # one, so pinning only the answering line leaves the louder of the pair free to
    # spam: a reader sees one honest refusal in the log and the file fills with the
    # other. Three properties, the same three the frontend is held to -- the guard IS
    # the negative latch test, the block that prints also ARMS the latch as a direct
    # statement of itself, and the latch CLEARS on the success path as a direct
    # statement of the function, not from inside the failing branch.
    #
    # Review-15 F2: the first version resolved ONE printf and ONE clear, which is the
    # exact shape review-14 F7 told this project to stop writing. A second
    # `log3dsPrintf("... FAILED again this frame", cap)` placed after the guarded
    # block inside `if (!mesh.data)` printed on every refusing frame with the check
    # green, and `r->meshAllocFailureLogged = false;` added to
    # `mdk_render_begin_frame` -- once per frame, under a sustained refusal -- re-armed
    # the latch every frame and put the 1710-line spam back, also green. So the
    # refusal net now reads EVERY refusal-format printf in the function, and the
    # clear is checked twice: in place in THIS function, and as the ONLY clear of the
    # latch anywhere in the file.
    mesh_body = fn_body_span(text, "mdk_render_create_dynamic_mesh")
    refusals = []
    guards = []
    armed = False
    cleared = False
    sole_clear = False
    if mesh_body is not None:
        seg = blank_comments(text[mesh_body[0]:mesh_body[1]])
        open_brace = seg.index("{")
        # `refusals` is a list of POSITIONS, not match objects: the union below mixes
        # two finditer results and only the offset is ever used.
        refusals = sorted(set(
            [p.start() for p in re.finditer(r"log3dsPrintf[ \t]*\(", seg)
             if REFUSAL_FMT.search(printf_format(seg, p.end() - 1))]))
        # Review-15 F3's lesson applies on this side too, so the net is the same UNION
        # the frontend now runs: a line belongs to the refusal family when its format
        # names the refusal WHEREVER it sits in the function, or when it sits inside
        # the failing branch whatever it says. A `log3dsPrintf("dynamic mesh: %d
        # corners requested", cap)` after the guard would print on every refusing
        # frame, would name no FAILED, and a format filter alone reads straight past
        # it -- which is review-15 F3 on the frontend, one file over.
        failing = next((block_span_from(seg, m.end() - 1)
                        for m in re.finditer(r"\bif[ \t]*\([ \t]*![ \t]*mesh\.data"
                                             r"[ \t]*\)[ \t]*\{", seg)), None)
        if failing is not None:
            refusals = sorted(set(
                refusals
                + [p.start() for p in re.finditer(r"log3dsPrintf[ \t]*\(", seg)
                   if failing[0] <= p.start() < failing[1]]))
        guards = [block_span_from(seg, g.end() - 1)
                  for g in re.finditer(r"\bif[ \t]*\(([^{}();]*)\)[ \t]*\{", seg)
                  if LATCH_NEG.fullmatch(g.group(1).strip())]
        guards = [sp for sp in guards if sp is not None]

        def arms_latch(sp):
            """Does the block `sp` arm the latch as a direct statement of itself?"""
            s = re.search(r"(?:->|\.)[ \t]*meshAllocFailureLogged[ \t]*=[ \t]*true[ \t]*;",
                          seg[sp[0]:sp[1]])
            return (s is not None
                    and brace_depth(seg, sp[0], sp[0] + s.start()) == 1)

        armed = bool(refusals) and all(
            any(sp[0] <= at < sp[1] and arms_latch(sp) for sp in guards)
            for at in refusals)
        # The clear must be a direct statement of the FUNCTION, which is what puts it
        # on the success path: inside `if (!mesh.data)` it would be depth 2 and would
        # clear on the very frame it refused.
        #
        # Every write scan requires the `->` (or `.`) because the field's DECLARATION
        # in the renderer struct -- `bool meshAllocFailureLogged = false;` -- is an
        # initializer, not a write, and a scan that counted it would count two clears
        # in an unmutated file and red correct code (review-10 F6 again: a gate that
        # rejects a shape the port may write is a gate that gets routed around).
        c = re.search(r"(?:->|\.)[ \t]*meshAllocFailureLogged[ \t]*=[ \t]*false[ \t]*;", seg)
        cleared = (c is not None
                   and brace_depth(seg, open_brace, c.start()) == 1)
        # And it must be the ONLY clear of the latch in the file. A reset written in
        # some per-frame function instead runs once per frame, which is exactly the
        # condition the latch exists to suppress, and no per-function scan can see it.
        clears = list(re.finditer(
            r"(?:->|\.)[ \t]*meshAllocFailureLogged[ \t]*=[ \t]*false[ \t]*;",
            blank_comments(text)))
        sole_clear = (len(clears) == 1
                      and mesh_body[0] <= clears[0].start() < mesh_body[1])
        # Review-16 open question 2, closed with a pin rather than a comment. The net
        # counts WRITES spelled `x->meshAllocFailureLogged = false;`. A write through
        # an alias -- `bool* const l = &renderer->meshAllocFailureLogged; *l = false;`
        # in `mdk_render_begin_frame` -- is the same per-frame reset and is invisible
        # to a name scan. The reviewer judged an alias implausible in this file, and
        # probably right; the counterargument is that every other write net in this
        # family (the frontend's check 20) had to learn the same lesson, and taking the
        # address of a latch is a normal refactor for a field written in two places.
        # So: if the address of the latch is ever taken, the name scan no longer
        # describes the write set and this check says so instead of passing.
        aliased = bool(re.search(r"&[ \t]*(?:[A-Za-z_][A-Za-z0-9_]*[ \t]*->[ \t]*)?"
                                 r"meshAllocFailureLogged", blank_comments(text)))
        sole_clear = sole_clear and not aliased
        # STATED LIMIT, not a hidden one: the net is "exactly one clear in the file",
        # which is the property the spam needs. A future teardown reset would be a
        # second clear and would have to be argued into this check on purpose, which
        # is the point -- a per-frame reset is the defect (review-15 F2), and anything
        # else has to be a decision rather than an oversight.
        #
        # Review-16 open question 1, stated rather than fixed: the guard and refusal
        # scan is scoped to the mesh factory's body, because that is where the latch
        # lives. A refusal printf for a mesh moved into a helper called from here would
        # sit outside that span and this net would not see it. That is the same scope
        # assumption review-16 R1 caught on the frontend one level up, and it is here
        # on purpose: the frontend's frame body is a single function that owns its
        # whole frame, whereas a mesh factory legitimately delegates. If mesh creation
        # ever does move, this check moves with it; it is not a defect today, but it is
        # not coverage either.
    rep.check("mesh-refusal-line-is-edge-triggered",
              bool(refusals) and armed and cleared,
              "%d refusal printf(s), all inside a latch-guarded block that arms it: "
              "%s, cleared on the success path: %s (an unguarded or never-armed line "
              "prints on every frame of one refusal -- this is the line that printed "
              "1710 times; a latch that never clears silences the second, larger "
              "refusal)"
              % (len(refusals), "yes" if armed else "no",
                 "yes" if cleared else "no"))
    rep.check("mesh-refusal-latch-cleared-nowhere-else", sole_clear,
              "the latch must be cleared exactly once in the file, on the mesh "
              "factory's success path (review-15 F2); a second clear anywhere else "
              "runs on some other schedule, and one that runs every frame re-arms the "
              "latch under a sustained refusal -- the spam this latch exists to stop. "
              "A write through an alias (`&renderer->meshAllocFailureLogged`) also "
              "reddens this check: a name scan cannot see it, so it must not be trusted "
              "while it exists (review-16 open question 2)")
    return rep


# --- mutation arms ----------------------------------------------------------
# Each one reintroduces one of the defects the checks claim to prevent. An arm
# that cannot build its mutant is a FAIL below, never a skip (review-10 F1).

def arm_drop_diagnostic(text):
    """The diagnostic call removed entirely."""
    out, n = re.subn(r"(?m)^[ \t]*drawDiagnostic\w*\([A-Za-z0-9_]*\);[ \t]*\n", "",
                     text, count=1)
    return out if n else text


def arm_move_diagnostic(text, where):
    """Move the diagnostic call out of its claimed position in the eye loop.
    'after-loop' is one triangle for one eye; 'before-replay' is batch 28."""
    sp = eye_loop_span(text)
    if sp is None:
        return text
    a, b = sp
    loop = text[a:b]
    m = re.search(r"(?m)^([ \t]*)(drawDiagnostic\w*\([A-Za-z0-9_]*\);)[ \t]*\n", loop)
    if m is None:
        return text
    indent, call = m.group(1), m.group(2)
    loop2 = loop[:m.start()] + loop[m.end():]
    if where == "after-loop":
        spliced = text[:a] + loop2 + text[b:]
        cut = a + len(loop2)
        return spliced[:cut] + "\n" + indent + call + spliced[cut:]
    p = re.search(r"(?m)^[ \t]*replayQueue\s*\(", loop2)
    if p is None:
        return text
    loop2 = loop2[:p.start()] + indent + call + "\n" + loop2[p.start():]
    return text[:a] + loop2 + text[b:]


def arm_seam_replay(text, fn, arg):
    """Restore a mid-frame replay at a seam, with the argument shape a builder
    would actually write there."""
    m = re.search(r"void[ \t]+" + fn + r"[ \t]*\([^)]*\)[ \t]*\{", text)
    if not m:
        return text
    return text[:m.end()] + "\n    replayQueue(%s);" % arg + text[m.end():]


def arm_guard_loop_statement(text, name):
    """Wrap one statement of the eye loop in `if (eye == 0) { ... }`.

    Still exactly one call site, still inside the loop, and it runs for ONE eye
    only (review-3 F2). The optimisation that writes it is a plausible one: the
    diagnostic is a debug aid, so "only draw it in the left eye" reads as a
    saving on a 268 MHz ARM11."""
    sp = eye_loop_span(text)
    if sp is None:
        return text
    a, b = sp
    loop = text[a:b]
    m = re.search(r"(?m)^([ \t]*)(" + name + r"\w*\([A-Za-z0-9_]*\);)[ \t]*\n", loop)
    if m is None:
        return text
    ind, call = m.group(1), m.group(2)
    wrapped = "%sif (eye == 0) {\n%s    %s\n%s}\n" % (ind, ind, call, ind)
    loop2 = loop[:m.start()] + wrapped + loop[m.end():]
    return text[:a] + loop2 + text[b:]


def arm_reset_in_loop(text):
    """Add `r->queue.reset();` as the last statement of the eye loop, the shape
    a tidy-up writes when it wants the queue released as soon as it is used.
    The second eye then replays an empty queue (review-3 F2)."""
    sp = eye_loop_span(text)
    if sp is None:
        return text
    a, b = sp
    return text[:b - 1] + "        r->queue.reset();\n    " + text[b - 1:]


def arm_install_hoisted(text):
    """Move installEye out of the eye loop and above it — the pre-batch-30 shape
    that made the frame a screen of clear colour (review-3 F3). The hoist has to
    name one target, because there is no `eye` outside the loop, which is itself
    the tell: the per-eye target only exists inside the loop."""
    sp = eye_loop_span(text)
    if sp is None:
        return text
    a, b = sp
    loop = text[a:b]
    m = re.search(r"(?m)^([ \t]*)(installEye\([^)]*\);)[ \t]*\n", loop)
    if m is None:
        return text
    ind = m.group(1)
    loop2 = loop[:m.start()] + loop[m.end():]
    spliced = text[:a] + loop2 + text[b:]
    cut = text[:a].rfind("\n") + 1
    hoisted = ind + "installEye(r, r->targetLeft, r->eyes.offset[0]);\n"
    return spliced[:cut] + hoisted + spliced[cut:]


def arm_install_after_replay(text):
    """Swap installEye and replayQueue inside the loop: the replay goes to
    whatever target the PREVIOUS eye installed. Every count is still one and
    every call is still a depth-0 statement of the loop body, so only the
    ORDER inside the loop can catch it."""
    sp = eye_loop_span(text)
    if sp is None:
        return text
    a, b = sp
    loop = text[a:b]
    mi = re.search(r"(?m)^([ \t]*)(installEye\([^)]*\);)[ \t]*\n", loop)
    mr = re.search(r"(?m)^([ \t]*)(replayQueue\([^)]*\);)[ \t]*\n", loop)
    if mi is None or mr is None or mi.start() > mr.start():
        return text
    loop2 = loop[:mi.start()] + loop[mi.end():]
    shift = -len(mi.group(0))
    pos = mr.end() + shift
    return text[:a] + loop2[:pos] + mi.group(0) + loop2[pos:] + text[b:]


def arm_loop_early_exit(text, stmt):
    """Add `if (eye == 0) <kw>;` as the last statement of the eye loop (review-4
    F3). Every call count stays at 1 and every call stays a depth-0 statement of
    the loop body, so only an early-exit check can see it: the loop now runs once
    however many eyes eyePlan asked for. `return` is the worse spelling, because
    it also skips C3D_FrameEnd and the frame is never submitted."""
    sp = eye_loop_span(text)
    if sp is None:
        return text
    a, b = sp
    return text[:b - 1] + "        " + stmt + "\n    " + text[b - 1:]


def arm_reset_before_loop(text):
    """Put the frame's reset ABOVE the eye loop (review-4 F2). The queue is empty
    before the first eye ever replays, so both eyes draw nothing and the panel
    shows the clear colour — the same defect the in-loop arm produces, from a
    placement the in-loop check cannot see."""
    sp = eye_loop_span(text)
    if sp is None:
        return text
    a = text.rfind("for", 0, sp[0])
    if a < 0:
        return text
    cut = text.rfind("\n", 0, a) + 1
    return text[:cut] + "    r->queue.reset();\n" + text[cut:]


def arm_reset_in_install_eye(text):
    """Empty the queue in installEye (review-5 F2), before replay starts for
    either target. The eye loop calls this function, but its own reset scan
    cannot see into the callee."""
    sp = body_span(text, "installEye")
    if sp is None:
        return text
    brace = text.find("{", sp[0], sp[1])
    return text[:brace + 1] + "\n    r->queue.reset();" + text[brace + 1:]


def arm_clamp_eye_pass_count(text):
    """Clamp the saved eyePlan count just before replay (review-5 F3)."""
    sp = eye_loop_span(text)
    if sp is None:
        return text
    a = text.rfind("for", 0, sp[0])
    if a < 0:
        return text
    cut = text.rfind("\n", 0, a) + 1
    return text[:cut] + "    r->eyes.passes = 1;\n" + text[cut:]


def arm_loop_increment_skips_eye(text):
    """Advance by the number of passes rather than one (review-5 F3)."""
    return text.replace("eye++", "eye += r->eyes.passes", 1)


def arm_loop_early_goto(text):
    """Use goto to leave after the first eye; an early-exit scan must include it."""
    mutated = arm_loop_early_exit(text, "if (eye == 0) goto after_eyes;")
    sp = eye_loop_span(mutated)
    if sp is None:
        return mutated
    return mutated[:sp[1]] + "\n    after_eyes: ;" + mutated[sp[1]:]


def arm_reset_in_replay(text):
    """Empty the queue at the end of `replayQueue` (review-4 F2), the shape a
    tidy-up writes when it wants the records released as soon as they are used.
    The first eye draws and the second replays nothing, and the reset is not
    inside the loop's own text at all."""
    sp = body_span(text, "replayQueue")
    if sp is None:
        return text
    return text[:sp[1] - 1] + "    r->queue.reset();\n" + text[sp[1] - 1:]


def arm_reset_removed_from_the_frame(text):
    """Remove every queue reset from both frame boundaries. The queue keeps its
    capacity by design, so nothing else ever drops the records and each frame
    replays every earlier frame's draws on top of its own."""
    out, n = re.subn(r"(?m)^[ \t]*r->queue\.reset\(\);[ \t]*\n", "", text)
    return out if n else text


def arm_install_targets_swapped(text):
    """Swap the two targets in the per-eye ternary (review-4 F4). The left eye's
    matrix goes to the right buffer and vice versa: pseudoscopic stereo, which a
    player experiences as the scene turning inside-out when they put the 3D on.
    Every count in the file is still 1 and the offset is still the plan's."""
    return text.replace("eye == 0 ? r->targetLeft : r->targetRight",
                        "eye == 0 ? r->targetRight : r->targetLeft", 1)


def arm_install_offset_negated(text):
    """Negate the per-eye offset in the installEye call (review-4 F4). eyePlan
    already gives the left eye the negative half, so this flips both eyes the
    wrong way: the same inside-out picture, written on the other argument."""
    return text.replace(", r->eyes.offset[eye]);", ", -r->eyes.offset[eye]);", 1)


def arm_loop_bound(text, old, new):
    """Rewrite the eye loop's header (review-4 F5). `eye < 1` is the mono-only
    loop a builder writes when the slider is not on the desk; `int eye = 1` skips
    the buffer the panel shows in mono. Neither changes a call count or a
    statement depth, so only the header itself can carry the invariant."""
    sp = eye_loop_span(text)
    if sp is None:
        return text
    a = text.rfind("for", 0, sp[0])
    if a < 0:
        return text
    end = text.index("{", a)
    header = text[a:end]
    if old not in header:
        return text
    return text[:a] + header.replace(old, new, 1) + text[end:]


def arm_canvas_quad_emits(text, stmt):
    """Give mdk_render_canvas_quad a body that reaches the GPU, which is what a
    HUD batch writes if it reads the seam contract as "do not flush" instead of
    "the queue is the only way" (review-3 F1)."""
    one = " void mdk_render_canvas_quad(MdkRenderer r, const mdk::CanvasQuad* q) { (void)r; (void)q; }"
    if one not in text:
        return text
    return text.replace(one,
                        " void mdk_render_canvas_quad(MdkRenderer r, const mdk::CanvasQuad* q) {\n"
                        "    (void)r; (void)q;\n    " + stmt + "\n }", 1)


real = adjudicate(SRC)

# The last line the guest wrote while it was hanging (batch 66), used as the
# anchor for the arms that restore the create-time shader traffic.
SHADER_OK_LINE = ('    mdk::log3dsPrintf("shaderProgramSetVsh ok result=0x%08lx", '
                  '(unsigned long)spVsh);')

ARMS = [
    ("diagnostic-back-in-the-per-draw-path",
     lambda s: s.replace("    C3D_DrawArrays(GPU_TRIANGLES, first, count);",
                         "    C3D_DrawArrays(GPU_TRIANGLES, first, count);\n"
                         "    if (r->diagnosticTriangle) {\n"
                         "        C3D_DrawArrays(GPU_TRIANGLES, 0, 3);\n"
                         "    }", 1),
     "diagnostic-drawn-once"),
    ("diagnostic-dropped-from-the-eye-loop", arm_drop_diagnostic,
     "diagnostic-called-in-the-eye-loop"),
    ("diagnostic-after-the-eye-loop", lambda s: arm_move_diagnostic(s, "after-loop"),
     "diagnostic-called-in-the-eye-loop"),
    ("diagnostic-before-its-replay", lambda s: arm_move_diagnostic(s, "before-replay"),
     "diagnostic-drawn-after-its-replay"),
    ("canvas-flush-restored",
     # Review-15 F5's own harvest: this arm spliced `flushDraws(asC3d(r));`, a name
     # that exists only in a comment in this file -- the emitter is `replayQueue`. The
     # mutant never compiled, so it was a mutant of nothing (review-14 F3's class), and
     # the check it claimed to isolate was being certified by text no compiler accepts.
     lambda s: re.sub(r"(void mdk_render_canvas_begin\(MdkRenderer r\) \{)",
                      r"\1\n    replayQueue(asC3d(r));", s, count=1),
     "replay-only-at-frame-end"),
    ("inset-replay-restored",
     lambda s: re.sub(r"(void mdk_render_begin_inset\(MdkRenderer r, const mdk::Inset\* i\) \{)",
                      r"\1\n    replayQueue(asC3d(r));", s, count=1),
     "replay-only-at-frame-end"),
    ("present-replay-restored",
     lambda s: arm_seam_replay(s, "mdk_render_present", "asC3d(rr)"),
     "replay-only-at-frame-end"),
    ("second-depth-writer",
     lambda s: s.replace("    C3D_SetViewport(0, 0, kScreenWidth, kScreenHeight);",
                         "    C3D_DepthTest(true, GPU_LEQUAL, GPU_WRITE_ALL);\n"
                         "    C3D_SetViewport(0, 0, kScreenWidth, kScreenHeight);", 1),
     "depth-state-single-writer"),
    # review-3's five mutants, reproduced as arms so the checks that close those
    # findings cannot be satisfied by a reading of the file.
    ("canvas-quad-draws-immediately",
     lambda s: arm_canvas_quad_emits(s, "C3D_DrawArrays(GPU_TRIANGLE_STRIP, 0, 4);"),
     "no-mid-frame-flush"),
    ("canvas-quad-draws-a-record-directly",
     lambda s: arm_canvas_quad_emits(s, "mdk::MeshDraw rec;\n    drawRecorded(asC3d(r), &rec);"),
     "no-mid-frame-flush"),
    ("diagnostic-guarded-to-one-eye",
     lambda s: arm_guard_loop_statement(s, "drawDiagnostic"),
     "eye-loop-statements-unconditional"),
    ("replay-guarded-to-one-eye",
     lambda s: arm_guard_loop_statement(s, "replayQueue"),
     "eye-loop-statements-unconditional"),
    ("queue-reset-inside-the-eye-loop", arm_reset_in_loop,
     "no-queue-reset-in-the-eye-loop"),
    ("install-hoisted-above-the-eye-loop", arm_install_hoisted,
     "eye-loop-statements-unconditional"),
    ("install-after-its-replay", arm_install_after_replay,
     "install-eye-first-in-loop"),
    ("install-fixed-target-for-both-eyes",
     lambda s: s.replace("installEye(r, eye == 0 ? r->targetLeft : r->targetRight, r->eyes.offset[eye]);",
                         "installEye(r, r->targetLeft, 0.0f);", 1),
     "install-eye-per-eye-target"),
    # review-4's five mutants, each one a placement or a spelling that left every
    # review-3 check green. Written as arms so the checks that close those findings
    # cannot be satisfied by a reading of the file.
    ("eye-loop-breaks-after-the-first-eye",
     lambda s: arm_loop_early_exit(s, "if (eye == 0) break;"),
     "eye-loop-runs-every-pass"),
    ("eye-loop-returns-after-the-first-eye",
     lambda s: arm_loop_early_exit(s, "if (eye == 0) return;"),
     "eye-loop-runs-every-pass"),
    ("queue-reset-before-the-eye-loop", arm_reset_before_loop,
     "frame-end-reset-after-the-eye-loop"),
    ("queue-reset-at-the-end-of-the-replay", arm_reset_in_replay,
     "per-eye-functions-do-not-empty-the-queue"),
    ("queue-reset-in-install-eye", arm_reset_in_install_eye,
     "per-eye-functions-do-not-empty-the-queue"),
    ("queue-reset-removed-from-both-frame-boundaries", arm_reset_removed_from_the_frame,
     "queue-reset-happens-in-the-frame"),
    ("eye-loop-bound-hardcoded-to-one",
     lambda s: arm_loop_bound(s, "eye < r->eyes.passes", "eye < 1"),
     "eye-loop-bound-is-the-eye-plan"),
    ("eye-plan-pass-count-clamped-before-loop", arm_clamp_eye_pass_count,
     "eye-plan-count-not-written-in-end-frame"),
    ("eye-loop-increment-skips-pass", arm_loop_increment_skips_eye,
     "eye-loop-uses-the-unchanged-plan-count"),
    ("eye-loop-exits-with-goto", arm_loop_early_goto,
     "eye-loop-runs-every-pass"),
    ("eye-loop-starts-at-the-second-eye",
     lambda s: arm_loop_bound(s, "int eye = 0", "int eye = 1"),
     "eye-loop-uses-the-unchanged-plan-count"),
    ("install-targets-swapped-between-eyes", arm_install_targets_swapped,
     "install-eye-target-order"),
    ("install-offset-negated", arm_install_offset_negated,
     "install-eye-offset-sign"),
    # review-6 F6's mutants: the batch-66 hang, restored three ways. Each is a
    # plausible edit and none of them is visible to any other gate.
    ("create-time-shader-configure-restored",
     # Review-15 F5's second harvest: `shaderProgramConfigure(&r->program, false)` is
     # two arguments where libctru declares three (`sp, sendVshCode, sendGshCode`), so
     # this mutant never compiled either. The third argument is the GSH half, which is
     # what the frame params' `state>>12 & 1` would supply.
     lambda s: s.replace(SHADER_OK_LINE,
                         SHADER_OK_LINE
                         + "\n    shaderProgramConfigure(&r->program, false, false);",
                         1),
     "shader-configure-never-sent-by-hand"),
    ("shader-upload-removed-from-create",
     lambda s: s.replace("        ? shaderProgramSetVsh(&r->program, &r->dvlb->DVLE[0])",
                         "        ? (Result)0", 1),
     "shader-uploaded-exactly-once-at-create"),
    ("program-bound-at-create-as-well",
     lambda s: s.replace(SHADER_OK_LINE,
                         "    C3D_BindProgram(&r->program);\n" + SHADER_OK_LINE, 1),
     "program-bound-only-where-an-eye-is-installed"),
    # The red clear, restored. The single most plausible edit here is the tidy-up
    # that deletes the conversion as a pointless local, and no other gate in the
    # suite can see it, because renderer_c3d.cpp does not compile on a host.
    ("clear-colour-passed-through-to-the-pica",
     lambda s: s.replace(
         "    const uint32_t clear =\n"
         "        mdk::pica::clearColour(params ? params->clear_colour : 0xFF000000u);",
         "    const uint32_t clear = params ? params->clear_colour : 0xFF000000u;", 1),
     "clear-colour-is-converted-for-the-pica"),
    # review-3 F3's three mutants, each a plausible edit that no runtime capture
    # in this frontend can show, because the start arena has no blended pass.
    ("second-alpha-blend-writer",
     lambda s: s.replace("    C3D_SetViewport(0, 0, kScreenWidth, kScreenHeight);",
                         "    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE,\n"
                         "                       GPU_ZERO, GPU_ONE, GPU_ZERO);\n"
                         "    C3D_SetViewport(0, 0, kScreenWidth, kScreenHeight);", 1),
     "alpha-blend-single-writer"),
    ("alpha-test-in-the-per-draw-path",
     lambda s: s.replace("    C3D_DrawArrays(GPU_TRIANGLES, first, count);",
                         "    C3D_AlphaTest(true, GPU_GREATER, mdk::pica::kAlphaTestReference);\n"
                         "    C3D_DrawArrays(GPU_TRIANGLES, first, count);", 1),
     "alpha-test-single-writer-in-install-eye"),
    ("alpha-test-reference-moved",
     lambda s: s.replace("C3D_AlphaTest(true, GPU_GREATER, mdk::pica::kAlphaTestReference);",
                         "C3D_AlphaTest(true, GPU_GREATER, 5);", 1),
     "alpha-test-is-the-oracles-discard"),
    ("alpha-test-disabled",
     lambda s: s.replace("C3D_AlphaTest(true, GPU_GREATER, mdk::pica::kAlphaTestReference);",
                         "C3D_AlphaTest(false, GPU_ALWAYS, mdk::pica::kAlphaTestReference);",
                         1),
     "alpha-test-is-the-oracles-discard"),
    # The backend's own refused-mesh latch (review-14's related observation). This is
    # the line that printed 1710 times in one run; the frontend's copy of the same
    # edge trigger is pinned three times and this one was pinned not at all.
    ("mesh-refusal latch never clears (the second, larger refusal is silent)",
     lambda s: re.sub(r"[ \t]*renderer->meshAllocFailureLogged[ \t]*=[ \t]*false;[ \t]*\n",
                      "", s, count=1),
     "mesh-refusal-line-is-edge-triggered"),
    ("mesh-refusal latch never arms (the guard is always true, so the line prints on "
     "every frame of one refusal)",
     lambda s: re.sub(r"[ \t]*renderer->meshAllocFailureLogged[ \t]*=[ \t]*true;[ \t]*\n",
                      "", s, count=1),
     "mesh-refusal-line-is-edge-triggered"),
    ("mesh-refusal guard widened to nothing (the line prints on every frame)",
     lambda s: s.replace("if (!renderer->meshAllocFailureLogged) {",
                         "if (true) {", 1),
     "mesh-refusal-line-is-edge-triggered"),
    ("mesh-refusal latch cleared from inside the failing branch (it clears on the very "
     "frame it refused, so the line prints every frame anyway)",
     lambda s: s.replace(
         "        if (!renderer->meshAllocFailureLogged) {",
         "        renderer->meshAllocFailureLogged = false;\n"
         "        if (!renderer->meshAllocFailureLogged) {", 1),
     "mesh-refusal-line-is-edge-triggered"),
    # Review-15 F2: the two mutants the one-printf, one-function reading let through.
    ("a second refused-mesh line sits after the guard and prints on every refusing "
     "frame (review-15 F2)",
     lambda s: s.replace(
         "            mdk::log3dsPrintf(\"linearAlloc(%u) FAILED for a dynamic mesh of "
         "%d corners\",\n"
         "                              (unsigned)((size_t)cap * 5 * sizeof(float)), "
         "(int)cap);\n        }\n",
         "            mdk::log3dsPrintf(\"linearAlloc(%u) FAILED for a dynamic mesh of "
         "%d corners\",\n"
         "                              (unsigned)((size_t)cap * 5 * sizeof(float)), "
         "(int)cap);\n        }\n"
         "        mdk::log3dsPrintf(\"linearAlloc(%u) FAILED again this frame\",\n"
         "                          (unsigned)((size_t)cap * 5 * sizeof(float)));\n",
         1),
     "mesh-refusal-line-is-edge-triggered"),
    ("the refused-mesh latch is cleared once per frame from mdk_render_begin_frame "
     "(review-15 F2)",
     lambda s: s.replace("    r->inFrame = true;\n    r->frame++;",
                         "    r->inFrame = true;\n"
                         "    r->meshAllocFailureLogged = false;\n"
                         "    r->frame++;", 1),
     "mesh-refusal-latch-cleared-nowhere-else"),
    # Review-16 open question 2's pin: the same per-frame reset written through an
    # alias, which a scan for the field NAME cannot see. It reddens the sole-clear check
    # because the check refuses to certify a write set it cannot enumerate.
    ("the refused-mesh latch is cleared every frame through a pointer alias "
     "(review-16 open question 2)",
     lambda s: s.replace("    if (r->inFrame) return;   // a begin without an end is a bug, not a re-entry",
                         "    if (r->inFrame) return;   // a begin without an end is a bug, not a re-entry\n"
                         "    bool* const latchAlias = &r->meshAllocFailureLogged;\n"
                         "    *latchAlias = false;", 1),
     "mesh-refusal-latch-cleared-nowhere-else"),
    # Review-15 F3's lesson, applied to the backend's copy of the same net: a line in
    # the failing branch whose format never spells the refusal is invisible to a
    # format filter, and this arm is what isolates the branch half here.
    ("a line in the refused-mesh failing branch names a size without the word FAILED "
     "and prints on every refusing frame (review-15 F3)",
     lambda s: s.replace(
         "            mdk::log3dsPrintf(\"linearAlloc(%u) FAILED for a dynamic mesh of "
         "%d corners\",\n"
         "                              (unsigned)((size_t)cap * 5 * sizeof(float)), "
         "(int)cap);\n        }\n",
         "            mdk::log3dsPrintf(\"linearAlloc(%u) FAILED for a dynamic mesh of "
         "%d corners\",\n"
         "                              (unsigned)((size_t)cap * 5 * sizeof(float)), "
         "(int)cap);\n        }\n"
         "        mdk::log3dsPrintf(\"dynamic mesh: %d corners requested\", (int)cap);\n",
         1),
     "mesh-refusal-line-is-edge-triggered"),
]

# --- probes (review-16 R2) ----------------------------------------------------
# Same contract as `PROBES` in `main_3ds_flow_test.sh`: a probe is a change to the
# real file that must stay GREEN, because it is a shape the port may legitimately
# write. Without this mechanism the c3d gate could only ever prove that a mutation
# is caught, never that a legal spelling is accepted -- so when review-16 R2 found
# that `if (renderer->meshAllocFailureLogged == false) {`, the second spelling
# `LATCH_NEG` documents, failed `fullmatch` and reddened correct code, nothing in
# the gate could have said so. The first probe pins exactly that.
PROBES = [
    ("the refused-mesh guard is written as a comparison, not a negation (review-16 "
     "R2: the second LATCH_NEG spelling is a read, not a write)",
     lambda s: s.replace("        if (!renderer->meshAllocFailureLogged) {",
                         "        if (renderer->meshAllocFailureLogged == false) {", 1)),
    # The other half of the same regex: the latch is reached through a struct member,
    # so a rename of the local that holds the renderer must not redden the gate.
    ("the refused-mesh guard reaches the latch through a renamed local pointer",
     lambda s: s.replace("        if (!renderer->meshAllocFailureLogged) {",
                         "        auto* const rd = renderer;\n"
                         "        if (!rd->meshAllocFailureLogged) {", 1)),
]

# --- mutant dump (review-15 F5) ------------------------------------------------
# Same switch as main_3ds_flow_test.sh: the mutants are written beside the list the
# gate adjudicates so the cross-compile check cannot drift from the arms it checks.
DUMP = os.environ.get("MDK_MUTANT_DUMP")
if DUMP:
    import os as _os
    _os.makedirs(DUMP, exist_ok=True)
    with open(_os.path.join(DUMP, "00_parent.cpp"), "w") as fh:
        fh.write(SRC)
    for _i, _e in enumerate(ARMS):
        with open(_os.path.join(DUMP, "arm%02d.cpp" % _i), "w") as fh:
            fh.write(_e[1](SRC))
    # Review-15 F5 asked for "every mutant"; the probes are mutants too, and a probe
    # that does not compile is a probe that has never tested anything.
    for _i, _p in enumerate(PROBES):
        with open(_os.path.join(DUMP, "probe%02d.cpp" % _i), "w") as fh:
            fh.write(_p[1](SRC))
    print("dumped %d arms and %d probes to %s" % (len(ARMS), len(PROBES), DUMP))
    sys.exit(0)

print("c3d_frame: %s — %d checks, %d mutation arms" % (PATH, real.checks, len(ARMS)))
arms_red = 0
for label, mutate, expect in ARMS:
    mutated = mutate(SRC)
    if mutated == SRC:
        print("  FAIL c3d_frame arm %s: the mutation changed nothing, so the arm tested the "
              "file it was handed instead of the invariant" % label)
        continue
    after = adjudicate(mutated, quiet=True)
    # Review-16 R5: the c3d gate used to require only that an arm turn SOME check red,
    # while the flow gate beside it required the check the arm NAMES. The difference is
    # not cosmetic: a later predicate rewrite can move an arm from the check it was cut
    # for onto a different one, and the gate still prints `ok`, so the arm no longer
    # isolates anything and nothing says so. Review-14 F3's "half-pinned" class, in the
    # one place in this family that still had it. Every one of the 44 arms is now bound,
    # and the binding was generated from the measured run rather than typed, so it is
    # the current truth and not an intention.
    if after.failed and after.broke == expect:
        arms_red += 1
        print("  ok   arm %s: turned `%s` red as required" % (label, after.broke))
    elif after.failed:
        print("  FAIL c3d_frame arm %s: turned `%s` red, but it is cut for `%s`, so it no "
              "longer isolates the check it claims to isolate (review-16 R5)"
              % (label, after.broke, expect))
    else:
        print("  FAIL c3d_frame arm %s: the mutation was accepted, so the check does not pin "
              "what it claims to pin" % label)

if real.failed:
    sys.exit(1)
if arms_red != len(ARMS):
    print("  FAIL c3d_frame: only %d of %d arms adjudicated red; a PASS here would be a "
          "reading of nothing" % (arms_red, len(ARMS)))
    sys.exit(1)
# Review-16 R2: the probes, with the same rule as the flow gate's -- a probe that
# reddens a check is the gate rejecting a shape the port may write, and a probe that
# reddens NOTHING is a mutation that changed nothing and so tested nothing.
probes_green = 0
for label, mutate in PROBES:
    mutated = mutate(SRC)
    if mutated == SRC:
        print("  FAIL c3d_frame probe %s: the probe changed nothing, so it tested the file "
              "it was handed instead of the invariant" % label)
        continue
    after = adjudicate(mutated, quiet=True)
    if not after.failed:
        probes_green += 1
        print("  ok   probe %s stayed green" % label)
    else:
        print("  FAIL c3d_frame probe %s: correct code turned `%s` red, so the check "
              "rejects a shape the port may legitimately write (review-16 R2)"
              % (label, after.broke))
if probes_green != len(PROBES):
    print("  FAIL c3d_frame: only %d of %d probes stayed green" % (probes_green, len(PROBES)))
    sys.exit(1)
print("PASS C3dFrame (%d checks, %d arms adjudicated red, %d probes green)"
      % (real.checks, arms_red, probes_green))
PY
exit $?
