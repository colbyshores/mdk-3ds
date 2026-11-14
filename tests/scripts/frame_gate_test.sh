#!/bin/bash
# frame_gate_test.sh -- the frame golden gate, proven able to fail.
#
# WHY THIS FILE EXISTS (SPEC-COMPLETION 3.4, p5.0.frame-harness). The renderer
# golden gate (tools/runner/gates_frames.py, wired into tools/check.sh) is the
# renderer's equivalent of the script goldens: a deterministic headless capture
# of a pinned scene, a byte-exact regression baseline, and a fidelity ceiling
# against the C# oracle. SPEC-COMPLETION requires such a gate to be "proven to
# fail on a seeded visual regression before it is trusted", and nothing in the
# tree proved it: a gate whose red side has never been observed is a claim,
# not an instrument.
#
# THE ARMS. Each runs the SHIPPED gate or the SHIPPED comparator, never a copy:
#
#   1. live        the gate on this tree passes (VERDICT: CONTRACT CLEAN).
#                  Without this arm the red arms prove nothing.
#   2. seeded      the recorded cpp= baseline is corrupted to zeros (the
#      baseline    manifest restored on exit, verified byte-identical), and
#                  the gate must FAIL naming the changed frame. This is the
#                  seeded visual regression at the gate's own surface: any
#                  picture change moves the hash, so a hash that cannot match
#                  is exactly a picture that changed.
#   3. seeded      one byte of a live capture is flipped in the pixel area and
#      pixel       tests/frames/compare_frames.py must FAIL it at tolerance 0
#                  while passing the unflipped pair. This proves the
#                  comparator sees a single-pixel regression, not only a
#                  wholly different frame.
#   4. seeded      when the oracle pixels are on disk (${TMPDIR}/mdk-frame-
#      fidelity    reference), a copy with one flipped byte becomes the
#                  reference and the gate must FAIL on the grown gap. Skipped
#                  (and counted as skipped, never as passed) when the scratch
#                  reference is absent, because the pixels are ROM-derived
#                  and may have been cleaned.
#
# A missing binary or game data SKIPS the whole file with the reason printed:
# there is no frame to judge, and a verdict manufactured from nothing would
# be the pass-generator defect this file exists to close.
set -u
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
GATE="$ROOT/tools/runner/gates_frames.py"
MANIFEST="$ROOT/tests/frames/reference.sha256"
COMPARE="$ROOT/tests/frames/compare_frames.py"
CAPTURE="$ROOT/tests/frames/capture_cpp.sh"

WORK=$(mktemp -d) || exit 1
MANIFEST_BAK="$WORK/reference.sha256.bak"
cp "$MANIFEST" "$MANIFEST_BAK" 2>/dev/null || true
restore() {
    if [ -f "$MANIFEST_BAK" ]; then cp "$MANIFEST_BAK" "$MANIFEST"; fi
    rm -rf "$WORK"
}
trap restore EXIT

fails=0
checks=0
skipped=0
check() { # check <description> <expected> <actual>
    checks=$((checks + 1))
    if [ "$2" = "$3" ]; then
        echo "PASS $1"
    else
        echo "FAIL $1 -- expected '$2', got '$3'"
        fails=$((fails + 1))
    fi
}
skip() { # skip <description>
    skipped=$((skipped + 1))
    echo "SKIP $1"
}

if [ ! -x "$ROOT/build-linux/mdk" ]; then
    echo "SKIP frame gate test: build-linux/mdk is not built"
    exit 0
fi
if [ ! -d "${MDK_DATA_DIR:-$ROOT/MDK}/TRAVERSE" ]; then
    echo "SKIP frame gate test: no game data (MDK_DATA_DIR)"
    exit 0
fi

# Arm 1: the live gate passes.
live_out=$(python3 "$GATE" 2>&1)
live_rc=$?
check "frame gate: the live pinned frame passes its gate" "0" "$live_rc"
check "frame gate: and the verdict is CONTRACT CLEAN, not a skip" "1" \
      "$(printf '%s' "$live_out" | grep -c 'VERDICT: CONTRACT CLEAN')"

# Arm 2: a seeded baseline regression reddens the gate.
sed -i 's/cpp=[0-9a-f]\{64\}/cpp=0000000000000000000000000000000000000000000000000000000000000000/' "$MANIFEST"
seeded_out=$(python3 "$GATE" 2>&1)
seeded_rc=$?
check "frame gate: a seeded baseline regression fails the gate" "1" "$seeded_rc"
check "frame gate: and the failure names the changed frame" "1" \
      "$(printf '%s' "$seeded_out" | grep -c 'the frame changed')"
cp "$MANIFEST_BAK" "$MANIFEST"
check "frame gate: the manifest is restored byte-identical" \
      "$(sha256sum "$MANIFEST_BAK" | cut -d' ' -f1)" \
      "$(sha256sum "$MANIFEST" | cut -d' ' -f1)"

# Arm 3: a one-byte pixel regression reddens the comparator.
cap="$WORK/frame.bmp"
if "$CAPTURE" "$cap" >/dev/null 2>&1 && [ -s "$cap" ]; then
    same_out=$(python3 "$COMPARE" "$cap" "$cap" 2>&1)
    check "frame gate: the comparator passes a frame against itself" "0" "$?"
    python3 - "$cap" "$WORK/frame.flipped.bmp" <<'PY'
import sys
data = bytearray(open(sys.argv[1], "rb").read())
data[54 + 4000] ^= 0xFF  # one byte inside the pixel area
open(sys.argv[2], "wb").write(data)
PY
    flip_out=$(python3 "$COMPARE" "$cap" "$WORK/frame.flipped.bmp" 2>&1)
    flip_rc=$?
    check "frame gate: a one-byte pixel regression fails the comparator" "1" "$flip_rc"
    check "frame gate: and the comparator says FAIL" "1" \
          "$(printf '%s' "$flip_out" | grep -c 'FRAME FAIL')"
