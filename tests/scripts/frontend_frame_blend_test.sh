#!/bin/sh
# frontend_frame_blend_test.sh -- both frontends draw Kurt, the camera and the objects
# through ONE FrameBlend (game/frame_blend.h), as the C# Viewer does.
#
# WHY. Until 2026-11-27 the frontends drew the objects blended between ticks
# (transform_at of the tick share) but Kurt and the follow camera at Player::feet, the
# last step's place. A moving platform was then drawn up to a whole tick behind the Kurt
# standing on it, by an amount that changed with every frame's place between two ticks:
# the owner's "the platform begins to jiggle rapidly and kurt sinks in to it". The
# arithmetic is pinned by tick1's FrameBlend cases (kurtBlendDump against the C#
# FrameBlend); what is pinned HERE is that both frontends actually draw with it, because
# neither frontend is host-compilable as a unit.
#
# Per frontend:
#   1. Kurt's sprite is drawn at frameBlend.feet(), never at player.feet;
#   2. the objects (and the ridden board) are drawn at frameBlend.transformOf, and no
#      frontend picks a transform_at alpha of its own;
#   3. the follow camera is placed for Kurt as drawn (follow.blend with frameBlend's
#      feet and yaw);
#   4. every step records Kurt AFTER the step's follow.update (C# Viewer.cs:519), inside
#      the step loop (before its `pending -= step`), and the frame blends after the loop;
#   5. the per-frame mouse turn reaches the blend (frameBlend.turn(player.mouseLook(..)));
#   6. a new level resets it (its carrier pointers are into the freed runtime).
#
# usage: sh frontend_frame_blend_test.sh [root-dir]
set -u
ROOT=${1:-$(cd "$(dirname "$0")/../.." && pwd)}
FAILED=0

fail() {
    echo "FAIL FrontendFrameBlend $1: $2"
    FAILED=1
}

check_file() {
    name=$1
    file=$2
    [ -f "$file" ] || { fail "$name" "no $file"; return; }
    grep -q 'kurtSprite.draw(renderer, player, frameBlend.feet()' "$file" ||
        fail "$name" "Kurt's sprite is not drawn at frameBlend.feet()"
    grep -q 'kurtSprite.draw(renderer, player, player.feet' "$file" &&
        fail "$name" "Kurt's sprite is drawn at player.feet"
    grep -q 'frameBlend.transformOf(obj)' "$file" ||
        fail "$name" "the objects are not drawn at frameBlend.transformOf"
    grep -q 'frameBlend.transformOf(board)' "$file" ||
        fail "$name" "the ridden board is not drawn at frameBlend.transformOf"
    grep -q 'transform_at(' "$file" &&
        fail "$name" "the frontend picks a transform_at alpha of its own"
    grep -q 'follow.blend(&camera, player, frameBlend.feet(), frameBlend.yaw()' "$file" ||
        fail "$name" "the follow camera is not placed for Kurt as drawn"
    grep -q 'frameBlend.turn(player.mouseLook(input))' "$file" ||
        fail "$name" "the mouse's turn does not reach the blend"
    grep -q 'frameBlend.reset(player);' "$file" ||
        fail "$name" "a new level does not reset the blend"
    # 4: the record sits after the step's follow.update and before the step loop's
    # `pending -= <step>` (so it runs every step), and the frame's update after that.
    update=$(grep -n 'follow.update(&camera' "$file" | head -1 | cut -d: -f1)
    record=$(grep -n 'frameBlend.record(player, runtime.get());' "$file" | head -1 | cut -d: -f1)
    stepEnd=$(grep -n '^ *pending -= k*[sS]tep;' "$file" | head -1 | cut -d: -f1)
    blend=$(grep -n 'frameBlend.update(player' "$file" | head -1 | cut -d: -f1)
    if [ -z "$update" ] || [ -z "$record" ] || [ -z "$stepEnd" ] || [ -z "$blend" ]; then
        fail "$name" "no follow.update / frameBlend.record / pending -= step / frameBlend.update to order"
    elif [ "$record" -le "$update" ] || [ "$record" -ge "$stepEnd" ] || [ "$stepEnd" -ge "$blend" ]; then
        fail "$name" "frameBlend.record (line $record) is not between the step's follow.update (line $update) and the step's end (line $stepEnd), before the frame's frameBlend.update (line $blend)"
    fi
}

check_file 3DS "$ROOT/mdk_3ds/src/main_3ds.cpp"
check_file Linux "$ROOT/mdk_linux/src/main.cpp"

[ $FAILED = 0 ] && echo "PASS FrontendFrameBlend (both frontends draw Kurt, the camera and the objects through FrameBlend)"
exit $FAILED
