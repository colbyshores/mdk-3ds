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

# The renderer is one translation unit assembled from c3d/*.inc (the split
# of 2026-11-15): expand those includes so the checks and the mutation arms
# see the same text the compiler does, in the same order.
def _expand(path):
    text = open(path, errors="replace").read()

    def _sub(match):
        inc = os.path.join(os.path.dirname(path), match.group(1))
        return open(inc, errors="replace").read()

    return re.sub(r'^#include "((?:c3d|vulkan)/[^"]+\.inc)"', _sub, text, flags=re.M)

SRC = _expand(PATH)

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

# A function definition, whatever it returns. `fn_body_span` above resolves ONE
# named function; review-7 F1 needs EVERY function in the file, because the rule
# it asks for is a whole-file rule ("any function that fills texel memory must
# rotate through the helper") and a rule that only sees the two functions this
# file happens to name is a name pin, not a rule.
FN_DEF = re.compile(r"(?m)^(?:(?:static|inline|constexpr|explicit)[ \t]+)*"
                    r"[A-Za-z_][A-Za-z0-9_:*<>&, \t]*?[ \t]+"
                    r"([A-Za-z_][A-Za-z0-9_]*)[ \t]*\([^;{]*\)[ \t]*(?:const[ \t]*)?\{")


def code_lines(text):
    """(index, text) for lines that are code. Comments are dropped, because this
    file documents its own rules in prose and a naive match would read the prose
    as a call site.

    Limit, stated because review-8 F5 asked for limits to be stated where the claim
    is made: a line whose first character is `*` is dropped as a block-comment
    continuation, so a bare `*dst = ...;` write is invisible HERE. That is why
    `texel_bypasses` scans `statements(blank_comments(...))` instead."""
    out = []
    for i, line in enumerate(text.splitlines()):
        s = line.strip()
        if s.startswith("//") or s.startswith("*") or s.startswith("/*"):
            continue
        out.append((i, line))
    return out


def block_span_from(text, pos):
    """(start, end) offsets of the brace-matched block whose '{' is at or after
    `pos`; `end` is one past the matching '}'.

    Review-8 F3: the scan is comment- and string-aware. A brace inside a comment
    used to move the boundary, so a function whose body followed a comment
    containing `}` was adjudicated over a TRUNCATED body and everything after the
    comment was never read -- the reviewer's mutant hid a full set of direct texel
    writes behind `// the oracle's shader discards index 0 }` and passed every
    check. `blank_comments` already states this rule for the bypass scan (prose is
    not code); this is the same rule applied where the SPANS every other check
    reads are resolved, without blanking the file once per function."""
    n = len(text)
    i, depth, start = pos, 0, -1
    while i < n:
        c = text[i]
        if c in '"\'':
            quote = c
            i += 1
            while i < n and text[i] != quote:
                i += 2 if text[i] == "\\" else 1
            i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                i += 1
            i += 2
            continue
        if c == "{":
            if depth == 0:
                start = i
            depth += 1
        elif c == "}" and start >= 0:
            depth -= 1
            if depth == 0:
                return (start, i + 1)
        i += 1
    return None


def statements(text, with_offsets=False):
    """`text` folded into statements: a `;`, `{` or `}` at bracket depth zero ends
    one, and one inside `(...)` or `[...]` does not.

    With `with_offsets` the result is `(start offset, statement)` pairs, for the one
    rule that has to compare WHERE two statements sit (review-12 F1). Default off, so
    every existing reader keeps the plain strings it already matches against -- the
    same opt-in shape as `call_args_at(keep_literals=...)`.

    Review-8 F1: the direct-write detector scanned PHYSICAL lines, and all three of
    its write regexes required a character AFTER the `=`, so the same write split
    after the `=` -- `((uint8_t*)pixel)[3] =\n    paletteRgba[index * 4 + 3];`, the
    exact statement batch 122's own F5 arm pins -- matched none of them and the gate
    printed PASS over the shipped R = 255 defect. This file wraps long expressions
    at ~100 columns routinely, so the wrapped spelling is the ordinary one, not an
    exotic one. A write happens in a statement; the line is a formatting artefact."""
    out, cur, depth = [], [], 0
    start, pending = None, True
    for pos, ch in enumerate(text):
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        elif ch in ";{}" and depth <= 0:
            folded = " ".join("".join(cur).split())
            if folded:
                out.append((start, folded) if with_offsets else folded)
            cur = []
            start, pending = None, True
            continue
        if pending and not ch.isspace():
            start, pending = pos, False
        cur.append(ch)
    folded = " ".join("".join(cur).split())
    if folded:
        out.append((start, folded) if with_offsets else folded)
    return out


def block_span(text, pattern):
    """The block that follows `pattern`, or None if the pattern is absent."""
    m = re.search(pattern, text)
    if not m:
        return None
    return block_span_from(text, m.end())


# A dereference WRITE, wherever it sits in a statement (review-9 F1).
#
# The batch-122 detector anchored the bare-dereference rule at `^`, i.e. at the start
# of the FOLDED statement — and folding is what makes that anchor a hole rather than a
# guard: `if (index != 0) *(pixel + 3) = paletteRgba[index * 4 + 3];` is ONE statement,
# so the write is not what the statement begins with, and the shipped R = 255 defect
# passed all 41 checks in that spelling. `else *(pixel + 3) = ...` and a one-line `for`
# body are the same miss with different punctuation in front of them.
#
# The `*` cannot simply be un-anchored, because `*` also opens a POINTER DECLARATION:
# `const uint8_t* colours = paletteRgba;` is a legal spelling this batch's own alias arm
# writes, and a rule that reddened it would be routed around (review-16 R2). What tells
# the two apart is the token before the `*`. A declaration puts a TYPE there — an
# identifier that is not a keyword. A dereference begins a primary expression, so what
# precedes it is a statement start, punctuation, an operator, or a control keyword.
DEREF_PRE_PUNCT = ";{}()=!<>+-*/%&|^~"
DEREF_PRE_WORDS = ("else", "return", "do", "case", "and", "or", "not", "xor")
# Review-10 F2: the tail used to be `[^=;]*=[ \t]*[^=]`, and `[^=;]*` swallows the
# operator of a SINGLE-equals comparison, so `*s != 0`, `*s <= 0` and `*s >= 1` all
# read as writes — a dead guard reddened a correct filler, review-16 R2's own failure
# class. The tail now requires an ASSIGNMENT: an `=` that is not part of `==`, `!=`,
# `<=` or `>=`, which is what the prose here always claimed the rule was.
# Review-11 F4: the tail used to be `[^=;]*(?<![=!<>])=(?!=)`, which refuses an `=`
# preceded by `<` or `>` -- correct for `<=` and `>=`, and also correct for `>>=` and
# `<<=`, which are WRITES. The shift-assign operators are now matched as themselves,
# so `*(pixel + 3) >>= 1` is a write and `*s >= 1` still is not.
DEREF_WRITE_TAIL = re.compile(r"[^=;]*(?:<<|>>|[^=!<>])=(?!=)")


# The bulk byte-setters, split by what they DO (review-11 F3). The class is "a byte
# written into texel memory without the helper", and the list cannot be a spelling
# list: `\bmemset` cannot match `__builtin_memset` (the character before it is a word
# character), and `std::fill`/`std::copy` are the same two operations in the other
# spelling the house style permits -- all three walked through a list of names.
# A COPY moves SOURCE bytes, so it is never the padding fill; a FILL sets a constant,
# and whether that constant is the transparent texel is what decides it.
BULK_FILL = re.compile(r"(?<![\w])(?:__builtin_|std::)?memset[ \t]*\(|"
                       r"(?<![\w])std::(?:fill_n|fill)[ \t]*\(")
BULK_COPY = re.compile(r"(?<![\w])(?:__builtin_|std::)?(?:memcpy|memmove)[ \t]*\(|"
                       r"(?<![\w])std::(?:copy_n|copy)[ \t]*\(")
# Review-12 F6: `std::fill(...)` and `fill(...)` after `using std::fill;` are one
# operation and the gate read only the first, which is review-11 F3's "the operation,
# not the spelling" claim stopped one spelling short. The bare algorithm names are
# therefore in scope ONLY when the file carries the matching `using std::` declaration
# -- the shape `helper_call_re` already uses for the helper itself (review-8 F5), with
# the same reason: matching a bare `fill(` unconditionally would sweep in any project
# function of that name, and review-16 R2's rule is that a gate reddening a shape the
# port may write gets routed around. The lookbehind refuses `q.fill(` and `std::fill(`,
# which are a member call and the qualified spelling respectively.
USING_STD = re.compile(r"using[ \t]+std::([A-Za-z_][A-Za-z0-9_]*)[ \t]*;")
BARE_FILL = re.compile(r"(?<![\w:.])(fill_n|fill)[ \t]*\(")
BARE_COPY = re.compile(r"(?<![\w:.])(copy_n|copy)[ \t]*\(")
# The value that means "transparent texel": an all-zero texel is alpha 0 under ANY
# byte order (the index-0 discard invariant batch 120 pinned). The scope is the
# spellings whose VALUE is the byte zero, and it has to be stated as a scope, because
# review-12 F2 found the previous alternative `'(?:\\0|x0+|0)'` accepting `'0'` -- the
# CHARACTER 0x30, so a padding row of [0x30, 0x30, 0x30, 0x30] is alpha 48, opaque
# garbage -- and `'x0'`, a multi-character constant, both of which `fullmatch`ed as
# "the transparent texel" while a NONZERO fill is the shipped symptom's own class.
# Batch 125 refused both; batch 126's literal fix let them through, so this was a
# regression, and review-12 F5 closed the hex-escape half the same way.
# INTEGER literals of value 0 in any base, with any integer suffix: decimal `0`,
# octal `00`, hex `0x0`, binary `0b0`. Review-13 F2: the class carried `[uU]?` only,
# so the `size_t`-flavoured `0L` and `0x0UL` -- byte for byte the shipped fill --
# returned "write" and reddened a correct filler while the prose above claimed the
# zero literals of the two languages, and a gate that reddens a suffix gets routed
# around (review-16 R2). The suffix class is `[uUlL]*` and the four bases are
# exhaustive, so what is refused is exactly the NONZERO value: `07` is octal 7 and
# `0b1` is one, and each is pinned by an arm so the widening cannot rot open.
# Inside the quotes only the ESCAPES are zero: `'\0'`, `'\00'`, `'\000'` (octal) and
# `'\x0'`, `'\x00'` (hex). The old `x0+` alternative could never have reached a hex
# escape anyway, because the text of one starts with the backslash (review-12 F5).
# Stated limit: a PREFIXED character literal (`L'\0'`) is not in the class and is
# refused; a memset fill value is an `int` argument and the unprefixed spellings are
# what this tree writes. This is why the argument reader below keeps literal text.
ZERO_FILL = re.compile(r"(?:0[xX]0+[uUlL]*|0[bB]0+[uUlL]*|0+[uUlL]*|"
                       r"'\\0{1,3}'|'\\x0{1,2}')")
# The two subscript spellings of a direct texel write: a subscript on a name and a
# subscript on a cast. Review-12 F3: the operator slot was `[+\-*/%&^|]?`, which cannot
# reach `<<=` or `>>=` because `<` and `>` are not in the class -- so
# `out[(size_t)x * 4 + 3] >>= 1` and `((uint8_t*)pixel)[3] >>= 1` halved a texel alpha
# with the gate green, review-11 F4's defect on the other side of the `*`. The slot now
# matches the shift-assigns as themselves and still refuses every comparison: `(?!=)`
# closes `==`, and `<=`/`>=` cannot match because the `=` of a comparison is preceded by
# `<` or `>`, which the alternation accepts only as part of `<<`/`>>`.
SUBSCRIPT_WRITE = re.compile(r"\w[ \t]*\[[^\]]*\][ \t]*(?:<<|>>|[+\-*/%&^|])?=(?!=)[ \t]*[^=]")
CAST_SUBSCRIPT_WRITE = re.compile(r"\)[ \t]*\[[^\]]*\][ \t]*(?:<<|>>|[+\-*/%&^|])?=(?!=)[ \t]*[^=]")


def deref_write(stmt):
    """True when `stmt` assigns through a dereference anywhere in it.

    `*(pixel + 3) = paletteRgba[index * 4 + 3]`, `*pixel = x`, `if (c) *(p + 3) = x`
    and `for (...) *p = x` are writes; `const uint8_t* colours = paletteRgba;`,
    `y = *p`, `texels * 4`, `a == *p`, `*s != 0`, `*s <= 0` and `*s >= 1` are not.
    The tail matches an `=` that is no part of a comparison -- `[^=!<>]` refuses the
    compound operators that compare, and `<<`/`>>` are matched as the shift-assigns
    they are (review-11 F4) -- which is what stops a READ through a pointer from
    counting. The subscript rules beside it are the same question on the other side of
    the `*`, and review-12 F3 found they had review-11 F4's hole after all: their old
    operator slot `[+\-*/%&^|]?` could not reach `<<=` or `>>=` either. Both now match
    the shift-assigns and still refuse every comparison.

    Stated limit, in the same spirit as the parameter-passing limit below: a write
    introduced after a comma in a multi-declarator (`uint8_t *a, *b = x;`) would read
    as a dereference write, so `,` is deliberately NOT in the punctuation set and
    `f(a, *p = 1)` is not caught. The first is a legal declaration and the second is
    not a shape this port writes."""
    for m in re.finditer(r"\*[ \t]*[A-Za-z_(]", stmt):
        j = m.start() - 1
        while j >= 0 and stmt[j] in " \t":
            j -= 1
        pre = stmt[j] if j >= 0 else ""
        ok = pre == "" or pre in DEREF_PRE_PUNCT
        if not ok:
            w = re.search(r"[A-Za-z_][A-Za-z0-9_]*$", stmt[:j + 1])
            ok = w is not None and w.group(0) in DEREF_PRE_WORDS
        if ok and DEREF_WRITE_TAIL.match(stmt[m.start():]):
            return True
    return False


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


_BLANK_ONE = None


def blank_cached(text):
    """`blank_comments` memoised on the one text an adjudication passes around.

    Offsets are preserved by construction, so a position found in the blanked copy
    still indexes the original -- which is what lets `fn_spans`, `fn_body_span` and
    `body_span` resolve definitions in the blanked copy (review-8 F3: a commented-out
    function definition used to be a definition) while every caller keeps slicing the
    original text. One entry, keyed on identity: the bodies are slices, so a growing
    cache would only grow."""
    global _BLANK_ONE
    if _BLANK_ONE is not None and _BLANK_ONE[0] is text:
        return _BLANK_ONE[1]
    out = blank_comments(text)
    _BLANK_ONE = (text, out)
    return out


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
    brace. `body_span` requires `void`, and the mesh factory returns MdkMeshId.

    Resolved over `blank_cached(text)`: a commented-out definition is not a
    definition (review-8 F3), and offsets survive blanking, so the span still
    indexes the original text every caller slices."""
    m = re.search(r"(?m)^[ \t]*[A-Za-z_][A-Za-z0-9_:* \t]*[ \t]+" + re.escape(name) +
                  r"[ \t]*\([^;{]*\)[ \t]*\{", blank_cached(text))
    if not m:
        return None
    sp = block_span_from(text, m.end() - 1)
    return (m.start(), sp[1]) if sp else None


def body_span(text, name):
    """Absolute (start, end) of one function, from its signature through its
    closing brace, or None.

    The `{` is part of the match on purpose: without it the pattern also matches
    the forward declaration `static void name(...);`, which comes first in this
    file, and the body of an unrelated block gets handed back as the function.

    Resolved over `blank_cached(text)` for the same reason as `fn_body_span.
    """
    m = re.search(r"(?m)^[ \t]*(?:static[ \t]+)?void[ \t]+" + re.escape(name) +
                  r"[ \t]*\([^;{]*\)[ \t]*\{", blank_cached(text))
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

# The ONLY shapes in which the latch's name may appear (review-18 F1, the same rule the
# frontend's check 20 states). This used to be a BLACKLIST of two alias spellings, and
# each review has found a third: `bool& a(r->meshAllocFailureLogged);` binds the same
# reference by direct initialisation, and `clearMeshRefusal(r->meshAllocFailureLogged);`
# writes the field from a helper that takes a reference without ever naming an alias, so
# there is no spelling left for a scan to look for. A name scan that cannot enumerate the
# writes must not certify them, so the whitelist refuses the name wherever it is not one
# of these shapes -- whatever it spells, including a read-only `const bool&` view.
# Shapes, in order: the declaration; the negative read; the `== false` read; the two
# pinned writes. (Written without trailing comments because the alternatives are joined
# with `+`, and a `#` ends an implicit string concatenation.)
_LATCH_ANY = r"(?:[A-Za-z_][A-Za-z0-9_]*[ \t]*(?:->|\.)[ \t]*)?meshAllocFailureLogged"
LATCH_SHAPE = re.compile(
    r"bool[ \t]+meshAllocFailureLogged[ \t]*=[ \t]*false[ \t]*;"
    "|![ \\t]*" + _LATCH_ANY +
    "|" + _LATCH_ANY + r"[ \t]*==[ \t]*false" +
    "|" + _LATCH_ANY + r"\s*=[ \t]*(?:true|false)[ \t]*;"
)


def unpinned_uses(text, name, shapes):
    """Offsets of every occurrence of `name` in `text` that is NOT inside one of
    `shapes`. Empty list means the name scan describes every use it can find."""
    spans = [m.span() for m in shapes.finditer(text)]
    return [m.start() for m in re.finditer(r"\b" + name + r"\b", text)
            if not any(a <= m.start() and m.end() <= b for a, b in spans)]


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


