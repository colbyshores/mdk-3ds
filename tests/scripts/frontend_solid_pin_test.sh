#!/bin/sh
# frontend_solid_pin_test.sh — the ONE claim both frontends make about Kurt's
# collision world, gated against both of them at once.
#
# WHY THIS EXISTS SEPARATELY FROM main_3ds_flow_test.sh. `arena_space.h` ships a
# sentence about BOTH frontends and about the branch each of them has. A claim that
# spans two files cannot be gated by a test that reads one of them, and review-7 F1
# is the proof: batch 100 reverted the 3DS frontend's frozen pin, wrote that sentence,
# and left the Linux frontend still pinning `runtime->currentArena()` — a value the
# DTI connection crossing left at the SPAWN arena for the whole level back when
# `CrossedArena` (ScriptRuntime.cs:791-803) was unported and `currentArena_` was
# written once, on the runtime's first tick. `ArenaSpace::setSolid` is STICKY: while
# `_solid` is non-empty `move()` uses only it (arena_space.cpp:111-124), so the pin
# stayed on the arena Kurt was born in. Walking out through an open doorway left
# that BSP's geometry behind, `bottom()` resolved the neighbour holding his feet,
# and the fall-out death fired into a respawn loop — in the frontend that takes the
# pinned frame.
#
# WHY A SOURCE CHECK AND NOT A GOLDEN. Neither frontend is host-compilable as a
# unit: `main_3ds.cpp` includes <3ds.h> and `mdk_linux/src/main.cpp` includes the
# SDL/Vulkan window seam. What is load-bearing here is also not arithmetic, it is
# WHERE a call sits and WHAT it reads, which is the reason main_3ds_flow_test.sh and
# c3d_frame_test.sh exist. The sticky-pin mechanism itself is executed by the host
# suite (vm_tests' ArenaSpace contract cases, and
# SolidArenasAreTheDrawnListAndThePinTakesBoth for the value), and the SHAPE that
# feeds it is pinned here.
#
# WHAT IS PINNED, per frontend:
#   1. the fixed-step loop refreshes the solid arena at all — a pin set only in the
#      spawn block is the defect review-8 F1 found;
#   2. that refresh reads the runtime's DRAWN LIST (`solidArenas()`), which is the
#      oracle's value (ScriptRuntime.cs:265-282, Viewer.cs:533: current arena plus
#      the active second);
#   3. the branch with NO runtime — the asset gate, no scripts, so no drawn list to
#      read — keeps the geometric arena under the feet (`arenaAt` feeding a
#      `setSolid({*that})`), which is the stand-in `arena_space.h` claims;
#   4. every in-loop refresh is GUARDED, so a Kurt outside every arena (or a runtime
#      that has not completed its first tick) keeps his last arena instead of
#      clearing the pin (an unguarded `setSolid` is the opposite defect, reached from
#      the other side);
#   5. the loop never pins the runtime's single `currentArena()`, and has exactly one
#      refresh per branch — the review-6 F1 / review-7 F1 rule, which is now an
#      ARITY rule and not a staleness rule: one name where the oracle passes two
#      drops the room behind the door Kurt came through;
#   6. the spawn pin survives, because the loop cannot supply it: outside every
#      arena `arenaAt` answers null and before the first tick the drawn list is the
#      empty name, so with no pin at all `bottom()` resolves nothing and the
#      fall-out death never fires.
#
# Checks 1-6 overlap several of main_3ds_flow_test.sh's checks for the 3DS file.
# That is deliberate: the shared claim needs one gate that reads both files, and a
# claim is only as gated as its weakest frontend.
#
# Every check is paired with a mutation arm per frontend, and every arm names the
# one check it must turn red (review-7 F2's rule: an arm satisfied by a different
# check leaves its own check with no mutation that isolates it).
#
# usage: sh frontend_solid_pin_test.sh [root-dir]
set -u
ROOT=${1:-$(cd "$(dirname "$0")/../.." && pwd)}

python3 - "$ROOT" <<'PY'
import re
import sys

ROOT = sys.argv[1]
FRONTENDS = [
    ("3DS", ROOT + "/mdk_3ds/src/main_3ds.cpp"),
    ("Linux", ROOT + "/mdk_linux/src/main.cpp"),
]

