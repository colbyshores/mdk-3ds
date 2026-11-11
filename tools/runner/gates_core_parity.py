#!/usr/bin/env python3
"""
gates_core_parity.py -- the two targets must build the SAME GAME.

    python3 tools/runner/gates_core_parity.py [--json] [--root DIR]

WHY THIS GATE EXISTS. MDK has two frontends over one shared game:

    mdk_core/src/**      the shared game, compiled into BOTH targets
    mdk_3ds/src/**       the 3DS platform layer (Citro3D / libctru)
    mdk_linux/src/**     the Linux platform layer (Vulkan 1.3 / SDL3)

and the two build systems reach the shared tree DIFFERENTLY:

  * `mdk_linux/CMakeLists.txt` LISTS the core sources EXPLICITLY;
  * `mdk_3ds/Makefile` GLOBS them, PER DEPTH.

That difference is the whole hazard. A core source that lands on one target
only is not "phase one", it is a SILENTLY DIFFERENT GAME: the 3DS build links
the BSP sweep and the Linux build does not, both are green, and the two
binaries disagree about the game. The explicit list turns that omission into
a build error; the glob cannot, because a glob has no memory of what it
should have found.

THE CHECKS. Seven of them, and two of them BUILD AND RUN a target rather than
reading text, because a list nobody builds rots (see 6 and 7).

  1. PARITY-LINUX-LIST. Every `.cpp` under `mdk_core/src/**` must appear in
     the Linux `SOURCES` list. A core `.cpp` that is not listed is compiled
     into the 3DS binary and NOT into the Linux one.

  2. PARITY-LINUX-HEADER. A core `.h` must either be listed, or be reachable
     from a listed Linux translation unit (directly or through a local
     `#include`). A header is not compiled on its own, so "not listed" is not
     by itself a divergence -- but a header NO Linux translation unit ever
     includes is a header the Linux build does not see, and the same class of
     omission the explicit list is there to catch.

  3. PARITY-3DS-DEPTH. The 3DS Makefile's globs are PER-DEPTH, so a core
     source in a NEW nesting level is silently dropped: the 3DS build stays
     green and links a different game. The gate parses the Makefile's own
     globs, works out the deepest level they reach, and reports every core
     source below it, plus the subdirectories that are too deep.

  4. PARITY-3DS-BASENAME. The Makefile keys objects by `notdir`, so two
     files with the same basename in different directories collapse into ONE
     object and one of them is never compiled. The gate reports a collision
     across the two globbed trees.

  5. PARITY-LINUX-GHOST. Every entry in the Linux `SOURCES` list must exist.
     A source that does not exist is a build error at configure time -- but
     it is reported here too, because a list entry that names a file that was
     RENAMED is the other half of the same omission.

  6. PARITY-FORMATS. `tests/formats/Makefile` is a THIRD hand-maintained
     source list over `mdk_core/src/**`, and it was the one nothing ever built:
     the target failed to LINK for 25 batches with every gate green, because
     `tools/check.sh` did not run it. A list nobody builds rots, and restating
     the rule is a known-ineffective control -- the only thing that has ever
     caught this class is building the config (vault:
     build-config-link-sweep-the-rule-is-not-the-remedy). So this gate builds
     and runs the target and reports three things: PARITY-FORMATS-GHOST (a
     SOURCES entry that does not exist), PARITY-FORMATS-BASENAME (two entries
     sharing a basename, which the Makefile's `notdir` object keys collapse
     into ONE object, exactly the hazard PARITY-3DS-BASENAME covers on the 3DS
     side), and PARITY-FORMATS-LINK (the target does not build and run clean).
     No `make`, a timeout, a nonzero exit, or a run that prints no check count
     is a MUST here and never a skip: a check that silently does not run is a
     pass generator, which is the mistake batch 130 removed from this same
     target's `nm` guard.

  7. PARITY-SCRIPTS-CROSSING. `tests/scripts` is a FOURTH hand-maintained list,
     and `tools/check.sh` has never run any of it: all 569 golden cases, the two
     doorway-crossing cases included, ran only because the builder prompt tells
     the agent to run them -- the same voluntary mechanism under which
     `tests/formats` sat broken for 25 batches (review-1 of run-20261110-230107
     F2). Running the whole suite here is not possible (measured: 7m27s warm, so
     the gate suite would go from about a minute to eight), so this check runs
     the ledger row's own acceptance command, `make -C tests/scripts
     test_crossing`, which is 0.7 s warm and 38 s from a clean build. It reports
     a MUST for no `make`, a timeout, a nonzero exit, a run that printed no
     verdict line, a run with a FAIL line, and -- the pass generator this shape
     makes possible -- a verdict line with ZERO `PASS L<n>_crossing` lines, which
     is what deleting the cases instead of fixing them would look like. It needs
     the game data, because the crossing cases read the shipped DTI; a host
     without it is a MUST naming that cause, not a skip.

Exit code 1 if any finding is a MUST.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Callable

REPO_ROOT = Path(__file__).resolve().parents[2]
CORE_SRC_REL = "mdk_core/src"
LINUX_DIR_REL = "mdk_linux"
CMAKE_REL = "mdk_linux/CMakeLists.txt"
MAKEFILE_3DS_REL = "mdk_3ds/Makefile"
# AGENTS.md 8 -- the excluded trees are not part of the contract's scope.
EXCLUDED_CORE = ("mdk_core/third_party", "mdk_core/tools")
SRC_EXTS = (".cpp", ".c", ".cc", ".cxx")
HDR_EXTS = (".h", ".hpp", ".hh", ".inc")
LOCAL_INCLUDE_RE = re.compile(r'^[ \t]*#[ \t]*include[ \t]+"([^"]+)"', re.M)
# Angle-bracket includes are resolved against the build's -I paths only (a
# quoted include also searches the including file's own directory first).
ANGLE_INCLUDE_RE = re.compile(r"^[ \t]*#[ \t]*include[ \t]+<([^>]+)>", re.M)
WILDCARD_CORE_RE = re.compile(r"\$\(wildcard\s+\$\(CORESRC\)([^)]*)\)")
# The format-test target: a third source list, and the one no gate built.
FORMATS_DIR_REL = "tests/formats"
FORMATS_MAKE_REL = "tests/formats/Makefile"
FORMATS_TREE_REL = f"{CORE_SRC_REL}/formats"
# The target compiles ~20 translation units at -O1, so a cold build is tens of
# seconds and a warm one is under a second. This bound is a hang detector, not a
# budget: running past it is a finding, never a skip.
FORMATS_LINK_TIMEOUT = 900
# The scripts' crossing cases: the acceptance command of the ledger row
# 3ds.collision-doors, and the only part of `tests/scripts` cheap enough for the
# gate suite to run on every batch (measured: 0.7 s warm, 38 s cold, against the
# full suite's 7m27s warm).
SCRIPTS_DIR_REL = "tests/scripts"
SCRIPTS_TARGET = "test_crossing"
SCRIPTS_TIMEOUT = 900
SCRIPTS_VERDICT = "All crossing cases match the oracle."
SCRIPTS_PASS_RE = re.compile(r"^PASS L(\d+)_crossing", re.M)
SCRIPTS_FAIL_RE = re.compile(r"^FAIL L\d+_crossing", re.M)
# The case set this check exists to protect, so "some crossing passed" can never be
# read as "the crossing is pinned". L4 is the corridor doorway (the oracle's
# ArenaSwitchTests) and L7 is the tunnel walk (DoorArenaTests) whose s=675 line is
# the only thing that separates the crossing rule from a geometric arena lookup --
# review-1 F3 of run-20261110-230107 bought it and review-2 F2 of the same run is
# this constant: a gate that accepts ONE PASS lets a future batch delete the L7
# entry from crossingCheck's levels[] with every gate green and its golden orphaned.
SCRIPTS_REQUIRED_CASES = frozenset({"4", "7"})
# How many seeded arms ran is DERIVED from the arms themselves (see crossing_arms);
# a constant here would be a third copy of the number, and the note, the selftest
# verdict and the arms could disagree about it with every gate green (review-4 F3).
SOURCES_ASSIGN_RE = re.compile(r"^SOURCES\s*[:+]?=")
CHECK_COUNT_RE = re.compile(r"(\d+) checks, (\d+) failures")


def parse_make_var(name: str, text: str) -> str:
    """The value of a simple `NAME := value` assignment ('' when absent)."""
    match = re.search(rf"^{name}\s*[:+]?=\s*(\S[^\n]*)", text, re.M)
    return match.group(1).split("#", 1)[0].strip() if match else ""


def parse_make_sources(text: str) -> list[str]:
    """The entries of a `SOURCES := \\` continuation block, comments stripped.

    This reads the SAME block `make` reads -- a check that parsed a copy of the
    list would be checking its own copy, not the build.
    """
    lines = text.splitlines()
    for index, raw in enumerate(lines):
        line = raw.split("#", 1)[0].strip()
        if not SOURCES_ASSIGN_RE.match(line):
            continue
        entries: list[str] = []
        rest = line.split("=", 1)[1].strip()
        while True:
            if rest.endswith("\\"):
                piece = rest[:-1].strip()
                if piece:
                    entries.append(piece)
                index += 1
                if index >= len(lines):
                    break
                rest = lines[index].split("#", 1)[0].strip()
                continue
            if rest:
                entries.append(rest)
            break
        return entries
    return []


def is_core_source(path: Path, root: Path) -> bool:
    """Whether a resolved path is under `mdk_core/src` (and not a test's own TU)."""
    try:
        path.resolve().relative_to((root / CORE_SRC_REL).resolve())
        return True
    except ValueError:
        return False


def resolve_format_entry(entry: str, make_dir: Path, core_rel: str) -> Path | None:
    """A format-test SOURCES entry resolved the way its own Makefile resolves it.

    None means "this parser cannot resolve it", which is reported rather than
    skipped silently: an entry that stops being readable is the same omission
    wearing a different hat.
    """
    if entry.startswith("$(CORE)/"):
        if not core_rel:
            return None
        return (make_dir / core_rel / entry[len("$(CORE)/"):]).resolve()
    if "$(" in entry:
        return None
    return (make_dir / entry).resolve()


def build_and_run_format_target(root: Path) -> tuple[bool, str]:
    """(clean, detail) from `make -C tests/formats`, which builds AND runs it.

    Every way this fails to produce a measurement is a failure. A host with no
    `make`, a timeout, a nonzero exit, and -- the subtle one -- an exit 0 that
    printed no check count, all mean "the format target was not verified", and
    reporting that as clean is how the target rotted for 25 batches.
    """
    make = shutil.which("make")
    if make is None:
        return False, ("make is not on PATH, so the format target could not be built "
                       "-- NOT CHECKED is not a pass")
    try:
        proc = subprocess.run(
            [make, "-C", str(root / FORMATS_DIR_REL)],
            capture_output=True, text=True, timeout=FORMATS_LINK_TIMEOUT,
        )
    except subprocess.TimeoutExpired:
        return False, f"make -C {FORMATS_DIR_REL} did not finish within {FORMATS_LINK_TIMEOUT}s"
    except OSError as exc:
        return False, f"make -C {FORMATS_DIR_REL} could not start: {exc}"
    output = (proc.stdout or "") + (proc.stderr or "")
    if proc.returncode != 0:
        reason = ""
        for line in output.splitlines():
            if any(needle in line for needle in ("undefined reference", "error:", "LINK GRAPH")):
                reason = line.strip()
                break
        if not reason:
            reason = (output.strip().splitlines() or ["(no output)"])[-1].strip()
        return False, f"make -C {FORMATS_DIR_REL} exited {proc.returncode}: {reason}"
    match = CHECK_COUNT_RE.search(output)
    if match is None:
        return False, (f"make -C {FORMATS_DIR_REL} exited 0 without printing a check count, "
                       "so its tests did not run")
    checks, failures = int(match.group(1)), int(match.group(2))
    if failures:
        return False, f"the format target reports {checks} checks, {failures} failures"
    if not checks:
        return False, "the format target reports zero checks, so it tested nothing"
    return True, f"make -C {FORMATS_DIR_REL} exit 0 -- {checks} checks, 0 failures"


def judge_crossing_output(returncode: int, output: str) -> tuple[bool, str]:
    """(clean, detail) for the text the crossing target produced.

    Split out of run_scripts_crossing_target so the adjudication -- the only part of
    this check that is a decision rather than a subprocess -- can be exercised by
    `--selftest` without building the world. Review-3 F2 of run-20261110-230107: the
    required-case rule that review-2 F2 bought was pinned by NOTHING in the tree, so
    shrinking SCRIPTS_REQUIRED_CASES or weakening the set difference back to "not
    zero" kept every gate green and no test noticed.

    Every way the target fails to produce a measurement is a failure, exactly as
    PARITY-FORMATS-LINK treats the format target: a nonzero exit (which is also what a
    host with no game data looks like), a verdict line that never printed, a FAIL line
    that printed anyway, and -- the pass generator this particular shape allows -- a
    verdict with ZERO `PASS L<n>_crossing` lines behind it, which is what deleting the
    cases instead of fixing them would look like. Zero is not the only escape hatch
    though: one PASS also satisfies "not zero", so the case set is required by name
    (SCRIPTS_REQUIRED_CASES) and a run that skipped one of the two levels is a MUST,
    not a smaller pass (review-2 F2).
    """
    if returncode != 0:
        reason = ""
        for line in output.splitlines():
            if any(needle in line for needle in ("FAIL", "error:", "undefined reference",
                                                 "No such file", "not found")):
                reason = line.strip()
                break
        if not reason:
            reason = (output.strip().splitlines() or ["(no output)"])[-1].strip()
        return False, (f"make -C {SCRIPTS_DIR_REL} {SCRIPTS_TARGET} exited {returncode}: "
                       f"{reason} (no game data reads exactly like this)")
    if SCRIPTS_VERDICT not in output:
        return False, (f"make -C {SCRIPTS_DIR_REL} {SCRIPTS_TARGET} exited 0 without printing "
                       f"{SCRIPTS_VERDICT!r}, so the crossing cases did not run")
    if SCRIPTS_FAIL_RE.search(output):
        return False, ("the crossing target printed a FAIL line and exited 0 anyway, so its "
                       "verdict is not the result of the cases")
    passed = SCRIPTS_PASS_RE.findall(output)
    if not passed:
        return False, ("the crossing target printed its verdict with no PASS L<n>_crossing "
                       "line behind it, so there are no crossing cases left to pass")
    missing = sorted(SCRIPTS_REQUIRED_CASES - set(passed))
    if missing:
        return False, ("the crossing target exited 0 but never ran %s, so the case set this "
                       "check exists to protect is not intact -- a deleted case is the same "
                       "pass generator as a zero case, one degree weaker"
                       % " and ".join("L" + n + "_crossing" for n in missing))
    return True, ("make -C %s %s exit 0 -- %d crossing case(s): %s"
                  % (SCRIPTS_DIR_REL, SCRIPTS_TARGET, len(passed),
                     ", ".join("L" + n for n in passed)))


def run_scripts_crossing_target(root: Path) -> tuple[bool, str]:
    """(clean, detail) from `make -C tests/scripts test_crossing`.

    The ledger row's own acceptance command, run by the gate suite instead of by the
    builder's good intentions. The decision itself is judge_crossing_output above,
    which is where the arms of `--selftest` point.
    """
    make = shutil.which("make")
    if make is None:
        return False, ("make is not on PATH, so the crossing target could not be built "
                       "-- NOT CHECKED is not a pass")
    try:
        proc = subprocess.run(
            [make, "-C", str(root / SCRIPTS_DIR_REL), SCRIPTS_TARGET],
            capture_output=True, text=True, timeout=SCRIPTS_TIMEOUT,
        )
    except subprocess.TimeoutExpired:
        return False, (f"make -C {SCRIPTS_DIR_REL} {SCRIPTS_TARGET} did not finish within "
                       f"{SCRIPTS_TIMEOUT}s")
    except OSError as exc:
        return False, f"make -C {SCRIPTS_DIR_REL} {SCRIPTS_TARGET} could not start: {exc}"
    output = (proc.stdout or "") + (proc.stderr or "")
    return judge_crossing_output(proc.returncode, output)


def crossing_arms() -> list[tuple[str, Callable[[], tuple[bool, str]], bool]]:
    """The seeded arms this gate must adjudicate: (label, adjudicate, want_clean).

    The same shape as `gates_completion --selftest`: an arm the adjudicator accepts
    when it should refuse is a pass generator restored, and an arm it refuses when it
    should accept means the gate is red on a healthy tree. Neither is visible from a
    normal run, because a normal run only ever sees the one output the tree produces.

    `run()` calls this on every gate run -- tools/check.sh is owner-protected and a
    batch cannot wire a new `--selftest` call into it, so the arms ride along with the
    gate they pin instead of waiting for an owner -- and `--selftest` prints them on
    their own.

    THE COUNT IS THE LENGTH OF THIS LIST. It used to be a constant next to it, which
    under-reported the moment anyone added an arm and reddened nothing (review-4 F3),
    so the wrapper's no-`make` arm is an entry like every other instead of a tail that
    had to be counted by hand.
    """
    green = ("PASS L4_crossing: the ported doorway crossing\n"
             "PASS L7_crossing: the ported doorway crossing\n" + SCRIPTS_VERDICT + "\n")

    def judge(returncode: int, output: str) -> Callable[[], tuple[bool, str]]:
        return lambda: judge_crossing_output(returncode, output)

    def make_missing() -> tuple[bool, str]:
        # The wrapper's own escape hatch: a host with no `make` is a MUST, never a
        # skip. This arm patches the module's lookup instead of feeding the judge.
        saved = shutil.which
        shutil.which = lambda _name: None
        try:
            return run_scripts_crossing_target(Path("/nonexistent"))
        finally:
            shutil.which = saved

    return [
        ("both required cases green", judge(0, green), True),
        ("the cases in the other order",
         judge(0, "PASS L7_crossing: x\nPASS L4_crossing: x\n" + SCRIPTS_VERDICT + "\n"), True),
        ("one case only -- review-2 F2's pass generator",
         judge(0, "PASS L4_crossing: x\n" + SCRIPTS_VERDICT + "\n"), False),
        ("the verdict printed with no PASS behind it", judge(0, SCRIPTS_VERDICT + "\n"), False),
        ("the verdict never printed at all", judge(0, "PASS L4_crossing: x\n"), False),
        ("a FAIL line under a green verdict", judge(0, green + "FAIL L7_crossing: x\n"), False),
        ("a nonzero exit (no game data, a broken build)",
         judge(2, "make: *** [test] Error 1\n"), False),
        ("make-missing-on-PATH: the wrapper's escape hatch", make_missing, False),
    ]


def crossing_arm_failures() -> tuple[list[str], int]:
    """The selftest's failures, and how many arms ran (derived -- see crossing_arms)."""
    failures: list[str] = []
    arms = crossing_arms()
    for label, adjudicate, want_clean in arms:
        clean, detail = adjudicate()
        if clean != want_clean:
            failures.append("%s: adjudicated %s, expected %s -- %s"
                            % (label, "clean" if clean else "red",
                               "clean" if want_clean else "red", detail))
    return failures, len(arms)


def crossing_selftest() -> int:
    """Print the seeded arms and their verdicts (the `--selftest` entry point)."""
    failures, count = crossing_arm_failures()
    for failure in failures:
        print("  FAIL crossing selftest " + failure)
    if failures:
        print(f"CROSSING SELFTEST: FAIL ({len(failures)} of {count} arms)")
        return 1
    print(f"CROSSING SELFTEST: PASS ({count} arms)")
    return 0
@dataclass
class Finding:
    rule: str
    severity: str
    file: str
    detail: str


@dataclass
class Report:
    status: str
    blockers: int
    reviews: int
    core_sources: int
    core_headers: int
    linux_entries: int
    three_ds_glob_depth: int
    format_target: str = ""
    crossing_target: str = ""
    findings: list[Finding] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)

    def to_json(self) -> dict[str, object]:
        return {
            "status": self.status,
            "blockers": self.blockers,
            "reviews": self.reviews,
            "core_sources": self.core_sources,
            "core_headers": self.core_headers,
            "linux_entries": self.linux_entries,
            "three_ds_glob_depth": self.three_ds_glob_depth,
            "format_target": self.format_target,
            "crossing_target": self.crossing_target,
            "notes": self.notes,
            "findings": [asdict(f) for f in self.findings],
        }


