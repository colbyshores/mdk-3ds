#!/usr/bin/env bash
# ============================================================================
# stage_romfs.sh — stage the MDK game data into the 3DS's RomFS tree.
#
# USAGE:  stage_romfs.sh <output-directory>
#
# THE GAME DATA IS THE USER'S OWN COPY of a commercial game. It is not
# committed, and it is not redistributed: this script stages it from
# MDK_DATA_DIR, or else from <repo>/MDK. Without it the script FAILS: a .3dsx
# with no data in it cannot run.
#
# FAILURES ARE FATAL. A converter that fails (the arena textures, Kurt's frames,
# the music) stops the build rather than shipping a slower or silent .3dsx. The
# explicit opt-outs are MDK_ARENA_ETC1=0 and MDK_KURT_ETC1=0: the game then
# expands those textures from their palette indices at run time.
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
    echo "stage_romfs: ERROR: no MDK game data. Set MDK_DATA_DIR to the folder MDK is" >&2
    echo "             installed in, or copy (or link) that folder to $REPO/MDK." >&2
    exit 1
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
# level load). A failed conversion stops the build; MDK_KURT_ETC1=0 skips it on
# purpose, and the game then draws every frame from its palette indices.
KURT_ETC1_DIR="${KURT_ETC1_DIR:-$REPO/build-kurt-etc1}"
if [ "${MDK_KURT_ETC1:-1}" != "0" ]; then
    if ! MDK_DATA_DIR="$SRC" python3 "$HERE/kurt_etc1_convert.py" "$KURT_ETC1_DIR" >&2 ||
       [ ! -f "$KURT_ETC1_DIR/kurt.pak" ]; then
        echo "stage_romfs: ERROR: kurt_etc1_convert.py failed (see above). MDK_KURT_ETC1=0" >&2
        echo "             builds without Kurt's compressed frames (slower level loads)." >&2
        exit 1
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

# THE LEVELS' MUSIC AS DSP-ADPCM (audio_dsp_3ds.h): committed in mdk_3ds/music
# (levelN/<name>.dsp, made from the soundtrack by music_dsp_convert.py; no
# download, no dependency on the soundtrack at build time), hard-linked here as
# music/levelN/. The audio worker streams them; the bank never reads a track.
#
# WITHOUT mdk_3ds/music the tracks are made here from the game data alone
# (music_dsp_convert.py with the original as both trees: ~15 s, ffmpeg) into
# $MUSIC_BUILD_DIR (default <repo>/build-music, kept across `make clean`). The
# tracks the original data has come out byte-identical to the committed ones;
# the three the soundtrack adds (level 3 AMB_DRIP and H9WATER, level 7 TRACK12)
# are absent, so those arenas are silent, as they are with the original game.
# HOST_CC: the encoder is a host program, and the 3DS Makefile exports
# CC=arm-none-eabi-gcc.
MUSIC_DIR="$HERE/../music"
if [ ! -d "$MUSIC_DIR" ]; then
    MUSIC_DIR="${MUSIC_BUILD_DIR:-$REPO/build-music}"
    if [ ! -f "$MUSIC_DIR/CONVERT_REPORT.txt" ]; then
        echo "stage_romfs:   music -- mdk_3ds/music is absent; converting the game's own music"
        rm -rf "$MUSIC_DIR"
        if ! CC="${HOST_CC:-cc}" python3 "$HERE/music_dsp_convert.py" "$SRC" "$SRC" "$MUSIC_DIR" >&2; then
            rm -rf "$MUSIC_DIR"
            echo "stage_romfs: ERROR: music_dsp_convert.py failed (see above)." >&2
            exit 1
        fi
    fi
fi
while IFS= read -r -d '' file; do
    rel="${file#"$MUSIC_DIR"/}"
    mkdir -p "$OUT/music/$(dirname "$rel")"
    ln "$file" "$OUT/music/$rel" 2>/dev/null || cp "$file" "$OUT/music/$rel"
done < <(find "$MUSIC_DIR" -type f -name '*.dsp' -print0)
echo "stage_romfs:   music ($(find "$OUT/music" -name '*.dsp' | wc -l) tracks, $(du -sh "$OUT/music" | cut -f1))"

