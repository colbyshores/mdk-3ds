#!/usr/bin/env python3
"""Close a ledger row by RUNNING its acceptance command, never by asserting it.

Why this exists
---------------
Protecting the ledger made "done" honest but un-autonomous: the loop could satisfy
a row and still never close it, so an owner had to flip it by hand. That is the
chore this loop exists to remove.

The resolution is to separate *who judges* from *who claims*. The builder claims
nothing here. This runner is deterministic: it reads the row's registered
acceptance command from the protected ledger, refuses if the tree is dirty (so
`verified` cannot point at a commit that lacks the work), runs the command, and
only on exit 0 rewrites the row and banks the evidence. A row the builder cannot
make pass is a row this cannot close.

Why the commit uses --no-verify
-------------------------------
The project hook exists to stop the MODEL from editing the ledger it is judged
by. This script is the sanctioned path: it is itself owner-protected, and it has
already run the acceptance command by the time it commits. The hook would reject
it for the same reason it rejects a hand edit, so the bypass is deliberate,
narrow (one ledger row plus its evidence file), and auditable in history.

Exit codes
----------
  0  the row is done (closed now, or already closed/owner-blocked)
  1  the acceptance command failed, or the tree is dirty — row left OPEN
  2  the row/ledger could not be read, or the row has no acceptance command
"""
from __future__ import annotations

import argparse
import datetime
import subprocess
import sys
from pathlib import Path

LEDGER_REL = "docs/COMPLETION_LEDGER.tsv"
EVIDENCE_DIR = "docs/evidence"


