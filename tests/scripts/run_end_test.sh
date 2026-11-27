#!/bin/sh
# run_end_test.sh -- every 3DS frame loop asks the run's own end.
#
# WHY A SOURCE CHECK. The frame loops (main_3ds.cpp's level loop, the front
# screens in menu_screens_3ds.cpp, the Gunter stream in stream_3ds.cpp) include
# <3ds.h> and cannot run on a host. What is pinned is WHERE a call sits, the
# same reason main_3ds_flow_test.sh and c3d_frame_test.sh exist.
#
# THE DEFECT THIS PINS (old3ds-lan167, 2026-11-26 08:17). The run's own end was
# a field per loop -- FrontScreens3ds::frameBudget, Stream3dsFrontend::frameBudget
# -- and a harness's deadline was one call in the level loop. A harness port
# that missed the front screens (the 7d86f347 port on the 11-26 throwaways) let a
# LEVEL=5 run whose Kurt died sit in the main menu for good: no autoquit, no
# session end, a hw_cycle TIMEOUT with the console's HTTP answering throughout.
# The end is now ONE function, platform/run_end_3ds.h's runEnded3ds(), and:
#
#   1. every source file in mdk_3ds/src that services aptMainLoop() calls
#      runEnded3ds() at least as many times as it has aptMainLoop() loops,
#      except the files listed in EXEMPT below with their reason. A COUNT per
#      file, not a proof that each call sits inside its loop: a call left in
#      dead code beside a loop that lost its own would pass;
#   2. no `frameBudget` exists outside platform/run_end_3ds.cpp -- a second copy
#      of the budget is the per-loop field coming back;
#   3. the three frame loops this test was written for still exist and still
#      call it (a check that finds no loops at all passes nothing);
#   4. NON-VACUITY: the checker FAILS a copy of the tree with the front screens'
#      call removed, and a copy with a frameBudget field put back.
#
# Comments and string literals are stripped before matching: this tree explains
# itself in prose ("aptMainLoop() every frame"), and prose is not a call.
#
#   usage: sh tests/scripts/run_end_test.sh [repo-root]

ROOT=${1:-$(cd "$(dirname "$0")/../.." && pwd)}
SRC="$ROOT/mdk_3ds/src"

if [ ! -d "$SRC" ]; then
    echo "FAIL RunEnd (no mdk_3ds/src under $ROOT)"
    exit 1
fi

WORK=$(mktemp -d "${TMPDIR:-/tmp}/mdk-runend.XXXXXX") || exit 1
trap 'rm -rf "$WORK"' EXIT
trap 'exit 130' INT TERM

cat >"$WORK/check.py" <<'PY'
import os, re, sys

# A wait that is not a run's frame loop: bounded on its own and returning into
# a loop that asks. Each with its reason.
EXEMPT = {
    # holdHdWindow3ds: the loading screen's HD wait, ended by 1 s idle or 60 s
    # in all; it returns into the level loop's load, which reaches the level loop.
    "packs_3ds.cpp",
}
# The loops the test was written for: they must be found, or the check is empty.
EXPECTED = {"main_3ds.cpp", "menu_screens_3ds.cpp", "stream_3ds.cpp"}

def strip(text):
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            out.append(" ")
        elif c == '"' or c == "'":
            q, i = c, i + 1
            while i < n and text[i] != q:
                i += 2 if text[i] == "\\" else 1
            i += 1
            out.append(q + q)
        else:
            out.append(c)
            i += 1
    return "".join(out)

src = sys.argv[1]
fails = []
found = set()
for dirpath, _, files in os.walk(src):
    for name in sorted(files):
        if not name.endswith((".cpp", ".inc", ".h")):
            continue
        path = os.path.join(dirpath, name)
        code = strip(open(path, encoding="utf-8", errors="replace").read())
        rel = os.path.relpath(path, src)
        if re.search(r"\bframeBudget\b", code) and rel != os.path.join("platform", "run_end_3ds.cpp"):
            fails.append("%s: a frameBudget outside platform/run_end_3ds.cpp (the per-loop copy is back)" % rel)
        loops = len(re.findall(r"\baptMainLoop\s*\(\s*\)", code))
        if loops == 0 or name in EXEMPT:
            continue
        found.add(name)
        calls = len(re.findall(r"\brunEnded3ds\s*\(", code))
        if calls < loops:
            fails.append("%s: %d aptMainLoop() loop(s) but %d runEnded3ds() call(s) -- a frame loop "
                         "that does not ask the run's own end" % (rel, loops, calls))
missing = EXPECTED - found
for name in sorted(missing):
    fails.append("%s: no aptMainLoop() frame loop found (moved? the test would check nothing)" % name)
for f in fails:
    print("  FAIL " + f)
print("checked %d frame-loop file(s): %s" % (len(found), ", ".join(sorted(found))))
sys.exit(1 if fails else 0)
PY

fails=0
pass() { echo "PASS RunEnd $1"; }
fail() { echo "FAIL RunEnd $1"; fails=$((fails + 1)); }

# 1-3. The tree as it is.
if python3 -I "$WORK/check.py" "$SRC" >"$WORK/live.txt" 2>&1; then
    pass "live: every frame loop asks runEnded3ds ($(tail -1 "$WORK/live.txt"))"
else
    cat "$WORK/live.txt"
    fail "live: a frame loop does not ask the run's own end"
fi

# 4a. Non-vacuity: the front screens' call removed must fail.
cp -r "$SRC" "$WORK/cut"
sed 's/runEnded3ds(\*front->frame, &front->exitReason)/false/' \
    "$SRC/menu_screens_3ds.cpp" >"$WORK/cut/menu_screens_3ds.cpp"
if cmp -s "$SRC/menu_screens_3ds.cpp" "$WORK/cut/menu_screens_3ds.cpp"; then
    fail "seeded (front screens): the seed did not apply -- the call's spelling moved"
elif python3 -I "$WORK/check.py" "$WORK/cut" >"$WORK/cut.txt" 2>&1; then
    fail "seeded (front screens): the checker PASSED a menu loop with no run end"
else
    pass "seeded (front screens): a menu loop without runEnded3ds is refused"
fi

# 4b. Non-vacuity: a frameBudget field put back must fail.
cp -r "$SRC" "$WORK/field"
sed 's/^struct Stream3dsFrontend {$/struct Stream3dsFrontend {\n    uint32_t frameBudget = 0;/' \
    "$SRC/stream_3ds.h" >"$WORK/field/stream_3ds.h"
if cmp -s "$SRC/stream_3ds.h" "$WORK/field/stream_3ds.h"; then
    fail "seeded (field): the seed did not apply -- the struct's spelling moved"
elif python3 -I "$WORK/check.py" "$WORK/field" >"$WORK/field.txt" 2>&1; then
    fail "seeded (field): the checker PASSED a per-loop frameBudget copy"
else
    pass "seeded (field): a per-loop frameBudget copy is refused"
fi

if [ "$fails" -ne 0 ]; then
    echo "RunEnd: $fails failure(s)"
    exit 1
fi
echo "RunEnd: all checks passed"