# The fixed-step loop header differs only in the name of the step constant
# (`step` on the console, `kStep` on Linux), so one regex covers both.
STEP_LOOP = r"while[ \t]*\([ \t]*pending[ \t]*>=[ \t]*[A-Za-z_][A-Za-z0-9_]*[ \t]*\)"

# The runtime branch: the scripts' drawn list, guarded on the runtime existing. The
# closing brace is left open on purpose: both frontends write `} else {`, and the
# fallback branch is the next check's.
RUNTIME_PIN = (r"(?m)^[ \t]*if[ \t]*\([ \t]*runtime[ \t]*\)[ \t]*\{[ \t]*\n"
               r"[ \t]*space\.setSolid\([ \t]*runtime->solidArenas\([ \t]*\)[ \t]*\);"
               r"[ \t]*\n[ \t]*\}")

# The no-runtime branch as both frontends write it: a lookup, then a guarded
# setSolid of what the lookup returned.
GEOMETRIC = (r"(?m)^[ \t]*const[ \t]+std::string\*[ \t]+solid[ \t]*=[ \t]*\n"
             r"[ \t]*space\.arenaAt\([^;]*;[ \t]*\n"
             r"[ \t]*if[ \t]*\([ \t]*solid[ \t]*\)[ \t]*\{[ \t]*\n"
             r"[ \t]*space\.setSolid\([ \t]*\{[ \t]*\*solid[ \t]*\}[ \t]*\);[ \t]*\n"
             r"[ \t]*\}[ \t]*\n")

SPAWN_PIN = (r"(?m)^[ \t]*if[ \t]*\(here\)[ \t]*\{[ \t]*\n"
             r"[ \t]*space\.setSolid\([ \t]*\{[ \t]*\*here[ \t]*\}[ \t]*\);[ \t]*\n"
             r"[ \t]*\}[ \t]*\n")

FROZEN_PIN = ("                if (!runtime->currentArena().empty()) {\n"
              "                    space.setSolid(runtime->currentArena());\n"
              "                }\n")

# The geometric stand-in, for the arm that restores the pre-batch shape.
GEOMETRIC_STANDIN = ("                const std::string* solid =\n"
                     "                    space.arenaAt(mdk::Vec{player.feet.x, player.feet.y, player.feet.z});\n"
                     "                if (solid) {\n"
                     "                    space.setSolid({*solid});\n"
                     "                }\n")

CHK = {
    "refresh-in-loop": "the fixed-step loop refreshes Kurt's solid arena",
    "pin-is-the-drawn-list":
        "with a runtime the refresh is the scripts' DRAWN list (current + active "
        "second), which is the oracle's value at Viewer.cs:533",
    "fallback-is-geometric": "the branch with no runtime keeps the geometric arena under his feet",
    "refresh-guarded": "every in-loop refresh is guarded",
    "pin-not-the-single-current-arena":
        "the loop never pins the runtime's single current arena, and has exactly one "
        "refresh per branch",
    "spawn-pin": "the spawn pin is still there for a Kurt outside every arena",
}


def code_lines(text):
    """(index, text) for lines that are code. Comments are dropped: this tree
    documents its own rules in prose, and prose about `setSolid` is not a call
    site."""
    out = []
    for i, line in enumerate(text.splitlines()):
        s = line.strip()
        if s.startswith("//") or s.startswith("*") or s.startswith("/*"):
            continue
        out.append((i, line))
    return out


def block_span_from(text, pos):
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


def step_loop_span(text):
    m = re.search(STEP_LOOP, text)
    return block_span_from(text, m.end() - 1) if m else None


def body_span(text, name):
    m = re.search(r"(?m)^[ \t]*(?:auto[ \t]+)?" + re.escape(name) +
                  r"[ \t]*(?:=[ \t]*\[&\]\([ \t]*\)|\([^;{]*\))[ \t]*\{", text)
    if not m:
        return None
    sp = block_span_from(text, m.end() - 1)
    return (m.start(), sp[1]) if sp else None


