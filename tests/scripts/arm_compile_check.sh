#!/bin/sh
# arm_compile_check.sh — compile every mutation arm and green probe a source gate
# can build, with the 3DS target's own flags.
#
# WHY THIS FILE EXISTS (review-15 F5). `main_3ds_flow_test.sh` carries a header
# claim that every arm and every probe compiles for the 3DS target, and review-14
# F3 had already found three that did not — an arm that cannot compile is a mutant
# of nothing, because the defect it "proves" could never have been written. The
# batch that fixed it recorded the flags and the result ("41 files, 0 diagnostics")
# in `docs/PROGRESS_LOG.md` and named no generator, so the claim was true and
# nobody could re-run it, which is the same thing as a claim that will rot. This is
# the generator. The record now points here instead of pasting a transcript.
#
# WHY IT IS NOT WIRED INTO `make -C tests/scripts`. The host suite runs on the
# host compiler and must keep working where devkitARM is not installed; the cross
# compiler is a several-hundred-megabyte dependency and the suite's own contract is
# that it runs anywhere a C++17 compiler does. So this is a tool you invoke, and it
# says so loudly when the toolchain is missing rather than skipping — a check that
# passes because its input was absent is a pass generator, which is the defect this
# project has now been reviewed for eleven times.
#
# WHAT IT NEEDS FROM THE TREE (review-17 R6). `mdk_core/src` and `mdk_3ds/src` (both
# source trees; a wrong-directory error if either is absent) and, for the c3d half only,
# the generated `mdk_3ds/build/mdk_c3d_shbin.h` that `renderer_c3d.cpp` includes. A tree
# that has not run `make -C mdk_3ds` therefore compiles the flow mutants and then
# reports INCOMPLETE with the missing file and the command that makes it named -- it does
# NOT pass, and it no longer dies on a gitignored directory before it can say so.
#
# WHAT IT PROVES AND WHAT IT DOES NOT. It proves the mutant text is valid C++ for
# armv6k/VFPv2 with the Makefile's flags, so every arm is a program a later batch
# could actually commit. It does NOT prove the arm reddens a check — that is the
# gate's own job, and the gate adjudicates arms on the host.
#
# THE FORMAT PASS, AND WHAT HALF OF THE ENFORCEMENT IT IS (batch 117; corrected by
# review-23 F1 in batch 118). `mdk::log3dsPrintf` carries
# `__attribute__((format (printf, 1, 2)))`, so the compiler checks the argument count and
# type of every one of the tree's log calls -- 72 of them in three files -- where check 17
# of `main_3ds_flow_test.sh` adjudicates ONE refusal line. Batch 117 said this script is
# "where the diagnostic becomes a gate", and that was false: no command in the loop's gate
# chain runs this file, so enforcement put here lived in a runner, not a gate. It now lives
# in `mdk_3ds/Makefile`, which adds `-Werror=format` to `CXXFLAGS`, so `make -C mdk_3ds` --
# which the loop DOES run -- fails a mismatched call site. The two halves are different and
# both are needed: the Makefile enforces the tree, and THIS script proves the attribute is
# still alive, by keeping four mutants that MUST be refused (a dropped argument, a star
# width with no value, a pointer where an int is asked, an `int32_t` printed by `%d`) plus
# honest calls that must compile. Take the attribute out of `platform/log_3ds.h` and the
# four mutants compile, which fails this script by name -- the attribute cannot rot away
# silently. Both halves are pinned BY NAME in a manifest below, because a glob that stops
# matching is not a compile error: it is a pass that measured nothing (review-4 F1).
# Closing that last gap (a deleted attribute caught by a GATE, not a runner) needs
# this script wired into owner-protected `tools/check.sh`, which is an owner decision.
# What the compiler CANNOT see, and check 17 still owns, is WHICH quantity a figure
# claims to be: `("objects: refused %d bytes", objectCorners)` is format-clean and is
# review-11 F1's 9 KB refusal of an 80 KB request.
#
# usage: sh arm_compile_check.sh [flow|c3d|all]     (default: all)
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
WHICH=${1:-all}

