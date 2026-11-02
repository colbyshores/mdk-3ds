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
    in_loop_reset = sites(loop or "", r"queue\.(?:reset|clear)\s*\(")
    rep.check("no-queue-reset-in-the-eye-loop", not in_loop_reset,
              "%d queue reset(s) inside the eye loop; the reset belongs after the LAST eye "
              "(reset inside it and the second eye replays nothing)"
              % len(in_loop_reset))

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

ARMS = [
    ("diagnostic-back-in-the-per-draw-path",
     lambda s: s.replace("    C3D_DrawArrays(GPU_TRIANGLES, first, count);",
                         "    C3D_DrawArrays(GPU_TRIANGLES, first, count);\n"
                         "    if (r->diagnosticTriangle) {\n"
                         "        C3D_DrawArrays(GPU_TRIANGLES, 0, 3);\n"
                         "    }", 1)),
    ("diagnostic-dropped-from-the-eye-loop", arm_drop_diagnostic),
    ("diagnostic-after-the-eye-loop", lambda s: arm_move_diagnostic(s, "after-loop")),
    ("diagnostic-before-its-replay", lambda s: arm_move_diagnostic(s, "before-replay")),
    ("canvas-flush-restored",
     lambda s: re.sub(r"(void mdk_render_canvas_begin\(MdkRenderer r\) \{)",
                      r"\1\n    flushDraws(asC3d(r));", s, count=1)),
    ("inset-replay-restored",
     lambda s: re.sub(r"(void mdk_render_begin_inset\(MdkRenderer r, const mdk::Inset\* i\) \{)",
                      r"\1\n    replayQueue(asC3d(r));", s, count=1)),
    ("present-replay-restored",
     lambda s: arm_seam_replay(s, "mdk_render_present", "asC3d(rr)")),
    ("second-depth-writer",
     lambda s: s.replace("    C3D_SetViewport(0, 0, kScreenWidth, kScreenHeight);",
                         "    C3D_DepthTest(true, GPU_LEQUAL, GPU_WRITE_ALL);\n"
                         "    C3D_SetViewport(0, 0, kScreenWidth, kScreenHeight);", 1)),
    # review-3's five mutants, reproduced as arms so the checks that close those
    # findings cannot be satisfied by a reading of the file.
    ("canvas-quad-draws-immediately",
     lambda s: arm_canvas_quad_emits(s, "C3D_DrawArrays(GPU_TRIANGLE_STRIP, 0, 4);")),
    ("canvas-quad-draws-a-record-directly",
     lambda s: arm_canvas_quad_emits(s, "mdk::MeshDraw rec;\n    drawRecorded(asC3d(r), &rec);")),
    ("diagnostic-guarded-to-one-eye",
     lambda s: arm_guard_loop_statement(s, "drawDiagnostic")),
    ("replay-guarded-to-one-eye",
     lambda s: arm_guard_loop_statement(s, "replayQueue")),
    ("queue-reset-inside-the-eye-loop", arm_reset_in_loop),
    ("install-hoisted-above-the-eye-loop", arm_install_hoisted),
    ("install-after-its-replay", arm_install_after_replay),
    ("install-fixed-target-for-both-eyes",
     lambda s: s.replace("installEye(r, eye == 0 ? r->targetLeft : r->targetRight, r->eyes.offset[eye]);",
                         "installEye(r, r->targetLeft, 0.0f);", 1)),
]

print("c3d_frame: %s — %d checks, %d mutation arms" % (PATH, real.checks, len(ARMS)))
arms_red = 0
for label, mutate in ARMS:
    mutated = mutate(SRC)
    if mutated == SRC:
        print("  FAIL c3d_frame arm %s: the mutation changed nothing, so the arm tested the "
              "file it was handed instead of the invariant" % label)
        continue
    after = adjudicate(mutated, quiet=True)
    if after.failed:
        arms_red += 1
        print("  ok   arm %s: turned `%s` red as required" % (label, after.broke))
    else:
        print("  FAIL c3d_frame arm %s: the mutation was accepted, so the check does not pin "
              "what it claims to pin" % label)

if real.failed:
    sys.exit(1)
if arms_red != len(ARMS):
    print("  FAIL c3d_frame: only %d of %d arms adjudicated red; a PASS here would be a "
          "reading of nothing" % (arms_red, len(ARMS)))
    sys.exit(1)
print("PASS C3dFrame (%d checks, %d arms adjudicated red)" % (real.checks, arms_red))
PY
exit $?
