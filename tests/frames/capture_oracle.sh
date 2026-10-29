#!/usr/bin/env bash
# capture_oracle.sh -- one frame from the C# oracle, written as a 32-bit BMP.
#
# This is the REFERENCE SOURCE for the frame goldens (SPEC-COMPLETION 3.4): a
# reference frame changes only when the oracle changes, never because the C++
# build produced something. The bytes are ROM-derived pixels, so they live in
# the scratch directory this script writes to and are never committed; only the
# hash manifest that regenerate_reference.sh derives from them is.
#
#   usage: tests/frames/capture_oracle.sh <out.bmp>
#
# Environment (all optional, defaults are what this machine was set up with):
#   MDK_FRAME_ORACLE_DIR   the scratch build tree (default ${TMPDIR:-/tmp}/mdk-frame-oracle)
#   MDK_DOTNET             the dotnet host (default /tmp/kilo/dotnet10/dotnet)
#   DOTNET_ROOT            must point at the same tree as MDK_DOTNET's root
#   NUGET_PACKAGES         the package cache. There is no network, so it must
#                          already hold ppy.SDL3-cs; pass it through if it is
#                          not in ~/.nuget/packages.
#   MDK_GLSLANG            glslangValidator (default ~/.local/opt/glslang/bin/glslangValidator)
#   MDK_DATA_DIR           the game data (default <repo>/MDK)
#   MDK_FRAME_LEVEL / MDK_FRAME_SIZE   the pinned scene (default level 3, 320x240)
#   MDK_FRAME_SEED / MDK_FRAME_CAPTURE accepted for symmetry with the C++
#                          capture and ignored: the oracle's first frame is
#                          already deterministic (its test mode advances a
#                          fixed step and nothing on frame 1 draws on the
#                          unseeded Random), and a later frame needs the
#                          oracle's own --frames mode.
#   VK_ICD_FILENAMES       the lavapipe ICD, for a GPU-less host
#
# Why the namespace: /tmp/.X11-unix and /tmp/.X99-lock belong to another user
# on this machine and cannot be removed, so a plain `Xvfb :99` cannot start.
# AGENTS.md P7 still requires the dedicated display and forbids the operator's,
# so the display is created inside a private mount namespace with its own
# /tmp/.X11-unix. The display used is printed, because a capture with no
# display recorded does not count as evidence.
set -uo pipefail

OUT=${1:?usage: capture_oracle.sh <out.bmp>}
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)

SCRATCH=${MDK_FRAME_ORACLE_DIR:-${TMPDIR:-/tmp}/mdk-frame-oracle}
DOTNET=${MDK_DOTNET:-/tmp/kilo/dotnet10/dotnet}
export DOTNET_ROOT=${DOTNET_ROOT:-/tmp/kilo/dotnet10}
GLSLANG=${MDK_GLSLANG:-}
if [ -z "$GLSLANG" ]; then
    # The distro package, the Vulkan SDK, and the user-local prebuilt this
    # machine was set up with (AGENTS.md: record the tool, do not assume it).
    for candidate in "$HOME/.local/opt/glslang/bin/glslangValidator" \
                     /home/coleshores/.local/opt/glslang/bin/glslangValidator \
                     "${VULKAN_SDK:-/nonexistent}/bin/glslangValidator" \
                     /usr/local/bin/glslangValidator /usr/bin/glslangValidator; do
        if [ -x "$candidate" ]; then GLSLANG=$candidate; break; fi
    done
    GLSLANG=${GLSLANG:-glslangValidator}
fi
DATA=${MDK_DATA_DIR:-$ROOT/MDK}
LEVEL=${MDK_FRAME_LEVEL:-3}
SIZE=${MDK_FRAME_SIZE:-320x240}
BIN="$SCRATCH/src/Mdk.App/bin/Debug/net10.0/mdk_sdl"