def call_args_at(text, paren, keep_literals=False):
    """The top-level argument strings of the call whose '(' sits at `paren`.

    Multi-line and string-aware on purpose (review-6 F1). `call_args` above takes a
    LINE and a function name, and both shipped `texelBytes` calls wrap across two or
    three lines, so the argument a check needs -- the forced alpha in expandIndices,
    the source alpha in expandRgba -- is not on the line the call was found on. A
    line-based reader would report those calls as argument-free.

    Review-11 F2: by default a quoted literal contributes only its OPENING quote, so
    `memset(pixels, '\\0', n)` arrives as `['pixels', "'", 'n']` and a rule that
    whitelists the fill cannot see it. `keep_literals` keeps the literal text instead,
    for the one rule that adjudicates a literal -- the bulk fill. It is opt-in because
    the other readers of this function match identifiers and subscripts, and a literal
    body would only give them more text to mistake for code."""
    depth = 0
    cur = ""
    args = []
    i = paren
    n = len(text)
    while i < n:
        c = text[i]
        if c in '"\'':
            quote = c
            start = i
            i += 1
            while i < n and text[i] != quote:
                i += 2 if text[i] == "\\" else 1
            if keep_literals:
                if depth >= 1:
                    cur += text[start:i + 1]
                i += 1
                continue
        elif c in "([{":
            depth += 1
            if depth == 1:
                i += 1
                continue
        elif c in ")]}":
            depth -= 1
            if depth == 0:
                args.append(cur.strip())
                return args
        elif c == "," and depth == 1:
            args.append(cur.strip())
            cur = ""
            i += 1
            continue
        if depth >= 1:
            cur += c
        i += 1
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

    # THE CANARY IS A QUAD, NOT A TRIANGLE, AND THE VERTEX COUNT IS PART OF THE PIN.
    # This literal used to read `0, 3`, the shipped white TRIANGLE. The canary is now
    # a 16x16 marker in the canvas's own pixel space -- two triangles, six vertices --
    # so `0, 3` matched nothing and this check was red for every correct build, which
    # in turn made EVERY arm below report a spurious red and test nothing (batch 145,
    # docs/OPEN_ITEMS.md). What the check is FOR has not changed: exactly one
    # diagnostic emission, and it is inside drawDiagnostic, because per-draw it cost
    # 2 depth writes + 1 draw per real draw, per eye. Do not "tidy" the 6 back to a 3.
    diag = calls(text, r"C3D_DrawArrays\(GPU_TRIANGLES, 0, 6\)")
    rep.check("diagnostic-drawn-once",
              len(diag) == 1 and diag_body is not None and
              "C3D_DrawArrays(GPU_TRIANGLES, 0, 6)" in diag_body,
              "%d diagnostic draw(s); inside drawDiagnostic: %s (per-draw it cost 2 depth "
              "writes + 1 draw per real draw, per eye)"
              % (len(diag), "yes" if diag_body and
                 "C3D_DrawArrays(GPU_TRIANGLES, 0, 6)" in diag_body else "no"))

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

    # The alpha test is pinned three ways: ONE writer, installAlphaTest (a cached
    # helper, since Enhanced Mode filtering made the reference per draw: the half
    # cover for a filtered scene draw, the oracle's discard for the canvas and for
    # nearest -- pica::alphaTestReferenceFor); installEye CALLS it, forced (so both
    # eyes get the frame's default -- the same placement rule that made batch
    # 30's black screen a checked property); and it is spelled with the oracle's
    # own predicate. A reference moved to 5, or GPU_ALWAYS, or a `false`, is
    # invisible in every scene this frontend can draw today, so the spelling is
    # the pin.
    atest = calls(text, r"C3D_AlphaTest\(")
    writer = line_range(text, body_span(text, "installAlphaTest"))
    atest_stray = [loc for ln, loc in atest
                   if not (writer and writer[0] <= ln <= writer[1])]
    eye_body = body(text, "installEye") or ""
    eye_calls = re.findall(r"installAlphaTest\(\s*r\s*,\s*mdk::pica::alphaTestReferenceFor\("
                           r"\s*r->toggles\.enableFiltering\s*,\s*mdk::Pass::Solid\s*\)\s*,"
                           r"\s*true\s*\)", eye_body)
    rep.check("alpha-test-single-writer-in-install-eye",
              len(atest) == 1 and writer and not atest_stray and len(eye_calls) == 1,
              "%d C3D_AlphaTest call(s), %d outside installAlphaTest: %s; installEye's forced "
              "installAlphaTest(r, alphaTestReferenceFor(filtering, Solid), true) calls: %d "
              "(frame state one eye does not get is a hole in that eye)"
              % (len(atest), len(atest_stray),
                 "none" if not atest_stray else "; ".join(atest_stray), len(eye_calls)))

    discard = [loc.strip() for _, loc in code_lines(text)
               if re.search(r"C3D_AlphaTest\(\s*true\s*,\s*GPU_GREATER\s*,\s*reference\s*\)",
                            loc)]
    draw_body = body(text, "drawRecorded") or ""
    draw_calls = re.findall(r"installAlphaTest\(\s*r\s*,\s*mdk::pica::alphaTestReferenceFor\("
                            r"\s*r->toggles\.enableFiltering\s*,\s*draw->material\.pass\s*\)"
                            r"\s*,\s*false\s*\)", draw_body)
    # (The reference VALUES -- kAlphaTestReference with filtering off or on the
    # canvas, the half cover for a filtered scene draw -- are PicaMatrix's pins.)
    rep.check("alpha-test-is-the-oracles-discard", len(discard) == 1 and len(draw_calls) == 1,
              "%d C3D_AlphaTest written as (true, GPU_GREATER, reference): %s; drawRecorded's "
              "installAlphaTest(r, alphaTestReferenceFor(filtering, draw->material.pass), false) "
              "calls: %d (GPU_GREATER against 0 IS `if (index == 0) discard;`, because the "
              "palette expansion encodes index 0 as alpha 0 and every other index as 255; any "
              "other spelling is a different predicate and a discarded fragment does not "
              "write depth)" % (len(discard), discard or ["none"], len(draw_calls)))

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

        # Review-19 F3: the whitelist's write alternative is POSITION-FREE, so
        # `r->meshAllocFailureLogged = true;` written as the first statement of
        # `mdk_render_begin_frame` -- the natural "make sure the latch cannot carry
        # over" refactor -- is a PINNED write as far as the name scan is concerned,
        # and it is review-11 F7's defect MIRRORED: the guard is never true again, so
        # a refused `linearAlloc` reports NOTHING at all and the run reads as a
        # successful frame with no objects. This side position-checked only the CLEAR;
        # the frontend has position-checked its arming since review-14 F1, which is
        # why the same mutant was red there and green here. So every write that ARMS
        # the latch, anywhere in the file, must be the arming: a direct statement
        # (depth 1) of a latch-guarded block.
        armed_only = True
        for w in re.finditer(_LATCH_ANY + r"\s*=[ \t]*true[ \t]*;", blank_comments(text)):
            pos = w.start() - mesh_body[0]
            armed_only = any(sp[0] <= pos < sp[1]
                             and brace_depth(seg, sp[0], pos) == 1 for sp in guards)
            if not armed_only:
                break
        armed = (bool(refusals) and armed_only and all(
            any(sp[0] <= at < sp[1] and arms_latch(sp) for sp in guards)
            for at in refusals))
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
        #
        # TWO spellings, not one (review-17 R1's asymmetry). The address-of form is
        # `bool* const l = &renderer->meshAllocFailureLogged;`. The reference form is
        # `bool& latchAlias = renderer->meshAllocFailureLogged;` -- where the `&` sits
        # on the ALIAS's name and never touches the field's, so an `&`-before-the-field
        # scan cannot see it. The frontend's check 20 has accepted both spellings since
        # batch 109; this side only had the first, which is the same property stated two
        # ways in two gates and enforced one way.
        #
        # Review-18 F1: both of those were spellings of ONE idea, and the blacklist kept
        # losing to new spellings of it, so this is now the whitelist above -- the name
        # is refused outside its declaration, its two reads and its two writes.
        aliased = unpinned_uses(blank_comments(text), "meshAllocFailureLogged",
                                LATCH_SHAPE)
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
              "refusal, and a latch ARMED on every frame silences it forever: the "
              "arming is only allowed inside the guard that prints, never as a "
              "per-frame reset in the frame path -- review-19 F3)"
              % (len(refusals), "yes" if armed else "no",
                 "yes" if cleared else "no"))
    rep.check("mesh-refusal-latch-cleared-nowhere-else", sole_clear,
              "the latch must be cleared exactly once in the file, on the mesh "
              "factory's success path (review-15 F2); a second clear anywhere else "
              "runs on some other schedule, and one that runs every frame re-arms the "
              "latch under a sustained refusal -- the spam this latch exists to stop. "
              "Any OTHER use of the latch's name also reddens this check ON PURPOSE -- "
              "a pointer or reference to the field, even a read-only `const bool&` that "
              "writes nothing today, and a helper called with it by reference -- because "
              "a name scan cannot see a write through one, so it must not be trusted "
              "while it exists (review-16 open question 2, review-18 F1/F6)")
    # --- the two places that fill GPU texel memory (review-6 F1) --------------
    # `pica::texelBytes` is pinned on the host by `pica_matrix_tests.cpp`, but the
    # helper being right is NOT the shipped property. The property is that the two
    # places which fill texel memory CALL it. Batch 120 pinned the helper and
    # nothing else, so reverting both expansions to the pre-batch writes -- the
    # exact shipped defect, R = 255 on 96000 of 96000 panel pixels -- left the whole
    # suite and every runnable gate green (review-6 F1, mutation run quoted). And
    # `renderer_c3d.cpp` includes <3ds.h>, so no host binary can reach these two
    # call sites: a source check is the only thing that can.
    #
    # THE RULE IS "NO DIRECT TEXEL WRITE", NOT "NO WRONG WRITE". Inside an
    # expansion the only permitted way to put a byte into texel memory is the
    # helper, so ANY indexed assignment or bulk copy in either body is refused
    # instead of a catalogue of the bad shapes: the two shipped defects were a
    # four-line `pixel[N] = ...` block and a row `memcpy`, and a third is one
    # keystroke from either. A zero-fill `memset` at a base pointer is not a texel
    # write (it fills the POT padding with the transparent texel, which is alpha 0
    # under ANY byte order) and a read like `row[(size_t)x * 4 + 0]` is not an
    # assignment, so neither is caught -- review-10 F3 drew that line: a memset with
    # a NONZERO fill, or aimed at an OFFSET, is a targeted write and IS caught. A
    # probe pins that hoisting such a read into `src[0]` stays green.
    def expansion(name):
        """(span, body, [absolute '(' of each helper call]) for one expansion.
        `body_span` only matches `void` functions and these return a `uint8_t*`, so
        the more permissive `fn_body_span` is the one that resolves them; a missing
        expansion returns nothing, which every check below reads as a failure rather
        than a skip."""
        sp = fn_body_span(text, name)
        if sp is None:
            return None, None, []
        body = text[sp[0]:sp[1]]
        parens = [sp[0] + m.end() - 1
                  for m in re.finditer(helper_call_re(text), body)]
        return sp, body, parens

    def texel_bypasses(body):
        """Statements in a body that write texel memory without the helper.

        Review-7 F5: this used to look only for the `name[i] = ...` shape, so a
        post-write through a cast -- `((uint8_t*)pixel)[3] = paletteRgba[index*4+3];`,
        the palette's own alpha restored -- kept all four checks green while memory
        held something else, and the alpha check went on printing `255u` because it
        reads the ARGUMENT. The rule is "no direct texel write", so every spelling of
        a write is refused: a subscript on a name, a subscript on a cast, a bare
        dereference, and the bulk copies. A compound assignment (`pixel[0] += 1`) is a
        write too and is refused; a COMPARISON is not, and the `[^=]` terminator is
        what keeps `==`, `!=`, `<=` and `>=` out of the hit set.

        Review-8 F1: the scan ran over LINES, and every one of those regexes needs a
        character after the `=`, so the same statement split after the `=` -- the
        spelling this file itself wraps to at ~100 columns -- matched none of them.
        It now runs over `statements(blank_comments(body))`: comment-aware (so a bare
        `*dst = ...;` is seen, which `code_lines` drops) and continuation-folded (so
        the write is one unit whatever the formatter did to it.

        Review-9 F1: folding makes the STATEMENT the unit, so anchoring the bare
        dereference at `^` refused only the write that BEGAN its statement. A guarded
        `if (index != 0) *(pixel + 3) = ...`, an `else`, and a one-line `for` body are
        that same write with punctuation in front of it, and all three passed the gate;
        the rule is now `deref_write(stmt)`, which asks what precedes the `*` instead of
        asking whether the `*` is first.

        Review-10 F3: `memcpy` and `memmove` were adjudicated as bulk texel writes and
        `memset` was not, and the asymmetry is a hole, not a simplification -- memset is
        the one intrinsic that can set a byte to a CONSTANT, so `memset(pixel + 3,
        0xFF, 1);` one line after a correct helper call re-pins the alpha to 255, byte
        for byte the shipped symptom, with the gate's blessing.

        Review-11 F1 and F3 closed the two ways batch 125's own memset rule failed. F3:
        the rule was a NAME list, and `\bmemset` cannot match `__builtin_memset` (the
        character before it is a word character) while `std::fill` and `std::copy` were
        in no list at all -- the exact defect, one spelling over, green at 41/41. The
        match is now the OPERATION: memset/__builtin_memset/std::memset/std::fill/
        std::fill_n set a constant, memcpy/memmove/std::copy move source bytes, and a
        COPY is a texel write whatever it copies because the source is in the seam's
        order. F1: the destination was adjudicated by whether its TEXT contained a `+`,
        which is neither "is this an offset" nor "is this the padding", and it failed
        both ways -- `memset(&pixel[3], 0, 1)`, `memset(&out[(size_t)x * 4], 0, 4)` and
        `memset(pixel, 0, 4)` each erase texel bytes the helper just wrote and each
        printed `checks=41 failed=0`. A zero fill is now the padding fill only when the
        destination is the buffer this body allocated AND the length is that allocation's
        own size expression -- i.e. it fills the whole buffer with the transparent texel
        and nothing else. Stated limit: a per-row padding fill (`memset(out + width * 4,
        0, (paddedWidth - width) * 4)`) is refused, because proving that region is all
        padding is arithmetic on width and paddedWidth this scan does not do; the shipped
        filler zeroes the whole buffer once at the base, and a batch that wants a per-row
        fill must widen this rule with that proof, not work around it.

        Review-12 F1 is the dimension batch 126 left free: it read the fill's
        DESTINATION and its LENGTH and never looked at where the fill SITS, so
        `memset(pixels, 0, texels * 4)` moved to the line before `return pixels;` --
        after every row -- erased the whole texture and printed `checks=41 failed=0`.
        That is the case review-11 F1 named ("at any length and ANY POSITION, including
        after the draw") and the one batch 126's own closing sentence asserted
        impossible. A padding fill is now legal only where it cannot erase a texel:
        before the first statement that writes one -- the helper call counts, because
        the fill's whole purpose is to be overwritten -- and not inside a loop whose
        body writes one, which is the same defect with the fill left where it was and
        the rows moved around it (review-12 open question 2). Stated limit, in the same
        shape as the per-row one above: a loop this scan cannot read as a loop (`do` is
        read, a macro that expands to one is not) is not searched for a write inside it.
        Review-12 F6 is review-11 F3 one spelling further: `std::fill` was matched and
        `fill` after `using std::fill;` was not.

        Review-13 F1 is that rule one dimension further. Batch 126 read the fill's
        DESTINATION and LENGTH, batch 127 read where it SITS, and neither read how MANY:
        `bulk_op` adjudicated the FIRST bulk setter in a statement and no more, and a C++
        statement is delimited by a semicolon rather than a newline, so the comma
        operator puts two of them in one statement. `memset(pixels, 0, texels * 4),
        memset(pixels + 4, 0xFF, 4);` is one statement carrying the legal padding fill
        and an offset opaque fill of the base of the texture, and the gate vouched for
        the statement on the strength of the first -- green at 41/41, and green on the
        pre-batch gate too, so this was a hole batch 125 left open rather than one this
        batch cut. Every match is now adjudicated and the statement is 'padding' only if
        EVERY setter in it is the padding fill, which is the same fail-closed doctrine
        the argument reader already states about an argument list it cannot read. Stated
        limit, in the shape of the two above: a setter that reaches the statement through
        a macro this scan does not expand is still invisible, so the rule is on the
        statement and not on the program."""
        def bulk_ops(stmt, using):
            """Every bulk byte-setter in this statement, in source order, as a list of
            (kind, paren). `kind` is 'copy' when the operation MOVES SOURCE BYTES and one
            of 'memset' / 'fill' / 'fill_n' when it SETS A CONSTANT (review-11 F3: the
            operation, not the spelling). A qualified spelling is always in scope; a bare
            algorithm name is in scope only when the file carries its `using std::`
            declaration (review-12 F6)."""
            found = []
            for rx in (BULK_COPY, BULK_FILL, BARE_COPY, BARE_FILL):
                bare = rx is BARE_COPY or rx is BARE_FILL
                for m in rx.finditer(stmt):
                    if bare and m.group(1) not in using:
                        continue
                    g = m.group(0)
                    found.append((m.start(), m.end() - 1,
                                  "copy" if rx is BULK_COPY or rx is BARE_COPY
                                  else ("memset" if "memset" in g
                                        else ("fill_n" if "fill_n" in g else "fill"))))
            # A plain `memcpy(` is matched by both the qualified and the bare rule and
            # the two are ONE operation; the open paren is the identity that separates
            # that duplicate from a second setter further along the same statement.
            out, seen = [], set()
            for _start, paren, kind in sorted(found):
                if paren not in seen:
                    seen.add(paren)
                    out.append((kind, paren))
            return out

        def bulk_one(kind, paren, stmt, allocs):
            """'write' or 'padding' for ONE bulk setter, located by its kind and the index
            of its open paren in `stmt`.

            'write' puts texel bytes into texel memory without the helper. 'padding' is
            the whole-buffer transparent fill of a buffer THIS BODY ALLOCATED, which is
            legal only in a position that cannot erase a texel (see below). An argument
            list this scan cannot read fails closed: a bulk write whose fill,
            destination or length it cannot parse is a write it cannot vouch for."""
            if kind == "copy":
                return "write"
            args = call_args_at(stmt, paren, keep_literals=True)
            if not args or len(args) < 3:
                return "write"
            if kind == "memset":
                dst, fill, length = args[0], args[1], args[2]
            elif kind == "fill_n":
                dst, length, fill = args[0], args[1], args[2]
            else:                       # std::fill(first, last, value)
                dst, fill = args[0], args[2]
                head = " ".join(dst.split()) + " + "
                last = " ".join(args[1].split())
                if not last.startswith(head):
                    return "write"      # `std::fill(p + 3, ...)` is not a base fill
                length = last[len(head):]
            if not ZERO_FILL.fullmatch(" ".join(fill.split())):
                return "write"
            # The length is compared as arithmetic with the whitespace taken out, not as
            # the author's spacing (review-12 F4): `memset(pixels, 0, texels*4)` is the
            # shipped statement after a column wrap, and a gate that reddens a reflow
            # gets routed around. The allocation side is normalised the same way, so all
            # this removes is a formatting difference; two different expressions still
            # cannot compare equal.
            dst = " ".join(dst.split())
            n = re.sub(r"\s+", "", length)
            return "padding" if any(dst == base and n == size
                                    for base, size in allocs) else "write"

        def bulk_classify(stmt, allocs, using):
            """'write', 'padding' or None for the bulk byte-setters of this statement.

            EVERY setter in the statement is adjudicated, not the first the scan reaches
            (review-13 F1), and the statement is 'padding' only when EVERY one of them is
            the padding fill: one offset, opaque or unreadable second setter makes the
            whole statement a write, because a setter this scan cannot vouch for is a
            setter it has not read."""
            verdicts = [bulk_one(kind, paren, stmt, allocs)
                        for kind, paren in bulk_ops(stmt, using)]
            if not verdicts:
                return None
            return "write" if "write" in verdicts else "padding"

        if body is None:
            return ["(the expansion is absent)"]
        blank = blank_comments(body)
        # The buffers this body allocated, by name and by the size expression it asked
        # for: that pair is the only destination+length the padding fill may have. The
        # assignment is read as `name = <anything but ;> malloc(` because the shipped
        # spelling wraps the size in a `static_cast<uint8_t*>(...)`. Read from the
        # BLANKED body like every other scan in this file (review-12 open question 1):
        # prose is not code, so a commented-out `// junk = malloc(n)` must not put `n`
        # on the list of sizes a fill may legitimately use.
        allocs = []
        for m in re.finditer(r"([A-Za-z_][A-Za-z0-9_]*)[ \t]*=[ \t]*[^;]*?\bmalloc[ \t]*\(", blank):
            margs = call_args_at(blank, m.end() - 1)
            if margs and len(margs) == 1:
                allocs.append((m.group(1), re.sub(r"\s+", "", margs[0])))
        using = set(USING_STD.findall(blank_cached(text)))
        stmts = statements(blank, with_offsets=True)
        cls = [bulk_classify(s, allocs, using) for _, s in stmts]
        helper = re.compile(helper_call_re(text))
        # Brace depth of each statement, so a loop's body is the run of statements after
        # its header that sit deeper than the header and ends at the first one that no
        # longer does -- which is the only way to see a fill that sits INSIDE the loop it
        # erases (review-12 open question 2).
        depths = [brace_depth(blank, 0, off) for off, _ in stmts]

        def direct_write(i):
            """True when statement i writes texel bytes WITHOUT the helper: a subscript on
            a name or a cast, a write through a dereference, or a bulk setter that is
            not the padding fill."""
            s = stmts[i][1]
            return (cls[i] == "write" or SUBSCRIPT_WRITE.search(s) is not None
                    or CAST_SUBSCRIPT_WRITE.search(s) is not None or deref_write(s))

        def writes_texel(i):
            """True when statement i puts a byte into texel memory at all, including the
            helper call. The padding fill is measured against THIS and not against
            `direct_write`: a fill that follows the first helper call is a wipe, and the
            helper call is the legal write it would wipe."""
            return direct_write(i) or helper.search(stmts[i][1]) is not None

        first = next((i for i in range(len(stmts)) if writes_texel(i)), None)

        def erased_from_inside_a_loop(i):
            """True when statement i sits inside a loop whose body also writes texels.

            A whole-buffer fill in a loop that paints texels wipes every earlier
            iteration's rows: the review-12 F1 defect one brace deeper, and source order
            cannot see it because the fill still precedes the write it erases."""
            for l in range(len(stmts)):
                head = stmts[l][1]
                if not (re.match(r"(?:for|while)[ \t]*\(", head) or head == "do"):
                    continue
                e = l + 1
                while e < len(stmts) and depths[e] > depths[l]:
                    e += 1
                if l < i < e and any(writes_texel(w) for w in range(l + 1, e)):
                    return True
            return False

        hits = []
        for i, (_, stmt) in enumerate(stmts):
            if direct_write(i):
                hits.append(stmt)
            elif cls[i] == "padding" and ((first is not None and i > first)
                                          or erased_from_inside_a_loop(i)):
                hits.append(stmt)
        return hits

    def alpha_of(text, paren):
        """The alpha argument of a `texelBytes(dst, r, g, b, a)` call, or None."""
        args = call_args_at(text, paren)
        if not args or len(args) != 5:
            return None
        return args[4]

    def args_of(text, paren):
        """The five arguments of a `texelBytes` call, or []."""
        args = call_args_at(text, paren) if paren is not None else None
        return args if args and len(args) == 5 else []

    def binding(ident, body):
        """The right-hand side `ident` is initialised or assigned from in `body`, or
        None. One line, declaration or assignment, first match wins.

        This is the whole data-flow model, and it is deliberately one hop deep: enough
        to read `const uint8_t* colours = paletteRgba;` and `const uint8_t r =
        row[(size_t)x * 4 + 0];` -- the two spellings review-9 F2 and F4 found the gate
        could not read -- and not an attempt at a call graph."""
        if not (body and ident):
            return None
        # Review-10 F2's class, one rule over: `(?!=)` keeps `a == b` from reading as
        # a binding of `a` to `= b`. (`!=`, `<=` and `>=` never matched: the operator
        # character sits where `[ \t]*=` requires the `=` itself.)
        m = re.search(r"(?:^|[;{}])[^;{}=]*\b" + re.escape(ident) +
                      r"[ \t]*=(?!=)[ \t]*([^;{}]+)", body)
        return m.group(1).strip() if m else None

    def byte_index(arg, body=None, depth=0):
        """The byte offset a source expression reads: `row[(size_t)x * 4 + 2]` -> 2,
        `src[3]` -> 3, and None for a constant such as `255u` or `0u`.

        Review-7 F2: the defect class being pinned is a wrong CHANNEL ORDER, and its
        cheapest alternate spelling was `texelBytes(pixel, palette[... + 2],
        palette[... + 1], palette[... + 0], 255u)` -- every batch-121 check stayed
        green over it, because they asked whether the helper was called and what the
        alpha was, never which byte each colour argument read.

        Review-9 F4: a bare identifier is a byte too when the body says so.
        `const uint8_t r = row[(size_t)x * 4 + 0]; texelBytes(p, r, g, b, a);` is the
        same four bytes in the same order, and a reader that stopped at the identifier
        reddened that legal refactor in four checks -- review-16 R2's class, a gate that
        reddens a shape the port may write. So a name is followed through its own binding
        to a bounded depth, and a name with no binding in this body still reads as None:
        fail closed on what the scan cannot read."""
        if not arg:
            return None
        a = arg.strip()
        m = re.search(r"(\d+)[ \t]*\]$", a)
        if m:
            return int(m.group(1))
        if body is not None and depth < 3 and re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", a):
            rhs = binding(a, body)
            if rhs:
                return byte_index(rhs, body, depth + 1)
        return None

    def stride_of(arg, body=None, depth=0):
        """The pixel stride an argument indexes its source with: `row[(size_t)x * 4 + 2]`
        -> 4, `rgb[(size_t)x * 3]` -> 3, and None when the expression does not say.

        Review-9 F3 asked for the 4-byte and 3-byte source paths to be told apart by
        their STRIDE rather than by "anything that is not palette-fed may be opaque",
        because the stride is what says whether the source HAS an alpha byte at all: a
        4-stride source does and its alpha must reach the PICA, a 3-stride source has
        none and its texel must be opaque. Stated limit: a 3-byte source read through a
        hoisted pointer (`src[0]`) hides its stride, and the gate then reads it as a
        4-byte source and asks for byte 3."""
        if not arg:
            return None
        a = arg.strip()
        m = re.search(r"\[[^\]]*\*[ \t]*(\d+)[^\]]*\][ \t]*$", a)
        if m:
            return int(m.group(1))
        if body is not None and depth < 3 and re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", a):
            rhs = binding(a, body)
            if rhs:
                return stride_of(rhs, body, depth + 1)
        return None

    def fn_spans(text):
        """[(name, (start, end))] for every function definition in the file.

        Review-7 F1 asked for a WHOLE-FILE rule, and a whole-file rule needs every
        function, not the two this file happens to have named. The definitions are
        found in `blank_cached(text)` so a commented-out definition is not a
        definition (review-8 F3); offsets survive blanking, so the spans still index
        the text every caller slices."""
        out = []
        for m in FN_DEF.finditer(blank_cached(text)):
            sp = block_span_from(text, m.end() - 1)
            if sp:
                out.append((m.group(1), (m.start(), sp[1])))
        return out

    def helper_call_re(text):
        """The regex for a call of the texel helper, qualified or not.

        Review-8 F5: a `using mdk::pica::texelBytes;` makes the unqualified spelling
        legal C++, and a gate that recognised only `pica::texelBytes` read such a
        file as having NO helper call at all -- 0 calls, every filler wrong. The
        unqualified name is accepted only when the file actually carries the
        using-declaration, so the helper's qualification stays the invariant and is
        now stated here rather than implicit in one regex (review-8 open question 2).

        Review-9 F8: this read the RAW text while every other scan in the family reads
        the blanked copy, so a commented-out `// using mdk::pica::texelBytes;` enabled
        unqualified matching for the whole file -- and a local function of its own
        called `texelBytes` then counted as "the helper", which is the defect with the
        gate's blessing. Prose is not code, here as everywhere else."""
        if re.search(r"\busing[ \t]+(?:mdk::)?pica::texelBytes[ \t]*;", blank_cached(text)):
            return r"\b(?:pica::)?texelBytes[ \t]*\("
        return r"\bpica::texelBytes[ \t]*\("

    def helper_writers(text):
        """Names of functions in this file that fill texel memory through the helper."""
        pat = re.compile(helper_call_re(text))
        return set(n for n, sp in fn_spans(text)
                   if pat.search(text[sp[0]:sp[1]]))

    def texel_calls(text):
        """[(offset, args)] for every 5-argument helper call in the FILE.

        Review-8 F2: the channel-order and alpha rules used to read the arguments of
        the two named expansions only, so a third filler that called the helper with
        the palette bytes swapped -- the shipped R = 255 defect with the helper
        intact -- passed every check. A rule about what the helper is handed has to
        look at every place the helper is handed something."""
        out = []
        blank = blank_cached(text)
        for m in re.finditer(helper_call_re(text), blank):
            args = call_args_at(text, m.end() - 1)
            if args and len(args) == 5:
                out.append((m.start(), args))
        return out

    def palette_fed(args, body=None, fname=None):
        """True when a helper call's colour arguments read a palette.

        The two paths have OPPOSITE alpha rules -- an indexed texel's alpha is forced
        opaque because the oracle's shader writes 1.0, an RGBA texel's alpha is the
        source's own fourth byte because the seam carries a real alpha -- so a
        file-wide rule must be able to tell them apart without naming them.

        Review-9 F2: it used to ask whether the argument TEXT said so, and the claim
        that "the argument text says which path the call is on" was false --
        `const uint8_t* colours = paletteRgba;` followed by `colours[index * 4 + 3]`
        as the alpha is the palette path with the word `palette` gone, and it walked
        through as a source-fed call, where a forced alpha is allowed. So the path is
        now resolved: an argument counts as palette-fed when the expression itself, or
        the one-line binding of any identifier in it, or the call-site argument that
        feeds a parameter in it, reaches a palette."""
        return any(provenance_palette(a, body, 0, fname) for a in args[1:4])

    def provenance_palette(expr, body=None, depth=0, fname=None):
        """Whether `expr` reads a palette, following one-line bindings and parameters
        at their call sites to a bounded depth.

        Review-10 F1: the walk used to dead-end at a PARAMETER, and the branch an
        unresolved call falls into is the source-fed one, whose acceptance test -- the
        alpha is the source's fourth byte -- is EXACTLY what the shipped defect does
        with the palette's alpha. So a filler whose palette parameter was renamed
        (`const uint8_t* colours`) was review-9 F2's defect moved one level up, and it
        passed all 41 checks. A parameter is now resolved at the function's call sites
        in this file: every filler here is called here (`pixels = expandIndices(...,
        texture.palette.data(), ...)`), so the argument that feeds the parameter is
        readable and is adjudicated in the caller's body, where it was written.

        Stated limit, beside the stride limit above: a palette that reaches the call
        through an expression this scan cannot read -- a member of a member, a call
        whose name says nothing, a filler with no call site in this file -- resolves
        as NOT palette-fed, and the source-fed branch then asks for the fourth byte.
        What stays fail-closed is the ALPHA: an alpha this scan cannot read at all is
        refused by the alpha rule, not waved through by this one.

        Two numbers this walk had never written down, in answer to review-11's open
        questions 2 and 3. (1) THE BOUND IS THREE HOPS: `depth >= 3` refuses at depth 3,
        and it fails open for the PATH only (the alpha stays fail-closed), because a
        palette needing a fourth hop has also passed through an argument the scan cannot
        read -- the residual stated above. It is written here so the next reviewer
        checks a number instead of re-deriving one. (2) `fn_def_site` resolves a name to
        the FIRST definition in the file and `fn_call_sites` gathers sites by name alone,
        so two same-name functions (legal C++, nothing in the tree forbids it) would cross
        parameters and arguments. Not reachable today; and a cross would resolve a palette
        as SOURCE-fed, which asks for the fourth byte -- it fails toward the stricter
        branch, not toward a free pass."""
        if not expr:
            return False
        if re.search(r"palette", expr, re.IGNORECASE):
            return True
        if body is None or depth >= 3:
            return False
        for ident in re.findall(r"[A-Za-z_][A-Za-z0-9_]*", expr):
            rhs = binding(ident, body)
            if rhs:
                if provenance_palette(rhs, body, depth + 1, fname):
                    return True
                continue
            if not fname:
                continue
            k = fn_param_index(fname, ident)
            if k is None:
                continue
            for cargs, cpos in fn_call_sites(fname):
                if cargs and len(cargs) > k and provenance_palette(
                        cargs[k], enclosing_fn(text, cpos), depth + 1,
                        enclosing_fn_name(text, cpos)):
                    return True
        return False

    def texel_fillers(text):
        """[(name, body)] for every function that allocates a padded RGBA8 buffer.

        The class is defined by what the function ALLOCATES -- a
        `paddedWidth * paddedHeight * 4` byte buffer is texel memory whatever the
        function is called -- so renaming both expansions reddens the check below
        instead of emptying it silently."""
        out = []
        for name, sp in fn_spans(text):
            body = text[sp[0]:sp[1]]
            if re.search(r"malloc[ \t]*\([^;]*\*[ \t]*4[ \t]*\)", body) and \
               re.search(r"padded(?:Width|Height)", body):
                out.append((name, body))
        return out

    def enclosing_fn(text, pos):
        """The text of the function containing the absolute offset `pos`."""
        for name, sp in fn_spans(text):
            if sp[0] <= pos < sp[1]:
                return text[sp[0]:sp[1]]
        return None

    def enclosing_fn_name(text, pos):
        """The NAME of the function containing the absolute offset `pos`."""
        for name, sp in fn_spans(text):
            if sp[0] <= pos < sp[1]:
                return name
        return None

    def fn_def_site(fname):
        """(name offset, '(' offset) of the DEFINITION of `fname`, or (None, None).

        FN_DEF requires the `(` to follow the name with only whitespace between,
        so the first `(` at or after a matched name is that definition's own.
        Stated limit (review-11 open question 2): with two same-name definitions this
        returns the FIRST, and `fn_call_sites` cannot tell their call sites apart."""
        blank = blank_cached(text)
        for m in FN_DEF.finditer(blank):
            if m.group(1) == fname:
                return m.start(1), blank.find("(", m.start(1))
        return None, None

    def fn_param_index(fname, ident):
        """The position of `ident` in `fname`'s parameter list, or None if it is not
        a parameter. A declarator's name is the last identifier of its parameter (an
        array suffix, `int argv[]`, is allowed for); the parameter list is read with
        the same bracket-aware splitter as a call's argument list."""
        _, paren = fn_def_site(fname)
        if paren is None:
            return None
        for i, prm in enumerate(call_args_at(text, paren) or []):
            m = re.search(r"([A-Za-z_][A-Za-z0-9_]*)[ \t]*(?:\[[^\]]*\])?$",
                          prm.strip())
            if m and m.group(1) == ident:
                return i
        return None

    def fn_call_sites(fname):
        """[(args, offset)] for every call of `fname` in this file outside its own
        definition. The sites are found in the blanked copy, so prose that names the
        function is not a call site (review-8 F5's rule, applied one level up)."""
        blank = blank_cached(text)
        npos, _ = fn_def_site(fname)
        out = []
        for m in re.finditer(r"\b" + re.escape(fname) + r"[ \t]*\(", blank):
            if npos is not None and m.start() == npos:
                continue
            out.append((call_args_at(text, m.end() - 1), m.start()))
        return out

    def sink_calls(text):
        """[(offset, args)] for every `C3D_TexLoadImage` call, comments excluded.

        Review-8 F4: the exclusion used to skip only a line whose first characters
        were `//`, so a sink inside a `/* */` block, or after a `//` on a shared
        line, was adjudicated as a real sink and reddened a correct file. Prose is
        not code -- the rule `blank_comments` exists to enforce -- so the sites are
        now found in the blanked copy."""
        out = []
        blank = blank_cached(text)
        for m in re.finditer(r"\bC3D_TexLoadImage[ \t]*\(", blank):
            args = call_args_at(text, m.end() - 1)
            if args:
                out.append((m.start(), args))
        return out

    def producers_of(arg, body_text):
        """The function names that can hand `arg` its value inside `body_text`: every
        call written in the argument, else every `arg = name(...)`.

        Calls are read wherever they sit in the argument, not only at its start, and a
        member call (`.data()`) is not a producer: an inline
        `C3D_TexLoadImage(&tex, expandIndices(a, b, c), ...)` and a ternary between the
        two expansions are the same data flow as the shipped `pixels = expandIndices(...)`
        one line earlier, and refusing them would be review-16 R2's failure -- a gate
        that reddens a shape the port may write gets routed around. `texture.source.data()`
        still resolves to nothing and is still refused, and so does pointer arithmetic
        into a buffer whose producer this scan cannot see: fail closed on what it cannot
        read.

        Review-8 F5 widened two of those refusals, because both are shapes a later
        batch may legitimately write: a buffer kept on the struct (`texture.pixels`,
        assigned in the enclosing function) resolves through that assignment, and a
        helper reached unqualified through a `using`-declaration is the helper. What
        is STILL refused, and is a stated limit rather than an oversight: a sink
        factored into its own function that takes the pointer as a PARAMETER
        (`uploadTexels(&texture.tex, pixels)`), because resolving a parameter needs
        the call sites of that function, i.e. a call graph this scan does not have.
        A gate that reddens that shape gets routed around; a gate that guessed at it
        would be reading a data flow it had not found."""
        a = (arg or "").strip()
        names = re.findall(r"(?:^|[^.\w])([A-Za-z_][A-Za-z0-9_]*)[ \t]*\(", " " + a)
        if names:
            return names
        # POINTER ARITHMETIC INTO A BUFFER THIS SCAN CAN SEE (2026-11-17, the mip
        # chain): `pixels + offset` is the buffer `pixels`, so it resolves to
        # whatever produced `pixels` -- the levels after level 0 live in the same
        # buffer the expansion filled (a local, or a member kept on the record:
        # `texture.pixels + offset`, review-8 F5's shape). Arithmetic on a CALL's
        # result (a member's `.data()`) or a cast still resolves to nothing and is
        # still refused; the arm `the sink is handed an offset into the unrotated
        # source` holds that.
        m = re.fullmatch(r"([A-Za-z_][A-Za-z0-9_]*(?:(?:\.|->)[A-Za-z_][A-Za-z0-9_]*)*)"
                         r"[ \t]*\+[ \t]*[A-Za-z_0-9 \t()*>]+", a)
        if m:
            return producers_of(m.group(1), body_text)
        if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*(?:[.-]>?[A-Za-z_][A-Za-z0-9_]*)*", a) \
                and body_text:
            return re.findall(r"\b" + re.escape(a) +
                              r"[ \t]*=[ \t]*([A-Za-z_][A-Za-z0-9_]*)[ \t]*\(", body_text)
        return []

    idx_span, idx_body, idx_parens = expansion("expandIndices")
    rgba_span, rgba_body, rgba_parens = expansion("expandRgba")
    idx_bypass = texel_bypasses(idx_body)
    rgba_bypass = texel_bypasses(rgba_body)
    idx_args = args_of(text, idx_parens[0]) if idx_parens else []
    rgba_args = args_of(text, rgba_parens[0]) if rgba_parens else []

    rep.check("index-texels-are-rotated-for-the-pica",
              idx_body is not None and bool(idx_parens) and not idx_bypass,
              "%d pica::texelBytes call(s), %d direct texel write(s): %s (the palette is "
              "RGBA in memory and the PICA reads [A, B, G, R]; writing the palette's own "
              "order into texel memory is what painted every pixel with R = 255)"
              % (len(idx_parens), len(idx_bypass),
                 "; ".join(idx_bypass) if idx_bypass else "none"))
    rep.check("rgba-texels-are-rotated-for-the-pica",
              rgba_body is not None and bool(rgba_parens) and not rgba_bypass,
              "%d pica::texelBytes call(s), %d direct texel write(s): %s (a row memcpy "
              "copies the seam's RGBA order straight through, which is the same defect "
              "in a tidier spelling)"
              % (len(rgba_parens), len(rgba_bypass),
                 "; ".join(rgba_bypass) if rgba_bypass else "none"))

    # THE DESTINATION'S ORDER, WHICH IS A SECOND, INDEPENDENT QUESTION FROM THE
    # BYTE ORDER ABOVE. Rotating the bytes correctly and writing them to
    # `y*width + x` still ships a texture the PICA samples scrambled: it reads an
    # 8x8 Morton tile (`pica::textureTexelIndex` holds the rule, the citation and
    # the reproduced symptom), and `C3D_TexLoadImage` is a plain copy, so nothing
    # complains. Measured as batch 173's symptom -- a period-32 wave came back as
    # period 8, and a left/right split came back as horizontal bands -- and the
    # host fixture in `pica_matrix_tests.cpp` pins the permutation itself. This
    # check pins the CALL: a body that writes a row-major offset has lost the
    # tile order however correct its bytes are. Both spellings are accepted
    # (`mdk::pica::` and the unqualified form a `using`-declaration allows) and
    # the row-major offset is refused explicitly rather than inferred from an
    # absent call, so a future refactor that hoists the slot into a local keeps
    # passing while a regression to rows does not.
    def tiled_destination(body):
        """(uses the tile helper, found a row-major offset or None).

        THE BODY IS BLANKED FIRST. This file documents the rule in prose --
        `// THE DESTINATION IS A TILE SLOT, NOT x*paddedWidth` -- and a naive
        scan read that sentence as the defect it warns about (the same
        prose-is-not-code trap review-8 F4 fixed for the sink scan)."""
        if body is None:
            return (False, "no body")
        code = blank_comments(body)
        helper = re.search(r"\b(?:mdk::)?(?:pica::)?textureTexelIndex\s*\(", code)
        rows = re.search(r"\by\s*\*\s*(?:\(\s*size_t\s*\)\s*)?paddedWidth\b", code) or \
            re.search(r"\(\s*size_t\s*\)\s*paddedWidth\s*\*\s*[A-Za-z_]", code) or \
            re.search(r"paddedWidth\s*\*\s*\(\s*size_t\s*\)\s*y\b", code)
        return (helper is not None, rows.group(0) if rows else None)

    idx_tiled, idx_rows = tiled_destination(idx_body)
    rgba_tiled, rgba_rows = tiled_destination(rgba_body)
    rep.check("index-texels-are-tiled-for-the-pica",
              idx_tiled and idx_rows is None,
              "textureTexelIndex %s, row-major offset %s (the PICA reads 8x8 Morton "
              "tiles and C3D_TexLoadImage copies verbatim, so a row-major destination "
              "is sampled scrambled with no error raised -- batch 173)"
              % ("called" if idx_tiled else "MISSING",
                 "found: %s" % idx_rows if idx_rows else "none"))
    rep.check("rgba-texels-are-tiled-for-the-pica",
              rgba_tiled and rgba_rows is None,
              "textureTexelIndex %s, row-major offset %s (same defect as the indexed "
              "path: correct bytes at the wrong slot is still a scrambled texture)"
              % ("called" if rgba_tiled else "MISSING",
                 "found: %s" % rgba_rows if rgba_rows else "none"))

    # THE ALPHA, WHICH IS THE HALF THAT IS GAMEPLAY, NOT COLOUR. An indexed texel's
    # alpha is FORCED OPAQUE because the oracle's own shader says so:
    # `shaders/palette.hlsl:70` returns
    # `float4(palette_texture.Load(...).rgb, 1.0)` — the palette's RGB and a literal
    # 1.0, never the palette's fourth byte, with `index == 0` discarded at :67. The
    # palette's own alpha is therefore not the oracle's alpha, and handing it to the
    # PICA instead makes any texture whose palette stores alpha 0 at a used index
    # vanish, because the alpha test is GPU_GREATER against reference 0. An RGBA
    # texture is the opposite case: the seam carries a real alpha there
    # (`render.h`), so forcing 255 throws away transparency the game asked for.
    # Both directions are pinned, and neither is pinned by the helper's host row,
    # which never sees a caller.
    idx_alpha = alpha_of(text, idx_parens[0]) if idx_parens else None
    rgba_alpha = alpha_of(text, rgba_parens[0]) if rgba_parens else None
    rep.check("index-texel-alpha-is-forced-opaque",
              idx_alpha is not None and
              re.fullmatch(r"(?:255|0xFF)u?", idx_alpha) is not None,
              "the alpha an indexed texel hands the PICA is `%s` (the C# writes 1.0; the "
              "palette's own alpha byte is not the oracle's alpha, and the alpha test is "
              "GPU_GREATER against 0, so a palette-stored 0 discards the texel)"
              % (idx_alpha or "(no 5-argument texelBytes call)"))
    # Review-7 F4: this refused only the LITERAL 255, so `..., row[(size_t)x*4+2], 0u)`
    # walked through and made every RGBA texel invisible -- the mirror image of the
    # defect the check exists to prevent. It now asks for the source's own fourth byte.
    rep.check("rgba-texel-alpha-is-the-sources",
              rgba_alpha is not None and byte_index(rgba_alpha, rgba_body) == 3,
              "the alpha an RGBA texel hands the PICA is `%s` (the seam carries a real "
              "alpha on this path, so it must be the source's OWN fourth byte: a forced "
              "255 discards the texture's transparency and a forced 0 hides every RGBA "
              "texel -- the index path's rule does not transfer here)"
              % (rgba_alpha or "(no 5-argument texelBytes call)"))

    # THE CHANNEL ORDER OF THE ARGUMENTS (review-7 F2). The batch-121 pair asked
    # whether the helper was called and what the alpha was; neither question reads
    # WHICH SOURCE BYTE each colour argument names, so swapping the red and blue
    # sources printed `ok` four times and put red on the console again.
    idx_order = [byte_index(a, idx_body) for a in idx_args[1:4]]
    rgba_order = [byte_index(a, rgba_body) for a in rgba_args[1:4]]
    rep.check("index-texel-channels-are-in-source-order",
              idx_order == [0, 1, 2],
              "the index expansion hands texelBytes the palette bytes %s (the palette is "
              "RGBA in memory, so the red source is byte 0 and the blue source is byte 2; "
              "handing them over swapped is the shipped defect in a spelling that still "
              "calls the helper)" % (idx_order or "nothing"))
    rep.check("rgba-texel-channels-are-in-source-order",
              rgba_order == [0, 1, 2],
              "the rgba expansion hands texelBytes the source bytes %s (same rule on the "
              "RGBA path: the alpha is pinned separately, because this path's alpha is the "
              "source's fourth byte rather than a constant)" % (rgba_order or "nothing"))

    # EVERY HELPER CALL IN THE FILE, NOT THE TWO NAMED ONES (review-8 F2). The four
    # checks above read the arguments of `expandIndices` and `expandRgba`; a THIRD
    # filler that called the helper with the palette bytes swapped, or with a forced
    # `0u` alpha, was the shipped R = 255 defect with the helper INTACT, and passed all
    # 39 checks, because `every-texel-filler-rotates-through-the-helper` asks only
    # WHETHER the helper is called. A rule about what the helper is handed has to look
    # at every place the helper is handed something, so the same two properties are
    # re-adjudicated here over every 5-argument call in the file. The path is resolved
    # per call, not by a name: `palette_fed` follows a one-line alias (review-9 F2),
    # then a PARAMETER at the function's call sites in this file (review-10 F1: the
    # rename had moved one level up -- a filler whose palette parameter is `colours`
    # carries no `palette` substring and no binding, and the source-fed branch's test,
    # "the alpha is the fourth byte", is exactly what the shipped defect does with the
    # palette's alpha), and `stride_of` says whether the source HAS an alpha byte at
    # all (review-9 F3). Three branches:
    #   * palette-fed -- must force opaque, because the oracle's shader writes 1.0;
    #   * a 4-stride (or stride-unknown) source -- must hand over its OWN fourth byte,
    #     because forcing 255 throws away the transparency the seam carries;
    #   * a 3-stride source -- has no fourth byte, so it must be opaque.
    # An alpha this scan cannot read at all (a call, a variable with no binding) is
    # refused: fail closed. The PATH, by contrast, resolves to source-fed when it
    # cannot read -- a palette handed over by a member of a member, or by a filler
    # with no call site in this file, is adjudicated as a source and asked for its
    # fourth byte. That is the residual stated in `provenance_palette`, recorded here
    # because review-10 F1 falsified batch 124's claim that no free pass existed.
    all_calls = texel_calls(text)
    order_bad, alpha_bad = [], []
    for cpos, cargs in all_calls:
        cline = text.count("\n", 0, cpos) + 1
        cbody = enclosing_fn(text, cpos)
        cfname = enclosing_fn_name(text, cpos)
        corder = [byte_index(a, cbody) for a in cargs[1:4]]
        if corder != [0, 1, 2]:
            order_bad.append("line %d hands texelBytes the source bytes %s"
                             % (cline, corder or "nothing"))
        calpha = (cargs[4] or "").strip()
        opaque = re.fullmatch(r"(?:255|0xFF)u?", calpha) is not None
        stride = stride_of(cargs[1], cbody)
        if palette_fed(cargs, cbody, cfname):
            if not opaque:
                alpha_bad.append("line %d hands a PALETTE-fed texel the alpha `%s`"
                                 % (cline, calpha or "nothing"))
        elif stride == 3:
            if not opaque:
                alpha_bad.append("line %d hands a 3-BYTE-sourced texel the alpha `%s`"
                                 % (cline, calpha or "nothing"))
        elif opaque or byte_index(cargs[4], cbody) != 3:
            alpha_bad.append("line %d hands a SOURCE-fed texel the alpha `%s`"
                             % (cline, calpha or "nothing"))
    rep.check("every-texel-call-hands-the-helper-the-source-order",
              bool(all_calls) and not order_bad,
              "%d helper call(s) in the file, %d with the wrong channel order: %s (the "
              "palette and the seam are both RGBA in memory and the PICA reads "
              "[A, B, G, R], so on every path the red source is byte 0 and the blue "
              "source is byte 2; a filler that calls the helper with them swapped is "
              "the shipped defect in a spelling that still calls the helper)"
              % (len(all_calls), len(order_bad),
                 "; ".join(order_bad) if order_bad else "none"))
    rep.check("every-texel-call-hands-the-pica-its-path-s-alpha",
              bool(all_calls) and not alpha_bad,
              "%d helper call(s) in the file, %d with an alpha its path does not allow: "
              "%s (a palette-fed texel must force opaque -- the oracle writes 1.0, and a "
              "palette-stored 0 discards the texel against the GPU_GREATER alpha test -- "
              "while a source-fed texel must hand over its OWN fourth byte, because a "
              "forced 255 discards the transparency the seam carries and a forced 0 hides "
              "every texel on that path; only a 3-stride source, which has no fourth "
              "byte, may force opaque)"
              % (len(all_calls), len(alpha_bad),
                 "; ".join(alpha_bad) if alpha_bad else "none"))

    # THE WHOLE FILE, NOT TWO NAMES (review-7 F1). Batch 121 pinned `expandIndices`
    # and `expandRgba`; a NAME pin is not the property. The reviewer's
    # `expandIndicesFast`, wired in INSTEAD of the pinned expansion and writing
    # `out[x * 4 + 0] = palette[...]`, reproduced the shipped R = 255 defect with all
    # 35 checks green, because nothing below looked at any other body or at the sink.
    # So: every function that ALLOCATES a padded RGBA8 buffer must rotate through the
    # helper, and the set is measured rather than named -- an empty set is a failure.
    fillers = texel_fillers(text)
    helper_pat = re.compile(helper_call_re(text))
    bad_fillers = ["%s (%s)" % (n, "no pica::texelBytes call" if not helper_pat.search(b)
                                else "; ".join(texel_bypasses(b)))
                   for n, b in fillers
                   if not helper_pat.search(b) or texel_bypasses(b)]
    rep.check("every-texel-filler-rotates-through-the-helper",
              bool(fillers) and not bad_fillers,
              "%d texel-filling function(s) in the file, %d wrong: %s (a texel filler is any "
              "function that mallocs a paddedWidth*paddedHeight*4 buffer; the set is measured, "
              "so renaming both expansions reddens this check instead of emptying it silently)"
              % (len(fillers), len(bad_fillers),
                 "; ".join(bad_fillers) if bad_fillers else "none"))

    # AND THE SINK (review-7 F1). The property is "the bytes the PICA is handed were
    # rotated", and the only place that can be enforced is where the PICA receives
    # them. Every `C3D_TexLoadImage` data argument must resolve -- directly, or
    # through the assignments to that local in the enclosing function -- to producer
    # functions in this file that fill texels through the helper. A third expansion
    # that USES the helper stays green: the gate refuses the defect, not a name
    # (review-16 R2's rule, that a check rejecting a legal shape gets routed around).
    sinks = sink_calls(text)
    writers = helper_writers(text)
    bad_sinks = []
    for pos, sargs in sinks:
        sarg = sargs[1] if len(sargs) > 1 else None
        prods = producers_of(sarg, enclosing_fn(text, pos))
        if not prods:
            bad_sinks.append("%s resolves to no producer in this file"
                             % (sarg or "(no data argument)"))
        for p in prods:
            if p not in writers:
                bad_sinks.append("%s = %s(...) fills texels without the helper"
                                 % (sarg, p))
    rep.check("texel-sink-is-fed-by-a-helper-writer",
              bool(sinks) and not bad_sinks,
              "%d C3D_TexLoadImage site(s), %d bad: %s (the sink is where the PICA receives "
              "the bytes, so it is the only place a texel-order pin can bite; a filler called "
              "INSTEAD of the two named ones used to reproduce the shipped defect with every "
              "batch-121 check green)"
              % (len(sinks), len(bad_sinks),
                 "; ".join(bad_sinks) if bad_sinks else "none"))
    return rep


