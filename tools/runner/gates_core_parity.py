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

THE THREE CHECKS.

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

Exit code 1 if any finding is a MUST.
"""

from __future__ import annotations

import argparse
import json
import re
from dataclasses import asdict, dataclass, field
from pathlib import Path

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
    args = parser.parse_args(argv)

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
        print("\n  VERDICT: CONTRACT CLEAN -- every core source is on both targets.")
        return 0
    print(f"\n  {len(report.findings)} FINDING(S):")
    for finding in report.findings:
        print(f"    [{finding.rule}] {finding.file}")
        print(f"        {finding.detail}")
    print("\n  VERDICT: VIOLATIONS -- a core source on one target only is a")
    print("  SILENTLY DIFFERENT GAME, not a phase.")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