# THE ARENA TEXTURES AS ETC1/ETC1A4 (arena streaming): every arena and level
# texture of levels 3-8, expanded through its arena's palette with the 3DS's
# rule, RGB565-quantized, bled, padded and encoded with a gaussian mip chain by
# arena_etc1_convert.py into $ARENA_ETC1_DIR (default <repo>/build-arena-etc1,
# kept across `make clean`) -- incremental, seconds when nothing changed -- so
# the console never expands a palette or builds a mip at runtime. The manifest
# and textures/ are hard-linked here as orig/ (the ModImages key is the
# @<hdkey> in each file name). A failed conversion stops the build;
# MDK_ARENA_ETC1=0 skips it on purpose, and the game then draws every arena
# texture from its palette indices.
ARENA_ETC1_DIR="${ARENA_ETC1_DIR:-$REPO/build-arena-etc1}"
if [ "${MDK_ARENA_ETC1:-1}" != "0" ]; then
    if ! MDK_DATA_DIR="$SRC" python3 "$HERE/arena_etc1_convert.py" "$ARENA_ETC1_DIR" >&2; then
        echo "stage_romfs: ERROR: arena_etc1_convert.py failed (see above). MDK_ARENA_ETC1=0" >&2
        echo "             builds without the compressed arena textures (slower, hitchier)." >&2
        exit 1
    fi
fi
rm -rf "$OUT/orig"
if [ -f "$ARENA_ETC1_DIR/manifest.txt" ]; then
    mkdir -p "$OUT/orig/textures"
    ln "$ARENA_ETC1_DIR/manifest.txt" "$OUT/orig/manifest.txt" 2>/dev/null ||
        cp "$ARENA_ETC1_DIR/manifest.txt" "$OUT/orig/manifest.txt"
    if [ -f "$ARENA_ETC1_DIR/byname.txt" ]; then
        ln "$ARENA_ETC1_DIR/byname.txt" "$OUT/orig/byname.txt" 2>/dev/null ||
            cp "$ARENA_ETC1_DIR/byname.txt" "$OUT/orig/byname.txt"
        # THE NAME INDEX AS A TABLE (orig/byname.bin, mdk_core/src/formats/
        # byname_index.h), made from the two files just staged, so it always
        # matches them: the console reads its level's block instead of parsing
        # byname.txt (2.5 s a level load on the Old 3DS). Its stamp is checked
        # at load; a table that does not match is refused and byname.txt is
        # parsed as before. A failed table stops the build.
        if ! python3 "$HERE/byname_index.py" "$OUT/orig" >&2; then
            echo "stage_romfs: ERROR: byname_index.py failed (see above)." >&2
            exit 1
        fi
    fi
    while IFS= read -r -d '' file; do
        rel="${file#"$ARENA_ETC1_DIR"/}"
        mkdir -p "$OUT/orig/$(dirname "$rel")"
        ln "$file" "$OUT/orig/$rel" 2>/dev/null || cp "$file" "$OUT/orig/$rel"
    done < <(find "$ARENA_ETC1_DIR/textures" -type f -name '*.t3x' -print0)
    # The skies' de-dithered ETC1 slices (romfs:/orig/sky/level<L>_<k>.t3x), drawn
    # with HD on (level_load_3ds.cpp buildSky3ds); absent, the sky is expanded.
    if [ -d "$ARENA_ETC1_DIR/sky" ]; then
        mkdir -p "$OUT/orig/sky"
        for file in "$ARENA_ETC1_DIR"/sky/*.t3x; do
            [ -f "$file" ] || continue
            ln "$file" "$OUT/orig/sky/" 2>/dev/null || cp "$file" "$OUT/orig/sky/"
        done
    fi
    # The mirror cubes, 512x512 ETC1 faces baked from the mirror panorama
    # (romfs:/orig/mirror/level<L>_<shift>.bin, mdk_core/src/render/mirror_cube.h),
    # read whole at level load (level_load_3ds.cpp buildMirrorCubes3ds); absent or
    # stale, the game builds its 128x128 cubes at load as before.
    if [ -d "$ARENA_ETC1_DIR/mirror" ]; then
        mkdir -p "$OUT/orig/mirror"
        for file in "$ARENA_ETC1_DIR"/mirror/*.bin; do
            [ -f "$file" ] || continue
            ln "$file" "$OUT/orig/mirror/" 2>/dev/null || cp "$file" "$OUT/orig/mirror/"
        done
    fi
    echo "stage_romfs:   orig ($(find "$OUT/orig" -name '*.t3x' | wc -l) textures, $(du -sh "$OUT/orig" | cut -f1))"
else
    echo "stage_romfs:   (skip) orig -- no compressed arena set; arena textures are expanded"
fi

# The HD pack is read from the SD card (sdmc:/3ds/mdk/hd), never the RomFS: a
# romfs hd/ left by an older build is removed so it does not ship.
rm -rf "$OUT/hd"

# A manifest, so the build can verify the staging WITHOUT depending on the
# bytes (which are not in the repository).
{
    echo "# mdk 3ds romfs manifest"
    echo "# generated by stage_romfs.sh -- the DATA ITSELF is not in the repository."
    ( cd "$OUT" && find . -type f -printf '%s %p\n' | sort -k2 ) || true
} > "$OUT/manifest.txt"

echo "stage_romfs: staged $(find "$OUT" -type f | wc -l) files, $(du -sh "$OUT" | cut -f1)"