def repo_rel(path: Path, root: Path) -> str:
    try:
        return path.resolve().relative_to(root.resolve()).as_posix()
    except ValueError:
        return path.as_posix()


def core_sources(root: Path) -> tuple[list[Path], list[Path]]:
    """(compilable sources, headers) under mdk_core/src, minus the excluded trees."""
    base = root / CORE_SRC_REL
    sources: list[Path] = []
    headers: list[Path] = []
    if not base.exists():
        return sources, headers
    for path in sorted(base.rglob("*")):
        if not path.is_file():
            continue
        rel = repo_rel(path, root)
        if any(rel == ex or rel.startswith(ex.rstrip("/") + "/") for ex in EXCLUDED_CORE):
            continue
        if path.suffix in SRC_EXTS:
            sources.append(path)
        elif path.suffix in HDR_EXTS:
            headers.append(path)
    return sources, headers


def parse_cmake_sources(text: str) -> list[str]:
    """The entries of the `set(SOURCES ...)` block, comments stripped."""
    match = re.search(r"set\s*\(\s*SOURCES\b(.*?)\n\)", text, re.S)
    if not match:
        return []
    entries: list[str] = []
    for raw in match.group(1).splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        entries.append(line.strip('"'))
    return entries


def resolve_linux_entry(entry: str, root: Path) -> Path:
    """A `SOURCES` entry resolved to a path on disk."""
    entry = entry.strip()
    if entry.startswith("${CORE}"):
        rest = entry[len("${CORE}"):].lstrip("/")
        return root / "mdk_core" / rest
    return root / LINUX_DIR_REL / entry


