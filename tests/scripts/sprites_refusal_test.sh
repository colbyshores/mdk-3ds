#!/bin/sh
# sprites_refusal_test.sh — the missing-TRAVSPRT.BNI refusal, gated.
#
# WHY THIS FILE EXISTS (review-9 F2). Batch 11 shipped the refusal, batch 22
# pictured it (docs/evidence/2026-11-01-L7-sprites-missing-scene.png) and pinned
# the PARSE GUARD behind it (archive_guard_tests.cpp). Neither of those checks
# the frontend's own branch: "the archive did not parse, so refuse" lived in
# tick1's main() with nothing able to go red if someone deleted it. Changing
# `if (scriptAssetsReady && spritesReady)` to `if (scriptAssetsReady)`, or
# dropping tick1's `return 1` into a fallthrough that builds a runtime against an
# empty Bni, is a one-line edit that changes the random stream and therefore
# every golden that touches a scripted random decision — with all goldens green,
# because the goldens run against the intact tree and never see the branch.
#
# THE PAIR. Both arms run the SAME argv against a fabricated data tree whose only
# file is TRAVERSE/TRAVSPRT.BNI; the archive is the only thing that differs, so
# the exit status can only be the guard's doing:
#
#   stub     4 zero bytes  ->  non-zero exit, and the refusal on stdout
#   control  8 zero bytes   ->  exit 0, and NO refusal on stdout
#
# Eight zero bytes is a LEGAL EMPTY archive (count 0): archive_guard_tests.cpp
# pins that it parses, because the C# can produce an archive with no such entry
# and a content check there would add a branch the oracle does not have. So the
# control is not "a file that happens to work", it is the smallest archive the
# oracle accepts, which is what makes the pair a test of the guard rather than of
# the file's existence.
#
# WHY `dump rolling`. The guard runs before the dump dispatch, so any argv shows
# it; `rolling` is the one that returns before the level load, so the pair needs
# no ROM-derived level data and runs identically on a machine with no game tree.
# A control that needed the tree would SKIP without it, and a check that skips on
# missing input is a pass generator, not a gate.
#
# No game data, no network, no display. Run by `make -C tests/scripts`; its exit
# status is the gate.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
TICK1=${1:-$HERE/build/tick1}
if [ ! -x "$TICK1" ]; then
    printf 'FAIL sprites_refusal: no tick1 binary at %s (run make -C tests/scripts bin first)\n' "$TICK1" >&2
    exit 1
fi

WORK=$(mktemp -d "${TMPDIR:-/tmp}/mdk-refusal.XXXXXX") || exit 1
trap 'rm -rf "$WORK"' EXIT INT TERM

FAILED=0

# arm <label> <archive-bytes> <expected: refuse|accept>
arm() {
    label=$1
    size=$2
    expect=$3
    dir="$WORK/$label"
    mkdir -p "$dir/TRAVERSE" || return 1
    # A zero-filled archive of `size` bytes. For size 4 the count field is
    # truncated; for size 8 the count is 0, which is a legal empty archive.
    dd if=/dev/zero of="$dir/TRAVERSE/TRAVSPRT.BNI" bs=1 count="$size" status=none || return 1

    out=$(MDK_DATA_DIR="$dir" "$TICK1" dump rolling 3 1 2>&1)
    rc=$?
    refused=$(printf '%s\n' "$out" | grep -c "TRAVSPRT.BNI did not load")

    if [ "$expect" = refuse ]; then
        if [ "$rc" -eq 0 ]; then
            printf '  FAIL sprites_refusal: the %s arm exited 0; a runtime built without the archive is a different random stream, not the oracle of the C#\n' "$label"
            FAILED=1
        fi
        if [ "$refused" -eq 0 ]; then
            printf '  FAIL sprites_refusal: the %s arm did not say why it refused (the message is the only thing a player or an operator reads)\n' "$label"
            FAILED=1
        fi
        if [ "$rc" -ne 0 ] && [ "$refused" -ne 0 ]; then
            printf '  ok   sprites_refusal: the %s arm refused (exit %s, message present)\n' "$label" "$rc"
        fi
    else
        if [ "$rc" -ne 0 ]; then
            printf '  FAIL sprites_refusal: the %s arm exited %s; a legal empty archive must not refuse, or the guard is unconditional and the pair proves nothing\n' "$label" "$rc"
            FAILED=1
        fi
        if [ "$refused" -ne 0 ]; then
            printf '  FAIL sprites_refusal: the %s arm refused a legal empty archive\n' "$label"
            FAILED=1
        fi
        if [ "$rc" -eq 0 ] && [ "$refused" -eq 0 ]; then
            printf '  ok   sprites_refusal: the %s arm was accepted (exit 0, no refusal)\n' "$label"
        fi
    fi
}

arm stub 4 refuse
arm control 8 accept

if [ "$FAILED" -eq 0 ]; then
    printf 'PASS SpritesRefusal\n'
    exit 0
fi
exit 1
