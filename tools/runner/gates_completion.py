#!/usr/bin/env python3
"""The completion ledger: the only answer to "are we done?" that is checked.

Why this exists
---------------
Before this file, the loop answered "are we done?" with something that was not a
check. Tetrisphere shipped a model-emitted `FINAL` line; MDK had no answer at all
and stopped only when `ITER >= MAX_ITER`, a budget rather than an answer. Both are
attestations. This gate is the verdict: it reads `docs/COMPLETION_LEDGER.tsv`, and a
`done` row is accepted only when it names evidence that exists AND a commit that is
an ancestor of HEAD. A row cannot be marked done by asserting it.

What it deliberately does NOT do
--------------------------------
It never writes, never captures, and never runs an acceptance command. Generating
the evidence would make the gate a participant in the claim it is checking. A gate
that both produces and accepts its own evidence is the exact failure mode
`a-scorer-that-prints-incompleteness-and-exits-zero-turns-absence-into-a-pass`
describes. Evidence comes from the batch; this file only reads it.

Exit codes (mirrors netload_stub/tests/run_tests.py, and never blurred)
-----------------------------------------------------------------------
  0  the ledger is well formed and every `done` row is verified
  1  a claim failed: a `done` row is unverified, or `--strict` sees open work
  2  the check could not run: ledger missing/malformed, or not a git checkout
     -- NOT VERIFIED, never a pass

Modes
-----
  (default) --report        validate shape + every `done` row; open rows are fine
  --next    --level dN      print the first open row's id, speed and command
  --strict  --level dN      exit 1 while any row at or below dN is still open
  --selftest                run the seeded arms; every arm must redden its check

Levels are cumulative: `--level d2` means d1 rows too.
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

LEDGER_REL = "docs/COMPLETION_LEDGER.tsv"
OWNER_REL = "docs/OWNER_BLOCKS.md"
LEVELS = {"d1": 1, "d2": 2, "d3": 3}
SPEEDS = {"fast", "slow", "-"}
STATUSES = {"todo", "in-progress", "done", "owner-blocked"}
FIELDS = ("id", "level", "speed", "status", "acceptance", "evidence", "verified")


class LedgerError(Exception):
    """The ledger cannot be interpreted. Always exit 2: unverified, not passed."""


class Row:
    __slots__ = ("id", "level", "speed", "status", "acceptance", "evidence", "verified", "line")

    def __init__(self, parts: list[str], line: int) -> None:
        (self.id, self.level, self.speed, self.status,
         self.acceptance, evidence, self.verified) = parts
        self.evidence = [p.strip() for p in evidence.split(",") if p.strip()]
        self.line = line

    def rank(self) -> int:
        return LEVELS[self.level]

    def open(self) -> bool:
        return self.status in ("todo", "in-progress")


def parse_ledger(path: Path) -> list[Row]:
    if not path.is_file():
        raise LedgerError(f"{path}: ledger does not exist")
    rows: list[Row] = []
    seen: dict[str, int] = {}
    for number, raw in enumerate(path.read_text(errors="replace").splitlines(), 1):
        line = raw.rstrip("\n")
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) != len(FIELDS):
            raise LedgerError(
                f"{path}:{number}: expected {len(FIELDS)} tab-separated fields "
                f"({'/'.join(FIELDS)}), got {len(parts)}"
            )
        parts = [p.strip() for p in parts]
        row = Row(parts, number)
        if row.id in seen:
            raise LedgerError(f"{path}:{number}: duplicate id {row.id!r} (first at line {seen[row.id]})")
        seen[row.id] = number
        if row.level not in LEVELS:
            raise LedgerError(f"{path}:{number}: level {row.level!r} is not one of {sorted(LEVELS)}")
        if row.speed not in SPEEDS:
            raise LedgerError(f"{path}:{number}: speed {row.speed!r} is not one of {sorted(SPEEDS)}")
        if row.status not in STATUSES:
            raise LedgerError(f"{path}:{number}: status {row.status!r} is not one of {sorted(STATUSES)}")
        rows.append(row)
    if not rows:
        raise LedgerError(f"{path}: ledger contains no rows")
    return rows


def git(root: Path, *args: str) -> subprocess.CompletedProcess:
    return subprocess.run(["git", "-C", str(root), *args], capture_output=True, text=True)


def is_git_checkout(root: Path) -> bool:
    return git(root, "rev-parse", "--git-dir").returncode == 0


def is_ancestor(root: Path, sha: str) -> bool | None:
    """True/False for a resolvable sha; None when the object is unknown."""
    if not re.fullmatch(r"[0-9a-fA-F]{7,40}", sha):
        return False
    result = git(root, "merge-base", "--is-ancestor", sha, "HEAD")
    if result.returncode == 0:
        return True
    if result.returncode == 1:
        return False
    return None


def owner_blocked_ids(root: Path) -> set[str]:
    text = ""
    path = root / OWNER_REL
    if path.is_file():
        text = path.read_text(errors="replace")
    return {
        match.group(1)
        for match in re.finditer(r"(?m)^\s*\|?\s*([a-z0-9][a-z0-9_.-]*)\s*\|", text)
    }


def word_present(text: str, ident: str) -> bool:
    return re.search(rf"(?<![A-Za-z0-9_.-]){re.escape(ident)}(?![A-Za-z0-9_.-])", text) is not None


def validate_done_rows(rows: list[Row], root: Path, owner_text: str) -> list[str]:
    errors: list[str] = []
    for row in rows:
        if row.status == "done":
            if not row.acceptance:
                errors.append(f"{row.id}: done but names no acceptance command")
            if not row.evidence:
                errors.append(f"{row.id}: done but names no evidence path")
            for rel in row.evidence:
                if not (root / rel).exists():
                    errors.append(f"{row.id}: evidence {rel!r} does not exist")
            if not row.verified:
                errors.append(f"{row.id}: done but has no verified commit")
                continue
            ancestor = is_ancestor(root, row.verified)
            if ancestor is None:
                errors.append(f"{row.id}: verified {row.verified!r} is not a commit in this repo")
            elif not ancestor:
                errors.append(f"{row.id}: verified {row.verified!r} is not an ancestor of HEAD")
        elif row.status == "owner-blocked":
            if not word_present(owner_text, row.id):
                errors.append(
                    f"{row.id}: owner-blocked but absent from {OWNER_REL} "
                    "(a row cannot block itself)"
                )
    return errors


def validate_no_regression(rows: list[Row], root: Path) -> list[str]:
    """A row may be closed, never deleted: deleting a done row would buy completeness."""
    result = git(root, "show", f"HEAD:{LEDGER_REL}")
    if result.returncode != 0:
        return []  # ledger not committed yet: nothing to compare against
    with tempfile.NamedTemporaryFile("w", suffix=".tsv", delete=False) as handle:
        handle.write(result.stdout)
        previous_path = Path(handle.name)
    try:
        previous = parse_ledger(previous_path)
    except LedgerError:
        return []  # a malformed historical ledger is a history question, not this gate's
    finally:
        previous_path.unlink(missing_ok=True)
    before = sum(1 for row in previous if row.status == "done")
    after = sum(1 for row in rows if row.status == "done")
    if after < before:
        return [f"done rows regressed: HEAD has {before}, working tree has {after} (a row may not be deleted)"]
    return []


def summarise(rows: list[Row], level: str) -> str:
    cap = LEVELS[level]
    scoped = [row for row in rows if row.rank() <= cap]
    done = sum(1 for row in scoped if row.status == "done")
    blocked = sum(1 for row in scoped if row.status == "owner-blocked")
    pending = len(scoped) - done - blocked
    return (
        f"COMPLETION[{level}]: {done} done / {len(scoped)} total / "
        f"{blocked} owner-blocked / {pending} open"
    )


def cmd_report(rows: list[Row], root: Path, level: str) -> int:
    text = (root / OWNER_REL).read_text(errors="replace") if (root / OWNER_REL).is_file() else ""
    errors = validate_done_rows(rows, root, text) + validate_no_regression(rows, root)
    print(summarise(rows, level))
    for row in rows:
        if row.open():
            print(f"  open  {row.level} {row.speed:<4} {row.id}")
    if errors:
        print("COMPLETION: FAIL")
        for error in errors:
            print(f"  - {error}")
        return 1
    print("COMPLETION: LEDGER VERIFIED")
    return 0


def cmd_strict(rows: list[Row], root: Path, level: str) -> int:
    text = (root / OWNER_REL).read_text(errors="replace") if (root / OWNER_REL).is_file() else ""
    errors = validate_done_rows(rows, root, text) + validate_no_regression(rows, root)
    cap = LEVELS[level]
    remaining = [row for row in rows if row.rank() <= cap and row.open()]
    print(summarise(rows, level))
    if errors:
        print("COMPLETION: FAIL")
        for error in errors:
            print(f"  - {error}")
        return 1
    if remaining:
        print("COMPLETION: INCOMPLETE")
        for row in remaining:
            print(f"  - {row.level} {row.id}: {row.acceptance or '(no command)'}")
        return 1
    print(f"COMPLETION: DONE[{level}] -- every row at or below {level} is done or owner-blocked")
    return 0


def cmd_count(rows: list[Row], level: str) -> int:
    cap = LEVELS[level]
    print(sum(1 for row in rows if row.rank() <= cap and row.open()))
    return 0


def cmd_next(rows: list[Row], level: str) -> int:
    cap = LEVELS[level]
    ordered = sorted(
        (row for row in rows if row.rank() <= cap and row.open()),
        key=lambda row: (row.rank(), row.line),
    )
    if not ordered:
        print("NEXT: none — every row at or below this level is closed", file=sys.stderr)
        return 1
    row = ordered[0]
    print(f"{row.id}\t{row.speed}\t{row.acceptance}")
    return 0


# ---------------------------------------------------------------------------
# the seeded arms: a check that has not been shown to fail is not a check
# ---------------------------------------------------------------------------
def _arm_repo(done_verified: bool) -> tuple[tempfile.TemporaryDirectory, Path, str]:
    temp = tempfile.TemporaryDirectory()
    root = Path(temp.name)
    (root / "docs").mkdir()
    (root / "evidence").mkdir()
    (root / "evidence" / "proof.txt").write_text("x\n")
    subprocess.run(["git", "-C", str(root), "init", "-q"], check=True)
    subprocess.run(["git", "-C", str(root), "config", "user.email", "t@t"], check=True)
    subprocess.run(["git", "-C", str(root), "config", "user.name", "t"], check=True)
    subprocess.run(["git", "-C", str(root), "add", "-A"], check=True)
    subprocess.run(["git", "-C", str(root), "commit", "-qm", "base"], check=True)
    sha = subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"],
                         capture_output=True, text=True, check=True).stdout.strip()
    return temp, root, sha


def _write(root: Path, body: str) -> Path:
    path = root / LEDGER_REL
    path.write_text(body)
    return path


def _run_arm(name: str, root: Path, level: str, expect: int, mode: str) -> tuple[bool, str]:
    try:
        rows = parse_ledger(root / LEDGER_REL)
    except LedgerError:
        return (2 == expect, "2")
    if mode == "strict":
        rc = cmd_strict(rows, root, level)
    elif mode == "next":
        rc = cmd_next(rows, level)
    else:
        rc = cmd_report(rows, root, level)
    return (rc == expect, str(rc))


def selftest() -> int:
    arms: list[tuple[str, str, str, int, str]] = []

    def add(name, body, expect, mode="report", level="d1"):
        arms.append((name, body, level, expect, mode))

    def ledger(status="done", evidence="evidence/proof.txt", verified="__SHA__"):
        return (
            "# id\tlevel\tspeed\tstatus\tacceptance\tevidence\tverified\n"
            f"a.item\td1\tfast\t{status}\tmake test\t{evidence}\t{verified}\n"
        )

    add("valid done row passes", ledger(), 0)
    add("done row with missing evidence fails", ledger(evidence="evidence/absent.txt"), 1)
    add("done row with no evidence fails", ledger(evidence=""), 1)
    add("done row with no verified sha fails", ledger(verified=""), 1)
    add("done row with unknown sha fails", ledger(verified="deadbeef" * 5), 1)
    add("duplicate id is unverified, not passed",
        ledger() + "a.item\td1\tfast\ttodo\tx\t\t\n", 2)
    add("bad status is unverified, not passed", ledger(status="maybe"), 2)
    add("short row is unverified, not passed", "a.item\td1\tdone\n", 2)
    add("owner-blocked without an owner row fails",
        ledger(status="owner-blocked", verified=""), 1)
    add("strict with open work fails",
        ledger(status="todo", verified=""), 1, mode="strict")
    add("strict with everything closed passes", ledger(), 0, mode="strict")

    failures = 0
    for name, body, level, expect, mode in arms:
        temp, root, sha = _arm_repo(True)
        try:
            _write(root, body.replace("__SHA__", sha))
            ok, got = _run_arm(name, root, level, expect, mode)
        finally:
            temp.cleanup()
        print(f"{'PASS' if ok else 'FAIL'} arm: {name} (expected {expect}, got {got})")
        if not ok:
            failures += 1

    # The owner-block arm needs a positive control too, or it only proves refusal.
    temp, root, sha = _arm_repo(True)
    try:
        (root / OWNER_REL).write_text("| a.item | needs hardware | 2026-11-10 |\n")
        _write(root, ledger(status="owner-blocked", verified=""))
        ok, got = _run_arm("owner-blocked with an owner row passes", root, "d1", 0, "report")
    finally:
        temp.cleanup()
    print(f"{'PASS' if ok else 'FAIL'} arm: owner-blocked with an owner row passes (expected 0, got {got})")
    if not ok:
        failures += 1

    # --next must not offer a closed row and must offer the first open one.
    temp, root, sha = _arm_repo(True)
    try:
        _write(root,
               ledger().replace("__SHA__", sha) +
               "b.item\td1\tfast\ttodo\tmake b\t\t\n")
        rows = parse_ledger(root / LEDGER_REL)
        capture = cmd_next(rows, "d1")
        ok = capture == 0
    finally:
        temp.cleanup()
    print(f"{'PASS' if ok else 'FAIL'} arm: --next finds the open row (got {capture})")
    if not ok:
        failures += 1

    total = len(arms) + 2
    if failures:
        print(f"COMPLETION SELFTEST: FAIL ({failures} of {total} arms)")
        return 1
    print(f"COMPLETION SELFTEST: PASS ({total} arms)")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", default=None, help="project root (default: this repo)")
    parser.add_argument("--ledger", default=None, help=f"ledger path (default: {LEDGER_REL})")
    parser.add_argument("--level", default="d1", choices=sorted(LEVELS))
    parser.add_argument("--report", action="store_true", help="validate shape and every done row (the default)")
    parser.add_argument("--next", action="store_true", help="print the first open row")
    parser.add_argument("--count", action="store_true", help="print the number of open rows")
    parser.add_argument("--strict", action="store_true", help="exit 1 while work remains")
    parser.add_argument("--selftest", action="store_true", help="run the seeded arms")
    args = parser.parse_args()

    if args.selftest:
        return selftest()

    root = Path(args.root).resolve() if args.root else Path(__file__).resolve().parents[2]
    ledger = Path(args.ledger) if args.ledger else root / LEDGER_REL
    try:
        rows = parse_ledger(ledger)
    except LedgerError as error:
        print(f"COMPLETION: NOT VERIFIED -- {error}", file=sys.stderr)
        return 2

    if args.count:
        return cmd_count(rows, args.level)
    if args.next:
        return cmd_next(rows, args.level)
    if not is_git_checkout(root):
        print(f"COMPLETION: NOT VERIFIED -- {root} is not a git checkout", file=sys.stderr)
        return 2
    if args.strict:
        return cmd_strict(rows, root, args.level)
    return cmd_report(rows, root, args.level)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, subprocess.SubprocessError) as exc:
        print(f"COMPLETION: NOT VERIFIED -- {exc}", file=sys.stderr)
        sys.exit(2)
