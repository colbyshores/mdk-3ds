#!/bin/sh
# main_3ds_flow_test.sh — the 3DS frontend's collision-world invariants, gated.
#
# WHY A SOURCE CHECK EXISTS. `main_3ds.cpp` includes <3ds.h> and cannot be
# compiled on a host, so nothing else in `make -C tests/scripts` can reach what
# its frame loop DOES. `pica_matrix` and `input_3ds` were split out of that tree
# because their content was arithmetic; this is not arithmetic, it is WHERE a call
# sits, which is the same reason c3d_frame_test.sh exists.
#
# THE DEFECT THIS PINS (external review 2026-11-05, run-20261105-142911 F1, High).
# The frontend pinned Kurt's solid arena ONCE, in `spawnPlayer`, and never again.
# `ArenaSpace::setSolid` is STICKY: while `_solid` is non-empty, `move()` uses only
# it (arena_space.cpp:118) and `kurts()` returns only it (:174-181), so the
# arenas-around-the-feet fallback is bypassed for the rest of the session. The C#
# refreshes it every step (`_space.SetSolid(_scripts.SolidArenas)`, Viewer.cs:533)
# and the Linux frontend refreshes it every step too (mdk_linux/src/main.cpp:530).
# A pin set once at the spawn therefore leaves Kurt colliding with the arena he was
# born in after he walks into its neighbour: the walk probes find no floor there,
# he drops through that neighbour's floor, and `bottom()` — which DOES consider any
# arena holding his feet (:160) — reports that neighbour's bottom and fires the
# fall-out death. Walking into the next room kills and respawns him.
#
# WHAT IS PINNED, and why each one is its own check:
#   1. a `setSolid` call inside the fixed-step loop body — presence anywhere in
#      the file is not enough, that is the defect;
#   2. that call AFTER `follow.update`, because the C# evaluates SetSolid after
#      UpdateCamera and the camera's clearance can shove Kurt into another arena;
#   3. the spawn pin still present, because the loop cannot supply it: a Kurt
#      standing outside every arena gets a null `arenaAt`, and with no pin at all
#      `bottom()` resolves nothing and the fall-out death never fires;
#   4. that the in-loop refresh is GUARDED on the lookup, not unconditional: an
#      unguarded `setSolid("")` when Kurt is outside every arena CLEARS the pin,
#      which is check 3's failure reached by the opposite edit.
#
# WHAT BATCH 99 ADDED (the ScriptRuntime on the console, priority item 6d). The
# frontend now owns a script runtime, which means four more things became load-
# bearing that no compiler can see:
#   6. the step loop's ONE pin reads the arena under Kurt and never the runtime's
#      current arena — which the unported connection crossing leaves at the spawn
#      arena for the whole level (review-6 F1: batch 99 pinned to it and a door walk
#      fell through the room it opened into). This is the check that keeps the
#      reverted stand-in reverted until CrossedArena lands.
#   7. the scripts tick inside the fixed-step loop between Kurt and the camera
#      (Viewer.cs:505-535), because the runtime's own 1/30 s clock assumes it is
#      called once per 1/60 s step;
#   8. the frame mixes the pointer that follows the runtime and not the standalone
#      mixer — ScriptRuntime OWNS its mixer and every script sound is written into
#      that one (script_vm.cpp:1258 opcode 89 -> ScriptRuntime::playSound), so a
#      frontend that keeps mixing its own gets a silent level with a healthy ledger;
#   9. a respawn re-points that pointer, rebinds Kurt, and re-attaches the sink, so
#      a rebuilt runtime is not a silent level after the first death;
#  10. the runtime is constructed under the asset gate, because a runtime missing
#      its CMI, bank or sprites archive is a different random stream and not a
#      degraded oracle run;
#  11. the camera's rest pitch comes from the runtime when one exists (camera_track
#      first, then the scripts' arena), with the geometric arena kept only as the
#      no-runtime stand-in.
# Check 4 was widened from "the first setSolid site" to "every setSolid site" for
# the same reason: the loop now has two, and a check that stopped at the first
# would let an unguarded second one clear the pin while the gate printed PASS.
#
# Every check is paired with a mutation arm that must turn it red; a gate that
# prints PASS over a mutant that never ran is a pass generator.
#
# WHAT THIS DOES NOT COVER, stated plainly: it reads the source, so it pins the
# STRUCTURE and not the pixels, and it cannot show that the geometric arena under
# the feet is a good stand-in for the scripts' `SolidArenas` (it is the same
# stand-in the runtime-less Linux frontend uses, and it is narrower than the
# oracle, which also lists the active second arena). Whether any of it is correct
# on a console stays UNVERIFIED (AGENTS.md P6/P8).
#
# usage: sh main_3ds_flow_test.sh [path-to-main_3ds.cpp]
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SRC=${1:-$ROOT/mdk_3ds/src/main_3ds.cpp}
[ -f "$SRC" ] || { printf '  FAIL main_3ds flow: cannot read %s\n' "$SRC"; exit 1; }

