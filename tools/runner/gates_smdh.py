#!/usr/bin/env python3
"""
gates_smdh.py -- the CIA's home-menu identity, verified against the BUILT .cia.

    python3 tools/runner/gates_smdh.py [--makefile FILE] [--cia FILE] [--json]

WHY THIS GATE EXISTS (AGENTS.md 9). The title's identity on the 3DS HOME menu
is THREE AUTHORED STRINGS -- the short description, the long description (shown
on hover) and the publisher -- and they are authored in one place,
`mdk_3ds/Makefile`:

    APP_TITLE        := MDK
    APP_DESCRIPTION  := Metal Dungeon
    APP_AUTHOR       := Shiny Entertainment

The publisher is NOT the engine's internal source name: it is the studio that
made the original game, which is what a player expects to see on a homebrew
screen. That is a rule about CONTENT, and a rule about content is not
enforced by reading the file that states it -- the file states the intent, the
CIA carries the result, and the two can disagree. A stale `build/mdk.smdh`
reused under a renamed title, a hand-edited SMDH, or a Makefile edited
without a rebuild all produce a CIA whose strings are simply not the ones the
Makefile says.

So this gate reads the strings from the Makefile AND re-reads the SMDH out of
the BUILT .cia, and fails if any populated language slot disagrees.

THE SMDH IS A STRUCTURE, NOT A STRING. `smdhtool` writes sixteen language
slots, each 0x200 bytes:

    0x000  short description   0x80 bytes  (64 UTF-16LE code units)
    0x080  long description    0x100 bytes (128 UTF-16LE code units)
    0x180  publisher           0x80 bytes  (64 UTF-16LE code units)

and the SMDH header is `magic "SMDH" | version u16 | reserved u16 | titles`.
Inside a CIA the SMDH is one of the sixteen CONTENT blocks, so the gate
searches the content table for the block whose first four bytes are the magic
rather than assuming an index.

NO .cia YET IS A SKIP, NOT A FAILURE. The gate needs a BUILT .cia to check;
before the first CIA build there is nothing to check, and failing would make
the gate unusable exactly when it is needed least. It says so, loudly, and
exits 0.

Exit code 1 if any populated language slot disagrees with the Makefile.
"""

from __future__ import annotations

import argparse
import json
import re
import struct
from dataclasses import asdict, dataclass, field
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
MAKEFILE_REL = "mdk_3ds/Makefile"
CIA_DIR_REL = "mdk_3ds"

# The SMDH's title slots.
SMDH_MAGIC = b"SMDH"
SMDH_TITLES_OFFSET = 0x08
TITLE_SLOT_SIZE = 0x200
TITLE_COUNT = 16
SHORT_SIZE = 0x80
LONG_SIZE = 0x100
PUBLISHER_SIZE = 0x80
TITLE_FIELDS = ("short", "long", "publisher")

# The CIA container: a 0x2020-byte header for the normal (type 0) CIA, then
# the content blocks.
CIA_HEADER_SIZE = 0x2020
CIA_CONTENT_SIZES_OFF = 0x18
CIA_CONTENT_OFFSETS_OFF = 0x58
CIA_CONTENT_COUNT = 16


@dataclass
class Finding:
    rule: str
    severity: str
    file: str
    detail: str


@dataclass
class Report:
    status: str  # PASS | FAIL | SKIP
    makefile: str
    cia: str
    expected: dict[str, str] = field(default_factory=dict)
    populated_slots: int = 0
    findings: list[Finding] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)

    def to_json(self) -> dict[str, object]:
        data = asdict(self)
        data["findings"] = [asdict(f) for f in self.findings]
        return data


def makefile_value(text: str, name: str) -> str:
    match = re.search(
        r"^[ \t]*" + re.escape(name) + r"[ \t]*:?=[ \t]*(.*)$", text, re.M
    )
    if not match:
        return ""
    value = match.group(1).strip()
    if value.endswith("\\"):
        value = value[:-1].strip()
    return value.strip('"').strip("'")


def decode_utf16(raw: bytes) -> str:
    text = raw.decode("utf-16-le", errors="replace")
    return text.split("\x00", 1)[0].strip()


def find_smdh(cia: bytes) -> tuple[bytes, int] | None:
    """(the SMDH block, the content index it was found at), or None."""
    if len(cia) < CIA_HEADER_SIZE:
        return None
    sizes = struct.unpack_from("<16I", cia, CIA_CONTENT_SIZES_OFF)
    offsets = struct.unpack_from("<16I", cia, CIA_CONTENT_OFFSETS_OFF)
    for index in range(CIA_CONTENT_COUNT):
        size = sizes[index]
        offset = offsets[index]
        if size == 0:
            continue
        start = offset
        end = offset + size
        if end > len(cia) or start < 0:
            continue
        block = cia[start:end]
        if block[:4] == SMDH_MAGIC:
            return block, index
    return None


def smdh_titles(smdh: bytes) -> list[dict[str, str]]:
    """The sixteen language slots of an SMDH block."""
    titles: list[dict[str, str]] = []
    for index in range(TITLE_COUNT):
        base = SMDH_TITLES_OFFSET + index * TITLE_SLOT_SIZE
        slot = smdh[base: base + TITLE_SLOT_SIZE]
        if len(slot) < TITLE_SLOT_SIZE:
            slot = slot + b"\x00" * (TITLE_SLOT_SIZE - len(slot))
        titles.append(
            {
                "short": decode_utf16(slot[0:SHORT_SIZE]),
                "long": decode_utf16(slot[SHORT_SIZE:SHORT_SIZE + LONG_SIZE]),
                "publisher": decode_utf16(
                    slot[SHORT_SIZE + LONG_SIZE: SHORT_SIZE + LONG_SIZE + PUBLISHER_SIZE]
                ),
            }
        )
    return titles


