#!/usr/bin/env bash
# capture_cpp.sh -- one deterministic C++ frame, written as a 32-bit BMP.
#
# The frame is the simulation state, not the wall clock: --fixed-step advances
# exactly the C# test step (1/60 s) per loop and --capture-frame says which
# frame to write, so the same arguments always produce the same bytes. Two
# runs of this script are byte-identical; check_frames.sh proves it.
#
#   usage: tests/frames/capture_cpp.sh <out.bmp>
#
# Environment (all optional):
#   MDK_DATA_DIR   the game data (default: <repo>/MDK)
#   MDK_FRAME_LEVEL / MDK_FRAME_SIZE / MDK_FRAME_SEED / MDK_FRAME_CAPTURE
#                  the pinned scene: level 3, 320x240, seed 1, frame 1
#   VK_ICD_FILENAMES   set this to the Vulkan ICD to render headless
set -uo pipefail
OUT=${1:?usage: capture_cpp.sh <out.bmp>}
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)

if [ ! -x "$ROOT/build-linux/mdk" ]; then
    echo "capture_cpp: build-linux/mdk is missing; run cmake --build build-linux --target mdk" >&2
    exit 1
fi

export DISPLAY=:99
if [ -z "${VK_ICD_FILENAMES:-}" ] && [ -f /usr/share/vulkan/icd.d/lvp_icd.json ]; then
    export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json
fi

# The pinned stereo mode, when the caller asks for one (the frame gate's
# stereo arm). Empty means the mono frame, byte-identical to before.
STEREO_ARGS=()
if [ -n "${MDK_FRAME_STEREO:-}" ]; then
    STEREO_ARGS=(--stereo="$MDK_FRAME_STEREO")
    [ -z "${MDK_FRAME_STEREO_SEPARATION:-}" ] || STEREO_ARGS+=(--stereo-separation="$MDK_FRAME_STEREO_SEPARATION")
    [ -z "${MDK_FRAME_STEREO_CONVERGENCE:-}" ] || STEREO_ARGS+=(--stereo-convergence="$MDK_FRAME_STEREO_CONVERGENCE")
fi

exec env \
    MDK_DATA_DIR="${MDK_DATA_DIR:-$ROOT/MDK}" \
    "$ROOT/build-linux/mdk" \
        --headless \
        --fixed-step \
        --level="${MDK_FRAME_LEVEL:-3}" \
        --size="${MDK_FRAME_SIZE:-320x240}" \
        --seed "${MDK_FRAME_SEED:-1}" \
        --capture-frame="${MDK_FRAME_CAPTURE:-1}" \
        --capture="$OUT" \
        "${STEREO_ARGS[@]}"