def line_depths(block):
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
    def __init__(self, who):
        self.who = who
        self.checks = 0
        self.failed = False
        self.broke = ""

    def check(self, label, ok, detail=""):
        self.checks += 1
        if ok:
            print("  ok   %s: %s" % (self.who, label))
        else:
            self.failed = True
            if not self.broke:
                self.broke = label
            print("  FAIL %s: %s%s" % (self.who, label, (" — " + detail) if detail else ""))

    def quiet(self, label, ok):
        self.checks += 1
        if not ok:
            self.failed = True
            if not self.broke:
                self.broke = label


def adjudicate(who, text, quiet=False):
    r = Result(who)
    sp = step_loop_span(text)
    loop = text[sp[0]:sp[1]] if sp else None
    spawn = body_span(text, "spawnPlayer")
    spawn_body = text[spawn[0]:spawn[1]] if spawn else None

    def say(label, ok, detail=""):
        if quiet or sp is None:
            # A file with no fixed-step loop fails every check that lives in it:
            # "the loop is missing" must never read as "the loop is clean".
            r.quiet(label, False if sp is None else ok)
            return
        r.check(label, ok, detail)

    loop_code = "".join(line + "\n" for _, line in code_lines(loop)) if loop else ""

    say(CHK["refresh-in-loop"], "setSolid(" in loop_code,
        "with no refresh in the step body the pin is wherever the spawn left it, and "
        "move() uses only the pinned arena (arena_space.cpp:111-124)")

    say(CHK["pin-is-the-drawn-list"], re.search(RUNTIME_PIN, loop or "") is not None,
        "the oracle hands SetSolid the scripts' drawn list (ScriptRuntime.cs:265-282, "
        "Viewer.cs:533); a pin fed by anything else while a runtime exists is a "
        "different claim, and one name drops the room behind the door")

    say(CHK["fallback-is-geometric"], re.search(GEOMETRIC, loop or "") is not None,
        "the header claims the geometric arena under the feet for the branch with no "
        "scripts; a fallback fed by anything else is a different claim")

    depths = line_depths(loop) if loop else []
    sites = [(i, line) for i, line in code_lines(loop or "") if re.search(r"setSolid\s*\(", line)]
    guarded = bool(sites) and all((depths[i] if i < len(depths) else 0) >= 1 for i, _ in sites)
    say(CHK["refresh-guarded"], guarded,
        "an unconditional setSolid of a list that resolves to nothing clears the pin, "
        "which is the spawn-pin check's failure reached from the other side")

    frozen = re.search(r"setSolid\([ \t]*runtime->currentArena\(", loop or "") is not None
    say(CHK["pin-not-the-single-current-arena"],
        not frozen and len(sites) == 2,
        "the oracle pins the DRAWN list (current + active second), so a pin that reads "
        "currentArena() is one name where the oracle has two and drops the room behind "
        "the door Kurt came through (review-6 F1); the loop must have exactly one "
        "refresh per branch, the drawn list with a runtime and the lookup without one")

    say(CHK["spawn-pin"],
        spawn_body is not None and re.search(SPAWN_PIN, spawn_body) is not None,
        "outside every arena the loop's lookup answers null and before the first tick "
        "the drawn list is the empty name, and with no pin at all bottom() resolves "
        "nothing and the fall-out death never fires")
    return r


FILES = {}
real = []
for who, path in FRONTENDS:
    try:
        FILES[who] = open(path, errors="replace").read()
    except OSError as exc:
        print("  FAIL frontend_solid_pin: cannot read %s (%s)" % (path, exc))
        sys.exit(1)
    real.append(adjudicate(who, FILES[who]))


def _in_loop(text, mutate):
    sp = step_loop_span(text)
    if sp is None:
        return text
    a, b = sp
    mutated = mutate(text[a:b])
    return text[:a] + mutated + text[b:]


def arm_delete_refresh(who, text):
    """The pin set once and never refreshed: review-8 F1 and review-7 F1. BOTH
    branches go, because either one alone still refreshes and would leave this arm
    for another check to catch."""
    def mutate(loop):
        out, n1 = re.subn(RUNTIME_PIN, "", loop)
        out, n2 = re.subn(GEOMETRIC, "", out)
        return out if (n1 and n2) else loop
    return _in_loop(text, mutate)


def arm_restore_geometric(who, text):
    """The pre-batch shape put back: the runtime branch replaced by the geometric
    stand-in, so the pin is one arena chosen by the feet instead of the scripts'
    drawn list. Only the drawn-list check can catch this one."""
    def mutate(loop):
        out, n = re.subn(RUNTIME_PIN, GEOMETRIC_STANDIN, loop)
        return out if n else loop
    return _in_loop(text, mutate)