# The flags are the Makefile's, read from it rather than restated, so a change to
# the build moves this script instead of drifting from it (the same reason check 17
# reads the corner stride out of renderer_c3d.cpp instead of spelling 5).
MK=$ROOT/mdk_3ds/Makefile
DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
DEVKITARM=${DEVKITARM:-$DEVKITPRO/devkitARM}
CXX=${CXX:-$DEVKITARM/bin/arm-none-eabi-g++}

if [ ! -x "$CXX" ]; then
    echo "FAIL arm_compile_check: no cross compiler at $CXX" >&2
    echo "  Set DEVKITPRO/DEVKITARM, or pass CXX=<arm-none-eabi-g++>. This script" >&2
    echo  "  refuses to report a pass it did not measure (AGENTS.md P6)." >&2
    exit 1
fi
for f in "$ROOT/mdk_core/src" "$ROOT/mdk_3ds/src"; do
    if [ ! -d "$f" ]; then
        echo "FAIL arm_compile_check: include directory $f is missing" >&2
        exit 1
    fi
done
# Review-17 R6: `mdk_3ds/build` is NOT required here. It is a build artefact and is
# gitignored, so on the tree shape review-16 R4 named -- a fresh clone -- requiring it
# as a directory made the new named-header SKIP unreachable: the run died with
# "include directory .../mdk_3ds/build is missing" before it could say which generated
# file was absent and what command makes it. The c3d half below already reports that
# file BY NAME, so the directory check was both the wrong error and the one that
# hid the right one. A missing `-I` on a directory that does not exist is not an
# error to the compiler, and the c3d gate is skipped in exactly that case anyway.
if [ ! -d "$ROOT/mdk_3ds/build" ]; then
    echo "  note: $ROOT/mdk_3ds/build does not exist; the c3d mutants need the" >&2
    echo "        generated shader header the shader step writes there." >&2
fi

ARCH=$(sed -n 's/^ARCH *: *= *//p' "$MK" | head -1)
# Review-17 open question 1: the Makefile sets DEFINES in two places -- the `:=` line,
# and a `DEFINES += -D__3DS_DEBUG__` inside `ifeq ($(DEBUG),1)`. Taking only the first
# made this script's "the Makefile's own flags" claim only half true. Taking BOTH
# unconditionally would be worse: it would syntax-check every mutant under a macro the
# shipped build does not define, which is a different divergence and a quieter one. So
# the script mirrors the Makefile's own condition -- DEBUG=1 here adds the same macro
# the Makefile adds under DEBUG=1 -- and says so.
DEFINES=$(sed -n 's/^DEFINES *: *= *//p' "$MK" | head -1)
if [ "${DEBUG:-0}" = 1 ]; then
    DEFINES="$DEFINES $(sed -n 's/^DEFINES *+= *//p' "$MK" | tr '\n' ' ')"
fi
# -fsyntax-only, not -c: the mutants are never linked, and the shader DVLB and the
# libraries a link would need are build artefacts this script must not require.
# ARCH and DEFINES are READ from the Makefile. -O2, -std=gnu++17, -fno-exceptions,
# -fno-rtti, -fno-math-errno and -fno-strict-aliasing are RESTATED here, because the
# Makefile reaches them through $(COMMONFLAGS)/$(CXXFLAGS) expansion and a sed cannot
# expand a variable. That is the whole of the gap: a mutant that fails only under some
# other flag the Makefile adds through another variable would compile here. Stated
# rather than hidden (review-17 open question 1).
FLAGS="$ARCH -O2 -ffunction-sections -fdata-sections -fno-math-errno \
       -fno-strict-aliasing $DEFINES -std=gnu++17 -fno-exceptions -fno-rtti \
       -fsyntax-only"
INCLUDES="-I$ROOT/mdk_core/src -I$ROOT/mdk_3ds/src -I$ROOT/mdk_3ds/build \
          -I$DEVKITPRO/libctru/include -I$DEVKITPRO/portlibs/3ds/include"

