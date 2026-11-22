#!/usr/bin/env bash
# ============================================================================
# stage_romfs.sh — stage the MDK game data into the 3DS's RomFS tree.
#
# USAGE:  stage_romfs.sh <output-directory>
#
# THE GAME DATA IS THE USER'S OWN COPY of a commercial game. It is not
# committed, and it is not redistributed: this script stages it from the
# tree the formats library already knows how to find (MDK_DATA_DIR, the
# mdk_paths.cfg next to the program, the folders above the program, the GOG
# and Steam folders) or from ../MDK.
#
# WHY THE TREE IS LOWERCASED:
#
#   The 3DS's filesystem is CASE-SENSITIVE and the game's data is not: the
#   game asks for MISC/MDKFONT.FTI and the GOG tree ships MISC/mdkfont.fti.
#   The formats library's case-insensitive resolver handles that for the
#   game's own file opens, but the RESOLVER IS A PORTABLE LOOKUP AGAINST A
#   STAGED TREE, so the staged tree has to be consistent. Staging everything
#   lower-case gives one canonical spelling, and the resolver finds it
#   whatever case the game asks for.
#
# WHY THE TREE IS HARD-LINKED, NOT COPIED:
#
#   3dsxtool does NOT follow symlinks, so a symlinked tree would be silently
#   empty in the .3dsx. Hard links cost no space and are followed.
#
# WHY NOT `cp -r` WITH A RENAME:
#
#   The data is 80 MB and there are thousands of files; a rename pass over
#   it is slow. The staging is per-file and only touches the top of the
#   tree, so it is quick.
# ============================================================================
set -euo pipefail

OUT="${1:-}"
if [ -z "$OUT" ]; then
    echo "usage: $0 <output-directory>" >&2
    exit 2
fi

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"

# ---- find the data --------------------------------------------------------
SRC=""
if [ -n "${MDK_DATA_DIR:-}" ] && [ -d "$MDK_DATA_DIR" ]; then
    SRC="$MDK_DATA_DIR"
elif [ -d "$REPO/MDK" ]; then
    SRC="$REPO/MDK"
else
    echo "stage_romfs: no MDK data found. Set MDK_DATA_DIR, or put the game's" >&2
    echo "               data in $REPO/MDK. (The .3dsx still builds without it;" >&2
    echo "               the game then reports the data as missing.)" >&2
    mkdir -p "$OUT"
    exit 0
fi

echo "stage_romfs: staging from $SRC"

rm -rf "$OUT"
mkdir -p "$OUT"

# The directories the game reads, and what goes in them. The 3DS build needs
# the level data, the shared archives, the 2D resources and the fall.
stage_dir() {
    local rel="$1"
    local from="$2"
    if [ ! -d "$from" ]; then
        echo "stage_romfs:   (skip) $rel -- not present"
        return 0
    fi
    local dst="$OUT/$rel"
    mkdir -p "$dst"
    # find -print0 and a per-file loop: the data has spaces in some names.
    while IFS= read -r -d '' file; do
        local name
        name="$(basename "$file" | tr '[:upper:]' '[:lower:]')"
        local target="$dst/$name"
        [ -e "$target" ] && continue
        ln "$file" "$target" 2>/dev/null || cp "$file" "$target"
    done < <(find "$from" -mindepth 1 -maxdepth 1 -type f -print0)
    echo "stage_romfs:   $rel"
}

# The MISC folder: the font, the movies, the sound banks, the stats.
stage_dir misc "$SRC/MISC"

# MISC/FLIC: stage_dir is one level deep, so the main menu's video never reached
# the RomFS and the menu stayed on MDKOPT with no slideshow (the slideshow starts
# when MDK12.FLC ends). Only MDK12.FLC (632 KB) is read by the game; pie.flc and
# shiny.flc (16 MB) are not, and MDKEND.FLC/MDKBZK.MVE are not in the GOG data.
mdk12="$(find "$SRC/MISC" -maxdepth 2 -ipath '*/flic/mdk12.flc' -print -quit)"
if [ -n "$mdk12" ]; then
    mkdir -p "$OUT/misc/flic"
    ln "$mdk12" "$OUT/misc/flic/mdk12.flc" 2>/dev/null || cp "$mdk12" "$OUT/misc/flic/mdk12.flc"
    echo "stage_romfs:   misc/flic (mdk12.flc)"
