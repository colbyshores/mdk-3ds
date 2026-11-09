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
for f in "$ROOT/mdk_core/src" "$ROOT/mdk_3ds/src" "$ROOT/mdk_3ds/build"; do
    if [ ! -d "$f" ]; then
        echo "FAIL arm_compile_check: include directory $f is missing" >&2
        exit 1
    fi
done

ARCH=$(sed -n 's/^ARCH *: *= *//p' "$MK" | head -1)
DEFINES=$(sed -n 's/^DEFINES *: *= *//p' "$MK" | head -1)
# -fsyntax-only, not -c: the mutants are never linked, and the shader DVLB and the
# libraries a link would need are build artefacts this script must not require.
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
for gate in flow c3d; do
    case "$WHICH" in
        all) ;;
        "$gate") ;;
        *) continue ;;
    esac
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
echo "PASS ArmCompile ($total mutant(s), 0 diagnostics, $CXX)"