python3 - "$SRC" <<'PY'
import re
import sys

PATH = sys.argv[1]
SRC = open(PATH, errors="replace").read()


def code_lines(text):
    """(index, text) for lines that are code. Comments are dropped: this tree
    documents its own rules in prose, and prose about `setSolid` is not a call
    site (the same trap audit_c_with_classes.py documents for itself)."""
    out = []
    for i, line in enumerate(text.splitlines()):
        s = line.strip()
        if s.startswith("//") or s.startswith("*") or s.startswith("/*"):
            continue
        out.append((i, line))
    return out


def block_span_from(text, pos):
    """(start, end) offsets of the brace-matched block whose '{' is at or after
    `pos`; `end` is one past the matching '}'."""
    i = text.find("{", pos)
    if i < 0:
        return None
    depth = 0
    for j in range(i, len(text)):
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
            if depth == 0:
                return (i, j + 1)
    return None


def block_span(text, pattern):
    m = re.search(pattern, text)
    if not m:
        return None
    return block_span_from(text, m.end())


def body_span(text, name):
    """Absolute (start, end) of one lambda or function body, from
    `auto name = [&]() {` or `void name(...) {`, or None. The `auto` matters:
    the frontend's spawn block is a lambda, and a pattern that required the
    identifier at the start of the line matched nothing and made its check
    unconditionally red."""
    m = re.search(r"(?m)^[ \t]*(?:auto[ \t]+)?" + re.escape(name) +
                  r"[ \t]*(?:=[ \t]*\[&\]\([ \t]*\)|\([^;{]*\))[ \t]*\{",
                  text)
    if not m:
        return None
    sp = block_span_from(text, m.end() - 1)
    return (m.start(), sp[1]) if sp else None


def step_loop_span(text):
    """Absolute (start, end) of the fixed-step loop body, resolved by brace
    matching on its own header. Absolute so the arms can splice at these
    offsets."""
    sp = block_span(text, r"while[ \t]*\([ \t]*pending[ \t]*>=[ \t]*step[ \t]*\)")
    return sp


def step_loop(text):
    sp = step_loop_span(text)
    return text[sp[0]:sp[1]] if sp else None


def call_line(block, name):
    """0-based line index of the first code line of `block` that is a statement
    calling `name(`, or None. Whole-line match, so a call wrapped in `if (x)` on
    the same line is not mistaken for a guarded one."""
    if block is None:
        return None
    for i, line in code_lines(block):
        if re.match(r"[ \t]*" + re.escape(name) + r"\s*\(", line.strip()):
            return i
    return None


def line_depths(block):
    """Brace depth at the START of each line of `block`, relative to the block's
    own '{'. A statement written directly in the block is depth 0; one inside an
    `if` in the block is depth 1. This is what turns "inside the loop" into
    "executed every time the loop runs"."""
    depths = []
    d = -1
    depths.append(d)
    for ch in block:
        if ch == "\n":
            depths.append(d)
        elif ch == "{":
            d += 1
        elif ch == "}":
            d -= 1
    return depths


class Result:
    def __init__(self):
        self.checks = 0
        self.failed = False
        self.broke = ""

    def check(self, label, ok, detail=""):
        self.checks += 1
        if ok:
            print("  ok   %s" % label)
        else:
            self.failed = True
            if not self.broke:
                self.broke = label
            print("  FAIL %s%s" % (label, (" — " + detail) if detail else ""))


