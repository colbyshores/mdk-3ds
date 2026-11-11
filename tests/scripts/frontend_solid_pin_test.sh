#!/bin/sh
# frontend_solid_pin_test.sh — the ONE claim both frontends make about Kurt's
# collision world, gated against both of them at once.
#
# WHY THIS EXISTS SEPARATELY FROM main_3ds_flow_test.sh. `arena_space.h` ships a
# sentence about BOTH frontends: "BOTH frontends here use the geometric arena under
# his feet as the stand-in". A claim that spans two files cannot be gated by a test
# that reads one of them, and review-7 F1 is the proof: batch 100 reverted the 3DS
# frontend's frozen pin, wrote that sentence, and left the Linux frontend still
# pinning `runtime->currentArena()` — a value the DTI connection crossing left at
# the SPAWN arena for the whole level back when `CrossedArena` (ScriptRuntime.cs:791
# -803) was unported and `currentArena_` was written once, on the runtime's first
# tick. `ArenaSpace::setSolid` is STICKY: while
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
# suite (vm_tests' ArenaSpace contract cases); this gate pins the call sites.
#
# WHAT IS PINNED, per frontend:
#   1. the fixed-step loop refreshes the solid arena at all — a pin set only in the
#      spawn block is the defect review-8 F1 found;
#   2. that refresh reads the GEOMETRIC arena under the feet (`arenaAt` feeding a
#      `setSolid(*that)`), which is the stand-in the header claims;
#   3. every in-loop refresh is GUARDED on its lookup, so a Kurt outside every arena
#      keeps his last arena instead of clearing the pin (an unguarded
#      `setSolid("")` is the opposite defect, reached from the other side);
#   4. the loop never pins the runtime's single-name `currentArena()`, and has exactly one
#      refresh — the review-6 F1 / review-7 F1 rule;
#   5. the spawn pin survives, because the loop cannot supply it: outside every
#      arena `arenaAt` answers null, and with no pin at all `bottom()` resolves
#      nothing and the fall-out death never fires.
#
# Checks 1-5 overlap three of main_3ds_flow_test.sh's eleven checks for the 3DS
# file. That is deliberate: the shared claim needs one gate that reads both files,
# and a claim is only as gated as its weakest frontend.
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

# The geometric refresh as both frontends write it: a lookup, then a guarded
# setSolid of what the lookup returned.
GEOMETRIC = (r"(?m)^[ \t]*const[ \t]+std::string\*[ \t]+solid[ \t]*=[ \t]*\n"
             r"[ \t]*space\.arenaAt\([^;]*;[ \t]*\n"
             r"[ \t]*if[ \t]*\([ \t]*solid[ \t]*\)[ \t]*\{[ \t]*\n"
             r"[ \t]*space\.setSolid\([ \t]*\*solid[ \t]*\);[ \t]*\n[ \t]*\}[ \t]*\n")

SPAWN_PIN = (r"(?m)^[ \t]*if[ \t]*\(here\)[ \t]*\{[ \t]*\n"
             r"[ \t]*space\.setSolid\([ \t]*\*here[ \t]*\);[ \t]*\n[ \t]*\}[ \t]*\n")

FROZEN_PIN = ("                if (!runtime->currentArena().empty()) {\n"
              "                    space.setSolid(runtime->currentArena());\n"
              "                }\n")

