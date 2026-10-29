#!/usr/bin/env bash
# MDK gate suite -- SPEC.md 9.4, AGENTS.md.
#
#   bash tools/check.sh          run the whole suite
#   bash tools/check.sh --help   this header
#
# THE SEVEN GATES, IN ORDER. The order is the order the contract builds up in:
# the language contract first (it decides what the code may say at all), then
# the hot-path rules and the exemption grammar, then the two maths screens,
# then the two parity gates, then the frame golden. The suite FAILS ON THE FIRST
# FAILURE: a gate that fails stops the run, because a suite that keeps going
# past a red gate trains its reader to look at the last line instead of the
# first.
#
#   1. audit_c_with_classes.py  AGENTS.md 10 -- the language contract
#   2. gates_vfp.py             R1-R11 + T1/T2, the exemption grammar, R10
#   3. audit_arm11_math.py      trig-LUT coverage, fixed-point candidates
#   4. audit_r11_idiv.py        runtime integer division/modulo in hot loops
#   5. gates_core_parity.py     the two targets must link the SAME game
#   6. gates_smdh.py            the CIA's home-menu identity
#   7. gates_frames.py          the pinned frame matches its recorded capture
#
# WHAT A "SKIP" IS. gates_smdh has nothing to read until a .cia has been
# built, so it says SKIPPED and exits 0. gates_frames has nothing to capture
# until build-linux/mdk and the game data exist, and it needs
# tests/frames/reference.sha256 before it has a reference at all. A skip is
# recorded in the summary as a skip -- never as a pass. The rest of the suite is
# a hard pass/fail: the R11 audit is a SCREEN, not a proof, so a clean run means
# "nothing obvious", never "nothing present".
#
# ENVIRONMENT
#   VFP_FROZEN_AT=""   put the corpus-age and pin-age clocks back (AGENTS.md 2).
#                      The default is the port's freeze date, and the waiver is
#                      reported in the gate's own output, not hidden here.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
    sed -n '2,/^set -uo pipefail/p' "${BASH_SOURCE[0]}" \
        | sed -e '/^set -uo pipefail/d' -e 's/^# \{0,1\}//'
    exit 0