def adjudicate(text, quiet=False):
    """Run every check over `text`. Returns the tally so the arms can reuse it."""
    r = Result()
    loop = step_loop(text)
    spawn = body_span(text, "spawnPlayer")
    spawn_body = text[spawn[0]:spawn[1]] if spawn else None

    def say(label, ok, detail=""):
        if quiet:
            r.checks += 1
            if not ok:
                r.failed = True
                if not r.broke:
                    r.broke = label
            return
        r.check(label, ok, detail)

    # 1. the collision world is refreshed INSIDE the fixed-step loop.
    #    Over CODE lines, not the raw block: this tree documents its own rules in
    #    prose, and a comment inside the loop naming `setSolid(` must not satisfy a
    #    check whose docstring says prose is not a call site (review-2 F5).
    loop_code = "".join(line + "\n" for _, line in code_lines(loop)) if loop is not None else None
    say("the step loop refreshes Kurt's solid arena (review-8 F1)",
        loop_code is not None and "setSolid(" in loop_code,
        "no setSolid call in the `while (pending >= step)` body, so the pin is set "
        "wherever else it is set and never follows Kurt")

    # 2. and it is refreshed AFTER the camera step, as the C# orders them.
    ok_order = False
    if loop is not None:
        cam = call_line(loop, "follow.update")
        sol = call_line(loop, "space.setSolid")
        ok_order = cam is not None and sol is not None and sol > cam
    say("the refresh runs after follow.update, the C#'s order (Viewer.cs:517 then :533)",
        ok_order, "the camera's clearance can move Kurt, so a refresh placed before it "
                  "pins the arena he left rather than the one he is in")

    # 3. the spawn pin survives: the loop cannot supply it for a Kurt outside
    #    every arena, and without it bottom() resolves nothing.
    say("the spawn pin is still there for a Kurt outside every arena",
        spawn_body is not None and "setSolid(" in spawn_body,
        "with no pin at all bottom() answers false and the fall-out death never fires")

    # 4. the in-loop refresh is guarded, so being outside every arena keeps the
    #    last pin instead of clearing it. Depth is the load-bearing half: a call
    #    at depth 0 is unconditional whatever its argument looks like. EVERY call
    #    site is tested, not the first: a second site has been added before and an
    #    unguarded one would clear the pin while the gate printed PASS.
    depths = line_depths(loop) if loop is not None else []
    sites = [(i, line) for i, line in code_lines(loop)
             if re.search(r"setSolid\s*\(", line)] if loop is not None else []
    guarded = bool(sites) and all(
        (depths[i] if i < len(depths) else 0) >= 1 for i, _ in sites)
    say("every in-loop refresh is guarded, so an empty lookup does not clear the pin",
        guarded, "an unconditional setSolid with an empty name clears the pin, which is "
                 "check 3's failure reached from the other side")

    # 5. the guard is the arena lookup, not some other condition that happens to
    #    wrap it. `if (solid) space.setSolid(*solid)` is the shape; a guard on an
    #    unrelated flag would pass check 4 while clearing the pin.
    shape = False
    if loop is not None:
        m = re.search(r"if[ \t]*\(([A-Za-z_][A-Za-z0-9_]*)\)[ \t]*\{[ \t]*\n[ \t]*"
                      r"space\.setSolid\([ \t]*\*([A-Za-z_][A-Za-z0-9_]*)[ \t]*\)", loop)
        if m:
            shape = m.group(1) == m.group(2) and re.search(
                r"arenaAt\([^;]*\);[ \t]*\n[ \t]*const[ \t]+std::string\*[ \t]*" + re.escape(m.group(1)),
                loop) is not None or bool(re.search(
                r"std::string\*[ \t]+" + re.escape(m.group(1)) + r"[ \t]*=[ \t]*\n?[ \t]*space\.arenaAt",
                loop))
    say("the guard is the arenaAt lookup itself", shape, 
        "a guarded setSolid whose guard is not the lookup can still clear the pin")

    # 6. the step loop's pin NEVER reads the runtime's current arena. Review-6 F1
    #    (High): `currentArena_` is written exactly once, on the runtime's first
    #    tick (script_runtime.cpp:362-378), because the DTI connection crossing that
    #    moves the oracle's `CurrentArena` (CrossedArena, ScriptRuntime.cs:791-803)
    #    is not ported. Pinning to it therefore freezes the collision world at the
    #    SPAWN arena for the whole level: walk through an open door and move() --
    #    which uses ONLY the pinned arena, arena_space.cpp:118 -- finds no floor in
    #    the room Kurt entered, he drops through it, and bottom() fires the fall-out
    #    death into a respawn loop. Batch 99 shipped exactly that; this check is the
    #    pin that stops it coming back, and it is the reason the claim "with a
    #    runtime the pin is the oracle's own" was deleted rather than kept quiet.
    frozen = bool(re.search(r"setSolid\([ \t]*runtime->currentArena\(", loop or ""))
    say("the step loop pins from the arena under Kurt, never from the runtime's frozen current arena",
        not frozen and loop_code is not None and loop_code.count("setSolid(") == 1,
        "currentArena() does not move until CrossedArena is ported, so a pin that "
        "reads it is the spawn arena forever; the loop must have exactly one refresh "
        "and it must be the geometric one")

    # 7. the scripts tick INSIDE the fixed-step loop, between Kurt and the camera.
    #    The C#'s Viewer order is UpdateKurt, _scripts.Update, UpdateCamera
    #    (Viewer.cs:505-535); a runtime updated after the camera feeds the camera a
    #    tick behind, and one updated outside the loop runs at the frame rate while
    #    its own 1/30 s clock assumes it is called every 1/60 s step.
    ok_scripts = False
    if loop is not None:
        kurt = call_line(loop, "player.update")
        scr = call_line(loop, "runtime->update")
        cam = call_line(loop, "follow.update")
        ok_scripts = kurt is not None and scr is not None and cam is not None \
            and kurt < scr < cam
    say("the scripts tick in the step loop, after Kurt and before the camera", ok_scripts,
        "Viewer.cs:505-535 is Kurt, scripts, camera; outside the loop the runtime's 1/30 s "
        "clock is driven by frames, not by steps")

    # 8. the frame mixes the RUNTIME's mixer. ScriptRuntime owns its SoundMixer and
    #    every script sound is written into that one (script_vm.cpp:1258 opcode 89
    #    -> ScriptRuntime::playSound -> mixer_), so a frontend that keeps mixing its
    #    own standalone mixer attaches the sink to a device the scripts never write
    #    to: the level would run its scripts and be silent, with a healthy ledger.
    #    (Reads `text`, the adjudicate argument, and not the module's SRC: a check
    #    that read SRC would adjudicate the unmutated file for every arm and turn
    #    its own arm green, which is the pass-generator shape this file exists to
    #    avoid — and the arm for this check caught it on its first run.)
    mix_ok = all(re.search(p, text) for p in (
        r"mix->listenFromCamera\(camera\);", r"mix->update\(elapsed\);",
        r"mix->device\(\)\.update\(elapsed\);")) and not re.search(
        r"(?m)^[ \t]*mixer\.(listenFromCamera|update|device\(\)\.update)[ \t]*\(", text)
    say("the frame mixes the pointer that follows the runtime, not the standalone mixer",
        mix_ok, "the scripts write to the runtime's mixer; mixing another one is a silent "
                "level that still drains its audio queue")

    # 9. a respawn re-points the mixer AND re-attaches the sink. The runtime owns
    #    its mixer and its SoundDevice, so a rebuilt runtime is a new device: the
    #    sink attached to the old one leaves every level after the first death
    #    silent, which is the kind of bug that only shows up after a minute of play.
    spawn_mix = (spawn_body is not None
                 and re.search(r"mix[ \t]*=[ \t]*&runtime->mixer\(\)[ \t]*;", spawn_body) is not None
                 and re.search(r"player\.setMixer\(&runtime->mixer\(\)\)", spawn_body) is not None
                 and re.search(r"mix->device\(\)[ \t]*\.setSink\(", spawn_body) is not None)
    say("a respawn re-points the mixer, rebinds Kurt, and re-attaches the sink", spawn_mix,
        "the rebuilt runtime owns a new SoundDevice; the old sink is a device nothing reads")

    # 10. the runtime is built only when its three assets are present. A runtime
    #     with a hole in one of them is a different random stream, not a degraded
    #     oracle run (script_runtime.h's constructor note), so the gate has to be
    #     the reason it is not built.
    gate_ok = (spawn_body is not None
               and re.search(r"if[ \t]*\([ \t]*scriptAssetsReady[ \t]*\)[ \t]*\{", spawn_body)
               is not None
               and re.search(r"runtime\.reset\([ \t]*new[ \t]+mdk::scripts::ScriptRuntime",
                             spawn_body) is not None)
    say("the runtime is constructed under the asset gate", gate_ok,
        "a runtime without its CMI, bank or sprites archive plays a different game")

    # 11. the camera's rest pitch comes from the runtime when there is one: a live
    #     camera_track request first, then the scripts' current arena (the C#'s two
    #     sources, Viewer.cs:746-747). The geometric arena is the no-runtime
    #     stand-in and nothing more.
    pitch_ok = (loop is not None
                and "runtime->cameraTrackTicks" in loop
                and "runtime->cameraTrackPitch" in loop
                and "level.arenaPitch(runtime->currentArena())" in loop)
    say("the pitch target is camera_track, then the scripts' arena", pitch_ok,
        "Viewer.cs:746-747 asks the scripts first; the geometric arena is only the "
        "stand-in for a frontend with no runtime")
    return r


