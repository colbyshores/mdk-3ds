#!/usr/bin/env bash
# publish_frame.sh -- capture the pinned frame and publish it as a committed PNG.
# Owner directive 2026-10-29: the commit history is a VISUAL record of progress.
#   usage: tools/publish_frame.sh <label>
# Output: docs/frames/<YYYY-MM-DD>-<label>.png (+ a .txt with the measured
# C++-to-oracle gap at that moment, so the picture is never separated from its
# number). The game's data/executable bytes stay gitignored at /MDK/; only these
# derived, low-resolution (320x240) frames are committed, as a progress record.
set -uo pipefail
LABEL=${1:?usage: publish_frame.sh <label>}
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OUTDIR="$ROOT/docs/frames"
TMP=$(mktemp -d /tmp/mdk-publish.XXXXXX)
trap 'rm -rf "$TMP"' EXIT
BMP="$TMP/frame.bmp"
if ! VK_ICD_FILENAMES="${VK_ICD_FILENAMES:-/usr/share/vulkan/icd.d/lvp_icd.json}" \
        "$ROOT/tests/frames/capture_cpp.sh" "$BMP" >"$TMP/capture.log" 2>&1; then
    echo "publish_frame: capture failed" >&2; tail -10 "$TMP/capture.log" >&2; exit 1
fi
[ -s "$BMP" ] || { echo "publish_frame: capture produced no frame" >&2; exit 1; }
mkdir -p "$OUTDIR"
STAMP=$(date -u +%Y-%m-%d)
PNG="$OUTDIR/$STAMP-$LABEL.png"
if python3 -c 'import PIL' 2>/dev/null; then
    python3 - "$BMP" "$PNG" <<'PY'
import sys
from PIL import Image
Image.open(sys.argv[1]).save(sys.argv[2], optimize=True)
PY
elif command -v convert >/dev/null 2>&1; then
    convert "$BMP" "$PNG"
else
    echo "publish_frame: no BMP->PNG converter" >&2; exit 1
fi
{
    echo "frame: level 3, 320x240, seed 1, frame 1 (tests/frames/capture_cpp.sh)"
    echo "captured: $(date -u +%FT%TZ) at $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
    ORACLE_DIR="${MDK_FRAME_REFERENCE_DIR:-${TMPDIR:-/tmp}/mdk-frame-reference}"
    # An enhanced capture (MDK_FRAME_ENHANCED, passed through to
    # capture_cpp.sh) is measured against the oracle's enhanced frame.
    ORACLE="$ORACLE_DIR/L3_f1.bmp"
    if [ -n "${MDK_FRAME_ENHANCED:-}" ]; then
        ORACLE="$ORACLE_DIR/L3_f1_enhanced.bmp"
    fi
    if [ -s "$ORACLE" ]; then
        read -r BAD MAX MEAN < <(python3 "$ROOT/tests/frames/compare_frames.py" "$ORACLE" "$BMP" --measure)
        printf 'live C++ vs C# oracle: bad_percent=%s max_delta=%s mean_delta=%s\n' "$BAD" "$MAX" "$MEAN"
    else
        grep "^name=" "$ROOT/tests/frames/reference.sha256" 2>/dev/null \
            | sed 's/oracle=[a-f0-9]* cpp=[a-f0-9]* //' | sed 's/^/recorded gap ceiling: /'
        echo "live measurement: unavailable (oracle pixels absent at $ORACLE)"
    fi
} > "${PNG%.png}.txt"
echo "publish_frame: wrote docs/frames/$(basename "$PNG")"
