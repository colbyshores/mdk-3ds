#!/usr/bin/env bash
# ============================================================================
# check_prereqs.sh -- is everything the build needs installed?
#
#   tools/check_prereqs.sh 3ds      the 3DS .3dsx (make -C mdk_3ds runs this first)
#   tools/check_prereqs.sh linux    the Linux build (cmake)
#   tools/check_prereqs.sh hd       the 3DS HD pack (mdk_3ds/tools/build_hd_pack.sh)
#
# Prints one line per missing thing and exits 1 if anything is missing, so a
# build stops in the first second instead of after minutes of compiling, or
# worse, finishes with a .3dsx that is missing its textures or its data.
# README.md, "Prerequisites", lists the packages that provide each item.
# ============================================================================
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TARGET="${1:-}"
MISSING=0

missing() {
    echo "  MISSING: $*" >&2
    MISSING=1
}

need_cmd() { # need_cmd <command> <what it is / where it comes from>
    command -v "$1" >/dev/null 2>&1 || missing "$1 ($2)"
}

check_python() { # check_python <reason>
    if ! command -v python3 >/dev/null 2>&1; then
        missing "python3 ($1)"
        return
    fi
    python3 -c 'import numpy' 2>/dev/null || missing "Python module numpy ($1; pip install numpy)"
    python3 -c 'import PIL' 2>/dev/null || missing "Python module Pillow ($1; pip install Pillow)"
}

check_host_cxx() { # check_host_cxx <reason>
    local cxx="${HOST_CXX:-g++}"
    if ! command -v "$cxx" >/dev/null 2>&1; then
        missing "$cxx, a host C++17 compiler ($1; set HOST_CXX to use another)"
        return
    fi
    echo '#include <zlib.h>' | "$cxx" -x c++ -fsyntax-only - 2>/dev/null ||
        missing "zlib development headers for $cxx ($1; e.g. zlib1g-dev / zlib-devel)"
}

case "$TARGET" in
3ds)
    if [ -z "${DEVKITPRO:-}" ] || [ -z "${DEVKITARM:-}" ]; then
        missing "DEVKITPRO / DEVKITARM are not set (export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM)"
    else
        [ -x "$DEVKITARM/bin/arm-none-eabi-g++" ] || missing "devkitARM compiler in $DEVKITARM/bin (devkitPro package devkitARM)"
        [ -f "$DEVKITPRO/libctru/lib/libctru.a" ] || missing "libctru (devkitPro package libctru)"
        [ -f "$DEVKITPRO/libctru/lib/libcitro3d.a" ] || missing "citro3d (devkitPro package citro3d)"
        [ -f "$DEVKITPRO/portlibs/3ds/lib/libz.a" ] || missing "zlib for the 3DS (devkitPro package 3ds-zlib)"
        for tool in picasso bin2s 3dsxtool smdhtool tex3ds; do
            [ -x "$DEVKITPRO/tools/bin/$tool" ] || missing "$tool in $DEVKITPRO/tools/bin (devkitPro packages 3dstools, picasso, tex3ds, general-tools)"
        done
    fi
    need_cmd make "GNU make"

    # The textures converted at build time (stage_romfs.sh). Each has an
    # explicit opt-out; the build then draws those textures the slow way.
    if [ "${MDK_ARENA_ETC1:-1}" != "0" ] || [ "${MDK_KURT_ETC1:-1}" != "0" ]; then
        check_python "texture conversion"
        check_host_cxx "texture conversion"
    fi

    # The music: committed in mdk_3ds/music, or made from the game data.
    if [ ! -d "$REPO/mdk_3ds/music" ]; then
        check_python "music conversion"
        if ! command -v ffmpeg >/dev/null 2>&1; then
            missing "ffmpeg (music conversion, mdk_3ds/music is absent)"
        elif ! ffmpeg -hide_banner -version 2>/dev/null | grep -q -- '--enable-libsoxr'; then
            missing "an ffmpeg built with libsoxr (music conversion, mdk_3ds/music is absent)"
        fi
        need_cmd "${HOST_CC:-cc}" "a host C compiler for the music encoder; set HOST_CC to use another"
    fi

    # The game data. It goes into the .3dsx, so the build cannot do without it.
    if [ -n "${MDK_DATA_DIR:-}" ]; then
        DATA="$MDK_DATA_DIR"
        [ -d "$DATA" ] || missing "game data: MDK_DATA_DIR=$DATA is not a directory"
    else
        DATA="$REPO/MDK"
        [ -d "$DATA" ] || missing "game data: no $DATA (copy or link your MDK folder there, or set MDK_DATA_DIR)"
    fi
    if [ -d "$DATA" ]; then
        found=0
        for entry in "$DATA"/*; do
            case "$(basename "$entry" | tr '[:upper:]' '[:lower:]')" in
                traverse) [ -d "$entry" ] && found=1 ;;
            esac
        done
        [ "$found" -eq 1 ] || missing "game data: $DATA has no TRAVERSE folder; point it at the folder MDK is installed in"
    fi
    ;;
hd)
    check_python "the HD mod and pack"
    check_host_cxx "the HD texture export"
    [ -x "${TEX3DS:-${DEVKITPRO:-/opt/devkitpro}/tools/bin/tex3ds}" ] ||
        missing "tex3ds (devkitPro package tex3ds; or set TEX3DS)"
    # Not `| grep -q`: with pipefail, grep quitting early kills ldconfig (SIGPIPE).
    case "$(ldconfig -p 2>/dev/null || /sbin/ldconfig -p 2>/dev/null)" in *libvulkan.so.1*) ;; *) false ;; esac ||
        missing "the Vulkan loader libvulkan.so.1 (e.g. libvulkan1) and a GPU driver, for Real-ESRGAN"
    ;;
linux)
    need_cmd cmake "CMake 3.16 or newer"
    need_cmd make "GNU make"
    command -v c++ >/dev/null 2>&1 || command -v g++ >/dev/null 2>&1 ||
        command -v clang++ >/dev/null 2>&1 || missing "a C++17 compiler (g++ or clang++)"
    command -v glslangValidator >/dev/null 2>&1 || command -v glslang >/dev/null 2>&1 ||
        missing "glslangValidator (package glslang-tools, or the Vulkan SDK)"
    need_cmd python3 "the shader embedding step"
    if command -v pkg-config >/dev/null 2>&1; then
        pkg-config --exists sdl3 || missing "SDL3 development files (pkg-config sdl3; e.g. libsdl3-dev, or SDL3 built from source)"
        pkg-config --exists zlib || missing "zlib development files (e.g. zlib1g-dev / zlib-devel)"
    else
        echo "  note: no pkg-config, so SDL3 and zlib are left to cmake to find" >&2
    fi
    ;;
*)
    echo "usage: $0 3ds|linux|hd" >&2
    exit 2
    ;;
esac

if [ "$MISSING" -ne 0 ]; then
    echo "check_prereqs: the $TARGET build cannot run; install the items above (README.md, Prerequisites)." >&2
    exit 1
fi
echo "check_prereqs: $TARGET prerequisites found"