real = adjudicate(SRC)

# --- mutation arms ----------------------------------------------------------
# Each reintroduces one of the defects the checks claim to prevent. An arm that
# cannot build its mutant is a FAIL, never a skip.

STEP_LOOP = step_loop_span(SRC)


def in_loop(mutate):
    """Splice a mutated loop body back into the file at the loop's own offsets."""
    def arm(text):
        sp = step_loop_span(text)
        if sp is None:
            return text
        a, b = sp
        return text[:a] + mutate(text[a:b]) + text[b:]
    return arm


def strip_all_set_solid(loop):
    """Remove every in-loop solid-pin site, runtime branch and geometric branch
    both. Deleting only the geometric one — which is what this arm used to do,
    and what it still has to do to stay honest — leaves the runtime's call behind,
    so the mutant would keep refreshing the pin and the arm would test nothing."""
    out = re.sub(r"(?m)^[ \t]*if[ \t]*\(solid\)[ \t]*\{[ \t]*\n"
                 r"[ \t]*space\.setSolid\([ \t]*\*solid[ \t]*\);[ \t]*\n[ \t]*\}[ \t]*\n",
                 "", loop)
    out = re.sub(r"(?m)^[ \t]*if[ \t]*\([ \t]*![ \t]*runtime->currentArena\([ \t]*\)"
                 r"\.empty\([ \t]*\)[ \t]*\)[ \t]*\{[ \t]*\n"
                 r"[ \t]*space\.setSolid\([ \t]*runtime->currentArena\([ \t]*\)[ \t]*\);[ \t]*\n"
                 r"[ \t]*\}[ \t]*\n", "", out)
    return out


