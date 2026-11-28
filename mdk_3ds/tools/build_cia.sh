#!/usr/bin/env bash
# ============================================================================
# build_cia.sh -- build the .cia from the .elf, the SMDH and the RomFS tree.
#
# USAGE:  build_cia.sh <output.cia> <elf> <smdh> <romfs-dir> <icon.png> <banner.bnr>
#
# makerom (Project_CTR, v0.19) is NOT part of the devkitPro toolchain as
# installed here; it is discovered on PATH, in the devkitPro tools bin, or
# in ~/.local/bin (or via $MAKEROM). When it is absent this script says so
# PLAINLY and exits non-zero rather than producing a broken .cia: a .cia
# that installs and then shows the wrong title is worse than no .cia.
#
# The HOME-menu identity comes from the SMDH the Makefile built with
# smdhtool from APP_TITLE / APP_DESCRIPTION / APP_AUTHOR (-icon below);
# the RSF (mdk_3ds/cia/mdk.rsf) carries the title id and access control.
# tools/runner/gates_smdh.py re-reads the SMDH out of the built .cia and
# fails if any populated language slot disagrees with the Makefile.
#
# The .3dsx -- which the same build produces, with the same RomFS embedded
# and the same SMDH -- is the artifact that runs in an emulator and through
# a homebrew loader, and it does not need makerom.
# ============================================================================
set -euo pipefail

if [ "$#" -lt 6 ]; then
    echo "usage: $0 <out.cia> <elf> <smdh> <romfs-dir> <icon.png> <banner.bnr>" >&2
    exit 2
fi

OUT="$1"; ELF="$2"; SMDH="$3"; ROMFS="$4"; ICON="$5"; BANNER="$6"

# The harness must never ship (mdk_3ds/Makefile's AUTOQUIT): a CIA built
# from a harness ELF would quit by itself and chain-load a stub that is
# not on a retail console.
if [ -n "${AUTOQUIT:-}" ] || [ -n "${CAPTURE:-}" ] || [ -n "${PROFILE:-}" ] || [ -n "${WALK:-}" ] ||
   [ -n "${GIVE:-}" ] || [ -n "${RIDE:-}" ] || [ -n "${AIM:-}" ] || [ -n "${UNLOCK:-}" ]; then
    echo "build_cia: AUTOQUIT, CAPTURE, PROFILE, WALK, GIVE, RIDE, AIM or UNLOCK is set -- this is a harness build, not a release." >&2
    echo "            Rebuild without them (after make clean) for a CIA." >&2
    exit 2
fi

# The environment is not the binary: the Makefile does not track -D changes, so
# a plain `make` after a harness build without `make clean` can relink the old
# harness objects. Ask the ELF itself; each literal exists only under its flag.
for lit in '/3ds/netload_stub.3dsx' 'sdmc:/3ds/mdk/hwcap.bmp.log' 'profile: console model=' \
           'main: harness walk WALK=' 'profile: arena-entry table' 'main: harness aids'; do
    if grep -aqF -- "$lit" "$ELF"; then
        echo "build_cia: $ELF carries the harness literal '$lit' -- it is a harness build." >&2
        echo "            make -C mdk_3ds clean && make -C mdk_3ds, then build the CIA." >&2
        exit 2
    fi
done

# The banner must be a compiled HOME-menu banner (a CBMD: the 3D model, then a
# CWAV sound), not a picture. makerom copies -banner into the ExeFS byte for
# byte; given a PNG it builds a CIA whose banner the HOME Menu cannot read.
# MDK's is banner/v3/mdk_banner_v3.bnr (owner-approved 2026-11-27).
if [ ! -s "$BANNER" ] || [ "$(head -c 4 "$BANNER")" != "CBMD" ]; then
    echo "build_cia: $BANNER is not a compiled banner (no CBMD header)." >&2
    echo "            Use banner/v3/mdk_banner_v3.bnr (see banner/v3/README.md)." >&2
    exit 2
fi
if ! grep -aqF CWAV "$BANNER"; then
    echo "build_cia: $BANNER has no CWAV sound block." >&2
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

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
RSF_SRC="$SCRIPT_DIR/../cia/mdk.rsf"
if [ ! -f "$RSF_SRC" ]; then
    echo "build_cia: RSF template missing: $RSF_SRC" >&2
    exit 2
fi

# makerom resolves RomFs RootPath relative to its own working directory,
# so hand it an absolute path substituted into a scratch copy of the RSF.
ROMFS_ABS=$(cd "$ROMFS" && pwd)
TMP_RSF=$(mktemp /tmp/mdk-rsf.XXXXXX)
trap 'rm -f "$TMP_RSF"' EXIT
sed "s|@ROMFS_DIR@|$ROMFS_ABS|" "$RSF_SRC" > "$TMP_RSF"
if grep -q "@ROMFS_DIR@" "$TMP_RSF"; then
    echo "build_cia: RSF substitution failed for $RSF_SRC" >&2
    exit 2
fi

echo "build_cia: $MAKEROM -f cia -o $OUT"
"$MAKEROM" -f cia -o "$OUT" \
    -rsf "$TMP_RSF" \
    -elf "$ELF" \
    -icon "$SMDH" \
    -banner "$BANNER"

if [ ! -s "$OUT" ]; then
    echo "build_cia: makerom produced no output at $OUT" >&2
    exit 1
fi

echo "build_cia: wrote $OUT ($(stat -c%s "$OUT") bytes)"
