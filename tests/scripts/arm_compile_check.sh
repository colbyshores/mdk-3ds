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
if [ "$bad" != 0 ]; then
    echo "FAIL arm_compile_check: $bad of $total mutant(s) did not compile; an arm" >&2
    echo "  that cannot build is a mutant of nothing (review-14 F3)." >&2
    exit 1
fi
if [ -n "$skipped" ]; then
    echo "FAIL ArmCompile ($total mutant(s), 0 diagnostics) -- INCOMPLETE: no mutant of" >&2
    echo "       this gate was compiled:$skipped. The SKIP line above names the missing" >&2
    echo "       generated file and the command that makes it. A partial run of this" >&2
    echo "       script is not a pass." >&2
    exit 1
fi
echo "PASS ArmCompile ($total mutant(s), 0 diagnostics, $CXX)"