def arm_no_refresh(text):
    """The defect itself: the in-loop refresh deleted, the spawn pin left alone."""
    sp = STEP_LOOP
    if sp is None:
        return text
    a, b = sp
    loop = text[a:b]
    stripped = strip_all_set_solid(loop)
    if stripped == loop:
        return text
    return text[:a] + stripped + text[b:]


def arm_before_camera(text):
    """Refresh moved above follow.update: the arena the step STARTED in."""
    sp = STEP_LOOP
    if sp is None:
        return text
    a, b = sp
    loop = text[a:b]
    m = re.search(r"(?m)^[ \t]*if[ \t]*\(solid\)[ \t]*\{[ \t]*\n"
                  r"[ \t]*space\.setSolid\([ \t]*\*solid[ \t]*\);[ \t]*\n[ \t]*\}[ \t]*\n", loop)
    if not m:
        return text
    block = m.group(0)
    rest = loop[:m.start()] + loop[m.end():]
    p = re.search(r"(?m)^[ \t]*follow\.update\(", rest)
    if p is None:
        return text
    return text[:a] + rest[:p.start()] + block + rest[p.start():] + text[b:]


def arm_clears_the_pin(text):
    """The tidy-up that clears the pin when Kurt is outside every arena."""
    sp = STEP_LOOP
    if sp is None:
        return text
    a, b = sp
    loop = text[a:b]
    m = re.search(r"(?m)^[ \t]*if[ \t]*\(solid\)[ \t]*\{[ \t]*\n"
                  r"[ \t]*space\.setSolid\([ \t]*\*solid[ \t]*\);[ \t]*\n[ \t]*\}[ \t]*\n", loop)
    if not m:
        return text
    return text[:a] + loop[:m.start()] + \
        "                space.setSolid(solid ? *solid : std::string());\n" + \
        loop[m.end():] + text[b:]


