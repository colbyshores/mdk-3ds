#!/bin/sh
# panel_geom_test.sh — the console picture instrument's own invariants, gated.
#
# WHY THIS FILE EXISTS (review-10 F1, Critical). Batch 103 made
# `tools/panel_geom.py geom` the binding picture claim for every future console
# A/B, and the reviewer proved it could not fail: `mask()` returns 0 only for a
# pixel exactly equal to the clear colour, the object pass now covers the whole
# panel, so BOTH sides of every current comparison are 96000/96000 painted, the
# map is a constant, and a constant cannot differ from anything. Two panels that
# visibly differ print `geom: 0 px of 96000`. A comparison that cannot fail
# enforces nothing — the same argument AGENTS.md 2 makes about a permanently red
# gate — so the tool now REFUSES a degenerate side with exit 3, and this file is
# what keeps the refusal from being deleted the next time someone wants a number.
#
# WHY THE ARMS MUTATE THE TOOL AND NOT THE GAME. Every other gate in this
# directory adjudicates a source file or a binary. Here the artefact under test is
# the measuring instrument itself, and the failure mode is an instrument that
# prints a pass over a defect. So each arm below rewrites one clause of
# panel_geom.py, runs the whole battery against the mutant, and requires the
# mutant to be caught by the ONE check that claims the property. An arm caught by
# a different check is a FAIL, exactly as in main_3ds_flow_test.sh (review-7 F2):
# a check certified by a mutation it never reached is not a check.
#
# THE GREEN PROBES ARE HALF THE GATE. A refusal that reddened a legitimate
# comparison would be a pass-generator pointed the other way, so two probes build
# panels the tool must still accept and require every check to stay green.
#
# SYNTHETIC PANELS, NOT COMMITTED CROPS. The fixtures are built here from the
# clear colour and two painted colours, so the expectations are arithmetic and not
# a transcription of a capture. The committed crops are the subject of a batch's
# claim, never a test's expectation (a golden comes from the oracle, not from the
# implementation).
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TOOL=${PANEL_GEOM:-$ROOT/tools/panel_geom.py}
[ -f "$TOOL" ] || { printf '  FAIL panel_geom: cannot read %s\n' "$TOOL"; exit 1; }

python3 - "$TOOL" "$ROOT" <<'PY'
import os
import re
import shutil
import subprocess
import sys
import tempfile

TOOL = sys.argv[1]
ROOT = sys.argv[2]
TOOLS = os.path.join(ROOT, "tools")
sys.path.insert(0, TOOLS)
from PIL import Image  # noqa: E402  (panel_geom itself needs PIL, so this is no new dependency)

CLEAR = (16, 32, 48)      # params.clear_colour = 0xFF102030u, main_3ds.cpp
PAINT = (255, 0, 0)
PAINT2 = (255, 44, 141)
W, H = 400, 240
TOTAL = W * H


class Result:
    def __init__(self):
        self.checks = 0
        self.failed = False
        self.broke = None

    def say(self, label, ok, detail=""):
        self.checks += 1
        if not ok and not self.broke:
            self.broke = label
            self.failed = True
            if not getattr(self, "quiet", False):
                print("  FAIL panel_geom: %s — %s" % (label, detail))
        elif ok and not getattr(self, "quiet", False):
            print("  ok   panel_geom: %s" % label)