# There is no network, so the restore reads ppy.SDL3-CS out of a local package
# cache. Default it to the first cache that actually exists rather than to
# ~/.nuget/packages, which is not where it lives on every machine here; an
# unset NUGET_PACKAGES otherwise fails with NU1101 minutes into the build.
if [ -z "${NUGET_PACKAGES:-}" ]; then
    for cache in "$HOME/.nuget/packages" /home/coleshores/.nuget/packages; do
        if [ -d "$cache" ]; then export NUGET_PACKAGES="$cache"; break; fi
    done
fi
if [ -z "${NUGET_PACKAGES:-}" ] || [ ! -d "$NUGET_PACKAGES" ]; then
    echo "capture_oracle: no NuGet package cache (set NUGET_PACKAGES to one holding ppy.SDL3-CS)" >&2
    exit 1
fi

if [ ! -x "$DOTNET" ]; then
    echo "capture_oracle: no dotnet host at $DOTNET (set MDK_DOTNET)" >&2
    exit 1
fi
if [ ! -x "$GLSLANG" ] && ! command -v "$GLSLANG" >/dev/null 2>&1; then
    echo "capture_oracle: no glslangValidator found (set MDK_GLSLANG)" >&2
    exit 1
fi
if [ ! -f "$DATA/TRAVERSE/TRAVSPRT.BNI" ]; then
    echo "capture_oracle: no game data in $DATA (set MDK_DATA_DIR)" >&2
    exit 1
fi

# --- the source tree -------------------------------------------------------
# Copied, not symlinked: the build writes obj/ and bin/ next to the sources,
# and the repository's own src/ must stay untouched.
mkdir -p "$SCRATCH"
for item in src assets shaders Directory.Build.props; do
    if [ ! -e "$ROOT/$item" ]; then
        echo "capture_oracle: $ROOT/$item is missing" >&2
        exit 1
    fi
    rm -rf "${SCRATCH:?}/$item"
    # cp -R, not cp -a: the working tree lives on a mount that cannot carry the
    # source's mode/ownership, so -a spends a line of "Operation not supported"
    # per file and changes nothing.
    cp -R "$ROOT/$item" "$SCRATCH/$item"
done