CHK = {
    "refresh-in-loop": "the fixed-step loop refreshes Kurt's solid arena",
    "refresh-is-geometric": "the refresh reads the geometric arena under his feet",
    "refresh-guarded": "every in-loop refresh is guarded on its own lookup",
    "pin-not-the-runtime": "the loop never pins the runtime's frozen current arena",
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

    say(CHK["refresh-is-geometric"], re.search(GEOMETRIC, loop or "") is not None,
        "the header claims the geometric arena under the feet; a pin fed by anything "
        "else is a different claim")

    depths = line_depths(loop) if loop else []
    sites = [(i, line) for i, line in code_lines(loop or "") if re.search(r"setSolid\s*\(", line)]
    guarded = bool(sites) and all((depths[i] if i < len(depths) else 0) >= 1 for i, _ in sites)
    say(CHK["refresh-guarded"], guarded,
        "an unconditional setSolid of an empty name clears the pin, which is the "
        "spawn-pin check's failure reached from the other side")

    frozen = re.search(r"setSolid\([ \t]*runtime->currentArena\(", loop or "") is not None
    say(CHK["pin-not-the-runtime"],
        not frozen and len(sites) == 1,
        "the oracle pins the DRAWN list (current + active second) and setSolid takes "
        "one name, so a pin that reads currentArena() is a single name where the "
        "oracle has two -- and before batch 134 ported the crossing it was worse still, "
        "the spawn arena for the whole level; the loop must have exactly "
        "one refresh and it must be the geometric one")

    say(CHK["spawn-pin"],
        spawn_body is not None and re.search(SPAWN_PIN, spawn_body) is not None,
        "outside every arena the loop's lookup answers null, and with no pin at "
        "all bottom() resolves nothing and the fall-out death never fires")
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


def arm_delete_refresh(who, text):
    """The pin set once and never refreshed: review-8 F1 and review-7 F1."""
    sp = step_loop_span(text)
    if sp is None:
        return text
    a, b = sp
    mutated, n = re.subn(GEOMETRIC, "", text[a:b])
    return (text[:a] + mutated + text[b:]) if n else text


def arm_frozen_pin_replacing(who, text):
    """Batch 99's shape: the geometric refresh REPLACED by the runtime's current
    arena. It trips the geometric check first (there is no lookup left to read);
    arm_frozen_pin_added is the one that isolates the frozen-pin check."""
    sp = step_loop_span(text)
    if sp is None:
        return text
    a, b = sp
    mutated, n = re.subn(GEOMETRIC, FROZEN_PIN, text[a:b])
    return (text[:a] + mutated + text[b:]) if n else text


def arm_frozen_pin_added(who, text):
    """The frozen pin put back ALONGSIDE the geometric refresh, after it, so it is
    the last writer and the geometric shape survives: only the frozen-pin check can
    catch this one."""
    sp = step_loop_span(text)
    if sp is None:
        return text
    a, b = sp
    loop = text[a:b]
    m = re.search(GEOMETRIC, loop)
    if not m:
        return text
    return text[:a] + loop[:m.end()] + FROZEN_PIN + loop[m.end():] + text[b:]


def arm_unguarded(who, text):
    """The tidy-up that clears the pin when Kurt is outside every arena. It ADDS an
    unguarded refresh rather than replacing the guarded one, because replacing it
    also removes the lookup the geometric check reads, and that check would then
    catch the mutant first — which is the pass-generator shape this file's own arm
    table forbids. An extra unguarded site clears the pin just as dead."""
    sp = step_loop_span(text)
    if sp is None:
        return text
    a, b = sp
    loop = text[a:b]
    m = re.search(GEOMETRIC, loop)
    if not m:
        return text
    return text[:a] + loop[:m.end()] + \
        "                space.setSolid(solid ? *solid : std::string());\n" + \
        loop[m.end():] + text[b:]


def arm_delete_spawn_pin(who, text):
    sp = body_span(text, "spawnPlayer")
    if sp is None:
        return text
    a, b = sp
    mutated, n = re.subn(SPAWN_PIN, "", text[a:b])
    return (text[:a] + mutated + text[b:]) if n else text


ARMS = [
    ("in-loop-refresh-deleted", arm_delete_refresh, CHK["refresh-in-loop"]),
    ("frozen-runtime-pin-replacing-the-geometric-one", arm_frozen_pin_replacing,
     CHK["refresh-is-geometric"]),
    ("frozen-runtime-pin-added-alongside", arm_frozen_pin_added, CHK["pin-not-the-runtime"]),
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