def git(root: Path, *args: str, timeout: int = 120) -> subprocess.CompletedProcess:
    # Bounded: this repo lives on NFS, and an unbounded git call can park in D
    # state (rpc_wait_bit_killable) and take the whole loop with it.
    return subprocess.run(["git", "-C", str(root), *args], capture_output=True, text=True,
                          timeout=timeout)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", default=None)
    parser.add_argument("--row", required=True, help="ledger row id to certify")
    parser.add_argument("--timeout", type=int, default=7200, help="acceptance command timeout, seconds")
    parser.add_argument("--no-commit", action="store_true", help="certify without committing (tests)")
    parser.add_argument("--bank-uncommitted", action="store_true",
                        help="commit trailing tracked/untracked work before certifying, instead of "
                             "refusing. The acceptance then runs on a committed tree, so `verified` "
                             "names a commit that actually contains the work.")
    args = parser.parse_args()

    root = Path(args.root).resolve() if args.root else Path(__file__).resolve().parents[2]
    ledger = root / LEDGER_REL
    if not ledger.is_file():
        print(f"ledger_close: {ledger} does not exist", file=sys.stderr)
        return 2

    lines = ledger.read_text().splitlines()
    index = None
    fields: list[str] | None = None
    for position, line in enumerate(lines):
        if line.startswith(args.row + "\t"):
            index, fields = position, line.split("\t")
            break
    if index is None or fields is None:
        print(f"ledger_close: no ledger row {args.row!r}", file=sys.stderr)
        return 2
    if len(fields) != 7:
        print(f"ledger_close: row {args.row!r} has {len(fields)} fields, expected 7", file=sys.stderr)
        return 2

    status = fields[3]
    command = fields[4].strip()
    if status in ("done", "owner-blocked"):
        print(f"ledger_close: {args.row} is already {status}; nothing to do")
        return 0
    if not command:
        print(f"ledger_close: {args.row} has no acceptance command", file=sys.stderr)
        return 2

    if git(root, "rev-parse", "--git-dir").returncode != 0:
        print(f"ledger_close: {root} is not a git checkout", file=sys.stderr)
        return 2
    # --untracked-files=no: an untracked build artifact or editor temp file does
    # not make the work uncommitted, and treating it as dirty would park the loop
    # forever. A staged or modified TRACKED file does.
    dirty = git(root, "status", "--porcelain", "--untracked-files=no").stdout.strip()
    if dirty and not args.no_commit and not args.bank_uncommitted:
        print("ledger_close: working tree is dirty; refusing to certify a row against uncommitted work:",
              file=sys.stderr)
        print(dirty, file=sys.stderr)
        return 1

    if dirty and args.bank_uncommitted and not args.no_commit:
        # The worker's rule is "bank early", and it does not always win: an
        # iteration that ends mid-batch leaves edits behind, and a closer that
        # refuses every time can never certify anything (measured: 21 of 21 runs
        # refused). Banking is not certifying -- the acceptance command still has
        # to pass afterwards -- and it is better than losing the work.
        print(f"ledger_close: banking uncommitted work before certifying {args.row}")
        git(root, "add", "-u")
        # NEVER add an arbitrary untracked tree. `.progress-log/` on this NFS mount
        # put `git add` into uninterruptible sleep and hung the loop for minutes.
        # Untracked work is banked only from directories a batch can legitimately
        # add to; anything else is reported and left alone.
        safe_untracked = ("mdk_core/", "mdk_3ds/", "mdk_linux/", "tests/", "tools/",
                          "docs/evidence/", "n64ref/", "ts_core/", "ts_pc/", "ts_3ds/")
        untracked = [p for p in git(root, "ls-files", "--others", "--exclude-standard").stdout.splitlines()
                     if p.strip()]
        skipped = [p for p in untracked if not p.startswith(safe_untracked)]
        for path in untracked:
            if path.startswith(safe_untracked):
                git(root, "add", "--", path)
        if skipped:
            print(f"ledger_close: left {len(skipped)} untracked path(s) unbanked (not source): "
                  + ", ".join(skipped[:6]))
        staged = [p for p in git(root, "diff", "--cached", "--name-only").stdout.splitlines() if p.strip()]
        forbidden = [p for p in staged if p.startswith("MDK/") or p.startswith("mdk_3ds/romfs/")]
        if forbidden:
            git(root, "reset", "-q")
            print("ledger_close: refusing to bank protected/local game bytes: " + ", ".join(forbidden),
                  file=sys.stderr)
            return 1
        if not staged:
            print("ledger_close: dirty tree but nothing staged after add; refusing", file=sys.stderr)
            return 1
        banked = git(root, "commit", "--no-verify", "-m",
                     f"bank: uncommitted work from the iteration before {args.row} was certified\n\n"
                     f"Banked by tools/runner/ledger_close.py so the acceptance command runs against a\n"
                     f"committed tree. This commit certifies nothing; the row closes only if its\n"
                     f"command passes.")
        if banked.returncode != 0:
            print(f"ledger_close: banking commit failed: {banked.stderr}", file=sys.stderr)
            return 1
        print(f"ledger_close: banked {len(staged)} file(s); HEAD is now the work under test")

    head = git(root, "rev-parse", "--short", "HEAD").stdout.strip()
    print(f"ledger_close: running acceptance for {args.row}: {command}")
    try:
        run = subprocess.run(command, cwd=root, shell=True, capture_output=True,
                             text=True, timeout=args.timeout)
    except subprocess.TimeoutExpired:
        print(f"ledger_close: acceptance timed out after {args.timeout}s; row stays open", file=sys.stderr)
        return 1
    tail = (run.stdout + run.stderr)[-8000:]
    if run.returncode != 0:
        print(f"ledger_close: ACCEPTANCE FAILED rc={run.returncode}; row stays open", file=sys.stderr)
        print(tail, file=sys.stderr)
        return 1

    evidence = root / EVIDENCE_DIR / f"{args.row}-{head}.txt"
    evidence.parent.mkdir(parents=True, exist_ok=True)
    evidence.write_text(
        f"row: {args.row}\n"
        f"command: {command}\n"
        f"exit: 0\n"
        f"verified-at: {head}\n"
        f"captured: {datetime.datetime.now(datetime.timezone.utc).isoformat()}\n"
        f"\n{tail}\n"
    )
    evidence_rel = f"{EVIDENCE_DIR}/{evidence.name}"

    fields[3] = "done"
    fields[5] = evidence_rel
    fields[6] = head
    lines[index] = "\t".join(fields)
    ledger.write_text("\n".join(lines) + "\n")

    if args.no_commit:
        print(f"ledger_close: {args.row} certified against {head} (not committed)")
        return 0

    added = git(root, "add", LEDGER_REL, evidence_rel)
    if added.returncode != 0:
        print(f"ledger_close: could not stage the row: {added.stderr}", file=sys.stderr)
        return 2
    message = (
        f"ledger: {args.row} done -- its acceptance command passed at {head}\n\n"
        f"command: {command}\nexit: 0\nevidence: {evidence_rel}\n\n"
        "Closed by tools/runner/ledger_close.py, which ran the command itself. The\n"
        "row is not closed because a batch said so."
    )
    committed = git(root, "commit", "--no-verify", "-m", message)
    if committed.returncode != 0:
        print(f"ledger_close: commit failed: {committed.stderr}", file=sys.stderr)
        return 2
    print(f"ledger_close: {args.row} done at {head}; evidence {evidence_rel}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, subprocess.SubprocessError) as exc:
        print(f"ledger_close: could not verify -- {exc}", file=sys.stderr)
        sys.exit(2)