def find_cia(root: Path, explicit: Path | None) -> Path | None:
    if explicit is not None:
        return explicit if explicit.exists() else None
    build_dir = root / CIA_DIR_REL
    if not build_dir.is_dir():
        return None
    candidates = sorted(build_dir.glob("*.cia"))
    return candidates[0] if candidates else None


def run(root: Path | None = None, makefile: Path | None = None,
        cia: Path | None = None) -> Report:
    root = (root or REPO_ROOT).resolve()
    makefile_path = makefile or (root / MAKEFILE_REL)
    findings: list[Finding] = []
    notes: list[str] = []

    expected: dict[str, str] = {}
    if not makefile_path.exists():
        findings.append(
            Finding("SMDH-IDENTITY", "MUST", MAKEFILE_REL,
                    "the 3DS Makefile does not exist -- the home-menu identity "
                    "cannot be read")
        )
        return Report("FAIL", MAKEFILE_REL, "", {}, 0, findings, notes)

    text = makefile_path.read_text(encoding="utf-8", errors="replace")
    expected = {
        "short": makefile_value(text, "APP_TITLE"),
        "long": makefile_value(text, "APP_DESCRIPTION"),
        "publisher": makefile_value(text, "APP_AUTHOR"),
    }
    for field_name, value in expected.items():
        if not value:
            findings.append(
                Finding("SMDH-IDENTITY", "MUST", MAKEFILE_REL,
                        f"the identity block does not define the {field_name} "
                        "string (APP_TITLE / APP_DESCRIPTION / APP_AUTHOR) -- "
                        "AGENTS.md 9 requires all three")
            )
    if findings:
        return Report("FAIL", MAKEFILE_REL, "", expected, 0, findings, notes)

    cia_path = find_cia(root, cia)
    if cia_path is None:
        notes.append(
            "no built .cia found -- the SMDH check is SKIPPED, not passed. "
            "Build one with: make -C mdk_3ds cia"
        )
        return Report("SKIP", MAKEFILE_REL, "", expected, 0, findings, notes)

    blob = cia_path.read_bytes()
    found = find_smdh(blob)
    if found is None:
        findings.append(
            Finding("SMDH-MISSING", "MUST", cia_path.name,
                    "the .cia carries no SMDH content block -- the HOME menu "
                    "would show the system's default identity")
        )
        return Report("FAIL", MAKEFILE_REL, cia_path.name, expected, 0, findings, notes)

    smdh, index = found
    titles = smdh_titles(smdh)
    populated = 0
    for slot_index, slot in enumerate(titles):
        if not any(slot[name] for name in TITLE_FIELDS):
            continue
        populated += 1
        for name in TITLE_FIELDS:
            got = slot[name]
            want = expected[name]
            if got != want:
                findings.append(
                    Finding("SMDH-MISMATCH", "MUST", cia_path.name,
                            f"language slot {slot_index}: {name} is {got!r}, the "
                            f"Makefile says {want!r} -- the HOME menu would show "
                            "a different identity than the build declares")
                )
    if populated == 0:
        findings.append(
            Finding("SMDH-EMPTY", "MUST", cia_path.name,
                    "the SMDH has no populated language slot -- the HOME menu "
                    "would show the system's default identity")
        )
    notes.append(
        f"SMDH found in .cia content block {index}; {populated} populated "
        f"language slot(s) checked against {MAKEFILE_REL}"
    )
    status = "FAIL" if findings else "PASS"
    return Report(status, MAKEFILE_REL, cia_path.name, expected, populated,
                  findings, notes)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="gates_smdh",
        description="the CIA's home-menu identity, re-read from the BUILT .cia",
    )
    parser.add_argument("--root", type=Path, default=REPO_ROOT)
    parser.add_argument("--makefile", type=Path, default=None)
    parser.add_argument("--cia", type=Path, default=None)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)

    report = run(args.root, args.makefile, args.cia)
    if args.json:
        print(json.dumps(report.to_json(), indent=2))
        return 1 if report.status == "FAIL" else 0

    print("SMDH GATE -- the 3DS HOME-menu identity (AGENTS.md 9)")
    print(f"  makefile: {report.makefile}")
    for name, value in report.expected.items():
        print(f"  expected {name}: {value!r}")
    for note in report.notes:
        print(f"  note: {note}")
    if report.status == "SKIP":
        print("\n  VERDICT: SKIPPED -- no built .cia to read the SMDH out of.")
        return 0
    if not report.findings:
        print(f"\n  VERDICT: CONTRACT CLEAN -- {report.populated_slots} language "
              "slot(s) agree with the Makefile.")
        return 0
    print(f"\n  {len(report.findings)} FINDING(S):")
    for finding in report.findings:
        print(f"    [{finding.rule}] {finding.file}")
        print(f"        {finding.detail}")
    print("\n  VERDICT: VIOLATIONS -- the built CIA and the Makefile disagree.")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
