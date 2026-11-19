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
# time -- and hard-linked here as kurt/ with its own manifest.txt, CASE KEPT (the
# game opens these by the manifest's exact names, not through the data's
# case-insensitive resolver). Not fatal: without the set (no tex3ds, no numpy or
# PIL, a failed encode) the game draws every frame from its palette indices.
KURT_ETC1_DIR="${KURT_ETC1_DIR:-$REPO/build-kurt-etc1}"
if [ "${MDK_KURT_ETC1:-1}" != "0" ]; then
    if MDK_DATA_DIR="$SRC" python3 "$HERE/kurt_etc1_convert.py" "$KURT_ETC1_DIR" >&2; then
        :
    else
        echo "stage_romfs:   (warning) kurt_etc1_convert.py failed; Kurt is expanded" >&2
    fi
fi
if [ -f "$KURT_ETC1_DIR/manifest.txt" ] && [ -d "$KURT_ETC1_DIR/textures" ]; then
    mkdir -p "$OUT/kurt"
    ln "$KURT_ETC1_DIR/manifest.txt" "$OUT/kurt/manifest.txt" 2>/dev/null ||
        cp "$KURT_ETC1_DIR/manifest.txt" "$OUT/kurt/manifest.txt"
    ( cd "$KURT_ETC1_DIR" && find textures -type f -name '*.t3x' -print0 ) |
        while IFS= read -r -d '' rel; do
            mkdir -p "$OUT/kurt/$(dirname "$rel")"
            ln "$KURT_ETC1_DIR/$rel" "$OUT/kurt/$rel" 2>/dev/null ||
                cp "$KURT_ETC1_DIR/$rel" "$OUT/kurt/$rel"
        done
    echo "stage_romfs:   kurt ($(find "$OUT/kurt" -name '*.t3x' | wc -l) ETC1A4 frames)"
else
    echo "stage_romfs:   (skip) kurt -- no compressed set; Kurt is expanded"
fi

# A manifest, so the build can verify the staging WITHOUT depending on the
# bytes (which are not in the repository).
{
    echo "# mdk 3ds romfs manifest"
    echo "# generated by stage_romfs.sh -- the DATA ITSELF is not in the repository."
    ( cd "$OUT" && find . -type f -printf '%s %p\n' | sort -k2 ) || true
} > "$OUT/manifest.txt"

echo "stage_romfs: staged $(find "$OUT" -type f | wc -l) files, $(du -sh "$OUT" | cut -f1)"
