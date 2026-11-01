#!/bin/sh
# sprites_refusal_test.sh — the missing-TRAVSPRT.BNI refusal, gated.
#
# WHY THIS FILE EXISTS (review-9 F2). Batch 11 shipped the refusal, batch 22
# pictured it (docs/evidence/2026-11-01-L7-sprites-missing-scene.png) and pinned
# the PARSE GUARD behind it (archive_guard_tests.cpp). Neither of those checks
# the harness's own branch: "the archive did not parse, so refuse" lived in
# tick1's main() with nothing able to go red if someone deleted it. Dropping
# tick1's `return 1` into a fallthrough that builds a runtime against an empty
# Bni changes the random stream and therefore every golden that touches a
# scripted random decision — with all goldens green, because the goldens run
# against the intact tree and never see the branch.
#
# WHAT THIS GATE COVERS, AND WHAT IT DOES NOT (review-10 F2). The refusal exists
# TWICE on this tree, in two different programs:
#
#   covered here   tests/scripts/tick1.cpp:1101-1117 — the golden harness's copy
#   NOT covered    mdk_linux/src/main.cpp:383-394   — the shipped frontend's copy
#
# The frontend's copy is not covered, and it is not covered cheaply: it sits
# inside `if (data.found())` -> LevelData::load -> `if (arena)` -> spawnPlayer(),
# so reaching it needs the licensed tree, a level that loads, and a renderer. A
# gate that needs the licensed tree SKIPS on a host without it, and a check that
# skips on missing input is a pass generator, not a gate — the same defect class
# this file exists to close (see the verdict accounting below). So the frontend's
# branch stays a declared gap, listed in docs/CONTINUATION.md "Standing open
# items", and the picture of it is the committed evidence pair
# docs/evidence/2026-11-01-L7-sprites-missing-scene.{png,txt}. Do not describe
# this file as gating the frontend.
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
# VERDICT ACCOUNTING (review-10 F1). An arm that cannot set up — a `mkdir` that
# fails, a `dd` that does not accept its operands, a `grep` that errors instead of
# reporting zero matches — used to `return 1` into a caller that discarded it, and
# the script printed `PASS SpritesRefusal` over exit 0 having adjudicated nothing.
# That is the vault's pass-generator shape (a-gate-that-skips-on-missing-input)
# reproduced inside the gate written to close it, and the fix is the vault's rule:
# a verdict must state how many subjects it adjudicated, and the count is checked
# against the DECLARED number of arms. Every arm therefore either adjudicates or
# fails loudly, and the PASS line is unreachable unless ADJUDICATED equals
# DECLARED_ARMS. The cardinality check is the structural part of the fix: it
# catches a future early return that forgets to set FAILED, which a per-call-site
# `|| exit 1` would not.
#
# WHERE THE COUNT MOVES (review-11 F1, re-opened and closed by review-12 F1). The
# count used to sit on the line before the verdict `if/else`, so it measured
# "reached this line" rather than "printed a verdict": an early return inserted
# between the count and the verdict printed `PASS SpritesRefusal (2 arms
# adjudicated)` over zero verdict lines — the gate asserting a count of
# comparisons that were never made. Batch 25 moved the count inside verdict() but
# left it BEFORE the printf, so one `return 0` between those two statements
# reproduced the same false PASS (re-measured on 07345d6: PASS, rc 0, no verdict
# line printed). The window moved from one line to two; it did not close.
#
# THE DIRECTION OF THE ERROR IS THE FIX. ADJUDICATED now moves AFTER the printf.
# That is the only ordering the backstop can read: the cardinality check fires on
# a SHORT count, so an over-count is invisible to it by construction. Counting
# ahead of the print therefore turned a skipped print into a false PASS, while
# counting after it turns a skipped print into an UNDER-count, which is a FAIL.
# Every increment is now preceded by a verdict line printed by the same call, and
# ARM_VERDICT (reset at the top of each arm, set only after a print) bounds the
# total at one per arm, so PASS with DECLARED_ARMS cannot print unless every
# declared arm printed a verdict. An early return anywhere inside verdict() or
# arm() is covered by that: before the print it drops the count and goes red,
# after the count it changes nothing.
#
# WHAT THAT DOES NOT PIN, because a bound with no stated direction is a bound a
# reader will over-read: an edit that calls verdict() OUTSIDE arm(), or that
# resets ARM_VERDICT mid-arm, can still over-count, and no backstop of this shape
# can see an over-count. That is a structural edit to this file rather than a stray
# return; if the PASS line ever reports more arms than printed, the per-arm guard
# is the thing to re-read. The setup guards deliberately do NOT go through
# verdict(): an arm that never ran has adjudicated nothing, which is precisely the
# condition the cardinality check exists to report.
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
ADJUDICATED=0
DECLARED_ARMS=2
ARM_VERDICT=0