def arm_drops_the_spawn_pin(text):
    """The spawn pin removed, leaving only the per-step refresh."""
    sp = body_span(text, "spawnPlayer")
    if sp is None:
        return text
    a, b = sp
    body = text[a:b]
    m = re.search(r"(?m)^[ \t]*if[ \t]*\(here\)[ \t]*\{[ \t]*\n"
                  r"[ \t]*space\.setSolid\([ \t]*\*here[ \t]*\);[ \t]*\n[ \t]*\}[ \t]*\n", body)
    if not m:
        return text
    return text[:a] + body[:m.start()] + body[m.end():] + text[b:]


def arm_guard_on_something_else(text):
    """The guard kept, but on a flag instead of on the lookup."""
    sp = STEP_LOOP
    if sp is None:
        return text
    a, b = sp
    loop = text[a:b]
    m = re.search(r"(?m)^[ \t]*if[ \t]*\(solid\)[ \t]*\{[ \t]*\n"
                  r"[ \t]*space\.setSolid\([ \t]*\*solid[ \t]*\);[ \t]*\n[ \t]*\}[ \t]*\n", loop)
    if not m:
        return text
    return text[:a] + loop[:m.start()] + \
        "                if (playerReady) {\n" \
        "                    space.setSolid(*solid);\n" \
        "                }\n" + loop[m.end():] + text[b:]


def arm_scripts_tick_deleted(text):
    """The runtime is still built and its sounds still reach the mixer, but nothing
    ever ticks it: the level's objects never spawn and its scripts never run."""
    mutated, n = re.subn(r"(?m)^[ \t]*runtime->update\(step\);[ \t]*\n", "", text)
    return mutated if n else text


def arm_scripts_tick_after_camera(text):
    """The tick kept, but placed after the camera step: the camera is driven by the
    previous tick's arena, camera_track request and shake."""
    m = re.search(r"(?m)^([ \t]*)(if[ \t]*\([ \t]*runtime[ \t]*\)[ \t]*\{[ \t]*\n"
                  r"[ \t]*runtime->update\(step\);[ \t]*\n[ \t]*\}[ \t]*\n)", text)
    if m is None:
        return text
    rest = text[:m.start()] + text[m.end():]
    p = re.search(r"(?m)^[ \t]*follow\.update\([^;]*;[ \t]*\n", rest)
    if p is None:
        return text
    return rest[:p.end()] + m.group(2) + rest[p.end():]


def arm_mixes_standalone(text):
    """The C#'s one-mixer shape abandoned: the frame goes back to the standalone
    mixer, so every script sound is written into a device no sink reads."""
    mutated = text.replace("mix->listenFromCamera(camera);", "mixer.listenFromCamera(camera);")
    mutated = mutated.replace("mix->update(elapsed);", "mixer.update(elapsed);")
    mutated = mutated.replace("mix->device().update(elapsed);", "mixer.device().update(elapsed);")
    return mutated if mutated != text else text


def arm_sink_not_reattached(text):
    """The respawn rebuilds the runtime — and with it the mixer and its device — but
    leaves the sink on the device the old runtime owned."""
    sp = body_span(text, "spawnPlayer")
    if sp is None:
        return text
    a, b = sp
    body = text[a:b]
    mutated, n = re.subn(r"(?m)^[ \t]*mix->device\(\)[ \t]*\.setSink\([^;]*;[ \t]*\n", "", body)
    if not n:
        return text
    return text[:a] + mutated + text[b:]


