#!/bin/sh
# ubsan_decode_test.sh — the audio decodes, run under -fsanitize=undefined,address.
#
# WHY TWO SANITIZERS AND NOT ONE. The filename says ubsan because that is what
# batch 96 shipped, and the name is quoted in the Makefile and in the progress
# log's history, so it stays. Batch 97 added AddressSanitizer because UBSan cannot
# see the second half of the defect it now guards: a compressed MVE audio frame
# whose declared size is smaller than its channel count wrote a predictor past
# the end of the sample array, and the two shapes of that one bug need two
# different instruments —
#
#   mono,   declared 1 -> UBSan "reference binding to null pointer of type
#                         'value_type'" (the vector never allocated at all)
#   stereo, declared 2 -> AddressSanitizer "heap-buffer-overflow ... WRITE of
#                         size 4" (one slot past a real 4-byte allocation, which
#                         UBSan walks straight over and a plain build runs
#                         silently)
#
# WHY THIS EXISTS AND A VALUE TEST CANNOT. Batch 95 replaced
#
#     ((int32_t)byte - 128) << 8      with      ((int32_t)byte - 128) * 256
#
# in audio/sound.cpp and formats/mve.cpp. C++17 [expr.shift] leaves a left shift
# of a negative signed value UNDEFINED, and half of the byte domain is negative,
# so every quiet half of every 8-bit sound fed the mix an answer the standard
# does not supply. The multiply is the same value by definition — and that is the
# problem: on both targets `* 256` lowers to the same `lsl #8`, so putting the
# shift back changes no sample, no golden, and no frame. testSampleStoreExact and
# mve_audio_tests.cpp pin the VALUE and are blind to this class by construction.
# Only a sanitizer sees it.
#
# Batch 95 knew that and ran UBSan — outside the tree, in /tmp, and committed
# nothing, so nobody can reproduce the claim it recorded. That is the shape of
# finding this loop keeps being given: a number whose provenance was asserted
# instead of committed. This file is that run, in the repository, on the `test`
# target.
#
# WHAT IT RUNS. tests/scripts/ubsan_decode_probe.cpp: soundFromPcm over all 256
# byte values and over the 16-bit extremes, and the committed MVE test (which
# builds its own movie, so no game data is needed) — which since batch 97 also
# walks the compressed DPCM path and the broken-size shapes. The probe counts its own
# assertions and exits nonzero if any fail, so "the sanitizer was quiet" cannot be
# reported by a probe that decoded nothing.
#
# WHAT WOULD MAKE THIS LIE, AND WHAT STOPS IT. A stale binary read as a fresh
# measurement is the failure mode this loop has been told about most often, so the
# probe is recompiled on every run — there is no incremental build here to go
# stale — the compile's own stderr is kept and printed, a compile failure is a
# FAIL and not a skip, a missing compiler is a FAIL and not a skip, and the run
# log is written by this invocation and read back from the same path.
# `-fno-sanitize-recover=all` makes the first report fatal, so a report cannot be
# printed and then walked past.
#
# usage: sh ubsan_decode_test.sh
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)
CORE="$ROOT/mdk_core/src"
BUILD="$HERE/build"
CXX=${CXX:-g++}
PROBE="$BUILD/ubsan_decode_probe"
BLOG="$BUILD/ubsan_decode_build.log"
RLOG="$BUILD/ubsan_decode_run.log"

if ! command -v "$CXX" >/dev/null 2>&1; then
    printf '  FAIL ubsan_decode: no %s on PATH, so the sanitizer never ran; a check '
    printf 'that skips because its tool is missing is a pass generator, not a gate.\n' "$CXX"
    exit 1
fi

mkdir -p "$BUILD" || { printf '  FAIL ubsan_decode: cannot create %s\n' "$BUILD"; exit 1; }
rm -f "$PROBE" "$BLOG" "$RLOG"

if ! "$CXX" -std=c++17 -O1 -g -fno-strict-aliasing \
        -fsanitize=undefined,address -fno-sanitize-recover=all \
        -I"$CORE" -I"$HERE" \
        "$HERE/ubsan_decode_probe.cpp" "$HERE/mve_audio_tests.cpp" \
        "$CORE/audio/sound.cpp" "$CORE/formats/mve.cpp" "$CORE/formats/bin_reader.cpp" \
        -o "$PROBE" >"$BLOG" 2>&1; then
    printf '  FAIL ubsan_decode: the probe did not compile, so nothing was measured\n'
    sed 's/^/    /' "$BLOG"
    exit 1
fi
[ -x "$PROBE" ] || { printf '  FAIL ubsan_decode: the compiler reported success and '
    printf 'produced no executable\n'; exit 1; }

"$PROBE" >"$RLOG" 2>&1
rc=$?

if grep -qE "runtime error|ERROR: AddressSanitizer|ERROR: LeakSanitizer" "$RLOG"; then
    printf '  FAIL ubsan_decode: undefined behaviour or a memory error in the decodes —\n'
    grep -E "runtime error|ERROR: AddressSanitizer|ERROR: LeakSanitizer" "$RLOG" | sed 's/^/    /'
    printf '    the fix is the defined, in-bounds form, not a sanitizer suppression\n'
    exit 1
fi

if [ "$rc" -ne 0 ]; then
    printf '  FAIL ubsan_decode: the probe exited %d —\n' "$rc"
    sed 's/^/    /' "$RLOG"
    exit 1
fi

printf 'PASS UbsanDecode (%s)\n' "$(sed -n '1p' "$RLOG")"
exit 0