def resolve_include(including: Path, name: str, root: Path, quoted: bool = True) -> Path | None:
    """Where `#include "name"` lands, the way the compiler would look.

    A quoted include searches the including file's own directory first, then
    the -I paths (`mdk_core/src`, `mdk_linux/src`). An angle-bracket include
    searches the -I paths only. The system headers (`<cmath>`, `<3ds.h>`) do
    not exist under those roots, so they resolve to nothing -- which is the
    correct answer here: they are not the project's headers.
    """
    candidates = []
    if quoted:
        candidates.append(including.parent / name)
    candidates.append(root / CORE_SRC_REL / name)
    candidates.append(root / LINUX_DIR_REL / "src" / name)
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    return None


def reachable_headers(roots: list[Path], root: Path) -> set[Path]:
    """Every file (headers included) reachable from `roots` through local includes."""
    seen: set[Path] = set()
    stack = [p.resolve() for p in roots]
    while stack:
        current = stack.pop()
        if current in seen or not current.is_file():
            continue
        seen.add(current)
        try:
            text = current.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for match in LOCAL_INCLUDE_RE.finditer(text):
            target = resolve_include(current, match.group(1), root, quoted=True)
            if target is not None and target not in seen:
                stack.append(target)
        for match in ANGLE_INCLUDE_RE.finditer(text):
            target = resolve_include(current, match.group(1), root, quoted=False)
            if target is not None and target not in seen:
                stack.append(target)
    return seen