# The mutants are extracted by running each gate's own script with a dump switch,
# so what gets compiled is exactly what the gate adjudicates. A second copy of the
# arm lists here would be a second implementation, and the one that rots.
TMP=${TMPDIR:-/tmp}/arm_compile_check.$$
cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT INT TERM

total=0
bad=0
skipped=""
for gate in flow c3d; do
    case "$WHICH" in
        all) ;;
        "$gate") ;;
        *) continue ;;
    esac
    # The c3d gate needs the generated shader header. Name the file and the command,
    # and do NOT let a run that skipped a whole gate read as a full pass: a total that
    # quietly excludes a gate is how a check starts lying about its coverage
    # (review-16 R4's real point, once the include path itself is kept).
    if [ "$gate" = c3d ] && [ ! -f "$ROOT/mdk_3ds/build/mdk_c3d_shbin.h" ]; then
        echo "  SKIP c3d: $ROOT/mdk_3ds/build/mdk_c3d_shbin.h is missing."
        echo "       renderer_c3d.cpp includes that generated header, so no c3d mutant can"
        echo "       be syntax-checked until the shader step has run: make -C mdk_3ds"
        skipped=" c3d"
        continue
    fi
    case $gate in
        flow) SCRIPT=$ROOT/tests/scripts/main_3ds_flow_test.sh
              SUBJECT=$ROOT/mdk_3ds/src/main_3ds.cpp ;;
        c3d)  SCRIPT=$ROOT/tests/scripts/c3d_frame_test.sh
              SUBJECT=$ROOT/mdk_3ds/src/render/renderer_c3d.cpp ;;
    esac
    out=$TMP/$gate
    mkdir -p "$out"
    if ! MDK_MUTANT_DUMP=$out sh "$SCRIPT" "$SUBJECT" >"$out/.dump" 2>&1; then
        echo "FAIL arm_compile_check: $gate produced no mutants" >&2
        sed 's/^/  /' "$out/.dump" >&2
        exit 1
    fi
    n=0
    for f in "$out"/*.cpp; do
        [ -e "$f" ] || continue
        n=$((n + 1))
        total=$((total + 1))
        # shellcheck disable=SC2086
        if ! "$CXX" $FLAGS $INCLUDES "$f" >"$f.log" 2>&1; then
            bad=$((bad + 1))
            echo "FAIL arm_compile_check: $(basename "$f" .cpp) does not compile"
            sed 's/^/  /' "$f.log" | head -20
        fi
    done
    echo "  $gate: $n mutant(s) extracted and compiled"
done

if [ "$total" = 0 ]; then
    echo "FAIL arm_compile_check: no mutants were extracted; a run that measured" >&2
    echo "  nothing does not get to print a pass." >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# THE FORMAT PASS. Two halves, both needed.
#
# (1) every shipped source compiles with -Werror=format. This is the half that makes
#     the attribute load-bearing: the tree currently carries zero format diagnostics,
#     so a future one — anywhere, not just on the line check 17 reads — stops a gate.
# (2) four mutants that MUST be refused, and one honest call that must compile. Without
#     them the pass would still print PASS if someone deleted the format attribute, and
#     a check that cannot fail at the thing it names is the defect this loop has now
#     been reviewed for twenty-two times. The four are the classes check 17's arity rule
#     was written for (review-18 F4's dropped argument, review-19 F2's star width) plus
#     the class the shipped tree actually carried 23 times: `%d` handed an `int32_t`,
#     which on this target is `long int`.
#
# The mutant compile pass above deliberately does NOT add -Werror=format, and that is a
# boundary rather than an oversight: 26 of the 133 mutant sources are format defects on
# purpose (measured: 26 in the flow gate, 0 in the c3d gate; `probe20`'s dangling
# argument is review-21 F1's shape), and an arm's job is to be a text the gate must
# refuse, not a line the shipped build would accept. The shipped build carries
# -Werror=format itself (mdk_3ds/Makefile), and that is the half that enforces the tree;
# this pass is the half that proves the attribute is still alive.
# ---------------------------------------------------------------------------
FMT_FLAGS="$ARCH -O2 -ffunction-sections -fdata-sections -fno-math-errno \
          -fno-strict-aliasing $DEFINES -std=gnu++17 -fno-exceptions -fno-rtti \
          -Wall -Wextra -Wno-unused-parameter -Werror=format -fsyntax-only"
FMT_DIR=$TMP/format
mkdir -p "$FMT_DIR"

fmt_files=0
fmt_bad=0
for f in $(find "$ROOT/mdk_3ds/src" "$ROOT/mdk_core/src" -name '*.cpp' | sort); do
    if [ "$f" = "$ROOT/mdk_3ds/src/render/renderer_c3d.cpp" ] \
       && [ ! -f "$ROOT/mdk_3ds/build/mdk_c3d_shbin.h" ]; then
        echo "  SKIP format sweep: renderer_c3d.cpp includes the generated"
        echo "       mdk_3ds/build/mdk_c3d_shbin.h; make -C mdk_3ds writes it."
        skipped="$ skipped format:renderer_c3d.cpp"
        continue
    fi
    fmt_files=$((fmt_files + 1))
    # shellcheck disable=SC2086
    if ! "$CXX" $FMT_FLAGS $INCLUDES "$f" >"$FMT_DIR/$(basename "$f").log" 2>&1; then
        fmt_bad=$((fmt_bad + 1))
        echo "FAIL arm_compile_check: ${f#$ROOT/} is not format-clean"
        sed "s/^/  /" "$FMT_DIR/$(basename "$f").log" | head -8
    fi
done

cat > "$FMT_DIR/mut_dropped_argument.cpp" <<'MUT'
#include "platform/log_3ds.h"
// review-18 F4: `%d %d` with one argument. The second conversion reads whatever the
// stack hands back and writes it into the file the test harness reads.
void mdk_format_mutant() { mdk::log3dsPrintf("objects: refused %d %d bytes", 1); }
MUT

cat > "$FMT_DIR/mut_star_width_without_its_value.cpp" <<'MUT'
#include "platform/log_3ds.h"
// review-19 F2: `%*d` consumes TWO arguments, a width and then the value. One supplied
// is undefined behaviour, and `len(convs) > len(vals)` called that balanced.
void mdk_format_mutant() { mdk::log3dsPrintf("objects: refused %*d bytes", 1); }
MUT

cat > "$FMT_DIR/mut_pointer_where_int_is_asked.cpp" <<'MUT'
#include "platform/log_3ds.h"
// A pointer printed as a number: the type half, which no value test can see and which
// is why the attribute is worth having on every call site and not on one.
void mdk_format_mutant() { mdk::log3dsPrintf("objects: refused %d bytes", "81920"); }
MUT

cat > "$FMT_DIR/mut_int32_printed_as_int.cpp" <<'MUT'
#include "platform/log_3ds.h"
#include <stdint.h>
// The class the shipped tree carried 23 times before this batch: `%d` handed an
// `int32_t`, which on arm-none-eabi is `long int`.
void mdk_format_mutant() {
    const int32_t attempted = 4096;
    mdk::log3dsPrintf("objects: refused %d bytes", attempted);
}
MUT

cat > "$FMT_DIR/probe_honest_refusal_call.cpp" <<'MUT'
#include "platform/log_3ds.h"
#include <stdint.h>
// The accept side. This is the shipped refusal line, and it must compile: a format pass
// that refuses correct code is review-10 F6's failure, and the reason a refuse-only net
// is never enough.
void mdk_format_probe() {
    const int32_t attempted = 4096;
    mdk::log3dsPrintf("objects: stream mesh of %d corners = %d KB linear",
                      (int)attempted,
                      (int)((int64_t)attempted * 5 * sizeof(float) / 1024));
}
MUT

# THE MANIFEST (review-4 F1). Both halves of the format pass are found by a glob over
# files THIS script wrote a few lines above, so the way either half dies is not a
# compile error, it is a heredoc deleted or renamed: the glob matches nothing, the loop
# body never runs, and the script prints `0/0 honest call(s) accepted` and then
# `PASS ArmCompile` with exit 0. Reproduced on a copy of this file with the probe heredoc
# removed. The mutant sweep above has refused that shape since review-14 (`no mutants
# were extracted; a run that measured nothing does not get to print a pass`); the format
# pass is now held to the same rule from three directions, because each one catches a
# mutation the other two survive:
#   (1) every name listed here MUST exist -- catches a heredoc deleted or renamed;
#   (2) the directory MUST hold exactly as many sources as the manifest names -- catches
#       a heredoc ADDED without being listed, which would never be compiled;
#   (3) each loop MUST measure a nonzero count (below, next to the PASS line) -- catches
#       a glob renamed inside a loop while every file is still present.
# A nonzero-only guard, which is the shape review-4 recommended, does not by itself
# close the scenario review-4 named: deleting ONE of the four mutants satisfies it while
# quietly dropping a class the pass exists to refuse, so the names are what is pinned.
FMT_MUT_WANT="mut_dropped_argument mut_star_width_without_its_value
              mut_pointer_where_int_is_asked mut_int32_printed_as_int"
FMT_PROBE_WANT="probe_honest_refusal_call"

# number of words in $1 (a manifest entry per line or per space)
fmt_words() { set -- $1; echo $#; }

for want in $FMT_MUT_WANT $FMT_PROBE_WANT; do
    if [ ! -f "$FMT_DIR/$want.cpp" ]; then
        echo "FAIL arm_compile_check: the format pass wrote no $want.cpp, so the class it"
        echo "  names is no longer measured at all. The heredoc that writes it in this file"
        echo "  is missing or renamed (review-4 F1); restoring it is the fix, and deleting"
        echo "  it from the manifest above is not."
        exit 1
    fi
done
fmt_mut_want=$(fmt_words "$FMT_MUT_WANT")
fmt_probe_want=$(fmt_words "$FMT_PROBE_WANT")
fmt_mut_have=$(find "$FMT_DIR" -maxdepth 1 -name 'mut_*.cpp' | wc -l)
fmt_probe_have=$(find "$FMT_DIR" -maxdepth 1 -name 'probe_*.cpp' | wc -l)
if [ "$fmt_mut_have" != "$fmt_mut_want" ] || [ "$fmt_probe_have" != "$fmt_probe_want" ]; then
    echo "FAIL arm_compile_check: $FMT_DIR holds $fmt_mut_have mutant and $fmt_probe_have"
    echo "  probe source(s) while the manifest names $fmt_mut_want and $fmt_probe_want. A"
    echo "  source no manifest entry names is never compiled, and the pass would report a"
    echo "  smaller set than it claims (review-4 F1)."
    exit 1
fi

fmt_refused=0
fmt_uncaught=0
for m in "$FMT_DIR"/mut_*.cpp; do
    [ -e "$m" ] || continue
    # shellcheck disable=SC2086
    if "$CXX" $FMT_FLAGS $INCLUDES "$m" >"$m.log" 2>&1; then
        fmt_uncaught=$((fmt_uncaught + 1))
        echo "FAIL arm_compile_check: $(basename "$m" .cpp) COMPILED."
        echo "       mdk::log3dsPrintf is not format-checked: the"
        echo "       __attribute__((format (printf, 1, 2))) is gone from platform/log_3ds.h."
    else
        fmt_refused=$((fmt_refused + 1))
    fi
done

# The accept side is counted, never asserted. Review-23 F4: this used to be a single
# hard-coded call whose failure incremented the SOURCE counter, so a run that accepted
# zero honest calls still printed "1 honest call accepted" and then blamed a source file
# that was clean. A script whose whole contract is that it refuses to report a pass it did
# not measure must not print an acceptance it did not measure. Every probe_*.cpp is
# compiled, counted, and reported as `ok/total`.
fmt_probes=0
fmt_probe_ok=0
fmt_probe_bad=0
for p in "$FMT_DIR"/probe_*.cpp; do
    [ -e "$p" ] || continue
    fmt_probes=$((fmt_probes + 1))
    # shellcheck disable=SC2086
    if "$CXX" $FMT_FLAGS $INCLUDES "$p" >"$p.log" 2>&1; then
        fmt_probe_ok=$((fmt_probe_ok + 1))
    else
        fmt_probe_bad=$((fmt_probe_bad + 1))
        echo "FAIL arm_compile_check: $(basename "$p" .cpp) is an honest log call that does NOT"
        echo "       compile, so the format pass rejects code the port writes (review-10 F6's"
        echo "       failure: a net that refuses correct code is worse than no net). Diagnostic:"
        sed 's/^/  /' "$p.log" | head -8
    fi
done

# Each half of the format pass reports its own count, so a failure names the half that
# failed instead of blaming the source sweep for a probe (review-23 F4).
echo "  format: $fmt_files shipped source(s) with -Werror=format, $fmt_refused mutant(s)"
echo "          refused, $fmt_probe_ok/$fmt_probes honest call(s) accepted"

if [ "$fmt_bad" != 0 ]; then
    echo "FAIL arm_compile_check: the format pass is not clean -- $fmt_bad shipped source(s)"
    echo "  printed a format diagnostic (named above). The attribute is live; a call site is wrong."
    exit 1
fi
if [ "$fmt_uncaught" != 0 ]; then
    echo "FAIL arm_compile_check: $fmt_uncaught format mutant(s) COMPILED -- the attribute is"
    echo "  gone or weakened, so nothing checks the tree's 72 log call sites."
    exit 1
fi
if [ "$fmt_probe_bad" != 0 ]; then
    echo "FAIL arm_compile_check: $fmt_probe_bad of $fmt_probes honest log call(s) were refused"
    echo "  by the format pass."
    # Review-4 F2: this sentence used to claim that no honest call was accepted whatever
    # the count said, so `1/2 honest call(s) accepted` was immediately contradicted by a
    # line saying the whole accept side was dead -- and a reader told to look for a cause
    # that does not exist looks in the shipped sources, which this run just proved clean.
    # The claim is now conditional on the count it describes.
    if [ "$fmt_probe_ok" = 0 ]; then
        echo "  No honest call was accepted, so the accept side is dead. The source sweep"
        echo "  above was clean and is NOT what failed here."
    else
        echo "  $fmt_probe_ok of $fmt_probes was accepted, so the accept side is partly"
        echo "  broken and the named file above is the whole of it. The source sweep above"
        echo "  was clean and is NOT what failed here."
    fi
    exit 1
fi
# Review-4 F1, the third direction: the manifest proves the files exist and the counts
# prove nothing is unlisted, but each loop still finds its own files by glob. A glob
# renamed inside a loop measures nothing while every check above stays green, so a zero
# measured is refused here, at the last point before the pass could print a pass.
if [ "$fmt_refused" = 0 ] || [ "$fmt_probes" = 0 ]; then
    echo "FAIL arm_compile_check: the format pass measured nothing -- $fmt_refused mutant(s)"
    echo "  refused, $fmt_probe_ok/$fmt_probes honest call(s) accepted. The sources are all"
    echo "  present (the manifest above checked them), so a glob in one of the two loops is"
    echo "  broken; a run that measured nothing does not get to print a pass."
    exit 1
fi
if [ "$bad" != 0 ]; then
    echo "FAIL arm_compile_check: $bad of $total mutant(s) did not compile; an arm" >&2
    echo "  that cannot build is a mutant of nothing (review-14 F3)." >&2
    exit 1
fi
if [ -n "$skipped" ]; then
    echo "FAIL ArmCompile ($total mutant(s), 0 diagnostics) -- INCOMPLETE: part of this" >&2
    echo "       script was not compiled:$skipped. The SKIP line above names the missing" >&2
    echo "       generated file and the command that makes it. A partial run of this" >&2
    echo "       script is not a pass." >&2
    exit 1
fi
echo "PASS ArmCompile ($total mutant(s), 0 diagnostics, $fmt_files shipped source(s) format-clean, $fmt_refused format mutant(s) refused, $fmt_probe_ok/$fmt_probes honest call(s) accepted, $CXX)"
