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


def body(text, name):
    """One function's source, brace-matched from its definition."""
    m = re.search(r"(?m)^[ \t]*(?:static[ \t]+)?void[ \t]+" + re.escape(name) +
                  r"[ \t]*\([^;{]*\)[ \t]*\{", text)
    if not m:
        return None
    depth = 0
    for i in range(m.end() - 1, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[m.start():i + 1]
    return None


def calls(text, pattern):
    """Call sites of a pattern: comment lines and the pattern's own definition
    line are excluded."""
    hits = []
    for i, line in code_lines(text):
        if re.search(r"\bvoid[ \t]+" + pattern + r"[ \t]*\(", line):
            continue
        if re.search(pattern, line):
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
    """The four invariants, over whatever text is handed in."""
    rep = Report(quiet)
    end = body(text, "mdk_render_end_frame") or ""
    diag_body = body(text, "drawDiagnostic")

    diag = calls(text, r"C3D_DrawArrays\(GPU_TRIANGLES, 0, 3\)")
    rep.check("diagnostic-drawn-once",
              len(diag) == 1 and diag_body is not None and
              "C3D_DrawArrays(GPU_TRIANGLES, 0, 3)" in diag_body,
              "%d diagnostic draw(s); inside drawDiagnostic: %s (per-draw it cost 2 depth "
              "writes + 1 draw per real draw, per eye)"
              % (len(diag), "yes" if diag_body and
                 "C3D_DrawArrays(GPU_TRIANGLES, 0, 3)" in diag_body else "no"))

    dcall = calls(text, r"drawDiagnostic\(r\)")
    in_end = bool(re.search(r"drawDiagnostic\(r\)", end))
    rep.check("diagnostic-called-per-eye", len(dcall) == 1 and in_end,
              "%d call(s); inside mdk_render_end_frame: %s" % (len(dcall), "yes" if in_end else "no"))

    replay = calls(text, r"replayQueue\(r\)")
    r_end = bool(re.search(r"replayQueue\(r\)", end))
    rep.check("replay-only-at-frame-end", len(replay) == 1 and r_end,
              "%d replay(s); inside mdk_render_end_frame: %s" % (len(replay), "yes" if r_end else "no"))

    leaking = []
    for seam in ("mdk_render_canvas_begin", "mdk_render_begin_inset", "mdk_render_end_inset"):
        b = body(text, seam)
        if b is None:
            leaking.append(seam + "(missing)")
            continue
        for i, line in code_lines(b):
            if re.search(r"flushDraws\(|replayQueue\(", line):
                leaking.append("%s:%d" % (seam, i + 1))
    rep.check("no-mid-frame-flush", not leaking,
              "canvas/inset seams emit nothing: %s"
              % ("clean" if not leaking else ", ".join(leaking)))

    depth = calls(text, r"C3D_DepthTest\(")
    rep.check("depth-state-single-writer", len(depth) == 1,
              "%d C3D_DepthTest call site(s); the cache is the only writer, so a second one "
              "writes behind its back" % len(depth))
    return rep


real = adjudicate(SRC)

ARMS = [
    ("diagnostic-back-in-the-per-draw-path",
     lambda s: s.replace("    C3D_DrawArrays(GPU_TRIANGLES, first, count);",
                         "    C3D_DrawArrays(GPU_TRIANGLES, first, count);\n"
                         "    if (r->diagnosticTriangle) {\n"
                         "        C3D_DrawArrays(GPU_TRIANGLES, 0, 3);\n"
                         "    }", 1)),
    ("diagnostic-dropped-from-the-eye-loop",
     lambda s: s.replace("        replayQueue(r);\n        drawDiagnostic(r);",
                         "        replayQueue(r);", 1)),
    ("canvas-flush-restored",
     lambda s: re.sub(r"(void mdk_render_canvas_begin\(MdkRenderer r\) \{)",
                      r"\1\n    flushDraws(asC3d(r));", s, count=1)),
    ("inset-replay-restored",
     lambda s: re.sub(r"(void mdk_render_begin_inset\(MdkRenderer r, const mdk::Inset\* i\) \{)",
                      r"\1\n    replayQueue(asC3d(r));", s, count=1)),
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