else
    echo "stage_romfs:   (skip) misc/flic -- MDK12.FLC not present; the menu has no video"
fi

# The levels. The game's level data lives in TRAVERSE/LEVELn.
for level in "$SRC"/TRAVERSE/LEVEL*; do
    [ -d "$level" ] || continue
    local_name="$(basename "$level" | tr '[:upper:]' '[:lower:]')"
    stage_dir "traverse/$local_name" "$level"
done

# The shared archives at the top of TRAVERSE.
if [ -d "$SRC/TRAVERSE" ]; then
    mkdir -p "$OUT/traverse"
    while IFS= read -r -d '' file; do
        name="$(basename "$file" | tr '[:upper:]' '[:lower:]')"
        target="$OUT/traverse/$name"
        [ -e "$target" ] && continue
        ln "$file" "$target" 2>/dev/null || cp "$file" "$target"
    done < <(find "$SRC/TRAVERSE" -mindepth 1 -maxdepth 1 -type f -print0)
fi

# The fall (FALL3D) and the stream.
stage_dir fall3d "$SRC/FALL3D"
stage_dir stream "$SRC/STREAM"

# KURT'S COMPRESSED FRAMES (EnhancedToggles::kurtCompressed): his original
# TRAVSPRT.BNI frames as ETC1A4, made from the same data by
# kurt_etc1_convert.py into $KURT_ETC1_DIR (default <repo>/build-kurt-etc1, kept
# across `make clean`) -- incremental, ~2 s when nothing changed, ~2 min the first
# time -- and packed there into ONE file, kurt.pak (kurt_pack.py;
# mdk_core/src/formats/kurt_pack.h), which is all that is hard-linked here, as
# kurt/kurt.pak. The 408 loose .t3x files and their manifest stay in the work
# directory for the next incremental run and are NOT staged: the game opens the
# pack once at level load (408 files opened one by one cost 4.6 s of an Old-3DS
# level load). Not fatal: without the pack (no tex3ds, no numpy or PIL, a failed
# encode) the game draws every frame from its palette indices.
KURT_ETC1_DIR="${KURT_ETC1_DIR:-$REPO/build-kurt-etc1}"
if [ "${MDK_KURT_ETC1:-1}" != "0" ]; then
    if MDK_DATA_DIR="$SRC" python3 "$HERE/kurt_etc1_convert.py" "$KURT_ETC1_DIR" >&2; then
        :
    else
        echo "stage_romfs:   (warning) kurt_etc1_convert.py failed; Kurt is expanded" >&2
    fi
fi
if [ -f "$KURT_ETC1_DIR/kurt.pak" ]; then
    mkdir -p "$OUT/kurt"
    ln "$KURT_ETC1_DIR/kurt.pak" "$OUT/kurt/kurt.pak" 2>/dev/null ||
        cp "$KURT_ETC1_DIR/kurt.pak" "$OUT/kurt/kurt.pak"
    echo "stage_romfs:   kurt (kurt.pak, $(stat -c %s "$OUT/kurt/kurt.pak") bytes)"
else
    echo "stage_romfs:   (skip) kurt -- no compressed set; Kurt is expanded"
fi

# THE ARENA TEXTURES AS ETC1/ETC1A4 (arena streaming): every arena and level
# texture of levels 3-8, expanded through its arena's palette with the 3DS's
# rule, RGB565-quantized, bled, padded and encoded with a gaussian mip chain by
# arena_etc1_convert.py into $ARENA_ETC1_DIR (default <repo>/build-arena-etc1,
# kept across `make clean`) -- incremental, seconds when nothing changed -- so
# the console never expands a palette or builds a mip at runtime. The manifest
# and textures/ are hard-linked here as orig/ (the ModImages key is the
# @<hdkey> in each file name). Not fatal: without the set (no tex3ds, no numpy
# or PIL, a failed encode) the game draws every arena texture from its palette
# indices.
ARENA_ETC1_DIR="${ARENA_ETC1_DIR:-$REPO/build-arena-etc1}"
if [ "${MDK_ARENA_ETC1:-1}" != "0" ]; then
    if MDK_DATA_DIR="$SRC" python3 "$HERE/arena_etc1_convert.py" "$ARENA_ETC1_DIR" >&2; then
        :
    else
        echo "stage_romfs:   (warning) arena_etc1_convert.py failed; arena textures are expanded" >&2
    fi
