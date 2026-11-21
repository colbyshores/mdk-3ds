#!/bin/sh
# linux_binary_fresh_test.sh -- refuse to run the Linux-binary suites on a stale binary.
#
# WHY: linux_arg_parse, level_chain, menu_frontend, stream_screen, fall_frontend and
# frame_gate all run build-linux/mdk as it is on disk. Nothing rebuilds it, so a binary
# older than the sources reports the CURRENT tree's failures with an OLD program: a flag
# the old program doesn't know reads as "the run exited 1", and every log expectation
# after it fails. That cost three wrong diagnoses on 2026-11-20. This guard turns that
# into one plain line naming the fix.
#
# An absent binary is a SKIP, the same as those suites' own SKIP, so a worktree without
# a Linux build still runs everything else.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BIN="$ROOT/build-linux/mdk"
if [ ! -x "$BIN" ]; then
    echo "SKIP LinuxBinaryFresh (build-linux/mdk is not built)"
    exit 0
fi
# Every source the Linux binary is built from: the shared core, the Linux frontend and
# its build file. A symlinked build-linux built from ANOTHER checkout's sources also
# reads as stale here, which is right: it is not this tree's program.
NEWER=$(find "$ROOT/mdk_core/src" "$ROOT/mdk_linux/src" "$ROOT/mdk_linux/CMakeLists.txt" \
    -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.inc' -o -name 'CMakeLists.txt' \) \
    -newer "$BIN" 2>/dev/null | head -5)
if [ -n "$NEWER" ]; then
    echo "FAIL LinuxBinaryFresh: build-linux/mdk is older than its sources; rebuild it first:"
    echo "    cmake --build build-linux -j8"
    echo "  newer sources (first 5):"
    echo "$NEWER" | sed "s|^$ROOT/|    |"
    exit 1
fi
echo "PASS LinuxBinaryFresh (build-linux/mdk is newer than every mdk_core/mdk_linux source)"