def three_ds_glob_depth(root: Path) -> tuple[int, list[str]]:
    """(deepest level the 3DS Makefile's core globs reach, the glob patterns)."""
    makefile = root / MAKEFILE_3DS_REL
    if not makefile.exists():
        return -1, []
    text = makefile.read_text(encoding="utf-8", errors="replace")
    patterns: list[str] = []
    depth = -1
    for match in WILDCARD_CORE_RE.finditer(text):
        inner = match.group(1)
        patterns.append(f"$(CORESRC){inner}")
        level = inner.count("/") - 1
        if level > depth:
            depth = level
    return depth, patterns


def source_depth(path: Path, base: Path) -> int:
    """The directory depth of a source under `base` (0 == directly in base)."""
    try:
        rel = path.resolve().relative_to(base.resolve())
    except ValueError:
        return 0
    return len(rel.parts) - 1


def run(root: Path | None = None) -> Report:
    root = (root or REPO_ROOT).resolve()
    findings: list[Finding] = []
    notes: list[str] = []

    sources, headers = core_sources(root)
    cmake_path = root / CMAKE_REL
    if not cmake_path.exists():
        findings.append(
            Finding("PARITY-LINUX-LIST", "MUST", CMAKE_REL,
                    "the Linux build file does not exist -- core parity cannot be checked")
        )
        entries: list[str] = []
    else:
        entries = parse_cmake_sources(cmake_path.read_text(encoding="utf-8", errors="replace"))

    listed: set[Path] = set()
    for entry in entries:
        path = resolve_linux_entry(entry, root)
        if not path.exists():
            findings.append(
                Finding("PARITY-LINUX-GHOST", "MUST", f"{LINUX_DIR_REL}/{entry}",
                        "listed in the Linux SOURCES but does not exist on disk")
            )
            continue
        listed.add(path.resolve())

    # --- 1. every core .cpp must be in the Linux source list ----------------
    for path in sources:
        if path.resolve() not in listed:
            findings.append(
                Finding("PARITY-LINUX-LIST", "MUST", repo_rel(path, root),
                        "core source is NOT in mdk_linux/CMakeLists.txt's SOURCES -- "
                        "the 3DS build compiles it (Makefile glob) and the Linux "
                        "build does not: a SILENTLY DIFFERENT GAME, not a phase")
            )

    # --- 2. every core .h must be listed or reachable ----------------------
    reachable = reachable_headers(list(listed), root)
    for path in headers:
        if path.resolve() in listed:
            continue
        if path.resolve() in reachable:
            continue
        findings.append(
            Finding("PARITY-LINUX-HEADER", "MUST", repo_rel(path, root),
                    "core header is neither listed in the Linux SOURCES nor "
                    "included by any Linux translation unit -- the Linux build "
                    "never sees it")
        )

    # --- 3. the 3DS Makefile's per-depth globs must reach every core source --
    depth, patterns = three_ds_glob_depth(root)
    core_base = root / CORE_SRC_REL
    if depth < 0:
        findings.append(
            Finding("PARITY-3DS-DEPTH", "MUST", MAKEFILE_3DS_REL,
                    "the 3DS Makefile does not exist -- its globs cannot be checked")
        )
    else:
        deep_dirs: set[str] = set()
        for path in sources:
            level = source_depth(path, core_base)
            if level > depth:
                rel = repo_rel(path, root)
                deep_dirs.add(str(Path(rel).parent))
                findings.append(
                    Finding("PARITY-3DS-DEPTH", "MUST", rel,
                            f"core source is {level} directory level(s) deep; the "
                            f"3DS Makefile's deepest core glob reaches {depth} -- "
                            "the 3DS build silently drops it")
                )
        notes.append(
            f"3DS Makefile core globs: {', '.join(patterns) if patterns else '(none)'} "
            f"-> deepest level reached = {depth}"
        )
        if deep_dirs:
            notes.append(
                "core subdirectories DEEPER than the 3DS Makefile's deepest glob "
                "(a new nesting level needs a new glob): "
                + ", ".join(sorted(deep_dirs))
            )
        else:
            notes.append(
                "no core subdirectory is deeper than the 3DS Makefile's deepest glob"
            )

    # --- 4. the Makefile keys objects by basename: no collisions ------------
    by_name: dict[str, list[str]] = {}
    for tree in ("mdk_core/src", "mdk_3ds/src"):
        base = root / tree
        if not base.exists():
            continue
        for path in sorted(base.rglob("*")):
            if path.is_file() and path.suffix in SRC_EXTS:
                by_name.setdefault(path.name, []).append(repo_rel(path, root))
    for name, rels in sorted(by_name.items()):
        if len(rels) > 1:
            findings.append(
                Finding("PARITY-3DS-BASENAME", "MUST", rels[0],
                        "basename collides with " + ", ".join(rels[1:])
                        + " -- the 3DS Makefile keys objects by `notdir`, so one of "
                          "them is never compiled")
            )

    # --- 6. the format-test target: a third list, and the one nothing built ---
    format_target = ""
    formats_make = root / FORMATS_MAKE_REL
    if not formats_make.exists():
        findings.append(
            Finding("PARITY-FORMATS", "MUST", FORMATS_MAKE_REL,
                    "the format-test Makefile does not exist -- its source list "
                    "cannot be checked")
        )
    else:
        make_text = formats_make.read_text(encoding="utf-8", errors="replace")
        fmt_entries = parse_make_sources(make_text)
        core_rel = parse_make_var("CORE", make_text)
        listed_core: set[Path] = set()
        if not fmt_entries:
            findings.append(
                Finding("PARITY-FORMATS", "MUST", FORMATS_MAKE_REL,
                        "no SOURCES block was parsed -- this check would be "
                        "reading a list make does not read")
            )
        else:
            make_dir = formats_make.parent
            by_name: dict[str, str] = {}
            for entry in fmt_entries:
                path = resolve_format_entry(entry, make_dir, core_rel)
                rel = repo_rel(path, root) if path is not None else entry
                if path is None:
                    findings.append(
                        Finding("PARITY-FORMATS-GHOST", "MUST", rel,
                                f"SOURCES entry {entry!r} uses a variable this "
                                "parser cannot resolve, so its existence is unknown")
                    )
                    continue
                if not path.exists():
                    findings.append(
                        Finding("PARITY-FORMATS-GHOST", "MUST", rel,
                                "listed in tests/formats/Makefile's SOURCES but "
                                "does not exist on disk")
                    )
                    continue
                if path.suffix in SRC_EXTS and is_core_source(path, root):
                    listed_core.add(path)
                previous = by_name.get(path.name)
                if previous is not None and previous != rel:
                    findings.append(
                        Finding("PARITY-FORMATS-BASENAME", "MUST", rel,
                                f"basename collides with {previous} -- the "
                                "Makefile keys objects by `notdir`, so one of "
                                "them is never compiled")
                    )
                by_name[path.name] = rel
            tree = sorted(p for p in (root / FORMATS_TREE_REL).glob("*.cpp")) \
                if (root / FORMATS_TREE_REL).is_dir() else []
            untested = [repo_rel(p, root) for p in tree if p not in listed_core]
            notes.append(
                f"format target SOURCES: {len(fmt_entries)} entries, "
                f"{len(listed_core)} of them under {CORE_SRC_REL}"
            )
            notes.append(
                f"format target compiles {len(tree) - len(untested)} of {len(tree)} sources "
                f"under {FORMATS_TREE_REL}; NOT compiled by it: "
                + (", ".join(Path(p).name for p in untested) if untested else "(none)")
            )
        clean, detail = build_and_run_format_target(root)
        format_target = detail
        if not clean:
            findings.append(
                Finding("PARITY-FORMATS-LINK", "MUST", FORMATS_DIR_REL,
                        detail + " -- the format target is the ledger row "
                                 "core.formats' acceptance command, and a target "
                                 "nothing builds is a target that rots")
            )
        notes.append(f"format target: {detail}")

    # --- 7. the scripts' crossing target: a row's acceptance command, gated ---
    # The fourth hand-maintained list, and the one the gate suite did not run: the
    # crossing cases lived inside `tick1`'s case list, which `tools/check.sh` never
    # invokes, so "the row cannot rot" was a promise about the builder's habits.
    clean, detail = run_scripts_crossing_target(root)
    crossing_target = detail
    if not clean:
        findings.append(
            Finding("PARITY-SCRIPTS-CROSSING", "MUST", SCRIPTS_DIR_REL,
                    detail + " -- this is the ledger row 3ds.collision-doors' "
                             "acceptance command, and a target only the builder "
                             "chooses to run is a target that rots")
        )
    notes.append(f"crossing target: {detail}")

    # --- 8. the crossing gate's own seeded arms ---
    # The check that exists to refuse a one-case PASS is worth nothing if the refusal
    # can be edited away unnoticed, and nothing in the tree imports this file
    # (review-3 F2 of run-20261110-230107). So the seeded outputs run HERE, inside the
    # gate, on every run: an arm that stops reddening is a finding on the same green
    # run that would otherwise have hidden it.
    arm_failures, arm_count = crossing_arm_failures()
    if arm_failures:
        findings.append(
            Finding("PARITY-SCRIPTS-CROSSING-SELFTEST", "MUST", SCRIPTS_DIR_REL,
                    "the crossing target's own seeded arms did not all adjudicate as "
                    "claimed: " + "; ".join(arm_failures)
                    + " -- a crossing arm that stops reddening is the pass generator "
                      "review-2 F2 bought this check to close, restored")
        )
    notes.append("crossing selftest: %d seeded arms, %s"
                 % (arm_count,
                    "all adjudicated as claimed" if not arm_failures
                    else f"{len(arm_failures)} WRONG"))

    blockers = sum(1 for f in findings if f.severity == "MUST")
    reviews = sum(1 for f in findings if f.severity == "SHOULD")
    status = "FAIL" if blockers else "PASS"
    return Report(
        status=status,
        blockers=blockers,
        reviews=reviews,
        core_sources=len(sources),
        core_headers=len(headers),
        linux_entries=len(entries),
        three_ds_glob_depth=depth,
        format_target=format_target,
        crossing_target=crossing_target,
        findings=findings,
        notes=notes,
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="gates_core_parity",
        description="core<->target parity: the two builds must link the same game",
    )
    parser.add_argument("--root", type=Path, default=REPO_ROOT)
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--selftest", action="store_true",
                        help="run the seeded crossing-target outputs; every arm must "
                             "adjudicate as claimed")
    args = parser.parse_args(argv)

    if args.selftest:
        return crossing_selftest()

    report = run(args.root)
    if args.json:
        print(json.dumps(report.to_json(), indent=2))
        return 1 if report.blockers else 0

    print("CORE PARITY GATE -- mdk_core/src vs the 3DS Makefile and the Linux CMakeLists")
    print(f"  core sources: {report.core_sources}   core headers: {report.core_headers}")
    print(f"  linux SOURCES entries: {report.linux_entries}")
    for note in report.notes:
        print(f"  note: {note}")
    if not report.findings:
        print("\n  VERDICT: CONTRACT CLEAN -- every core source is on both targets, and")
        print("  both build-and-run targets ran clean: the format tests and the crossing")
        print("  cases (the notes above say what each measured).")
        return 0
    print(f"\n  {len(report.findings)} FINDING(S):")
    for finding in report.findings:
        print(f"    [{finding.rule}] {finding.file}")
        print(f"        {finding.detail}")
    # The verdict line names NO cause of its own, because it is printed for every
    # finding and a generic sentence over a specific failure is a message that lies:
    # a red crossing golden used to be reported here as a core-source parity problem
    # (review-2 F3). The rule tag on each finding is the diagnosis.
    print("\n  VERDICT: VIOLATIONS -- see the rule tag on each finding above; that tag")
    print("  is the cause, not this line. A core source on one target only is a")
    print("  SILENTLY DIFFERENT GAME, and a crossing finding is the ledger row's")
    print("  acceptance command failing. They are different failures.")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