fi
rm -rf "$OUT/orig"
if [ -f "$ARENA_ETC1_DIR/manifest.txt" ]; then
    mkdir -p "$OUT/orig/textures"
    ln "$ARENA_ETC1_DIR/manifest.txt" "$OUT/orig/manifest.txt" 2>/dev/null ||
        cp "$ARENA_ETC1_DIR/manifest.txt" "$OUT/orig/manifest.txt"
    while IFS= read -r -d '' file; do
        rel="${file#"$ARENA_ETC1_DIR"/}"
        mkdir -p "$OUT/orig/$(dirname "$rel")"
        ln "$file" "$OUT/orig/$rel" 2>/dev/null || cp "$file" "$OUT/orig/$rel"
    done < <(find "$ARENA_ETC1_DIR/textures" -type f -name '*.t3x' -print0)
    echo "stage_romfs:   orig ($(find "$OUT/orig" -name '*.t3x' | wc -l) textures, $(du -sh "$OUT/orig" | cut -f1))"
else
    echo "stage_romfs:   (skip) orig -- no compressed arena set; arena textures are expanded"
fi

# THE HD PACK IN THE RomFS (harness builds only: `make AUTOQUIT=90 LEVEL=n
# HDROMFS=1` exports MDK_HD_ROMFS_LEVEL). One level's textures and every 2D
# image, hard-linked from build-hd-pack/ with the manifest lines that name them,
# so the appliance -- whose SD card the hardware loop never writes -- can run the
# HD stream from romfs:/hd. Without the variable no hd/ is staged, and a stale
# one from an earlier harness build is removed.
HD_PACK_DIR="${HD_PACK_DIR:-$REPO/build-hd-pack}"
if [ -n "${MDK_HD_ROMFS_LEVEL:-}" ]; then
    if [ ! -f "$HD_PACK_DIR/manifest.txt" ]; then
        echo "stage_romfs: HDROMFS asked for, but $HD_PACK_DIR/manifest.txt is missing" >&2
        exit 1
    fi
    rm -rf "$OUT/hd"
    mkdir -p "$OUT/hd"
    {
        grep '^#' "$HD_PACK_DIR/manifest.txt" || true
        grep -E "^(textures/LEVEL${MDK_HD_ROMFS_LEVEL}/|images/)" "$HD_PACK_DIR/manifest.txt" || true
    } > "$OUT/hd/manifest.txt"
    while IFS=$'\t' read -r key _; do
        case "$key" in '#'*|'') continue ;; esac
        mkdir -p "$OUT/hd/$(dirname "$key")"
        ln "$HD_PACK_DIR/$key.t3x" "$OUT/hd/$key.t3x" 2>/dev/null ||
            cp "$HD_PACK_DIR/$key.t3x" "$OUT/hd/$key.t3x"
    done < "$OUT/hd/manifest.txt"
    echo "stage_romfs:   hd (level ${MDK_HD_ROMFS_LEVEL}: $(find "$OUT/hd" -name '*.t3x' | wc -l) files, $(du -sh "$OUT/hd" | cut -f1))"
else
    rm -rf "$OUT/hd"
fi

# A manifest, so the build can verify the staging WITHOUT depending on the
# bytes (which are not in the repository).
{
    echo "# mdk 3ds romfs manifest"
    echo "# generated by stage_romfs.sh -- the DATA ITSELF is not in the repository."
    ( cd "$OUT" && find . -type f -printf '%s %p\n' | sort -k2 ) || true
} > "$OUT/manifest.txt"

echo "stage_romfs: staged $(find "$OUT" -type f | wc -l) files, $(du -sh "$OUT" | cut -f1)"