# verdict <ok|fail> <text> — the only place a verdict is printed, and the only
# place ADJUDICATED moves. The count moves AFTER the printf and never before it:
# a skipped print must be able to make the count SHORT, because a short count is
# the only thing the cardinality backstop can see (review-12 F1).
verdict() {
    kind=$1
    shift
    if [ "$kind" = fail ]; then
        printf '  FAIL sprites_refusal: %s\n' "$*"
        FAILED=1
    else
        printf '  ok   sprites_refusal: %s\n' "$*"
    fi
    if [ "$ARM_VERDICT" -eq 0 ]; then
        ARM_VERDICT=1
        ADJUDICATED=$((ADJUDICATED + 1))
    fi
}

# arm <label> <archive-bytes> <expected: refuse|accept>
# Returns 1 only after printing a FAIL line and setting FAILED. An arm counts
# toward ADJUDICATED only when it prints a verdict, through verdict().
arm() {
    label=$1
    size=$2
    expect=$3
    ARM_VERDICT=0
    dir="$WORK/$label"
    if ! mkdir -p "$dir/TRAVERSE"; then
        printf '  FAIL sprites_refusal: the %s arm could not create its data tree; the arm never ran, and a gate that did not run has checked nothing\n' "$label"
        FAILED=1
        return 1
    fi
    # A zero-filled archive of `size` bytes. For size 4 the count field is
    # truncated; for size 8 the count is 0, which is a legal empty archive.
    # `2>/dev/null` rather than `status=none`: the latter is a GNU coreutils
    # operand and a BSD/macOS dd rejects it, which would take the arm out
    # through the setup guard instead of adjudicating it.
    if ! dd if=/dev/zero of="$dir/TRAVERSE/TRAVSPRT.BNI" bs=1 count="$size" 2>/dev/null; then
        printf '  FAIL sprites_refusal: the %s arm could not write its %s-byte archive; the arm never ran, and a gate that did not run has checked nothing\n' "$label" "$size"
        FAILED=1
        return 1
    fi

    out=$(MDK_DATA_DIR="$dir" "$TICK1" dump rolling 3 1 2>&1)
    rc=$?
    # grep -c exits 0 with a positive count, 1 with a count of 0, and >1 when it
    # could not do its job. Only the first two are a reading; the third used to
    # leave $refused empty, and `[ "" -ne 0 ]` is a shell error inside an `if`,
    # which printed neither ok nor FAIL and left FAILED at 0.
    refused=$(printf '%s\n' "$out" | grep -c "TRAVSPRT.BNI did not load")
    grc=$?
    case $refused in
        '' | *[!0-9]*) refused='' ;;
    esac
    if [ "$grc" -gt 1 ] || [ -z "$refused" ]; then
        printf '  FAIL sprites_refusal: the %s arm could not read its own output (grep exited %s, printed "%s"); a verdict computed over unreadable input is not a clean one\n' "$label" "$grc" "$refused"
        FAILED=1
        return 1
    fi

    # From here both $rc and $refused are numbers, so every branch below prints a
    # verdict, and the count moves with the print rather than ahead of it.
    if [ "$expect" = refuse ]; then
        if [ "$rc" -eq 0 ]; then
            verdict fail "the $label arm exited 0; a runtime built without the archive is a different random stream, not the oracle of the C#"
        fi
        if [ "$refused" -eq 0 ]; then
            verdict fail "the $label arm did not say why it refused (the message is the only thing a player or an operator reads)"
        fi
        if [ "$rc" -ne 0 ] && [ "$refused" -ne 0 ]; then
            verdict ok "the $label arm refused (exit $rc, message present)"
        fi
    else
        if [ "$rc" -ne 0 ]; then
            verdict fail "the $label arm exited $rc; a legal empty archive must not refuse, or the guard is unconditional and the pair proves nothing"
        fi
        if [ "$refused" -ne 0 ]; then
            verdict fail "the $label arm refused a legal empty archive"
        fi
        if [ "$rc" -eq 0 ] && [ "$refused" -eq 0 ]; then
            verdict ok "the $label arm was accepted (exit 0, no refusal)"
        fi
    fi
}

arm stub 4 refuse
arm control 8 accept

# The cardinality backstop. Two arms were declared, so two verdicts are required;
# "neither ok nor FAIL was printed" can no longer fall through to PASS.
if [ "$ADJUDICATED" -ne "$DECLARED_ARMS" ]; then
    printf '  FAIL sprites_refusal: only %s of %s declared arms produced a verdict; a PASS here would be a reading of nothing\n' "$ADJUDICATED" "$DECLARED_ARMS"
    FAILED=1
fi

if [ "$FAILED" -eq 0 ]; then
    printf 'PASS SpritesRefusal (%s arms adjudicated)\n' "$ADJUDICATED"
    exit 0
fi
exit 1
