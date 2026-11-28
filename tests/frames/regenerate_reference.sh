#!/usr/bin/env bash
# regenerate_reference.sh -- rebuild the frame golden manifest from the C#
# oracle (SPEC.md 15.2: "a reference frame changes only from the
# reference source").
#
#   usage: tests/frames/regenerate_reference.sh [output-directory]
#
# RUN THIS WHEN THE ORACLE CHANGES, or when an intentional renderer change has
# been reviewed -- never to make a failing comparison pass. It
#
#   1. captures each pinned frame from the C# oracle (the reference source),
#   2. captures the same pinned frame from the C++ build,
#   3. measures the divergence between the two,
#   4. writes tests/frames/reference.sha256, which tools/runner/gates_frames.py
#      reads.
#
# The PIXELS are never committed: they are ROM-derived art and the original game
# install is gitignored. They are written to the output directory (default
# ${TMPDIR:-/tmp}/mdk-frame-reference) and only hashes and measured deltas go
# into the manifest.
#
# WHAT THE TWO HASHES MEAN, because conflating them is how a frame gate turns
# into a rubber stamp:
#
#   oracle=  THE REFERENCE. The C# renderer's bytes. Nothing about the C++
#            build decides it, and it is the only number that says "the
#            original look".
#   cpp=     THE REGRESSION BASELINE. The C++ build's own bytes, recorded so
#            that any change to the picture is visible as a hash change. It is
#            NOT a fidelity reference and must never be quoted as one; it
#            exists because the picture is not yet the oracle's (see
#            bad_percent) and a gate that is red from the day it is written
#            teaches everyone to skip the gate (AGENTS.md 2, same argument as
#            the waived corpus-age clocks).
#   bad_percent / max_delta / mean_delta
#            THE OPEN GAP, measured between the two, at tolerance 0. The gate
#            fails if the live measurement exceeds any of them. They are a
#            ceiling that may only SHRINK as P5 closes the renderer gap; raising
#            one is a regression that has been accepted, and should read like
#            one in the commit message.
#
# The pinned frames are the table below. Frame 1 is the state after exactly one
# fixed 1/60-second simulation step of the pinned level, which is what the C++
# --capture-frame=1 produces.
set -uo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
OUTDIR=${1:-${TMPDIR:-/tmp}/mdk-frame-reference}
MANIFEST="$ROOT/tests/frames/reference.sha256"

# name                      level  size     seed  frame
PINNED=(
    "L3_f1.bmp             3      320x240  1     1"
)

mkdir -p "$OUTDIR"
: >"$MANIFEST.tmp" || exit 1

printf '# MDK frame golden manifest -- written by tests/frames/regenerate_reference.sh\n' >>"$MANIFEST.tmp"
printf '# DO NOT EDIT BY HAND to make a gate pass. See that script for what each field is.\n' >>"$MANIFEST.tmp"
printf '# oracle= is the C# reference; cpp= is a regression baseline, not a fidelity reference.\n' >>"$MANIFEST.tmp"
printf '# bad_percent/max_delta/mean_delta are the measured C++-to-oracle gap at tolerance 0 and\n' >>"$MANIFEST.tmp"
printf '# are a ceiling that may only shrink. Generated: %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >>"$MANIFEST.tmp"

for spec in "${PINNED[@]}"; do
    read -r name level size seed frame <<<"$spec"
    echo "=== oracle: $name (level $level, $size, seed $seed, frame $frame) ==="
    if ! MDK_FRAME_LEVEL="$level" MDK_FRAME_SIZE="$size" MDK_FRAME_SEED="$seed" \
            MDK_FRAME_CAPTURE="$frame" \
            "$ROOT/tests/frames/capture_oracle.sh" "$OUTDIR/$name"; then
        echo "regenerate_reference: the oracle capture of $name failed" >&2
        rm -f "$MANIFEST.tmp"
        exit 1
    fi
    echo "=== c++: $name ==="
    if ! MDK_FRAME_LEVEL="$level" MDK_FRAME_SIZE="$size" MDK_FRAME_SEED="$seed" \
            MDK_FRAME_CAPTURE="$frame" \
            "$ROOT/tests/frames/capture_cpp.sh" "$OUTDIR/$name.cpp"; then
        echo "regenerate_reference: the C++ capture of $name failed" >&2
        rm -f "$MANIFEST.tmp"
        exit 1
    fi

    # The divergence, measured by the same comparator the gate uses, so the
    # recorded ceiling and the live measurement cannot drift apart.
    measure=$(python3 "$ROOT/tests/frames/compare_frames.py" "$OUTDIR/$name" \
              "$OUTDIR/$name.cpp" --measure 2>/dev/null) || { echo "regenerate_reference: measurement of $name failed" >&2; rm -f "$MANIFEST.tmp"; exit 1; }
    read -r bad_pct max_delta mean_delta <<<"$measure"

    printf 'name=%s level=%s size=%s seed=%s frame=%s oracle=%s cpp=%s bad_percent=%s max_delta=%s mean_delta=%s\n' \
        "$name" "$level" "$size" "$seed" "$frame" \
        "$(sha256sum "$OUTDIR/$name" | cut -d' ' -f1)" \
        "$(sha256sum "$OUTDIR/$name.cpp" | cut -d' ' -f1)" \
        "$bad_pct" "$max_delta" "$mean_delta" >>"$MANIFEST.tmp"
    printf '%s: oracle %s  cpp %s  gap %s%% of pixels (max %s, mean %s)\n' \
        "$name" "$(sha256sum "$OUTDIR/$name" | cut -c1-12)" \
        "$(sha256sum "$OUTDIR/$name.cpp" | cut -c1-12)" "$bad_pct" "$max_delta" "$mean_delta"
done
mv "$MANIFEST.tmp" "$MANIFEST"
echo "regenerate_reference: wrote $MANIFEST from $OUTDIR"