# --- the shaders -----------------------------------------------------------
# The repo's build uses dxc; this machine has no dxc that emits SPIR-V, but it
# does have glslangValidator, and the HLSL compiles with it unchanged (the
# entry points are vs_main/ps_main and the bindings are already Vulkan-shaped).
# The names the csproj expects are <name>.vs.spv / <name>.ps.spv under
# src/Mdk.Engine/obj/shaders -- so the OUTPUT suffix is ps, while the glslang
# STAGE name is frag. Writing <name>.frag.spv leaves the csproj looking at a
# stale (or missing) <name>.ps.spv, which is a silent empty shader.
#
# Two glslang specifics that are not optional, both found by it refusing to
# produce anything without them:
#   * -D -- input is HLSL. glslang auto-detects from a bare .hlsl suffix, but
#     the compound <name>.<stage>.hlsl it needs for -S is not enough on the
#     16.5.0 build here, and without -D the #include is parsed as GLSL and
#     fails with "required extension not requested".
#   * -I<dir> with NO space. glslang's own usage text: "include path must
#     immediately follow option (no spaces)".
SHADER_OUT="$SCRATCH/src/Mdk.Engine/obj/shaders"
mkdir -p "$SHADER_OUT"
stage=$(mktemp -d)
shopt -s nullglob
for shader in "$SCRATCH"/shaders/*.hlsl; do
    name=$(basename "$shader" .hlsl)
    for spec in "vs vert vs_main" "ps frag ps_main"; do
        read -r out_kind gstage entry <<<"$spec"
        cp "$shader" "$stage/$name.$gstage.hlsl"
        if ! "$GLSLANG" -D -V --target-env vulkan1.3 -S "$gstage" -e "$entry" \
                -I"$SCRATCH/shaders" -o "$SHADER_OUT/$name.$out_kind.spv" \
                "$stage/$name.$gstage.hlsl" >"$stage/$name.$out_kind.log" 2>&1; then
            echo "capture_oracle: glslang failed on $name ($out_kind); see $stage/$name.$out_kind.log" >&2
            tail -5 "$stage/$name.$out_kind.log" >&2
            rm -rf "$stage"
            exit 1
        fi
        # glslang exits 0 for a shader it compiled to nothing; an empty SPIR-V
        # module reaches SDL as a pipeline that draws nothing, which looks like
        # a black frame and not like a build failure. Refuse it here instead.
        if [ ! -s "$SHADER_OUT/$name.$out_kind.spv" ]; then
            echo "capture_oracle: glslang wrote an empty $name.$out_kind.spv" >&2
            tail -5 "$stage/$name.$out_kind.log" >&2
            rm -rf "$stage"
            exit 1
        fi
    done
done
rm -rf "$stage"

# --- the build -------------------------------------------------------------
# PublishAot is off: the AOT toolchain is not what this harness needs and it
# takes minutes. --ignore-failed-sources because there is no network: the
# packages come from the local NuGet cache.
if ! (cd "$SCRATCH" && "$DOTNET" build src/Mdk.App/Mdk.App.csproj \
        -p:PrecompiledShaders=spirv -p:PublishAot=false \
        --ignore-failed-sources -c Debug) >"${OUT}.build.log" 2>&1; then
    echo "capture_oracle: the oracle build failed; see ${OUT}.build.log" >&2
    tail -20 "${OUT}.build.log" >&2
    exit 1
fi
if [ ! -x "$BIN" ]; then
    echo "capture_oracle: the build produced no $BIN" >&2
    exit 1
fi

# --- the capture -----------------------------------------------------------
XDIR=$(mktemp -d /tmp/mdk-frame-x11.XXXXXX)
RUNDIR=$(mktemp -d /tmp/mdk-frame-run.XXXXXX)
chmod 700 "$RUNDIR"
rm -f "$OUT"

# The inner script goes in a FILE, not an inline single-quoted string: an
# apostrophe in a comment inside that string closes the quote and silently
# re-splits every following word (the arguments arrive as $0/$1/$2 garbage and
# the mount fails with "special device one does not exist"). A quoted heredoc
# has no such trap, and it keeps the script readable.
INNER=$(mktemp /tmp/mdk-frame-inner.XXXXXX.sh)
cat >"$INNER" <<'INNER_EOF'
set -uo pipefail
mount --bind "$1" /tmp/.X11-unix
# -nolock: the lock file would collide with the stale one on the host, and
# inside this namespace nothing else is competing for the display.
Xvfb :99 -screen 0 1280x1024x24 -nolock >"$2" 2>&1 &
xpid=$!
trap 'kill "$xpid" 2>/dev/null || true' EXIT
sleep 1
cd "$(dirname "$3")"
DOTNET_ROOT="$4" XDG_RUNTIME_DIR="$5" MDK_DATA_DIR="$6" MDK_USER_DIR="$5" \
    DISPLAY=:99 timeout 180 "$3" --hidden --original --gpu=vulkan \
        --level="$7" --size="$8" --screenshot="$9" --wait=0 --mute
INNER_EOF

unshare --user --map-root-user -m bash "$INNER" \
    "$XDIR" "${OUT}.xvfb.log" "$BIN" "$DOTNET_ROOT" "$RUNDIR" "$DATA" \
    "$LEVEL" "$SIZE" "$OUT"
rc=$?
rm -f "$INNER"
rm -rf "$XDIR" "$RUNDIR"

if [ $rc -ne 0 ] || [ ! -s "$OUT" ]; then
    echo "capture_oracle: the oracle run failed (rc=$rc); see ${OUT}.xvfb.log" >&2
    exit 1
fi
echo "capture_oracle: wrote $OUT (level $LEVEL, $SIZE, display :99 in a private namespace)"