fi
if [ $# -gt 0 ]; then
    printf 'CHECK FAILED: unknown option %s (tools/check.sh --help)\n' "$1" >&2
    exit 2
fi

PY=python3
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

FAILURES=0
SUMMARY=""

gate() { printf '\n=== %s ===\n' "$1"; }

record() { # record <status> <name> <detail>
    SUMMARY="${SUMMARY}$(printf '  %-6s %-24s %s' "$1" "$2" "$3")
"
}

fail() { # fail <gate> <message>
    record FAIL "$1" "$2"
    printf '\nCHECK FAILED: %s\n' "$2" >&2
    printf '\n%s' "$SUMMARY"
    printf 'CHECK: 1 gate FAILED -- %s\n' "$2" >&2
    exit 1
}

# ---------------------------------------------------------------------------
# 1. the language contract
# ---------------------------------------------------------------------------
gate "audit_c_with_classes (AGENTS.md 10 -- expect VERDICT: CONTRACT CLEAN)"
$PY tools/runner/audit_c_with_classes.py > "$tmp/cwc.log" 2>&1
rc=$?
grep "VERDICT" "$tmp/cwc.log" || true
if [ $rc -ne 0 ]; then
    grep -A3 "VIOLATION(S)" "$tmp/cwc.log" | head -30 >&2
    fail "audit_c_with_classes" "a zero-tolerance construct is in the tree"
fi
record PASS "audit_c_with_classes" "0 violations"

# ---------------------------------------------------------------------------
# 2. the hot-path gates
# ---------------------------------------------------------------------------
gate "gates_vfp (expect PASS blockers=0)"
$PY -m tools.runner.gates_vfp > "$tmp/gates.log" 2>&1
rc=$?
grep -E "^(PASS|FAIL)" "$tmp/gates.log" | head -1
grep -E "^note:" "$tmp/gates.log" | sed 's/^/  /'
if [ $rc -ne 0 ]; then
    grep -E "MUST|FAIL" "$tmp/gates.log" | head -20 >&2
    fail "gates_vfp" "exit $rc"
fi
record PASS "gates_vfp" "0 blockers"

# ---------------------------------------------------------------------------
# 3. the trig / fixed-point audit
# ---------------------------------------------------------------------------
gate "audit_arm11_math (expect VERDICT: CONTRACT CLEAN)"
$PY tools/runner/audit_arm11_math.py > "$tmp/math.log" 2>&1
rc=$?
grep "VERDICT" "$tmp/math.log" || true
if [ $rc -ne 0 ]; then
    grep -A20 "VIOLATIONS" "$tmp/math.log" | head -25 >&2
    fail "audit_arm11_math" "exit $rc"
fi
record PASS "audit_arm11_math" "contract clean"

# ---------------------------------------------------------------------------
# 4. the R11 screen
# ---------------------------------------------------------------------------
gate "audit_r11_idiv (screen -- expect no candidates)"
$PY tools/runner/audit_r11_idiv.py > "$tmp/r11.log" 2>&1
rc=$?
grep -E "filtered:|pinned:" "$tmp/r11.log" | sed 's/^/  /'
if [ $rc -ne 0 ]; then
    grep -A20 "candidate(s)" "$tmp/r11.log" | head -25 >&2
    fail "audit_r11_idiv" "exit $rc"
fi
record PASS "audit_r11_idiv" "no candidates"

# ---------------------------------------------------------------------------
# 5. core <-> target parity
# ---------------------------------------------------------------------------
gate "gates_core_parity (expect VERDICT: CONTRACT CLEAN)"
$PY tools/runner/gates_core_parity.py > "$tmp/parity.log" 2>&1
rc=$?
grep -E "core sources:|linux SOURCES|note:" "$tmp/parity.log" | sed 's/^/  /'
if [ $rc -ne 0 ]; then
    grep -A4 "FINDING(S)" "$tmp/parity.log" | head -40 >&2
    fail "gates_core_parity" "exit $rc"
fi
record PASS "gates_core_parity" "both targets link the same core"

# ---------------------------------------------------------------------------
# 6. the SMDH identity
# ---------------------------------------------------------------------------
gate "gates_smdh (expect CONTRACT CLEAN, or SKIPPED before a .cia exists)"
$PY tools/runner/gates_smdh.py > "$tmp/smdh.log" 2>&1
rc=$?
grep -E "VERDICT|note:" "$tmp/smdh.log" | sed 's/^/  /'
if [ $rc -ne 0 ]; then
    grep -A4 "FINDING(S)" "$tmp/smdh.log" | head -30 >&2
    fail "gates_smdh" "exit $rc"
fi
if grep -q "VERDICT: SKIPPED" "$tmp/smdh.log"; then
    record SKIP "gates_smdh" "no built .cia to read"
else
    record PASS "gates_smdh" "the built CIA agrees with the Makefile"
fi

# ---------------------------------------------------------------------------
# 7. the frame golden
# ---------------------------------------------------------------------------
gate "gates_frames (expect VERDICT: CONTRACT CLEAN, or SKIPPED before a frame exists)"
$PY tools/runner/gates_frames.py > "$tmp/frames.log" 2>&1
rc=$?
grep -E "^(PASS|FAIL) |^note: |^VERDICT" "$tmp/frames.log" | sed 's/^note: /  note: /'
if [ $rc -ne 0 ]; then
    grep -E "^FAIL|note: |VERDICT" "$tmp/frames.log" | head -20 >&2
    fail "gates_frames" "exit $rc"
fi
if grep -q "VERDICT: SKIPPED" "$tmp/frames.log"; then
    record SKIP "gates_frames" "no manifest, no built Linux binary, or no game data"
else
    record PASS "gates_frames" "every pinned frame is deterministic and unchanged"
fi

printf '\n%s' "$SUMMARY"
printf '\nCHECK: ALL GATES PASSED\n'