def arm_frozen_pin_replacing(who, text):
    """Batch 99's shape: the geometric fallback REPLACED by the runtime's current
    arena — one name where the oracle pins the drawn LIST. It trips the geometric
    check first (there is no lookup left to read); arm_frozen_pin_added is the one
    that isolates the single-name check."""
    def mutate(loop):
        out, n = re.subn(GEOMETRIC, FROZEN_PIN, loop)
        return out if n else loop
    return _in_loop(text, mutate)


def arm_frozen_pin_added(who, text):
    """Batch 99's runtime pin put back ALONGSIDE the two branch refreshes, after
    them, so it is the last writer and both shapes survive: only the single-name
    check can catch this one."""
    def mutate(loop):
        m = re.search(GEOMETRIC, loop)
        if not m:
            return loop
        return loop[:m.end()] + FROZEN_PIN + loop[m.end():]
    return _in_loop(text, mutate)


def arm_unguarded(who, text):
    """The tidy-up that clears the pin when the list resolves to nothing: the same
    refresh, hoisted out of its guard and placed before it. It ADDS a site rather
    than replacing one, because replacing the guarded pair also removes the shapes
    the other checks read, and that check would catch the mutant first — which is
    the pass-generator shape this file's own arm table forbids."""
    def mutate(loop):
        m = re.search(RUNTIME_PIN, loop)
        if not m:
            return loop
        return loop[:m.start()] + \
            "                space.setSolid(runtime->solidArenas());\n" + \
            loop[m.start():]
    return _in_loop(text, mutate)


def arm_delete_spawn_pin(who, text):
    sp = body_span(text, "spawnPlayer")
    if sp is None:
        return text
    a, b = sp
    mutated, n = re.subn(SPAWN_PIN, "", text[a:b])
    return (text[:a] + mutated + text[b:]) if n else text


ARMS = [
    ("in-loop-refresh-deleted", arm_delete_refresh, CHK["refresh-in-loop"]),
    ("geometric-stand-in-restored-for-the-runtime", arm_restore_geometric,
     CHK["pin-is-the-drawn-list"]),
    ("frozen-runtime-pin-replacing-the-fallback", arm_frozen_pin_replacing,
     CHK["fallback-is-geometric"]),
    ("frozen-runtime-pin-added-alongside", arm_frozen_pin_added,
     CHK["pin-not-the-single-current-arena"]),
    ("refresh-unguarded-clears-the-pin", arm_unguarded, CHK["refresh-guarded"]),
    ("spawn-pin-deleted", arm_delete_spawn_pin, CHK["spawn-pin"]),
]

checks = sum(r.checks for r in real)
total = len(ARMS) * len(FRONTENDS)
print("frontend_solid_pin: %d frontends, %d checks, %d mutation arms"
      % (len(FRONTENDS), checks, total))
arms_red = 0
for who, _ in FRONTENDS:
    for label, mutate, expected in ARMS:
        mutated = mutate(who, FILES[who])
        if mutated == FILES[who]:
            print("  FAIL %s arm %s: the mutation changed nothing, so the arm tested the "
                  "file it was handed instead of the invariant" % (who, label))
            continue
        after = adjudicate(who + " arm", mutated, quiet=True)
        if not after.failed:
            print("  FAIL %s arm %s: the mutation was accepted, so the check does not pin "
                  "what it claims to pin" % (who, label))
            continue
        if after.broke != expected:
            print("  FAIL %s arm %s: it turned `%s` red, but the check it claims to pin is "
                  "`%s` — so that check has no mutation that isolates it" %
                  (who, label, after.broke, expected))
            continue
        arms_red += 1
        print("  ok   %s arm %s: turned `%s` red as required" % (who, label, after.broke))

if any(r.failed for r in real):
    sys.exit(1)
if arms_red != total:
    print("  FAIL frontend_solid_pin: only %d of %d arms adjudicated red; a PASS here "
          "would be a reading of nothing" % (arms_red, total))
    sys.exit(1)
print("PASS FrontendSolidPin (%d checks, %d arms adjudicated red)" % (checks, arms_red))
PY
exit $?
