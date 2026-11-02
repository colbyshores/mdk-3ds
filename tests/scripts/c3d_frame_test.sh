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
              "(a second one emits into the last-installed target and empties the queue)"
              % (len(replay), "yes" if replay_at is not None else "no"))

    leaking = []
    for seam in ("mdk_render_canvas_begin", "mdk_render_canvas_end", "mdk_render_present",
                 "mdk_render_begin_inset", "mdk_render_end_inset"):
        b = body(text, seam)
        if b is None:
            leaking.append(seam + "(missing)")
            continue
        for i, line in code_lines(b):
            if re.search(r"flushDraws\s*\(|replayQueue\s*\(", line):
                leaking.append("%s:%d" % (seam, i + 1))
    rep.check("no-mid-frame-flush", not leaking,
              "canvas/inset/present seams emit nothing: %s"
              % ("clean" if not leaking else ", ".join(leaking)))

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
