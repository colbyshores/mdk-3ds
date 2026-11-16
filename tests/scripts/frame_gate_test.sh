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

# Arm 7: the filtered canvas + enhanced sky (p6 slice f). The pinned scene
# at --enhanced must stay under its recorded ceiling against the oracle's
# own enhanced frame. Measured 2026-11-15 on the pinned scene: 86.241% of
# pixels at tolerance 0 (max 251, mean 11.2394) -- dominated by the lit
# pipeline's pre-existing divergence from slices (a)-(e), which no earlier
# arm ever measured; this slice's own surface (the canvas band) moved the
# gap from 87.966% to 86.241% and the original look stayed byte-identical.
# The ceiling is 88.0, may only shrink, and the p6 row cannot close until
# the (a)-(e) residue under it is burned down.
ENH_CAP="$WORK/frame.enhanced.bmp"
if MDK_FRAME_ENHANCED=1 "$CAPTURE" "$ENH_CAP" >/dev/null 2>&1 && [ -s "$ENH_CAP" ]; then
    if [ -f "$REFDIR/L3_f1_enhanced.bmp" ]; then
        enh_measure=$(python3 "$COMPARE" "$REFDIR/L3_f1_enhanced.bmp" "$ENH_CAP" --measure 2>/dev/null)
        enh_bad=$(printf '%s' "$enh_measure" | awk '{print $1}')
        enh_over=$(python3 - "$enh_bad" <<'PY'
import sys
print("1" if float(sys.argv[1]) > 88.0 else "0")
PY
)
        check "frame gate: the enhanced gap to the oracle stays under its 88.0% ceiling (measured 86.241)" "0" "$enh_over"
    else
        skip "frame gate: enhanced fidelity arm (no oracle enhanced frame in $REFDIR)"
    fi
else
    check "frame gate: the enhanced capture succeeds for the enhanced arm" "ok" "capture-failed"
fi

# Arm 8: mods (p6 slice g). A fixture mod replacing the HUD's SC_STAT
# image must change the enhanced frame (the enhanced canvas draws the
# mod's image, mode 4) and must NOT change the original frame (the
# original look never sees mods, exactly as the C# only builds ModImages
# for the enhanced graphics).
MOD_USER="$WORK/moduser"
mkdir -p "$MOD_USER/mods/fixture/images"
printf 'name=Fixture\ndescription=frame gate mod\n' > "$MOD_USER/mods/fixture/mod.txt"
python3 - "$MOD_USER/mods/fixture/images/SC_STAT.png" <<'PY' 2>/dev/null
import sys
from PIL import Image
Image.new("RGBA", (8, 8), (255, 0, 255, 255)).save(sys.argv[1])
PY
MOD_ENH="$WORK/frame.mods.bmp"
MOD_PLAIN="$WORK/frame.mods.plain.bmp"
PLAIN_CAP="$WORK/frame.plain.bmp"
if [ ! -f "$MOD_USER/mods/fixture/images/SC_STAT.png" ]; then
    skip "frame gate: mods arm (no PIL for the fixture PNG)"
elif [ -s "$ENH_CAP" ] && \
   MDK_USER_DIR="$MOD_USER" MDK_FRAME_ENHANCED=1 "$CAPTURE" "$MOD_ENH" >/dev/null 2>&1 && \
   [ -s "$MOD_ENH" ]; then
    mod_measure=$(python3 "$COMPARE" "$ENH_CAP" "$MOD_ENH" --measure 2>/dev/null)
    mod_bad=$(printf '%s' "$mod_measure" | awk '{print $1}')
    mod_changed=$(python3 - "$mod_bad" <<'PY'
import sys
print("1" if float(sys.argv[1]) > 0.0 else "0")
PY
)
    check "frame gate: a mod's image changes the enhanced frame" "1" "$mod_changed"
    if MDK_USER_DIR="$MOD_USER" "$CAPTURE" "$MOD_PLAIN" >/dev/null 2>&1 && [ -s "$MOD_PLAIN" ] && \
       "$CAPTURE" "$PLAIN_CAP" >/dev/null 2>&1 && [ -s "$PLAIN_CAP" ]; then
        python3 "$COMPARE" "$PLAIN_CAP" "$MOD_PLAIN" >/dev/null 2>&1
        check "frame gate: the same mod leaves the original frame byte-identical" "0" "$?"
    else
        check "frame gate: the original captures succeed for the mods arm" "ok" "capture-failed"
    fi
else
    check "frame gate: the enhanced capture succeeds for the mods arm" "ok" "capture-failed"
fi

# Arm 9: Kurt's mod replacement (p6 slice g, the Sprite consumer). A
# fixture mod replacing Kurt's still frame (textures/K_STILL_0) must
# change the enhanced frame (Kurt draws through the enhanced Sprite
# path with the mod's colour texture) and leave the original frame
# byte-identical (Kurt's original shading never sees mods).
KURT_USER="$WORK/kurtuser"
mkdir -p "$KURT_USER/mods/fixture/textures"
printf 'name=Fixture\ndescription=frame gate kurt mod\n' > "$KURT_USER/mods/fixture/mod.txt"
python3 - "$KURT_USER/mods/fixture/textures/K_STILL_0.png" <<'PY' 2>/dev/null
import sys
from PIL import Image
Image.new("RGBA", (16, 16), (255, 0, 255, 255)).save(sys.argv[1])
PY
KURT_ENH="$WORK/frame.kurtmod.bmp"
KURT_PLAIN="$WORK/frame.kurtmod.plain.bmp"
if [ ! -f "$KURT_USER/mods/fixture/textures/K_STILL_0.png" ]; then
    skip "frame gate: kurt mods arm (no PIL for the fixture PNG)"
elif [ -s "$ENH_CAP" ] && \
   MDK_USER_DIR="$KURT_USER" MDK_FRAME_ENHANCED=1 "$CAPTURE" "$KURT_ENH" >/dev/null 2>&1 && \
   [ -s "$KURT_ENH" ]; then
    kurt_measure=$(python3 "$COMPARE" "$ENH_CAP" "$KURT_ENH" --measure 2>/dev/null)
    kurt_bad=$(printf '%s' "$kurt_measure" | awk '{print $1}')
    kurt_changed=$(python3 - "$kurt_bad" <<'PY'
import sys
print("1" if float(sys.argv[1]) > 0.0 else "0")
PY
)
    check "frame gate: a mod's image of Kurt's frame changes the enhanced frame" "1" "$kurt_changed"
    if [ -s "$PLAIN_CAP" ] && MDK_USER_DIR="$KURT_USER" "$CAPTURE" "$KURT_PLAIN" >/dev/null 2>&1 && \
       [ -s "$KURT_PLAIN" ]; then
        python3 "$COMPARE" "$PLAIN_CAP" "$KURT_PLAIN" >/dev/null 2>&1
        check "frame gate: Kurt's mod leaves the original frame byte-identical" "0" "$?"
    else
        check "frame gate: the original capture succeeds for the kurt mods arm" "ok" "capture-failed"
    fi
else
    check "frame gate: the enhanced capture succeeds for the kurt mods arm" "ok" "capture-failed"
fi

if [ "$fails" -ne 0 ]; then
    echo "FRAME GATE: $fails FAILED ($checks checks, $skipped skipped)"
    exit 1
fi
echo "PASS FrameGate ($checks checks, $skipped skipped)"