def build_fixtures(dirpath):
    """The five panels the battery needs, written under `dirpath`."""
    def panel(pixels):
        im = Image.new("RGB", (W, H))
        im.putdata(pixels)
        return im

    half = [CLEAR if x < W // 2 else PAINT for y in range(H) for x in range(W)]
    moved = list(half)
    for y in range(20, 30):
        for x in range(10, 20):
            moved[y * W + x] = PAINT2
    paths = {
        "half": os.path.join(dirpath, "half.png"),
        "moved": os.path.join(dirpath, "moved.png"),
        "painted": os.path.join(dirpath, "painted.png"),
        "clear": os.path.join(dirpath, "clear.png"),
        "shape": os.path.join(dirpath, "shape.png"),
    }
    panel(half).save(paths["half"])
    panel(moved).save(paths["moved"])
    panel([PAINT] * TOTAL).save(paths["painted"])
    panel([CLEAR] * TOTAL).save(paths["clear"])
    Image.new("RGB", (W - 1, H), PAINT).save(paths["shape"])
    return paths


FIXTURES = None


def run(tool, args):
    env = dict(os.environ, PYTHONPATH=TOOLS)
    p = subprocess.run([sys.executable, tool] + args, capture_output=True, text=True,
                       env=env, cwd=TOOLS)
    return p.returncode, p.stdout + p.stderr


def adjudicate(tool_path, quiet=False):
    r = Result()
    r.quiet = quiet
    f = FIXTURES

    def say(label, ok, detail=""):
        r.say(label, ok, detail)

    # 1. a pair with BOTH painted and clear pixels is measured, not refused.
    rc, out = run(tool_path, ["geom", f["half"], f["moved"]])
    say("geom-measures-a-two-valued-map",
        rc == 0 and "geom: 100 px of 96000 differ in the painted/clear map" in out,
        "expected exit 0 and a 100 px map diff, got exit %d:\n%s" % (rc, out))

    # 2. the clear fraction is printed, so a number is never quoted without the
    #    fraction of the panel the map can actually see (review-10 F1).
    say("geom-prints-the-clear-fraction",
        "clear 48000/96000 (50.0%) -> 47900/96000 (49.9%)" in out,
        "the fraction line is missing or wrong:\n%s" % out)

    # 3-5. a side whose map is a constant is REFUSED, with a non-zero exit and
    #      without a map-diff line that a sidecar could quote as a pass.
    for label, args in (("geom-refuses-an-all-painted-first-side",
                         ["geom", f["painted"], f["half"]]),
                        ("geom-refuses-an-all-painted-second-side",
                         ["geom", f["half"], f["painted"]]),
                        ("geom-refuses-an-all-clear-side",
                         ["geom", f["clear"], f["half"]])):
        rc, out = run(tool_path, args)
        say(label,
            rc == 3 and "REFUSED" in out and "differ in the painted/clear map" not in out,
            "expected exit 3 with REFUSED and no map-diff line, got exit %d:\n%s" % (rc, out))

    # 6. a refusal still prints what was measured. A refusal that prints nothing is
    #    indistinguishable from a crash, and the point of the guard is a readable
    #    "this scene cannot be measured here", not silence.
    rc, out = run(tool_path, ["geom", f["painted"], f["half"]])
    say("refusal-still-prints-the-census",
        "clear 0/96000 (0.0%) -> 48000/96000 (50.0%)" in out,
        "the refusal printed no measurement:\n%s" % out)

    # 7. census counts, including the half split the one-sided view is quoted in.
    rc, out = run(tool_path, ["census", f["half"]])
    say("census-counts-clear-painted-and-halves",
        rc == 0 and "clear=48000 of 96000" in out and "painted=48000" in out
        and "painted-left=0 painted-right=48000" in out,
        "census is wrong:\n%s" % out)

    # 8. a crop that is not the panel is refused (the panel_crop.py contract).
    rc, out = run(tool_path, ["geom", f["shape"], f["half"]])
    say("wrong-shape-is-refused", rc == 1 and "expected 400x240" in out,
        "expected exit 1, got exit %d:\n%s" % (rc, out))

    # 9. the rectangle is imported, not re-spelled (review-10 F5). panel_crop.py is
    #    the ONE definition; a second spelling is how batch 84 and 85 lost a row
    #    and a column, and this tool is the one quoted as canonical now.
    src = open(tool_path, errors="replace").read()
    body = "\n".join(l for l in src.splitlines() if not l.strip().startswith("#"))
    say("panel-size-is-imported",
        re.search(r"^\s*from panel_crop import .*PANEL_SIZE", body, re.M) is not None
        and re.search(r"PANEL_W\s*,\s*PANEL_H\s*=\s*\(\s*\d", body) is None,
        "the panel rectangle is spelled here instead of imported from panel_crop.py")

    # 10. the refusal is not reachable by a size error: exit 3 and exit 1 are
    #     different failures and a caller must be able to tell them apart.
    rc, _ = run(tool_path, ["geom", f["painted"], f["painted"]])
    say("refusal-exit-is-distinct-from-shape-exit", rc == 3,
        "expected exit 3 for a degenerate pair, got %d" % rc)
    return r


SRC = open(TOOL, errors="replace").read()
tmp = tempfile.mkdtemp(prefix="panel-geom-test.")
FIXTURES = build_fixtures(tmp)

real = adjudicate(TOOL)


def sub(old, new, count=1):
    def mutate(s):
        if old not in s:
            raise AssertionError("arm target is gone: %r" % old[:60])
        return s.replace(old, new, count)
    return mutate


GUARD = "        if clear == 0 or clear == total:"

ARMS = [
    # Each arm names the ONE check it must turn red.
    ("degeneracy guard deleted (review-10 F1 itself)",
     sub(GUARD, "        if False:"), "geom-refuses-an-all-painted-first-side"),
    ("guard checks only the first side",
     sub("for side, path, clear in ((\"first\", a, clear_a), (\"second\", b, clear_b)):",
         "for side, path, clear in ((\"first\", a, clear_a),):"),
     "geom-refuses-an-all-painted-second-side"),
    ("refusal returns success instead of exit 3",
     sub("            return 3\n", "            return 0\n"),
     "geom-refuses-an-all-painted-first-side"),
    ("the clear fraction is not printed",
     sub("    print(f\"geom: clear {clear_a}/{total}", "    if False: print(f\"geom: clear {clear_a}/{total}"),
     "geom-prints-the-clear-fraction"),
    ("mask() is constant-1 (the degenerate case, generalised)",
     sub("    return [0 if p == CLEAR else 1 for p in pixels]", "    return [1] * len(pixels)"),
     "geom-measures-a-two-valued-map"),
    ("the rectangle is re-spelled instead of imported (review-10 F5)",
     sub("from panel_crop import PANEL_SIZE", "PANEL_SIZE = (400, 240)"),
     "panel-size-is-imported"),
    ("the shape check is removed",
     sub("    if im.size != (PANEL_W, PANEL_H):", "    if False:"),
     "wrong-shape-is-refused"),
]

PROBES = [
    # Correct variants of the same property. A gate that reddens a re-spelling of
    # correct code is a pass-generator pointed the other way, and the house
    # precedent is that a guard must pin the behaviour, not one spelling of it
    # (main_3ds_flow_test.sh review-7 F2, c3d_frame_test.sh review-3 F3).
    ("probe: the guard written as a membership test",
     sub(GUARD, "        if clear in (0, total):")),
    ("probe: the mask compares tuples",
     sub("    return [0 if p == CLEAR else 1 for p in pixels]",
         "    return [0 if tuple(p) == CLEAR else 1 for p in pixels]")),
]

ARM_EXPECT = {a[0]: a[2] for a in ARMS}

print("panel_geom: %s — %d checks, %d mutation arms, %d green probes"
      % (TOOL, real.checks, len(ARMS), len(PROBES)))
arms_red = 0
for label, mutate, expect in ARMS:
    try:
        mutated = mutate(SRC)
    except AssertionError as exc:
        print("  FAIL panel_geom arm %s: %s" % (label, exc))
        continue
    if mutated == SRC:
        print("  FAIL panel_geom arm %s: the mutation changed nothing, so the arm tested "
              "the file it was handed instead of the invariant" % label)
        continue
    path = os.path.join(tmp, "mutant.py")
    with open(path, "w") as fh:
        fh.write(mutated)
    after = adjudicate(path, quiet=True)
    if after.failed and after.broke == ARM_EXPECT[label]:
        arms_red += 1
        print("  ok   arm %s: turned `%s` red as required" % (label, after.broke))
    else:
        print("  FAIL panel_geom arm %s: expected it to break `%s`, it broke `%s`"
              % (label, ARM_EXPECT[label], after.broke))

probes_green = 0
for label, mutate in PROBES:
    mutated = mutate(SRC)
    path = os.path.join(tmp, "probe.py")
    with open(path, "w") as fh:
        fh.write(mutated)
    after = adjudicate(path, quiet=True)
    if after.failed:
        print("  FAIL panel_geom %s: reddened `%s`, so the gate rejects correct input"
              % (label, after.broke))
    else:
        probes_green += 1
        print("  ok   %s stayed green" % label)

shutil.rmtree(tmp, ignore_errors=True)
if real.failed:
    sys.exit(1)
if arms_red != len(ARMS):
    print("  FAIL panel_geom: only %d of %d arms adjudicated red; a PASS here would be a "
          "reading of nothing" % (arms_red, len(ARMS)))
    sys.exit(1)
if probes_green != len(PROBES):
    print("  FAIL panel_geom: only %d of %d green probes stayed green" % (probes_green,
                                                                          len(PROBES)))
    sys.exit(1)
print("PASS PanelGeom (%d checks, %d arms adjudicated red, %d probes green)"
      % (real.checks, arms_red, probes_green))
PY
exit $?
