#!/usr/bin/env python3
"""Regenerate the explicit source list in mdk_linux/CMakeLists.txt.

WHY THIS EXISTS
---------------
The Linux build lists the shared sources EXPLICITLY, and the 3DS build
GLOBS them per depth. The explicit list is what makes a source that lands
on one target only a BUILD ERROR rather than a silently different game --
and that only works if the list is complete.

Keeping it complete by hand is how it stops being complete. So the list is
regenerated from the tree by this script, and
`tools/runner/gates_core_parity.py` checks that the committed list still
matches the tree. The list in the file is a LITERAL list, which is what the
build sees; the script is only how it is kept honest.

USAGE
-----
    python3 mdk_linux/tools/sync_sources.py          # rewrite the list
    python3 mdk_linux/tools/sync_sources.py --check  # fail if it is stale
"""

import argparse
import os
import re
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
CORE = os.path.join(REPO, "mdk_core", "src")
CMAKE = os.path.join(REPO, "mdk_linux", "CMakeLists.txt")

# The markers of the region this script owns.
BEGIN = "    # ---- mdk_core: the shared game, compiled into BOTH targets ----"
END = ")\n\n# ----------------------------------------------------------------------------\n# Shaders"


def core_sources():
    """Every .cpp under mdk_core/src, sorted, as CMake-relative paths."""
    found = []
    for directory, _subdirectories, files in os.walk(CORE):
        for name in files:
            if name.endswith(".cpp"):
                full = os.path.join(directory, name)
                found.append("${CORE}/src/" + os.path.relpath(full, CORE).replace(os.sep, "/"))
    found.sort()
    return found


def render(sources):
    lines = [BEGIN]
    for source in sources:
        lines.append("    " + source)
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true",
                        help="exit non-zero if the list is stale")
    args = parser.parse_args()

    with open(CMAKE, "r", encoding="utf-8") as handle:
        text = handle.read()

    begin = text.find(BEGIN)
    end = text.find(END)
    if begin < 0 or end < 0 or end <= begin:
        print("sync_sources: could not find the source-list region in the CMakeLists", file=sys.stderr)
        return 2

    wanted = render(core_sources())
    current = text[begin:end]

    if current == wanted:
        print("sync_sources: the source list is up to date")
        return 0

    if args.check:
        print("sync_sources: the source list is STALE -- run "
              "python3 mdk_linux/tools/sync_sources.py", file=sys.stderr)
        return 1

    text = text[:begin] + wanted + text[end:]
    with open(CMAKE, "w", encoding="utf-8") as handle:
        handle.write(text)
    print("sync_sources: rewrote the core source list (%d sources)" % len(core_sources()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
