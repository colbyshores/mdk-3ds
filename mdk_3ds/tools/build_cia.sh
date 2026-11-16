#!/usr/bin/env bash
# ============================================================================
# build_cia.sh — build the .cia from the .elf, the SMDH and the RomFS tree.
#
# USAGE:  build_cia.sh <output.cia> <elf> <smdh> <romfs-dir> <icon.png> <banner.png>
#
# makerom IS NOT PART OF THE DEVKITPRO TOOLCHAIN AS INSTALLED HERE. It lives
# in a separate package (devkitPro's "3ds-cia-tools" / makerom, from
# Project_CTR). When it is absent this script says so PLAINLY and exits
# non-zero, rather than producing a broken .cia: a .cia that installs and
# then shows the wrong title is worse than no .cia.
#
# The .3dsx -- which the same build produces, with the same RomFS embedded
# and the same SMDH -- is the artifact that runs in an emulator and through
# a homebrew loader, and it does not need makerom.
# ============================================================================
set -euo pipefail

if [ "$#" -lt 6 ]; then
    echo "usage: $0 <out.cia> <elf> <smdh> <romfs-dir> <icon.png> <banner.png>" >&2
    exit 2
fi

OUT="$1"; ELF="$2"; SMDH="$3"; ROMFS="$4"; ICON="$5"; BANNER="$6"

# The harness must never ship (mdk_3ds/Makefile's AUTOQUIT): a CIA built
# from a harness ELF would quit by itself and chain-load a stub that is
# not on a retail console.
if [ -n "${AUTOQUIT:-}" ]; then
    echo "build_cia: AUTOQUIT is set -- this is a harness build, not a release." >&2
    echo "            Rebuild without AUTOQUIT for a CIA." >&2
    exit 2
fi

MAKEROM="${MAKEROM:-}"
if [ -z "$MAKEROM" ]; then
    for candidate in makerom /opt/devkitpro/tools/bin/makerom "$HOME"/.local/bin/makerom; do
        if command -v "$candidate" >/dev/null 2>&1; then
            MAKEROM="$candidate"
            break
        fi
    done
fi

if [ -z "$MAKEROM" ]; then
    echo "build_cia: makerom was not found." >&2
    echo "            It is NOT part of the devkitPro toolchain as installed." >&2
    echo "            Install it (Project_CTR's makerom, plus libmxml) or set" >&2
    echo "            MAKEROM=/path/to/makerom, then re-run." >&2
    echo "            The .3dsx with the same RomFS and SMDH is already built" >&2
    echo "            and is the artifact that runs in an emulator." >&2
    exit 3
fi

TITLE="MDK"
DESCRIPTION="Metal Dungeon"
PUBLISHER="Shiny Entertainment"

echo "build_cia: $MAKEROM -f cia -o $OUT"
"$MAKEROM" -f cia -o "$OUT" \
    -rsf "$ROMFS" \
    -target D \
    -exefslogo \
    -elf "$ELF" \
    -icon "$ICON" \
    -banner "$BANNER" \
    -name "$TITLE" \
    -publisher "$PUBLISHER" \
    -title "$TITLE" \
    -desc "$DESCRIPTION"

echo "build_cia: wrote $OUT ($(stat -c%s "$OUT") bytes)"