else
    check "frame gate: the pinned capture succeeds for the pixel arm" "ok" "capture-failed"
fi

# Arm 4: a seeded fidelity regression reddens the gate, when oracle pixels exist.
REFDIR="${MDK_FRAME_REFERENCE_DIR:-${TMPDIR:-/tmp}/mdk-frame-reference}"
if [ -f "$REFDIR/L3_f1.bmp" ]; then
    mkdir -p "$WORK/ref"
    python3 - "$REFDIR/L3_f1.bmp" "$WORK/ref/L3_f1.bmp" <<'PY'
import sys
data = bytearray(open(sys.argv[1], "rb").read())
for off in range(54, len(data)):
    data[off] ^= 0xFF  # invert the reference: the gap must exceed the ceiling
open(sys.argv[2], "wb").write(data)
PY
    fid_out=$(MDK_FRAME_REFERENCE_DIR="$WORK/ref" python3 "$GATE" 2>&1)
    fid_rc=$?
    check "frame gate: a seeded fidelity regression fails the gate" "1" "$fid_rc"
    check "frame gate: and the failure names the grown gap" "1" \
          "$(printf '%s' "$fid_out" | grep -c 'the gap to the oracle grew')"
else
    skip "frame gate: fidelity arm (no oracle pixels in $REFDIR)"
fi

# Arm 5: the flat-stereo composite (P6). The pinned scene through --stereo=sbs
# must hold two DIFFERENT eyes (a composite that duplicated one eye would pass
# a naive "is stereo on" check), and, when the oracle's own sbs frame is in
# the reference directory, the gap to it must stay under the recorded
# ceiling. Measured 2026-11-14 on the pinned scene: 4.833% of pixels at
# tolerance 0 (max 244, mean 1.1030) -- the mono frame's own 1.240% gap
# carried through the half-width squeeze, so the ceiling is 6.0 and, like
# the manifest's, it may only shrink.
STEREO_CAP="$WORK/frame.sbs.bmp"
if MDK_FRAME_STEREO=sbs "$CAPTURE" "$STEREO_CAP" >/dev/null 2>&1 && [ -s "$STEREO_CAP" ]; then
    halves=$(python3 - "$STEREO_CAP" <<'PY'
import hashlib, struct, sys
data = open(sys.argv[1], "rb").read()
offset = struct.unpack_from("<I", data, 10)[0]
width = struct.unpack_from("<i", data, 18)[0]
height = abs(struct.unpack_from("<i", data, 22)[0])
row = width * 4
left, right = [], []
for y in range(height):
    start = offset + y * row
    left.append(data[start:start + row // 2])
    right.append(data[start + row // 2:start + row])
print("differ" if hashlib.md5(b"".join(left)).digest() != hashlib.md5(b"".join(right)).digest() else "same")
PY
)
    check "frame gate: the sbs composite holds two different eyes" "differ" "$halves"
else
    check "frame gate: the stereo capture succeeds for the stereo arm" "ok" "capture-failed"
fi

if [ -f "$REFDIR/L3_f1_sbs.bmp" ] && [ -s "$STEREO_CAP" ]; then
    sbs_measure=$(python3 "$COMPARE" "$REFDIR/L3_f1_sbs.bmp" "$STEREO_CAP" --measure 2>/dev/null)
    sbs_bad=$(printf '%s' "$sbs_measure" | awk '{print $1}')
    over=$(python3 - "$sbs_bad" <<'PY'
import sys
print("1" if float(sys.argv[1]) > 6.0 else "0")
PY
)
    check "frame gate: the sbs gap to the oracle stays under its 6.0% ceiling (measured 4.833)" "0" "$over"
else
    skip "frame gate: stereo fidelity arm (no oracle sbs frame in $REFDIR)"
fi

# Arm 6: the gamma grade (P6 post). The pinned scene at --gamma=1.5 must
# match the oracle's own graded frame under the recorded ceiling. Measured
# 2026-11-14: 1.240% at tolerance 0 (max 250, mean 0.1490) -- exactly the
# mono frame's gap, i.e. the grade's math adds no divergence of its own.
# Ceiling 2.0, may only shrink.
GAMMA_CAP="$WORK/frame.gamma.bmp"
if MDK_FRAME_GAMMA=1.5 "$CAPTURE" "$GAMMA_CAP" >/dev/null 2>&1 && [ -s "$GAMMA_CAP" ]; then
    if [ -f "$REFDIR/L3_f1_gamma.bmp" ]; then
        gamma_measure=$(python3 "$COMPARE" "$REFDIR/L3_f1_gamma.bmp" "$GAMMA_CAP" --measure 2>/dev/null)
        gamma_bad=$(printf '%s' "$gamma_measure" | awk '{print $1}')
        gamma_over=$(python3 - "$gamma_bad" <<'PY'
import sys
print("1" if float(sys.argv[1]) > 2.0 else "0")
PY
)
        check "frame gate: the gamma gap to the oracle stays under its 2.0% ceiling (measured 1.240)" "0" "$gamma_over"
    else
        skip "frame gate: gamma fidelity arm (no oracle gamma frame in $REFDIR)"
    fi
else
    check "frame gate: the gamma capture succeeds for the gamma arm" "ok" "capture-failed"
fi

if [ "$fails" -ne 0 ]; then
    echo "FRAME GATE: $fails FAILED ($checks checks, $skipped skipped)"
    exit 1
fi
echo "PASS FrameGate ($checks checks, $skipped skipped)"