def arm_no_asset_gate(text):
    """The gate replaced by `if (true)`: the runtime is built whatever the level
    loaded, which is the different-random-stream case the header forbids."""
    mutated, n = re.subn(r"if[ \t]*\([ \t]*scriptAssetsReady[ \t]*\)[ \t]*\{",
                         "if (true) {", text)
    return mutated if n else text


def arm_pin_from_the_runtime(text):
    """Batch 99's frozen pin, put back: the step loop reads the runtime's current
    arena instead of the arena under Kurt. `currentArena_` is written once, on the
    first tick, so this is the spawn arena for the whole level and a door walk falls
    through the room it opened into (review-6 F1)."""
    mutated, n = re.subn(
        r"(?m)^[ \t]*const[ \t]+std::string\*[ \t]+solid[ \t]*=[ \t]*\n"
        r"[ \t]*space\.arenaAt\([^;]*;[ \t]*\n"
        r"[ \t]*if[ \t]*\([ \t]*solid[ \t]*\)[ \t]*\{[ \t]*\n"
        r"[ \t]*space\.setSolid\([ \t]*\*solid[ \t]*\);[ \t]*\n[ \t]*\}[ \t]*\n",
        "                if (!runtime->currentArena().empty()) {\n"
        "                    space.setSolid(runtime->currentArena());\n"
        "                }\n", text)
    return mutated if n else text


def arm_pitch_ignores_runtime(text):
    """The pitch target back to the geometric arena even when a runtime exists: the
    scripts' camera_track request and their arena both ignored."""
    sp = STEP_LOOP
    if sp is None:
        return text
    a, b = sp
    loop = text[a:b]
    mutated, n = re.subn(
        r"if[ \t]*\(runtime\)[ \t]*\{[ \t]*\n[ \t]*target[ \t]*=[ \t]*\n?[ \t]*runtime->cameraTrackTicks"
        r"[^;]*;[ \t]*\n[ \t]*\}[ \t]*(?:\n[ \t]*)?else[ \t]*\{", "if (false) {", loop)
    if not n:
        return text
    return text[:a] + mutated + text[b:]


ARMS = [
    ("in-loop-refresh-deleted (review-8 F1 itself)", arm_no_refresh),
    ("refresh-moved-before-the-camera-step", arm_before_camera),
    ("refresh-clears-the-pin-when-outside-every-arena", arm_clears_the_pin),
    ("spawn-pin-removed", arm_drops_the_spawn_pin),
    ("guard-replaced-by-an-unrelated-flag", arm_guard_on_something_else),
    ("scripts-tick-deleted", arm_scripts_tick_deleted),
    ("scripts-tick-moved-after-the-camera", arm_scripts_tick_after_camera),
    ("frame-mixes-the-standalone-mixer", arm_mixes_standalone),
    ("respawn-does-not-re-attach-the-sink", arm_sink_not_reattached),
    ("runtime-built-without-the-asset-gate", arm_no_asset_gate),
    ("pin-taken-from-the-runtime's-frozen-current-arena (review-6 F1)", arm_pin_from_the_runtime),
    ("pitch-ignores-the-runtime", arm_pitch_ignores_runtime),
]

print("main_3ds_flow: %s — %d checks, %d mutation arms" % (PATH, real.checks, len(ARMS)))
arms_red = 0
for label, mutate in ARMS:
    mutated = mutate(SRC)
    if mutated == SRC:
        print("  FAIL main_3ds_flow arm %s: the mutation changed nothing, so the arm tested "
              "the file it was handed instead of the invariant" % label)
        continue
    after = adjudicate(mutated, quiet=True)
    if after.failed:
        arms_red += 1
        print("  ok   arm %s: turned `%s` red as required" % (label, after.broke))
    else:
        print("  FAIL main_3ds_flow arm %s: the mutation was accepted, so the check does not "
              "pin what it claims to pin" % label)

if real.failed:
    sys.exit(1)
if arms_red != len(ARMS):
    print("  FAIL main_3ds_flow: only %d of %d arms adjudicated red; a PASS here would be a "
          "reading of nothing" % (arms_red, len(ARMS)))
    sys.exit(1)
print("PASS Main3dsFlow (%d checks, %d arms adjudicated red)" % (real.checks, arms_red))
PY
exit $?