# --- mutation arms ----------------------------------------------------------
# Each one reintroduces one of the defects the checks claim to prevent. An arm
# that cannot build its mutant is a FAIL below, never a skip (review-10 F1).

def arm_drop_diagnostic(text):
    """The diagnostic call removed entirely.

    The argument list is matched as `[^;\n]*`, not `\w*`, because the shipped call
    is `drawDiagnostic(r, mdk_render_canvas_width(rr))` -- a comma and a nested
    call. With the narrow pattern this arm found nothing, returned the file
    unchanged, and reported "the mutation changed nothing": a mutant of nothing
    (review-14 F3's class), batch 145. Still anchored at `^[ \t]*`, so the
    `static void drawDiagnostic(...);` prototype and the definition are NOT
    matches and the arm still removes the CALL and nothing else."""
    out, n = re.subn(r"(?m)^[ \t]*drawDiagnostic\w*\([^;\n]*\);[ \t]*\n", "",
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
    m = re.search(r"(?m)^([ \t]*)(drawDiagnostic\w*\([^;\n]*\);)[ \t]*\n", loop)
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
    m = re.search(r"(?m)^([ \t]*)(" + name + r"\w*\([^;\n]*\);)[ \t]*\n", loop)
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
    "the queue is the only way" (review-3 F1).

    The seam is found by its SIGNATURE and not by its body. It used to be found by
    matching the whole one-line stub, which meant the arm could only ever mutate a
    stub: once the seam grew a real body the mutation matched nothing, the arm
    "changed nothing", and the invariant it exists to certify went uncertified with
    the gate still green -- review-14 F3's class from the other side. The renderer
    parameter's name is read out of the signature too, so renaming it cannot route
    the arm around the check (review-10 F6's lesson, applied to the arms instead of
    the checks). `stmt` spells the parameter `@`."""
    m = re.search(r"void[ \t]+mdk_render_canvas_quad[ \t]*\(([^)]*)\)[ \t]*\{", text)
    if not m:
        return text
    param = m.group(1).split(",")[0].split()[-1]
    return text[:m.end()] + "\n    " + stmt.replace("@", param) + "\n" + text[m.end():]


real = adjudicate(SRC)

# The last line the guest wrote while it was hanging (batch 66), used as the
# anchor for the arms that restore the create-time shader traffic.
SHADER_OK_LINE = ('    mdk::log3dsPrintf("shaderProgramSetVsh ok result=0x%08lx", '
                  '(unsigned long)spVsh);')

ARMS = [
    ("diagnostic-back-in-the-per-draw-path",
     # The splice has to match what the check counts, so it emits the QUAD's six
     # vertices. Splicing `0, 3` here would leave this arm a mutant of nothing:
     # it would add a draw the check cannot see, the target check would stay green,
     # and the arm would report a failure for the wrong reason (review-14 F3's class).
     lambda s: s.replace("    C3D_DrawArrays(GPU_TRIANGLES, first, count);",
                         "    C3D_DrawArrays(GPU_TRIANGLES, first, count);\n"
                         "    if (r->diagnosticTriangle) {\n"
                         "        C3D_DrawArrays(GPU_TRIANGLES, 0, 6);\n"
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
     #
     # It then used to splice after a literal copy of the seam's signature, which pinned
     # the parameter's name the way review-10 F6 says a check must not: the seam now
     # takes `rr`, an arm that cannot find its seam mutates nothing, and a mutation of
     # nothing certifies nothing. `arm_seam_replay` finds any seam by name, which is
     # what the two arms beside it already use.
     lambda s: arm_seam_replay(s, "mdk_render_canvas_begin", "asC3d(rr)"),
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
     lambda s: arm_canvas_quad_emits(
         s, "mdk::MeshDraw rec;\n    drawRecorded(asC3d(@), &rec);"),
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
     lambda s: s.replace("C3D_AlphaTest(true, GPU_GREATER, reference);",
                         "C3D_AlphaTest(true, GPU_GREATER, 5);", 1),
     "alpha-test-is-the-oracles-discard"),
    ("alpha-test-disabled",
     lambda s: s.replace("C3D_AlphaTest(true, GPU_GREATER, reference);",
                         "C3D_AlphaTest(false, GPU_ALWAYS, reference);",
                         1),
     "alpha-test-is-the-oracles-discard"),
    ("alpha-test-ignores-the-draw's-pass (the canvas takes the half cover)",
     lambda s: s.replace("draw->material.pass), false);",
                         "mdk::Pass::Solid), false);", 1),
     "alpha-test-is-the-oracles-discard"),
    ("installEye stops installing the frame's alpha test",
     lambda s: s.replace("    installAlphaTest(r, mdk::pica::alphaTestReferenceFor(r->toggles.enableFiltering,\n"
                         "                                                         mdk::Pass::Solid), true);\n",
                         "", 1),
     "alpha-test-single-writer-in-install-eye"),
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
    # Review-19 F3: the mirror image, and the shape the batch-112 whitelist still
    # accepted because its write alternative names no position. A per-frame PRE-arm
    # is not a spam defect and not a clear: the guard is never true again, so the
    # refusal this latch exists to report is never reported at all.
    ("the refused-mesh latch is ARMED once per frame from mdk_render_begin_frame, so "
     "the guard is never true and a refusal is never logged (review-19 F3)",
     lambda s: s.replace("    r->inFrame = true;\n    r->frame++;",
                         "    r->inFrame = true;\n"
                         "    r->meshAllocFailureLogged = true;\n"
                         "    r->frame++;", 1),
     "mesh-refusal-line-is-edge-triggered"),
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
    # Review-17 R1's asymmetry, closed on this side. The arm above takes the ADDRESS of
    # the latch, which is the spelling an `&field` scan sees. The reference-declaration
    # spelling puts the `&` on the alias's own name and never touches the field's, so it
    # needs the second alternative in the scan -- and a scan that gained it without an
    # arm would be a scan nobody could show still works.
    ("the refused-mesh latch is cleared every frame through a REFERENCE alias declared "
     "with `&` on the alias name (review-17 R1)",
     lambda s: s.replace("    if (r->inFrame) return;   // a begin without an end is a bug, not a re-entry",
                         "    if (r->inFrame) return;   // a begin without an end is a bug, not a re-entry\n"
                         "    bool& latchAlias = r->meshAllocFailureLogged;\n"
                         "    latchAlias = false;", 1),
     "mesh-refusal-latch-cleared-nowhere-else"),
    # Review-18 F1: the two spellings this side's blacklist could not see, and the reason
    # it is now the LATCH_SHAPE whitelist. Neither contains an `&` beside the field or an
    # `=` after the alias's name; the second names no alias at all, so the write exists
    # only under a parameter's name and no scan for alias spellings can find it.
    ("the refused-mesh latch is cleared every frame through a reference bound by DIRECT "
     "initialisation (review-18 F1)",
     lambda s: s.replace("    if (r->inFrame) return;   // a begin without an end is a bug, not a re-entry",
                         "    if (r->inFrame) return;   // a begin without an end is a bug, not a re-entry\n"
                         "    bool& latchAlias(r->meshAllocFailureLogged);\n"
                         "    latchAlias = false;", 1),
     "mesh-refusal-latch-cleared-nowhere-else"),
    ("the refused-mesh latch is cleared every frame by a helper that takes it by "
     "reference (review-18 F1)",
     lambda s: s.replace(
         " void mdk_render_begin_frame(MdkRenderer rr, const MdkFrameParams* params) {",
         "// review-18 F1 mutant: the reset moved into a helper.\n"
         "static void clearMeshRefusal(bool& latch) { latch = false; }\n\n"
         " void mdk_render_begin_frame(MdkRenderer rr, const MdkFrameParams* params) {",
         1).replace(
         "    if (r->inFrame) return;   // a begin without an end is a bug, not a re-entry",
         "    if (r->inFrame) return;   // a begin without an end is a bug, not a re-entry\n"
         "    clearMeshRefusal(r->meshAllocFailureLogged);", 1),
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
    # Review-6 F1: the two texel expansions, reverted to the shipped defect. Batch
    # 120 pinned `pica::texelBytes` on the host and nothing else, so both of these
    # reverts -- the exact code that painted the console red for the backend's whole
    # history -- left every gate green. They are written from the pre-batch file
    # (git 2624f5d), not invented, so each is a program a later batch could commit.
    ("index-expansion-writes-the-palette-order-straight-through (review-6 F1)",
     lambda s: s.replace(
         "            mdk::pica::texelBytes(pixel, paletteRgba[index * 4 + 0],\n"
         "                                  paletteRgba[index * 4 + 1], "
         "paletteRgba[index * 4 + 2], 255u);",
         "            pixel[0] = paletteRgba[index * 4 + 0];\n"
         "            pixel[1] = paletteRgba[index * 4 + 1];\n"
         "            pixel[2] = paletteRgba[index * 4 + 2];\n"
         "            pixel[3] = 255;", 1),
     "index-texels-are-rotated-for-the-pica"),
    ("rgba-expansion-restored-to-a-row-memcpy (review-6 F1)",
     lambda s: s.replace(
         "    for (int32_t y = 0; y < height; y++) {\n"
         "        const uint8_t* row = source + (size_t)y * (size_t)width * 4;\n"
         "        for (int32_t x = 0; x < width; x++) {\n"
         "            const size_t slot =\n"
         "                mdk::pica::textureTexelIndex((uint32_t)x, (uint32_t)y, "
         "(uint32_t)paddedWidth);\n"
         "            mdk::pica::texelBytes(pixels + slot * 4u, row[(size_t)x * 4 + 0],\n"
         "                                  row[(size_t)x * 4 + 1], row[(size_t)x * 4 + 2],\n"
         "                                  row[(size_t)x * 4 + 3]);\n"
         "        }\n"
         "    }",
         "    const size_t rowBytes = (size_t)width * 4;\n"
         "    for (int32_t y = 0; y < height; y++) {\n"
         "        memcpy(pixels + (size_t)y * (size_t)paddedWidth * 4,\n"
         "               source + (size_t)y * rowBytes, rowBytes);\n"
         "    }", 1),
     "rgba-texels-are-rotated-for-the-pica"),
    # THE TILE ORDER, WHICH NOTHING PINNED UNTIL BATCH 173'S SYMPTOM. The arm
    # above restores a row memcpy and is caught by the byte-order rule because a
    # memcpy copies the seam's RGBA order through. This one keeps the rotation
    # and the alpha and changes ONLY the destination -- `y*paddedWidth + x`,
    # which is exactly what the port shipped and what the PICA samples
    # scrambled. Without this arm the whole tile rule could be deleted from both
    # expansions and every other check would stay green.
    ("rgba-expansion-writes-rows-instead-of-tiles-batch-173",
     lambda s: s.replace(
         "            const size_t slot =\n"
         "                mdk::pica::textureTexelIndex((uint32_t)x, (uint32_t)y, "
         "(uint32_t)paddedWidth);\n"
         "            mdk::pica::texelBytes(pixels + slot * 4u, row[(size_t)x * 4 + 0],\n",
         "            mdk::pica::texelBytes(pixels + ((size_t)y * (size_t)paddedWidth + "
         "(size_t)x) * 4u,\n"
         "                                  row[(size_t)x * 4 + 0],\n", 1),
     "rgba-texels-are-tiled-for-the-pica"),
    ("index-expansion-writes-rows-instead-of-tiles-batch-173",
     lambda s: s.replace(
         "            const size_t slot =\n"
         "                mdk::pica::textureTexelIndex((uint32_t)x, (uint32_t)y, "
         "(uint32_t)paddedWidth);\n"
         "            uint8_t* pixel = pixels + slot * 4u;\n",
         "            uint8_t* pixel = pixels + ((size_t)y * (size_t)paddedWidth + "
         "(size_t)x) * 4u;\n", 1),
     "index-texels-are-tiled-for-the-pica"),
    # The two alpha arms. Each keeps the rotation intact and moves ONLY the alpha, so
    # neither can be caught by the byte-order check: they isolate the alpha rows.
    ("index-expansion-hands-the-palette-alpha-to-the-pica (review-6 F1)",
     lambda s: s.replace(
         "paletteRgba[index * 4 + 1], paletteRgba[index * 4 + 2], 255u);",
         "paletteRgba[index * 4 + 1], paletteRgba[index * 4 + 2],\n"
         "                                  paletteRgba[index * 4 + 3]);", 1),
     "index-texel-alpha-is-forced-opaque"),
    ("rgba-expansion-forces-an-opaque-alpha (review-6 F1)",
     lambda s: s.replace("row[(size_t)x * 4 + 2],\n"
                         "                                  row[(size_t)x * 4 + 3]);",
                         "row[(size_t)x * 4 + 2], 255u);", 1),
     "rgba-texel-alpha-is-the-sources"),
    # Review-7 F4: the batch-121 alpha check refused only the LITERAL 255, so a forced
    # `0u` -- which hides every RGBA texel behind the alpha test -- walked through it.
    ("rgba-expansion-forces-a-transparent-alpha (review-7 F4)",
     lambda s: s.replace("row[(size_t)x * 4 + 2],\n"
                         "                                  row[(size_t)x * 4 + 3]);",
                         "row[(size_t)x * 4 + 2], 0u);", 1),
     "rgba-texel-alpha-is-the-sources"),
    # Review-7 F2: the cheapest alternate spelling of the channel-order defect. Each
    # one still calls the helper and still hands over an alpha, so no batch-121 check
    # could see it; the reviewer measured all four green on both sites.
    ("index-expansion-swaps-the-palette-red-and-blue-sources (review-7 F2)",
     lambda s: s.replace(
         "            mdk::pica::texelBytes(pixel, paletteRgba[index * 4 + 0],\n"
         "                                  paletteRgba[index * 4 + 1], "
         "paletteRgba[index * 4 + 2], 255u);",
         "            mdk::pica::texelBytes(pixel, paletteRgba[index * 4 + 2],\n"
         "                                  paletteRgba[index * 4 + 1], "
         "paletteRgba[index * 4 + 0], 255u);", 1),
     "index-texel-channels-are-in-source-order"),
    ("rgba-expansion-swaps-the-source-red-and-blue-bytes (review-7 F2)",
     lambda s: s.replace(
         "            mdk::pica::texelBytes(pixels + slot * 4u, row[(size_t)x * 4 + 0],\n"
         "                                  row[(size_t)x * 4 + 1], row[(size_t)x * 4 + 2],\n"
         "                                  row[(size_t)x * 4 + 3]);",
         "            mdk::pica::texelBytes(pixels + slot * 4u, row[(size_t)x * 4 + 2],\n"
         "                                  row[(size_t)x * 4 + 1], row[(size_t)x * 4 + 0],\n"
         "                                  row[(size_t)x * 4 + 3]);", 1),
     "rgba-texel-channels-are-in-source-order"),
    # Review-7 F5: a write through a CAST was invisible to the batch-121 bypass
    # detector, which looked only for `name[i] = ...`. The alpha check kept printing
    # `255u` (it reads the argument) while memory held the palette's own alpha.
    ("index-expansion-restores-the-palette-alpha-through-a-cast (review-7 F5)",
     lambda s: s.replace(
         "                                  paletteRgba[index * 4 + 1], "
         "paletteRgba[index * 4 + 2], 255u);",
         "                                  paletteRgba[index * 4 + 1], "
         "paletteRgba[index * 4 + 2], 255u);\n"
         "            ((uint8_t*)pixel)[3] = paletteRgba[index * 4 + 3];", 1),
     "index-texels-are-rotated-for-the-pica"),
    # Review-7 F1, first clause: a THIRD texel filler, written instead of the pinned
    # two. This is the reviewer's own mutant, quoted from the pre-batch file: a new
    # `expandIndicesFast` that writes the palette's own order and a rewired call site.
    # It reproduces the shipped R = 255 defect and left all 35 checks green.
    ("a third texel filler writes the palette order and the sink is rewired to it "
     "(review-7 F1)",
     lambda s: s.replace(
         "// WHAT IS LEFT, when a texture does not fit.",
         "static uint8_t* expandIndicesFast(const uint8_t* indices, int32_t width,\n"
         "                                  int32_t height, const uint8_t* paletteRgba,\n"
         "                                  int32_t paddedWidth, int32_t paddedHeight) {\n"
         "    const size_t texels = (size_t)paddedWidth * (size_t)paddedHeight;\n"
         "    uint8_t* pixels = static_cast<uint8_t*>(malloc(texels * 4));\n"
         "    if (!pixels) {\n        return nullptr;\n    }\n"
         "    memset(pixels, 0, texels * 4);\n"
         "    for (int32_t y = 0; y < height; y++) {\n"
         "        const uint8_t* source = indices + (size_t)y * (size_t)width;\n"
         "        for (int32_t x = 0; x < width; x++) {\n"
         "            const uint8_t index = source[x];\n"
         "            uint8_t* pixel = pixels + mdk::pica::textureTexelIndex((uint32_t)x, (uint32_t)y, (uint32_t)paddedWidth) * 4u;\n"
         "            pixel[0] = paletteRgba[index * 4 + 0];\n"
         "            pixel[1] = paletteRgba[index * 4 + 1];\n"
         "            pixel[2] = paletteRgba[index * 4 + 2];\n"
         "            pixel[3] = 255;\n"
         "        }\n"
         "    }\n"
         "    return pixels;\n}\n\n"
         "// WHAT IS LEFT, when a texture does not fit.", 1).replace(
         "        pixels = expandIndices(texture.source.data(), texture.realWidth, "
         "texture.realHeight,",
         "        pixels = expandIndicesFast(texture.source.data(), texture.realWidth, "
         "texture.realHeight,", 1),
     "every-texel-filler-rotates-through-the-helper"),
    # Review-7 F1, second clause: the SINK itself. The expansion is skipped entirely
    # and the seam's unrotated source is handed to the PICA -- the shipped defect
    # reached from the other direction, and the one mutation that isolates the sink
    # from the filler rule (no new filler exists, so only the sink can refuse it).
    ("the sink is handed an offset into the unrotated source (the mip chain's "
     "pointer arithmetic must not become a free pass)",
     lambda s: s.replace("C3D_TexLoadImage(&texture.tex, pixels + offset, GPU_TEXFACE_2D, level);",
                         "C3D_TexLoadImage(&texture.tex, texture.source.data() + offset, "
                         "GPU_TEXFACE_2D, level);", 1),
     "texel-sink-is-fed-by-a-helper-writer"),
    ("the sink is handed the unrotated source directly (review-7 F1)",
     lambda s: s.replace("C3D_TexLoadImage(&texture.tex, pixels, GPU_TEXFACE_2D, 0);",
                         "C3D_TexLoadImage(&texture.tex, texture.source.data(),\n"
                         "                       GPU_TEXFACE_2D, 0);", 1),
     "texel-sink-is-fed-by-a-helper-writer"),
    # Review-8 F1: the SAME writes, wrapped after the `=`. Batch 122's F5 arm pins the
    # unwrapped cast write, and the reviewer showed that inserting a newline after the
    # `=` -- the spelling this file wraps to at ~100 columns routinely -- passed all 39
    # checks with memory holding the palette's alpha in the red slot. Three spellings,
    # because the detector recognises three spellings and a fix that closed only the
    # one the reviewer quoted would have left the other two open.
    ("a wrapped subscript-on-name write restores the palette alpha (review-8 F1)",
     lambda s: s.replace(
         "                                  paletteRgba[index * 4 + 1], "
         "paletteRgba[index * 4 + 2], 255u);",
         "                                  paletteRgba[index * 4 + 1], "
         "paletteRgba[index * 4 + 2], 255u);\n"
         "            pixel[3] =\n"
         "                paletteRgba[index * 4 + 3];", 1),
     "index-texels-are-rotated-for-the-pica"),
    ("a wrapped subscript-on-cast write restores the palette alpha (review-8 F1)",
     lambda s: s.replace(
         "                                  paletteRgba[index * 4 + 1], "
         "paletteRgba[index * 4 + 2], 255u);",
         "                                  paletteRgba[index * 4 + 1], "
         "paletteRgba[index * 4 + 2], 255u);\n"
         "            ((uint8_t*)pixel)[3] =\n"
         "                paletteRgba[index * 4 + 3];", 1),
     "index-texels-are-rotated-for-the-pica"),
    ("a wrapped bare-dereference write restores the palette alpha (review-8 F1)",
     lambda s: s.replace(
         "                                  paletteRgba[index * 4 + 1], "
         "paletteRgba[index * 4 + 2], 255u);",
         "                                  paletteRgba[index * 4 + 1], "
         "paletteRgba[index * 4 + 2], 255u);\n"
         "            *pixel =\n"
         "                paletteRgba[index * 4 + 3];", 1),
     "index-texels-are-rotated-for-the-pica"),
    # Review-8 F2: a THIRD filler that calls the helper -- so batch 122's filler clause
    # is satisfied -- but hands it the palette bytes swapped, or a forced-transparent
    # alpha. That is the shipped R = 255 defect with the helper intact, and it passed
    # every batch-122 check because the order and alpha rules read only the two named
    # expansions. Each arm keeps the OTHER property intact, so each isolates one check.
    ("a third filler calls the helper with the palette red and blue swapped "
     "(review-8 F2)",
     lambda s: s.replace(
         "// WHAT IS LEFT, when a texture does not fit.",
         "static uint8_t* expandIndicesSwapped(const uint8_t* indices, int32_t width,\n"
         "                                     int32_t height, const uint8_t* paletteRgba,\n"
         "                                     int32_t paddedWidth, int32_t paddedHeight) {\n"
         "    const size_t texels = (size_t)paddedWidth * (size_t)paddedHeight;\n"
         "    uint8_t* pixels = static_cast<uint8_t*>(malloc(texels * 4));\n"
         "    if (!pixels) {\n        return nullptr;\n    }\n"
         "    memset(pixels, 0, texels * 4);\n"
         "    for (int32_t y = 0; y < height; y++) {\n"
         "        const uint8_t* source = indices + (size_t)y * (size_t)width;\n"
         "        for (int32_t x = 0; x < width; x++) {\n"
         "            const uint8_t index = source[x];\n"
         "            uint8_t* pixel = pixels + mdk::pica::textureTexelIndex((uint32_t)x, (uint32_t)y, (uint32_t)paddedWidth) * 4u;\n"
         "            mdk::pica::texelBytes(pixel, paletteRgba[index * 4 + 2],\n"
         "                                  paletteRgba[index * 4 + 1],\n"
         "                                  paletteRgba[index * 4 + 0], 255u);\n"
         "        }\n    }\n    return pixels;\n}\n\n"
         "// WHAT IS LEFT, when a texture does not fit.", 1).replace(
         "        pixels = expandIndices(texture.source.data(), texture.realWidth, "
         "texture.realHeight,",
         "        pixels = expandIndicesSwapped(texture.source.data(), texture.realWidth, "
         "texture.realHeight,", 1),
     "every-texel-call-hands-the-helper-the-source-order"),
    ("a third filler calls the helper with a forced-transparent alpha (review-8 F2)",
     lambda s: s.replace(
         "// WHAT IS LEFT, when a texture does not fit.",
         "static uint8_t* expandIndicesClear(const uint8_t* indices, int32_t width,\n"
         "                                   int32_t height, const uint8_t* paletteRgba,\n"
         "                                   int32_t paddedWidth, int32_t paddedHeight) {\n"
         "    const size_t texels = (size_t)paddedWidth * (size_t)paddedHeight;\n"
         "    uint8_t* pixels = static_cast<uint8_t*>(malloc(texels * 4));\n"
         "    if (!pixels) {\n        return nullptr;\n    }\n"
         "    memset(pixels, 0, texels * 4);\n"
         "    for (int32_t y = 0; y < height; y++) {\n"
         "        const uint8_t* source = indices + (size_t)y * (size_t)width;\n"
         "        for (int32_t x = 0; x < width; x++) {\n"
         "            const uint8_t index = source[x];\n"
         "            uint8_t* pixel = pixels + mdk::pica::textureTexelIndex((uint32_t)x, (uint32_t)y, (uint32_t)paddedWidth) * 4u;\n"
         "            mdk::pica::texelBytes(pixel, paletteRgba[index * 4 + 0],\n"
         "                                  paletteRgba[index * 4 + 1],\n"
         "                                  paletteRgba[index * 4 + 2], 0u);\n"
         "        }\n    }\n    return pixels;\n}\n\n"
         "// WHAT IS LEFT, when a texture does not fit.", 1).replace(
         "        pixels = expandIndices(texture.source.data(), texture.realWidth, "
         "texture.realHeight,",
         "        pixels = expandIndicesClear(texture.source.data(), texture.realWidth, "
         "texture.realHeight,", 1),
     "every-texel-call-hands-the-pica-its-path-s-alpha"),
    # Review-8 F3: a `}` inside a comment used to close the resolved body early, so
    # everything after the comment was never adjudicated. This filler calls the helper
    # CORRECTLY before the comment -- so the order and alpha clauses are satisfied --
    # and writes the palette's own order after it. With a brace-naive span the whole
    # file passes; with a blanked span the writes are inside the body and the filler
    # clause refuses them.
    ("a third filler hides its direct writes behind a comment containing a brace "
     "(review-8 F3)",
     lambda s: s.replace(
         "// WHAT IS LEFT, when a texture does not fit.",
         "static uint8_t* expandIndicesBraced(const uint8_t* indices, int32_t width,\n"
         "                                    int32_t height, const uint8_t* paletteRgba,\n"
         "                                    int32_t paddedWidth, int32_t paddedHeight) {\n"
         "    const size_t texels = (size_t)paddedWidth * (size_t)paddedHeight;\n"
         "    uint8_t* pixels = static_cast<uint8_t*>(malloc(texels * 4));\n"
         "    if (!pixels) {\n        return nullptr;\n    }\n"
         "    memset(pixels, 0, texels * 4);\n"
         "    mdk::pica::texelBytes(pixels, paletteRgba[0], paletteRgba[1],\n"
         "                          paletteRgba[2], 255u);\n"
         "    // the oracle's shader discards index 0 }\n"
         "    for (int32_t y = 0; y < height; y++) {\n"
         "        const uint8_t* source = indices + (size_t)y * (size_t)width;\n"
         "        for (int32_t x = 0; x < width; x++) {\n"
         "            const uint8_t index = source[x];\n"
         "            uint8_t* pixel = pixels + mdk::pica::textureTexelIndex((uint32_t)x, (uint32_t)y, (uint32_t)paddedWidth) * 4u;\n"
         "            pixel[0] = paletteRgba[index * 4 + 0];\n"
         "            pixel[1] = paletteRgba[index * 4 + 1];\n"
         "            pixel[2] = paletteRgba[index * 4 + 2];\n"
         "            pixel[3] = 255;\n"
         "        }\n    }\n    return pixels;\n}\n\n"
         "// WHAT IS LEFT, when a texture does not fit.", 1).replace(
         "        pixels = expandIndices(texture.source.data(), texture.realWidth, "
         "texture.realHeight,",
         "        pixels = expandIndicesBraced(texture.source.data(), texture.realWidth, "
         "texture.realHeight,", 1),
     "every-texel-filler-rotates-through-the-helper"),
    # Review-9 F1: the batch-122 dereference rule was anchored at the start of the
    # FOLDED statement, so a write that merely had a guard in front of it was invisible
    # -- and folding makes the guard and the write ONE statement, so this is not an
    # exotic spelling. Three arms, one per guard spelling, because each was measured
    # green on its own: `if`, `else`, and a one-line `for` body.
    ("a guarded bare-dereference write restores the palette alpha (review-9 F1)",
     lambda s: s.replace(
         "                                  paletteRgba[index * 4 + 1], "
         "paletteRgba[index * 4 + 2], 255u);",
         "                                  paletteRgba[index * 4 + 1], "
         "paletteRgba[index * 4 + 2], 255u);\n"
         "            if (index != 0)\n"
         "                *(pixel + 3) =\n"
         "                    paletteRgba[index * 4 + 3];", 1),
     "index-texels-are-rotated-for-the-pica"),
    ("an else-branch bare-dereference write restores the palette alpha (review-9 F1)",
     lambda s: s.replace(
         "                                  paletteRgba[index * 4 + 1], "
         "paletteRgba[index * 4 + 2], 255u);",
         "                                  paletteRgba[index * 4 + 1], "
         "paletteRgba[index * 4 + 2], 255u);\n"
         "            if (index == 255)\n"
         "                pixel = out;\n"
         "            else\n"
         "                *(pixel + 3) = paletteRgba[index * 4 + 3];", 1),
     "index-texels-are-rotated-for-the-pica"),
    ("a one-line for-body bare-dereference write restores the palette alpha (review-9 F1)",
     lambda s: s.replace(
         "                                  paletteRgba[index * 4 + 1], "
         "paletteRgba[index * 4 + 2], 255u);",
         "                                  paletteRgba[index * 4 + 1], "
         "paletteRgba[index * 4 + 2], 255u);\n"
         "            for (int32_t q = 3; q < 4; q++)\n"
         "                *(pixel + q) = paletteRgba[index * 4 + q];", 1),
     "index-texels-are-rotated-for-the-pica"),
    # Review-9 F2: the path was told apart by the literal word `palette` in the colour
    # arguments, so a one-line alias made a palette-fed call look source-fed, where a
    # forced alpha was allowed. This is the shipped defect -- the palette's own alpha in
    # the PICA's red slot -- with the helper intact and the word `palette` gone.
    ("a third filler reaches the palette through an alias and hands over its alpha "
     "(review-9 F2)",
     lambda s: s.replace(
         "// WHAT IS LEFT, when a texture does not fit.",
         "static uint8_t* expandIndicesAliased(const uint8_t* indices, int32_t width,\n"
         "                                     int32_t height, const uint8_t* paletteRgba,\n"
         "                                     int32_t paddedWidth, int32_t paddedHeight) {\n"
         "    const size_t texels = (size_t)paddedWidth * (size_t)paddedHeight;\n"
         "    uint8_t* pixels = static_cast<uint8_t*>(malloc(texels * 4));\n"
         "    if (!pixels) {\n        return nullptr;\n    }\n"
         "    memset(pixels, 0, texels * 4);\n"
         "    const uint8_t* colours = paletteRgba;\n"
         "    for (int32_t y = 0; y < height; y++) {\n"
         "        const uint8_t* source = indices + (size_t)y * (size_t)width;\n"
         "        for (int32_t x = 0; x < width; x++) {\n"
         "            const uint8_t index = source[x];\n"
         "            uint8_t* pixel = pixels + mdk::pica::textureTexelIndex((uint32_t)x, (uint32_t)y, (uint32_t)paddedWidth) * 4u;\n"
         "            mdk::pica::texelBytes(pixel, colours[index * 4 + 0],\n"
         "                                  colours[index * 4 + 1],\n"
         "                                  colours[index * 4 + 2], colours[index * 4 + 3]);\n"
         "        }\n    }\n    return pixels;\n}\n\n"
         "// WHAT IS LEFT, when a texture does not fit.", 1).replace(
         "        pixels = expandIndices(texture.source.data(), texture.realWidth, "
         "texture.realHeight,",
         "        pixels = expandIndicesAliased(texture.source.data(), texture.realWidth, "
         "texture.realHeight,", 1),
     "every-texel-call-hands-the-pica-its-path-s-alpha"),
    # Review-9 F3: a source-fed call was allowed to force an opaque alpha, so a filler
    # on the RGBA seam could delete every transparent pixel in the game while the named
    # `expandRgba` stayed correct and unused. The stride now decides: a 4-byte source
    # must hand over its own fourth byte.
    ("a third filler on the RGBA seam forces an opaque alpha (review-9 F3)",
     lambda s: s.replace(
         "// WHAT IS LEFT, when a texture does not fit.",
         "static uint8_t* expandRgbaOpaque(const uint8_t* source, int32_t width,\n"
         "                                 int32_t height, int32_t paddedWidth,\n"
         "                                 int32_t paddedHeight) {\n"
         "    const size_t texels = (size_t)paddedWidth * (size_t)paddedHeight;\n"
         "    uint8_t* pixels = static_cast<uint8_t*>(malloc(texels * 4));\n"
         "    if (!pixels) {\n        return nullptr;\n    }\n"
         "    memset(pixels, 0, texels * 4);\n"
         "    for (int32_t y = 0; y < height; y++) {\n"
         "        const uint8_t* row = source + (size_t)y * (size_t)width * 4;\n"
         "        for (int32_t x = 0; x < width; x++) {\n"
         "            mdk::pica::texelBytes(pixels + mdk::pica::textureTexelIndex((uint32_t)x, (uint32_t)y, (uint32_t)paddedWidth) * 4u, row[(size_t)x * 4 + 0],\n"
         "                                  row[(size_t)x * 4 + 1], row[(size_t)x * 4 + 2],\n"
         "                                  255u);\n"
         "        }\n    }\n    return pixels;\n}\n\n"
         "// WHAT IS LEFT, when a texture does not fit.", 1).replace(
         "        pixels = expandRgba(texture.source.data(), texture.realWidth, "
         "texture.realHeight,",
         "        pixels = expandRgbaOpaque(texture.source.data(), texture.realWidth, "
         "texture.realHeight,", 1),
     "every-texel-call-hands-the-pica-its-path-s-alpha"),
    # Review-9 F8: helper resolution read the RAW text, so a commented-out
    # `using mdk::pica::texelBytes;` enabled unqualified matching for the whole file and
    # a local function of its own called `texelBytes` counted as the helper. This filler
    # rotates nothing: its local stand-in writes the palette's own order.
    ("a filler rotates through a local function named texelBytes behind a commented-out "
     "using-declaration (review-9 F8)",
     lambda s: s.replace(
         "// WHAT IS LEFT, when a texture does not fit.",
         "// using mdk::pica::texelBytes;\n"
         "static void texelBytes(uint8_t* dst, const uint8_t* paletteRgba,\n"
         "                       uint32_t index) {\n"
         "    dst[0] = paletteRgba[index * 4 + 0];\n"
         "    dst[1] = paletteRgba[index * 4 + 1];\n"
         "    dst[2] = paletteRgba[index * 4 + 2];\n"
         "    dst[3] = 255;\n"
         "}\n\n"
         "static uint8_t* expandIndicesLocal(const uint8_t* indices, int32_t width,\n"
         "                                   int32_t height, const uint8_t* paletteRgba,\n"
         "                                   int32_t paddedWidth, int32_t paddedHeight) {\n"
         "    const size_t texels = (size_t)paddedWidth * (size_t)paddedHeight;\n"
         "    uint8_t* pixels = static_cast<uint8_t*>(malloc(texels * 4));\n"
         "    if (!pixels) {\n        return nullptr;\n    }\n"
         "    memset(pixels, 0, texels * 4);\n"
         "    for (int32_t y = 0; y < height; y++) {\n"
         "        const uint8_t* source = indices + (size_t)y * (size_t)width;\n"
         "        for (int32_t x = 0; x < width; x++) {\n"
         "            const uint8_t index = source[x];\n"
         "            uint8_t* pixel = pixels + mdk::pica::textureTexelIndex((uint32_t)x, (uint32_t)y, (uint32_t)paddedWidth) * 4u;\n"
         "            texelBytes(pixel, paletteRgba, index);\n"
         "        }\n    }\n    return pixels;\n}\n\n"
         "// WHAT IS LEFT, when a texture does not fit.", 1).replace(
         "        pixels = expandIndices(texture.source.data(), texture.realWidth, "
         "texture.realHeight,",
         "        pixels = expandIndicesLocal(texture.source.data(), texture.realWidth, "
         "texture.realHeight,", 1),
     "every-texel-filler-rotates-through-the-helper"),
    # Review-10 F1: review-9 F2's defect, moved one level up. The path resolution
    # dead-ended at a PARAMETER, so a filler whose palette parameter is renamed
    # (`const uint8_t* colours`) — no `palette` substring anywhere, no binding to
    # follow — was adjudicated source-fed, where handing over `colours[i*4+3]` is
    # exactly what the shipped defect does. The parameter now resolves at the call
    # site in this file, which names `texture.palette.data()`.
    ("a third filler renames its palette parameter and hands over its alpha "
     "(review-10 F1)",
     lambda s: s.replace(
         "// WHAT IS LEFT, when a texture does not fit.",
         "static uint8_t* expandIndicesParam(const uint8_t* indices, int32_t width,\n"
         "                                   int32_t height, const uint8_t* colours,\n"
         "                                   int32_t paddedWidth, int32_t paddedHeight) {\n"
         "    const size_t texels = (size_t)paddedWidth * (size_t)paddedHeight;\n"
         "    uint8_t* pixels = static_cast<uint8_t*>(malloc(texels * 4));\n"
         "    if (!pixels) {\n        return nullptr;\n    }\n"
         "    memset(pixels, 0, texels * 4);\n"
         "    for (int32_t y = 0; y < height; y++) {\n"
         "        const uint8_t* source = indices + (size_t)y * (size_t)width;\n"
         "        for (int32_t x = 0; x < width; x++) {\n"
         "            const uint8_t index = source[x];\n"
         "            uint8_t* pixel = pixels + mdk::pica::textureTexelIndex((uint32_t)x, (uint32_t)y, (uint32_t)paddedWidth) * 4u;\n"
         "            mdk::pica::texelBytes(pixel, colours[index * 4 + 0],\n"
         "                                  colours[index * 4 + 1],\n"
         "                                  colours[index * 4 + 2],\n"
         "                                  colours[index * 4 + 3]);\n"
         "        }\n    }\n    return pixels;\n}\n\n"
         "// WHAT IS LEFT, when a texture does not fit.", 1).replace(
         "        pixels = expandIndices(texture.source.data(), texture.realWidth, "
         "texture.realHeight,",
         "        pixels = expandIndicesParam(texture.source.data(), texture.realWidth, "
         "texture.realHeight,", 1),
     "every-texel-call-hands-the-pica-its-path-s-alpha"),
    # Review-10 F1's second spelling: the hoisted byte locals the batch-124 probe
    # made legal, reached through a renamed parameter. Binding-follow and call-site
    # resolution must COMPOSE — a byte local of a parameter of a palette call is
    # still the palette's byte.
    ("a third filler hoists bytes through a renamed palette parameter and hands over "
     "its alpha (review-10 F1: the two resolutions must compose)",
     lambda s: s.replace(
         "// WHAT IS LEFT, when a texture does not fit.",
         "static uint8_t* expandIndicesHoist(const uint8_t* indices, int32_t width,\n"
         "                                   int32_t height, const uint8_t* colours,\n"
         "                                   int32_t paddedWidth, int32_t paddedHeight) {\n"
         "    const size_t texels = (size_t)paddedWidth * (size_t)paddedHeight;\n"
         "    uint8_t* pixels = static_cast<uint8_t*>(malloc(texels * 4));\n"
         "    if (!pixels) {\n        return nullptr;\n    }\n"
         "    memset(pixels, 0, texels * 4);\n"
         "    for (int32_t y = 0; y < height; y++) {\n"
         "        const uint8_t* source = indices + (size_t)y * (size_t)width;\n"
         "        for (int32_t x = 0; x < width; x++) {\n"
         "            const uint8_t index = source[x];\n"
         "            uint8_t* pixel = pixels + mdk::pica::textureTexelIndex((uint32_t)x, (uint32_t)y, (uint32_t)paddedWidth) * 4u;\n"
         "            const uint8_t r = colours[index * 4 + 0];\n"
         "            const uint8_t g = colours[index * 4 + 1];\n"
         "            const uint8_t b = colours[index * 4 + 2];\n"
         "            const uint8_t a = colours[index * 4 + 3];\n"
         "            mdk::pica::texelBytes(pixel, r, g, b, a);\n"
         "        }\n    }\n    return pixels;\n}\n\n"
         "// WHAT IS LEFT, when a texture does not fit.", 1).replace(
         "        pixels = expandIndices(texture.source.data(), texture.realWidth, "
         "texture.realHeight,",
         "        pixels = expandIndicesHoist(texture.source.data(), texture.realWidth, "
         "texture.realHeight,", 1),
     "every-texel-call-hands-the-pica-its-path-s-alpha"),
    # Review-10 F3: memcpy and memmove were bulk texel writes and memset was not —
    # and memset is the intrinsic that can set a byte to a constant. One line after
    # a CORRECT helper call, `memset(pixel + 3, 0xFF, 1);` re-pins the alpha to 255,
    # byte for byte the shipped symptom.
    ("a memset re-pins the alpha to 255 after a correct helper call (review-10 F3)",
     lambda s: s.replace(
         "                                  paletteRgba[index * 4 + 1], "
         "paletteRgba[index * 4 + 2], 255u);",
         "                                  paletteRgba[index * 4 + 1], "
         "paletteRgba[index * 4 + 2], 255u);\n"
         "            memset(pixel + 3, 0xFF, 1);", 1),
     "index-texels-are-rotated-for-the-pica"),
    # Review-11 F1: batch 125 adjudicated the memset DESTINATION by whether its text
    # contained a `+`, which is neither "is this an offset" nor "is this the padding".
    # Each of these three is one inserted line after a CORRECT helper call, each erases
    # texel bytes the helper just wrote, and each printed `checks=41 failed=0` on the
    # committed gate. They are arms now because the rule is "a bulk fill is the padding
    # fill only if it fills the whole buffer with the transparent texel".
    ("a zero memset aimed at an ADDRESS-OF SUBSCRIPT erases a texel's alpha after a "
     "correct helper call (review-11 F1)",
     lambda s: s.replace(
         "                                  paletteRgba[index * 4 + 1], "
         "paletteRgba[index * 4 + 2], 255u);",
         "                                  paletteRgba[index * 4 + 1], "
         "paletteRgba[index * 4 + 2], 255u);\n"
         "            memset(&pixel[3], 0, 1);", 1),
     "index-texels-are-rotated-for-the-pica"),
    ("a zero memset aimed at a SUBSCRIPTED row offset erases a whole texel after a "
     "correct helper call (review-11 F1)",
     lambda s: s.replace(
         "                                  paletteRgba[index * 4 + 1], "
         "paletteRgba[index * 4 + 2], 255u);",
         "                                  paletteRgba[index * 4 + 1], "
         "paletteRgba[index * 4 + 2], 255u);\n"
         "            memset(&out[(size_t)x * 4], 0, 4);", 1),
     "index-texels-are-rotated-for-the-pica"),
    ("a zero memset of a whole texel hides it after a correct helper call (review-11 "
     "F1: the destination is a bare name, so `+` could not see it)",
     lambda s: s.replace(
         "                                  paletteRgba[index * 4 + 1], "
         "paletteRgba[index * 4 + 2], 255u);",
         "                                  paletteRgba[index * 4 + 1], "
         "paletteRgba[index * 4 + 2], 255u);\n"
         "            memset(pixel, 0, 4);", 1),
     "index-texels-are-rotated-for-the-pica"),
    # Review-11 F3: the intrinsic list was a SPELLING list. `\bmemset` cannot match
    # `__builtin_memset` (the character before it is a word character), and the C++
    # algorithms that set or copy bytes were in no list at all -- the shipped defect,
    # three spellings over, green at 41/41.
    ("__builtin_memset re-pins the alpha after a correct helper call (review-11 F3: "
     "the same intrinsic, one prefix away)",
     lambda s: s.replace(
         "                                  paletteRgba[index * 4 + 1], "
         "paletteRgba[index * 4 + 2], 255u);",
         "                                  paletteRgba[index * 4 + 1], "
         "paletteRgba[index * 4 + 2], 255u);\n"
         "            __builtin_memset(pixel + 3, 0xFF, 1);", 1),
     "index-texels-are-rotated-for-the-pica"),
    ("std::fill re-pins the alpha after a correct helper call (review-11 F3: the C++ "
     "spelling of the same constant fill)",
     lambda s: s.replace("#include <utility>", "#include <algorithm>\n#include <utility>", 1)
         .replace(
         "                                  paletteRgba[index * 4 + 1], "
         "paletteRgba[index * 4 + 2], 255u);",
         "                                  paletteRgba[index * 4 + 1], "
         "paletteRgba[index * 4 + 2], 255u);\n"
         "            std::fill(pixel, pixel + 4, 0xFFu);", 1),
     "index-texels-are-rotated-for-the-pica"),
    ("std::copy copies the palette's own order into the texel after a correct helper "
     "call (review-11 F3: the row memcpy in the C++ spelling)",
     lambda s: s.replace("#include <utility>", "#include <algorithm>\n#include <utility>", 1)
         .replace(
         "                                  paletteRgba[index * 4 + 1], "
         "paletteRgba[index * 4 + 2], 255u);",
         "                                  paletteRgba[index * 4 + 1], "
         "paletteRgba[index * 4 + 2], 255u);\n"
         "            std::copy(&paletteRgba[index * 4], &paletteRgba[index * 4] + 4, pixel);", 1),
     "index-texels-are-rotated-for-the-pica"),
    # Review-11 F4: the batch-125 tail `(?<![=!<>])=(?!=)` refused an `=` preceded by
    # `<` or `>` -- right for `<=` and `>=`, and also right for `>>=`, which is a WRITE.
    # The pre-batch gate caught this one; the batch that fixed review-10 F2 narrowed it
    # away. `|=` is the control: a compound assign was never missed.
    ("a shift-assign through a dereference writes a texel byte (review-11 F4: the "
     "batch-125 tail read `>>=` as a comparison)",
     lambda s: s.replace(
         "                                  paletteRgba[index * 4 + 1], "
         "paletteRgba[index * 4 + 2], 255u);",
         "                                  paletteRgba[index * 4 + 1], "
         "paletteRgba[index * 4 + 2], 255u);\n"
         "            *(pixel + 3) >>= 1;", 1),
     "index-texels-are-rotated-for-the-pica"),
    # Review-12 F1: batch 126 pinned the padding fill's DESTINATION and LENGTH and never
    # looked at where it SITS, so the fill moved to the line before `return pixels;` --
    # after every row -- erased the whole texture, and printed `checks=41 failed=0`. It
    # is the case review-11 F1 had named ("any length and any position, including after
    # the draw") and the one batch 126's own closing sentence asserted impossible.
    ("the padding fill moved after the expansion loop and zeroes every texel the helper "
     "wrote (review-12 F1: the rule read the destination and the length, not the place)",
     # (Re-anchored 2026-11-17: since prepareForFiltering the loop is followed by
     # its comment, not by `return`; the wipe still lands right after the loop.)
     lambda s: s.replace("        }\n    }\n    // Ready for Enhanced Mode filtering",
                         "        }\n    }\n    memset(pixels, 0, texels * 4);\n"
                         "    // Ready for Enhanced Mode filtering", 1),
     "index-texels-are-rotated-for-the-pica"),
    # Review-12 open question 2, answered as a defect rather than a limit: the same wipe
    # with the fill left where it was and the rows moved around it. Source order cannot
    # see this one -- the fill still precedes the call it erases -- so the rule reads the
    # loop nesting, and this arm is what proves the nesting is not decoration.
    ("the padding fill sits inside the row loop, so every iteration wipes the rows "
     "before it (review-12 F1: the same defect one brace deeper)",
     lambda s: s.replace(
         "            mdk::pica::texelBytes(pixel, paletteRgba[index * 4 + 0],",
         "            memset(pixels, 0, texels * 4);\n"
         "            mdk::pica::texelBytes(pixel, paletteRgba[index * 4 + 0],", 1),
     "index-texels-are-rotated-for-the-pica"),
    # Review-12 F2: the literal alternative `'(?:\\0|x0+|0)'` accepted NONZERO fills --
    # `'0'` is the character 0x30 and `'x0'` is a multi-character constant -- and both
    # `fullmatch`ed as "the transparent texel", so a padding row shipped alpha 48 with
    # the gate green. Batch 125 refused both; batch 126's literal fix let them through.
    ("the padding fill is the CHARACTER '0', not the value 0 (review-12 F2: 0x30 is "
     "alpha 48, so the letterbox rows ship opaque garbage)",
     lambda s: s.replace("memset(pixels, 0, texels * 4);",
                         "memset(pixels, '0', texels * 4);", 1),
     "index-texels-are-rotated-for-the-pica"),
    ("the padding fill is the multi-character constant 'x0' (review-12 F2: the "
     "alternative that claimed to spell a hex escape could only ever spell this)",
     lambda s: s.replace("memset(pixels, 0, texels * 4);",
                         "memset(pixels, 'x0', texels * 4);", 1),
     "index-texels-are-rotated-for-the-pica"),
    # Review-12 F3: review-11 F4 was fixed in DEREF_WRITE_TAIL only. The two subscript
    # rules kept the operator slot `[+\-*/%&^|]?`, which cannot reach `<<=` or `>>=`, so
    # the shift-assign hole survived on the other side of the `*` while the batch's own
    # docstring presented that slot as proof there was no hole.
    ("a shift-assign through a SUBSCRIPT halves a texel alpha (review-12 F3: the "
     "review-11 F4 hole one side over)",
     lambda s: s.replace(
         "                                  paletteRgba[index * 4 + 1], "
         "paletteRgba[index * 4 + 2], 255u);",
         "                                  paletteRgba[index * 4 + 1], "
         "paletteRgba[index * 4 + 2], 255u);\n"
         "            out[(size_t)x * 4 + 3] >>= 1;", 1),
     "index-texels-are-rotated-for-the-pica"),
    ("a shift-assign through a CAST subscript halves a texel alpha (review-12 F3: the "
     "batch-122 spelling of the same write)",
     lambda s: s.replace(
         "                                  paletteRgba[index * 4 + 1], "
         "paletteRgba[index * 4 + 2], 255u);",
         "                                  paletteRgba[index * 4 + 1], "
         "paletteRgba[index * 4 + 2], 255u);\n"
         "            ((uint8_t*)pixel)[3] >>= 1;", 1),
     "index-texels-are-rotated-for-the-pica"),
    # Review-12 F6: the algorithms required a literal `std::`, so the operation was
    # matched one spelling short of the batch's own claim -- and review-8 F5 already
    # sanctions the unqualified spelling for the helper, so the same shape here is not
    # exotic. The using-declaration is what puts the bare name in scope.
    ("an unqualified fill() reached through `using std::fill;` re-pins the alpha after a "
     "correct helper call (review-12 F6: the operation, one spelling further)",
     lambda s: s.replace("#include <utility>",
                         "#include <algorithm>\n#include <utility>\nusing std::fill;", 1)
         .replace(
         "                                  paletteRgba[index * 4 + 1], "
         "paletteRgba[index * 4 + 2], 255u);",
         "                                  paletteRgba[index * 4 + 1], "
         "paletteRgba[index * 4 + 2], 255u);\n"
         "            fill(pixel, pixel + 4, 0xFFu);", 1),
     "index-texels-are-rotated-for-the-pica"),
    # Review-13 F1: batch 126 pinned the padding fill's destination and length, batch 127
    # pinned where it SITS, and neither pinned HOW MANY. `bulk_op` adjudicated the first
    # bulk setter in a statement and stopped, and a C++ statement ends at a semicolon and
    # not at a newline, so the comma operator puts a second setter in the same statement
    # behind the legal one -- four opaque bytes at the base of the texture, green at
    # 41/41 on the committed gate AND on the pre-batch one, so this is a hole batch 125
    # left open rather than one this batch cut. The rule is now EVERY setter in the
    # statement, and these arms are what say so.
    ("a comma-joined second memset paints the base of the texture behind the legal "
     "padding fill (review-13 F1: the scan read one bulk setter per statement)",
     lambda s: s.replace("    memset(pixels, 0, texels * 4);",
                         "    memset(pixels, 0, texels * 4), memset(pixels + 4, 0xFF, 4);",
                         1),
     "index-texels-are-rotated-for-the-pica"),
    ("the second setter in the statement is the C++ algorithm rather than the intrinsic "
     "(review-13 F1: multiplicity is not spelling-specific)",
     lambda s: s.replace("#include <utility>",
                         "#include <algorithm>\n#include <utility>", 1)
         .replace("    memset(pixels, 0, texels * 4);",
                  "    memset(pixels, 0, texels * 4), "
                  "std::fill(pixels + 4, pixels + 8, 0xFFu);", 1),
     "index-texels-are-rotated-for-the-pica"),
    ("three setters in one statement with the legal one first (review-13 F1: the rule is "
     "every match, not the first two)",
     lambda s: s.replace("    memset(pixels, 0, texels * 4);",
                         "    memset(pixels, 0, texels * 4), memset(pixels + 4, 0xFF, 4), "
                         "memset(pixels + 8, 0xFF, 4);", 1),
     "index-texels-are-rotated-for-the-pica"),
    ("a memcpy is the second setter in the padding fill's statement (review-13 F1: the "
     "pre-batch gate refused THIS one only because the copy rule happened to run first, "
     "which was an accident of order and not a rule)",
     lambda s: s.replace("    memset(pixels, 0, texels * 4);",
                         "    memset(pixels, 0, texels * 4), "
                         "memcpy(pixels + 4, paletteRgba, 4);", 1),
     "index-texels-are-rotated-for-the-pica"),
    # Review-13 F2's edge: the zero-literal class now carries every base and every integer
    # suffix, so the refusals have to be exactly the NONZERO values. `07` is octal 7 and
    # `0b1` is one; both were refused before the widening and stay refused, and these two
    # arms are what stop the widening rotting open.
    ("the padding fill is the OCTAL 07 (review-13 F2: widening the suffix class must not "
     "sweep in a nonzero octal)",
     lambda s: s.replace("memset(pixels, 0, texels * 4);",
                         "memset(pixels, 07, texels * 4);", 1),
     "index-texels-are-rotated-for-the-pica"),
    ("the padding fill is the BINARY 0b1 (review-13 F2: the same edge on the base the "
     "widening added)",
     lambda s: s.replace("memset(pixels, 0, texels * 4);",
                         "memset(pixels, 0b1, texels * 4);", 1),
     "index-texels-are-rotated-for-the-pica"),
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
    # Review-6 F1's two green halves. The texel checks refuse a WRITE that bypasses
    # the helper; a hoisted READ and an equivalent spelling of the forced alpha are
    # both shapes the port may write, and a check that reddened either would be routed
    # around the same way review-10 F6 and review-16 R2 predicted.
    ("the rgba expansion reads its source through a hoisted row pointer (review-6 F1: "
     "a read is not a texel write)",
     lambda s: s.replace(
         "            const size_t slot =\n"
         "                mdk::pica::textureTexelIndex((uint32_t)x, (uint32_t)y, (uint32_t)paddedWidth);\n"
         "            mdk::pica::texelBytes(pixels + slot * 4u, row[(size_t)x * 4 + 0],\n"
         "                                  row[(size_t)x * 4 + 1], row[(size_t)x * 4 + 2],\n"
         "                                  row[(size_t)x * 4 + 3]);",
         "            const size_t slot =\n"
         "                mdk::pica::textureTexelIndex((uint32_t)x, (uint32_t)y, (uint32_t)paddedWidth);\n"
         "            const uint8_t* src = row + (size_t)x * 4;\n"
         "            mdk::pica::texelBytes(pixels + slot * 4u, src[0], src[1], src[2],\n"
         "                                  src[3]);", 1)),
    ("the index expansion spells the forced alpha 0xFFu rather than 255u (review-6 F1)",
     lambda s: s.replace("paletteRgba[index * 4 + 1], paletteRgba[index * 4 + 2], 255u);",
                         "paletteRgba[index * 4 + 1], paletteRgba[index * 4 + 2], 0xFFu);",
                         1)),
    # The sink rule reads DATA FLOW, not a spelling. Handing the expansion inline -- no
    # intermediate local at all -- is the same program, and a check that refused it would
    # be routed around the first time someone tidied the temporary away.
    ("the sink is handed the expansion inline rather than through a local (review-7 F1: "
    "the pin is on the producer, not on the temporary)",
     lambda s: s.replace("    C3D_TexLoadImage(&texture.tex, pixels, GPU_TEXFACE_2D, 0);",
                         "    C3D_TexLoadImage(&texture.tex,\n"
                         "                       texture.indexed\n"
                         "                           ? expandIndices(texture.source.data(), "
                         "texture.realWidth, texture.realHeight,\n"
                         "                             texture.palette.data(), "
                         "texture.paddedWidth, texture.paddedHeight)\n"
                         "                           : expandRgba(texture.source.data(), "
                         "texture.realWidth, texture.realHeight,\n"
                         "                                texture.paddedWidth, "
                         "texture.paddedHeight),\n"
                         "                       GPU_TEXFACE_2D, 0);", 1)),
    # The other half of the same rule: a third expansion that rotates through the
    # helper and is wired into the sink is a shape the port may write -- an RGBA4 or a
    # HUD path would be exactly this -- and a gate that reddened it would be routed
    # around, which is the failure review-16 R2 named.
    ("a third expansion that rotates through the helper and takes the sink is accepted "
     "(review-7 F1: the pin is on the helper and the sink, not on two names)",
     lambda s: s.replace(
         "// WHAT IS LEFT, when a texture does not fit.",
         "static uint8_t* expandIndicesFast(const uint8_t* indices, int32_t width,\n"
         "                                  int32_t height, const uint8_t* paletteRgba,\n"
         "                                  int32_t paddedWidth, int32_t paddedHeight) {\n"
         "    const size_t texels = (size_t)paddedWidth * (size_t)paddedHeight;\n"
         "    uint8_t* pixels = static_cast<uint8_t*>(malloc(texels * 4));\n"
         "    if (!pixels) {\n        return nullptr;\n    }\n"
         "    memset(pixels, 0, texels * 4);\n"
         "    for (int32_t y = 0; y < height; y++) {\n"
         "        const uint8_t* source = indices + (size_t)y * (size_t)width;\n"
         "        for (int32_t x = 0; x < width; x++) {\n"
         "            const uint8_t index = source[x];\n"
         "            uint8_t* pixel = pixels + mdk::pica::textureTexelIndex((uint32_t)x, (uint32_t)y, (uint32_t)paddedWidth) * 4u;\n"
         "            mdk::pica::texelBytes(pixel, paletteRgba[index * 4 + 0],\n"
         "                                  paletteRgba[index * 4 + 1], "
         "paletteRgba[index * 4 + 2], 255u);\n"
         "        }\n"
         "    }\n"
         "    return pixels;\n}\n\n"
         "// WHAT IS LEFT, when a texture does not fit.", 1).replace(
         "        pixels = expandIndices(texture.source.data(), texture.realWidth, "
         "texture.realHeight,",
         "        pixels = expandIndicesFast(texture.source.data(), texture.realWidth, "
         "texture.realHeight,", 1)),
    # Review-8 F3's green half: a brace in PROSE must not truncate the resolved body.
    # With a brace-naive counter the body ended at the comment and the helper call fell
    # outside it, which reddened a correct file -- the same defect as the arm above, on
    # the side that must stay green.
    ("a brace inside a comment does not truncate the resolved body (review-8 F3)",
     lambda s: s.replace(
         "        for (int32_t x = 0; x < width; x++) {\n"
         "            const size_t slot =\n",
         "        // the PICA reads [A, B, G, R], so there is no } shortcut here\n"
         "        for (int32_t x = 0; x < width; x++) {\n"
         "            const size_t slot =\n", 1)),
    # Review-8 F4's green half: a commented-out sink is prose about a sink, not a sink.
    # The batch-122 skip removed only a line whose first characters were `//`, so a
    # block comment beside the real call reddened a correct file.
    ("a commented-out C3D_TexLoadImage beside the real one is not a second sink "
     "(review-8 F4)",
     lambda s: s.replace(
         "    C3D_TexLoadImage(&texture.tex, pixels, GPU_TEXFACE_2D, 0);",
         "    /* the first upload site, kept while the eviction path settled:\n"
         "       C3D_TexLoadImage(&texture.tex, texture.source.data(),\n"
         "                       GPU_TEXFACE_2D, 0); */\n"
         "    C3D_TexLoadImage(&texture.tex, pixels, GPU_TEXFACE_2D, 0);", 1)),
    # Review-8 F5's two widened refusals, pinned as shapes the port may write.
    ("the helper is reached through a using-declaration rather than qualified at every "
     "call site (review-8 F5: the qualification is not the invariant, the helper is)",
     lambda s: s.replace("static uint8_t* expandIndices(const uint8_t* indices,",
                         "using mdk::pica::texelBytes;\n\n"
                         "static uint8_t* expandIndices(const uint8_t* indices,", 1)
              .replace("mdk::pica::texelBytes(", "texelBytes(")),
    ("the expanded buffer is kept on the texture record and the sink reads it there "
     "(review-8 F5: a member assigned from a helper writer is the same data flow)",
     lambda s: s.replace("struct C3dTexture {\n    C3D_Tex tex;",
                         "struct C3dTexture {\n    C3D_Tex tex;\n"
                         "    uint8_t* expanded = nullptr;", 1)
              .replace(
                  "        pixels = expandIndices(texture.source.data(), "
                  "texture.realWidth, texture.realHeight,\n"
                  "                               texture.palette.data(), "
                  "texture.paddedWidth, texture.paddedHeight,\n"
                  "                               levels);",
                  "        texture.expanded = expandIndices(texture.source.data(),\n"
                  "                               texture.realWidth, texture.realHeight,\n"
                  "                               texture.palette.data(),\n"
                  "                               texture.paddedWidth, texture.paddedHeight,\n"
                  "                               levels);\n"
                  "        pixels = texture.expanded;", 1)
              .replace("    C3D_TexLoadImage(&texture.tex, pixels, GPU_TEXFACE_2D, 0);",
                       "    C3D_TexLoadImage(&texture.tex, texture.expanded,\n"
                       "                       GPU_TEXFACE_2D, 0);", 1)),
    # Review-9 F4's green half: hoisting the four source bytes into named locals is the
    # same program, and the batch that widened the byte reader to four checks reddened
    # it. A refactor the port may write has to stay green, or the next builder routes
    # around the reader instead of using it.
    ("the rgba expansion hoists its four source bytes into named locals (review-9 F4: "
     "a byte is a byte when the body says which one)",
     lambda s: s.replace(
         "            const size_t slot =\n"
         "                mdk::pica::textureTexelIndex((uint32_t)x, (uint32_t)y, (uint32_t)paddedWidth);\n"
         "            mdk::pica::texelBytes(pixels + slot * 4u, row[(size_t)x * 4 + 0],\n"
         "                                  row[(size_t)x * 4 + 1], row[(size_t)x * 4 + 2],\n"
         "                                  row[(size_t)x * 4 + 3]);",
         "            const uint8_t r = row[(size_t)x * 4 + 0];\n"
         "            const uint8_t g = row[(size_t)x * 4 + 1];\n"
         "            const uint8_t b = row[(size_t)x * 4 + 2];\n"
         "            const uint8_t a = row[(size_t)x * 4 + 3];\n"
         "            const size_t slot =\n"
         "                mdk::pica::textureTexelIndex((uint32_t)x, (uint32_t)y, (uint32_t)paddedWidth);\n"
         "            mdk::pica::texelBytes(pixels + slot * 4u, r, g, b, a);", 1)),
    # The other half of review-9 F2: an alias is legal when the alpha rule still holds.
    # Refusing the alias spelling would be the same route-around review-16 R2 names, and
    # it is the half that keeps the new resolution from being a name ban.
    ("a third filler reaches the palette through an alias and still forces an opaque "
     "alpha (review-9 F2: the alias is a path, not a defect)",
     lambda s: s.replace(
         "// WHAT IS LEFT, when a texture does not fit.",
         "static uint8_t* expandIndicesAliased(const uint8_t* indices, int32_t width,\n"
         "                                     int32_t height, const uint8_t* paletteRgba,\n"
         "                                     int32_t paddedWidth, int32_t paddedHeight) {\n"
         "    const size_t texels = (size_t)paddedWidth * (size_t)paddedHeight;\n"
         "    uint8_t* pixels = static_cast<uint8_t*>(malloc(texels * 4));\n"
         "    if (!pixels) {\n        return nullptr;\n    }\n"
         "    memset(pixels, 0, texels * 4);\n"
         "    const uint8_t* colours = paletteRgba;\n"
         "    for (int32_t y = 0; y < height; y++) {\n"
         "        const uint8_t* source = indices + (size_t)y * (size_t)width;\n"
         "        for (int32_t x = 0; x < width; x++) {\n"
         "            const uint8_t index = source[x];\n"
         "            uint8_t* pixel = pixels + mdk::pica::textureTexelIndex((uint32_t)x, (uint32_t)y, (uint32_t)paddedWidth) * 4u;\n"
         "            mdk::pica::texelBytes(pixel, colours[index * 4 + 0],\n"
         "                                  colours[index * 4 + 1],\n"
         "                                  colours[index * 4 + 2], 255u);\n"
         "        }\n    }\n    return pixels;\n}\n\n"
         "// WHAT IS LEFT, when a texture does not fit.", 1).replace(
         "        pixels = expandIndices(texture.source.data(), texture.realWidth, "
         "texture.realHeight,",
         "        pixels = expandIndicesAliased(texture.source.data(), texture.realWidth, "
         "texture.realHeight,", 1)),
    # Review-9 F3's green half, and the discriminator it asked for: a 3-byte source has
    # no alpha byte to hand over, so forcing opaque is CORRECT there, and the stride in
    # the argument is what says so.
    ("a filler expands a 3-byte source and forces an opaque alpha (review-9 F3: a "
     "3-stride source has no fourth byte, so opaque is the only correct alpha)",
     lambda s: s.replace(
         "// WHAT IS LEFT, when a texture does not fit.",
         "static uint8_t* expandRgb(const uint8_t* source, int32_t width, int32_t height,\n"
         "                           int32_t paddedWidth, int32_t paddedHeight) {\n"
         "    const size_t texels = (size_t)paddedWidth * (size_t)paddedHeight;\n"
         "    uint8_t* pixels = static_cast<uint8_t*>(malloc(texels * 4));\n"
         "    if (!pixels) {\n        return nullptr;\n    }\n"
         "    memset(pixels, 0, texels * 4);\n"
         "    for (int32_t y = 0; y < height; y++) {\n"
         "        const uint8_t* rgb = source + (size_t)y * (size_t)width * 3;\n"
         "        for (int32_t x = 0; x < width; x++) {\n"
         "            mdk::pica::texelBytes(pixels + mdk::pica::textureTexelIndex((uint32_t)x, (uint32_t)y, (uint32_t)paddedWidth) * 4u, rgb[(size_t)x * 3 + 0],\n"
         "                                  rgb[(size_t)x * 3 + 1], rgb[(size_t)x * 3 + 2],\n"
         "                                  255u);\n"
         "        }\n    }\n    return pixels;\n}\n\n"
         "// WHAT IS LEFT, when a texture does not fit.", 1)),
    # Review-10 F1's green half: the parameter resolution is data flow, not a name
    # ban. The same renamed parameter, called with the palette, is correct code when
    # the palette path's own alpha rule is honoured — refusing this spelling would
    # be the route-around review-16 R2 names, and it is the half that keeps the new
    # resolution from becoming a second spelling of the word `palette`.
    ("a third filler renames its palette parameter and still forces an opaque alpha "
     "(review-10 F1: the rename is a path, not a defect)",
     lambda s: s.replace(
         "// WHAT IS LEFT, when a texture does not fit.",
         "static uint8_t* expandIndicesParam(const uint8_t* indices, int32_t width,\n"
         "                                   int32_t height, const uint8_t* colours,\n"
         "                                   int32_t paddedWidth, int32_t paddedHeight) {\n"
         "    const size_t texels = (size_t)paddedWidth * (size_t)paddedHeight;\n"
         "    uint8_t* pixels = static_cast<uint8_t*>(malloc(texels * 4));\n"
         "    if (!pixels) {\n        return nullptr;\n    }\n"
         "    memset(pixels, 0, texels * 4);\n"
         "    for (int32_t y = 0; y < height; y++) {\n"
         "        const uint8_t* source = indices + (size_t)y * (size_t)width;\n"
         "        for (int32_t x = 0; x < width; x++) {\n"
         "            const uint8_t index = source[x];\n"
         "            uint8_t* pixel = pixels + mdk::pica::textureTexelIndex((uint32_t)x, (uint32_t)y, (uint32_t)paddedWidth) * 4u;\n"
         "            mdk::pica::texelBytes(pixel, colours[index * 4 + 0],\n"
         "                                  colours[index * 4 + 1],\n"
         "                                  colours[index * 4 + 2], 255u);\n"
         "        }\n    }\n    return pixels;\n}\n\n"
         "// WHAT IS LEFT, when a texture does not fit.", 1).replace(
         "        pixels = expandIndices(texture.source.data(), texture.realWidth, "
         "texture.realHeight,",
         "        pixels = expandIndicesParam(texture.source.data(), texture.realWidth, "
         "texture.realHeight,", 1)),
    # Review-10 F2: the read spellings the batch-124 record claimed its table had
    # checked but did not commit. A comparison through a pointer is a READ; the old
    # `[^=;]*=` tail swallowed the operator of `!=`, `<=` and `>=` and reddened a dead
    # guard — review-16 R2's failure class, red before the fix and green after it.
    ("a filler guards with reads through a pointer using !=, <= and >= (review-10 F2: "
     "a comparison is not an assignment)",
     lambda s: s.replace(
         "            mdk::pica::texelBytes(pixels + slot * 4u,",
         "            const uint8_t* s = row + (size_t)x * 4;\n"
         "            if (*s != 0) { continue; }\n"
         "            if (*s <= 0) { }\n"
         "            if (*s >= 1) { }\n"
         "            mdk::pica::texelBytes(pixels + slot * 4u,", 1)),
    # Review-10 F3's green half: the zero-fill padding write stays legal whatever the
    # literal's spelling, because an all-zero texel is transparent under ANY byte
    # order — the exemption is the VALUE, not one way of writing it.
    ("the padding zero-fill is spelled 0u rather than 0 (review-10 F3: the zero-fill "
     "exemption is the value, not the literal)",
     lambda s: s.replace("    memset(pixels, 0, texels * 4);",
                         "    memset(pixels, 0u, texels * 4);", 1)),
    # Review-11 F2: batch 125 whitelisted `'\\0'` but `call_args_at` dropped the body
    # of every literal, so the fill arrived as a bare quote and the spelling the rule
    # set out to accept was unreachable -- a byte-identical padding fill, refused.
    # Red on the pre-batch gate (2 checks), green now.
    ("the padding zero-fill is spelled '\\0' (review-11 F2: the literal reaches the "
     "rule that whitelists it)",
     lambda s: s.replace("    memset(pixels, 0, texels * 4);",
                         "    memset(pixels, '\\0', texels * 4);", 1)),
    # Review-11 F3's green half: the bulk rule is about what the fill MEANS, so the
    # C++ spelling of the same whole-buffer transparent fill is as legal as memset.
    # If these redden, the rule has become a name ban again.
    ("the padding fill is spelled std::fill over the whole buffer (review-11 F3: the "
     "C++ algorithm is the same operation)",
     lambda s: s.replace("#include <utility>", "#include <algorithm>\n#include <utility>", 1)
         .replace(
             "    memset(pixels, 0, texels * 4);\n"
             "    for (int32_t y = 0; y < height; y++) {\n        const uint8_t* source",
             "    std::fill(pixels, pixels + texels * 4, 0);\n"
             "    for (int32_t y = 0; y < height; y++) {\n        const uint8_t* source", 1)),
    ("the padding fill is spelled std::fill_n over the whole buffer (review-11 F3: "
     "the count form of the same operation)",
     lambda s: s.replace("#include <utility>", "#include <algorithm>\n#include <utility>", 1)
         .replace(
             "    memset(pixels, 0, texels * 4);\n"
             "    for (int32_t y = 0; y < height; y++) {\n        const uint8_t* source",
             "    std::fill_n(pixels, texels * 4, 0);\n"
             "    for (int32_t y = 0; y < height; y++) {\n        const uint8_t* source", 1)),
    # Review-12 F4: the fill's length was compared as the author's TEXT, so the shipped
    # statement after a column wrap -- `texels*4` -- reddened a correct filler. That is
    # review-16 R2's class, and review-11 had already rated the identical one (`'\0'`)
    # Medium for the same reason.
    ("the padding fill's length is written without spaces (review-12 F4: a reflow is the "
     "same arithmetic, and the allocation side already read it that way)",
     lambda s: s.replace("memset(pixels, 0, texels * 4);",
                         "memset(pixels, 0, texels*4);", 1)),
    # Review-12 F5: the alternative that claimed to cover a hex escape could only ever
    # match the text `'x0...'`, so `'\x00'` -- byte for byte the shipped fill -- was
    # refused while the comment promised it was accepted.
    ("the padding fill is spelled '\\x00' (review-12 F5: the hex escape the previous "
     "alternative could not reach)",
     lambda s: s.replace("memset(pixels, 0, texels * 4);",
                         "memset(pixels, '\\x00', texels * 4);", 1)),
    # The green half of review-12 F6: the bare algorithm name after a using-declaration
    # is legal C++ and the padding fill may be spelled that way. A rule that closed F6 by
    # reddening every bare `fill(` would have traded a false negative for a false
    # positive, which review-16 R2 says gets routed around.
    ("the padding fill is the unqualified spelling of the algorithm after a "
     "using-declaration (review-12 F6: the legal half of the same spelling)",
     lambda s: s.replace("#include <utility>",
                         "#include <algorithm>\n#include <utility>\nusing std::fill_n;", 1)
         .replace(
             "    memset(pixels, 0, texels * 4);\n"
             "    for (int32_t y = 0; y < height; y++) {\n        const uint8_t* source",
             "    fill_n(pixels, texels * 4, 0);\n"
             "    for (int32_t y = 0; y < height; y++) {\n        const uint8_t* source", 1)),
    # Review-13 F1's green half. Two whole-buffer transparent fills of the same buffer in
    # one comma-joined statement wipe nothing the helper wrote, so the rule -- every
    # setter in the statement is the padding fill -- accepts it. Nobody would ship it; it
    # is here because a rule that closed F1 by reddening every multi-setter statement
    # would have traded a false negative for a false positive, which review-16 R2 says
    # gets routed around, and only a probe can tell the two rules apart.
    ("two whole-buffer padding fills comma-joined in one statement (review-13 F1: the "
     "legal half of multiplicity)",
     lambda s: s.replace("    memset(pixels, 0, texels * 4);",
                         "    memset(pixels, 0, texels * 4), "
                         "memset(pixels, 0, texels * 4);", 1)),
    # Review-13 F2's green half: a memset fill value is an `int` parameter, so the
    # suffix-flavoured and non-decimal zero literals are byte for byte the shipped fill.
    ("the padding fill is spelled 0L (review-13 F2: a long zero is still zero)",
     lambda s: s.replace("memset(pixels, 0, texels * 4);",
                         "memset(pixels, 0L, texels * 4);", 1)),
    ("the padding fill is spelled 0x0UL (review-13 F2: the suffix class the previous "
     "regex refused)",
     lambda s: s.replace("memset(pixels, 0, texels * 4);",
                         "memset(pixels, 0x0UL, texels * 4);", 1)),
    ("the padding fill is the octal zero 00 (review-13 F2: a base the widening added)",
     lambda s: s.replace("memset(pixels, 0, texels * 4);",
                         "memset(pixels, 00, texels * 4);", 1)),
    ("the padding fill is the binary zero 0b0 (review-13 F2: and the other base it "
     "added)",
     lambda s: s.replace("memset(pixels, 0, texels * 4);",
                         "memset(pixels, 0b0, texels * 4);", 1)),
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
    # one place in this family that still had it. Every arm is now bound, and the
    # binding was generated from the measured run rather than typed, so it is the
    # current truth and not an intention. The count is deliberately NOT written here:
    # review-16 R3 caught transcribed counts going stale in the same file that named
    # the command, review-17 R4 caught it again in this very comment (it said 44 while
    # the run printed 45), and the only fix that cannot rot is not transcribing one.
    # The line this loop prints immediately above is the count, measured.
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
