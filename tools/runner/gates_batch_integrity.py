#!/usr/bin/env python3
"""Fail closed when the current batch violates the MDK loop contract."""
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PROTECTED = {
    "AGENTS.md",
    "SPEC.md",
    # The completion ledger is the definition of "done". A batch that can edit
    # the rows it is judged against can declare itself finished, so the ledger
    # and its owner-block register are protected exactly like the gates.
    "docs/COMPLETION_LEDGER.tsv",
    "docs/OWNER_BLOCKS.md",
    "tools/runner/gates_completion.py",
    # The closer runs acceptance commands and rewrites the ledger, so it is the
    # one script that can legitimately change a row. Protecting it keeps that
    # power out of reach of an ordinary batch; the sanctioned path to closing a
    # row is running this tool, not editing the row.
    "tools/runner/ledger_close.py",
    "tools/check.sh",
    "tools/runner/gates_frames.py",
    "tools/runner/gates_batch_integrity.py",
    "tests/frames/reference.sha256",
    "tests/frames/capture_oracle.sh",
    "tests/frames/regenerate_reference.sh",
    ".gitignore",
}


def git(*args: str) -> list[str]:
    result = subprocess.run(["git", "-C", str(ROOT), *args], capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or f"git {' '.join(args)} failed")
    return result.stdout.splitlines()


def validate_range(base: str, head: str, owner_approved: bool) -> list[str]:
    errors: list[str] = []
    changed = set(git("diff", "--name-only", f"{base}..{head}"))
    if not changed:
        errors.append(f"batch {base}..{head} contains no changed files")
    if not owner_approved:
        for path in sorted(changed & PROTECTED):
            errors.append(f"protected contract changed without owner approval: {path}")
    if any(p.startswith("MDK/") or p.startswith("mdk_3ds/romfs/") for p in changed):
        errors.append("batch touches protected/local game bytes or staged RomFS")
    if not owner_approved and changed & {
        "tools/check.sh", "tools/runner/gates_frames.py", "tests/frames/reference.sha256"
    }:
        errors.append("batch changes test/gate/fidelity ceiling; requires owner approval and explicit review")
    return errors


def validate_commit_chain(base: str, head: str, owner_approved: bool) -> list[str]:
    errors: list[str] = []
    commits = git("rev-list", "--first-parent", "--reverse", f"{base}..{head}")
    cursor = base
    for commit in commits:
        fields = git("rev-list", "--parents", "-n", "1", commit)[0].split()
        if len(fields) != 2:
            errors.append(f"commit {commit[:12]} is a merge commit; per-commit validation refuses unexamined merge parents")
            continue
        parent = fields[1]
        if parent != cursor:
            errors.append(f"commit chain is discontinuous before {commit[:12]}: expected parent {cursor[:12]}, got {parent[:12]}")
        errors.extend(validate_range(parent, commit, owner_approved))
        cursor = commit
    if not commits:
        errors.append(f"commit range {base}..{head} contains no commits")
    elif cursor != head:
        errors.append(f"first-parent chain ended at {cursor[:12]}, not requested head {head[:12]}")
    return errors


def self_test() -> int:
    from unittest.mock import patch

    commits = ["commit-a", "commit-b"]
    parents = {"commit-a": ["commit-a", "base"], "commit-b": ["commit-b", "commit-a"]}
    changes = {
        "base..commit-a": ["src/a.cpp"],
        "base..commit-b": ["src/a.cpp", "AGENTS.md"],
        "commit-a..commit-a": ["src/a.cpp"],
        "commit-a..commit-b": ["AGENTS.md"],
    }

    def fake_git(*args: str) -> list[str]:
        if args[:2] == ("rev-list", "--first-parent"):
            return commits
        if args[:2] == ("rev-list", "--parents"):
            return [" ".join(parents[args[-1]])]
        if args[:2] == ("diff", "--name-only"):
            return changes.get(args[-1], [])
        raise AssertionError(f"unexpected git args: {args}")

    with patch(__name__ + ".git", side_effect=fake_git):
        approved_errors = validate_commit_chain("base", "commit-b", True)
        refused_errors = validate_commit_chain("base", "commit-b", False)
        failed_chain = validate_commit_chain("base", "commit-c", False)
    if approved_errors:
        print(f"BATCH INTEGRITY SELF-TEST: FAIL: valid independent commits rejected: {approved_errors}")
        return 1
    if not any("protected contract" in error for error in refused_errors):
        print("BATCH INTEGRITY SELF-TEST: FAIL: unapproved protected change was accepted")
        return 1
    if not failed_chain:
        print("BATCH INTEGRITY SELF-TEST: FAIL: malformed commit chain was accepted")
        return 1
    print("BATCH INTEGRITY SELF-TEST: PASS")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base", help="base commit for this batch")
    parser.add_argument("--head", default="HEAD", help="batch commit to inspect")
    parser.add_argument("--staged", action="store_true", help="validate staged commit contents before git commit")
    parser.add_argument("--owner-approved", action="store_true", help="allow protected contract edits for an explicit owner-directed change")
    parser.add_argument("--each-commit", action="store_true", help="validate each first-parent commit separately; preserves per-commit checks")
    parser.add_argument("--self-test", action="store_true", help="run isolated unit tests for integrity range accounting")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if args.base:
        args.base = git("rev-parse", args.base)[0]
        args.head = git("rev-parse", args.head)[0]
    errors: list[str] = []

    # Static project invariants: do not let a low-tier worker silently rewrite
    # the mission, disable its gates, or replace the configured oracle.
    required = {
        "tools/check.sh": ["tools/runner/gates_frames.py", "gates_vfp.py", "gates_core_parity.py", "gates_batch_integrity.py", "gates_completion.py"],
        "SPEC.md": ["behavioural oracle", "post-process gap sweep", "frame-golden harness", "original `./MDK/MDKD3D.EXE`"],
        "tools/hooks/pre-commit": ["gates_batch_integrity.py", "--staged", "MDK_OWNER_APPROVED"],
    }
    for rel, markers in required.items():
        text = (ROOT / rel).read_text(errors="replace")
        for marker in markers:
            if marker not in text:
                errors.append(f"{rel}: required invariant is missing: {marker!r}")
    gitignore = (ROOT / ".gitignore").read_text(errors="replace")
    if "/MDK/" not in gitignore:
        errors.append(".gitignore: commercial game data /MDK/ is no longer excluded")
    if "mdk_3ds/romfs/*" not in gitignore:
        errors.append(".gitignore: staged RomFS is no longer excluded")

    if args.staged:
        staged = set(git("diff", "--cached", "--name-only"))
        if not args.owner_approved:
            for path in sorted(staged & PROTECTED):
                errors.append(f"staged change to protected contract requires owner approval: {path}")
        if any(p.startswith("MDK/") or p.startswith("mdk_3ds/romfs/") for p in staged):
            errors.append("staged changes include protected/local game bytes or RomFS")
    if args.base:
        if args.each_commit:
            errors.extend(validate_commit_chain(args.base, args.head, args.owner_approved))
        else:
            errors.extend(validate_range(args.base, args.head, args.owner_approved))

    if errors:
        print("BATCH INTEGRITY: FAIL")
        for error in errors:
            print(f"  - {error}")
        return 1
    print("BATCH INTEGRITY: PASS")
    if args.base:
        print(f"  checked {args.base}..{args.head}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError) as exc:
        print(f"BATCH INTEGRITY: ERROR: {exc}", file=sys.stderr)
        sys.exit(2)
