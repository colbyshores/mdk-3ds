#!/usr/bin/env python3
"""Fail closed when the current batch violates the MDK loop contract."""
from __future__ import annotations

import argparse
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PROTECTED = {
    "AGENTS.md",
    "SPEC.md",
    "SPEC-COMPLETION.md",
    # The completion ledger is the definition of "done". A batch that can edit
    # the rows it is judged against can declare itself finished, so the ledger
    # and its owner-block register are protected exactly like the gates.
    "docs/COMPLETION_LEDGER.tsv",
    "docs/OWNER_BLOCKS.md",
    "tools/runner/gates_completion.py",
    "tools/check.sh",
    "tools/runner/gates_frames.py",
    "tools/runner/gates_batch_integrity.py",
    "tests/frames/reference.sha256",
    "tests/frames/capture_oracle.sh",
    "tests/frames/regenerate_reference.sh",
    "ralph/builder.md",
    "ralph/PROMPT.md",
    ".gitignore",
}
FRAME_DIR = "docs/frames/"


def git(*args: str) -> list[str]:
    result = subprocess.run(["git", "-C", str(ROOT), *args], capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or f"git {' '.join(args)} failed")
    return result.stdout.splitlines()


def validate_png_pair(frame: str, sidecar: str) -> list[str]:
    errors: list[str] = []
    image_path = ROOT / frame
    note_path = ROOT / sidecar
    try:
        with image_path.open("rb") as handle:
            header = handle.read(24)
        if len(header) != 24 or header[:8] != bytes((137, 80, 78, 71, 13, 10, 26, 10)):
            errors.append(f"{frame}: invalid PNG header")
        else:
            width, height = struct.unpack(">II", header[16:24])
            if (width, height) != (320, 240):
                errors.append(f"{frame}: expected pinned size 320x240, got {width}x{height}")
        text = note_path.read_text(errors="replace")
        if "frame: level 3, 320x240, seed 1, frame 1" not in text:
            errors.append(f"{sidecar}: missing pinned-scene identity")
        if "captured:" not in text or ("live C++ vs C# oracle:" not in text and "golden gap:" not in text and "live measurement: unavailable" not in text):
            errors.append(f"{sidecar}: missing capture revision or oracle-gap measurement")
    except OSError as exc:
        errors.append(f"{frame}/{sidecar}: cannot validate frame pair: {exc}")
    return errors


def validate_range(base: str, head: str, owner_approved: bool) -> list[str]:
    errors: list[str] = []
    changed = set(git("diff", "--name-only", f"{base}..{head}"))
    if not changed:
        errors.append(f"batch {base}..{head} contains no changed files")
    if not owner_approved:
        for path in sorted(changed & PROTECTED):
            errors.append(f"protected contract changed without owner approval: {path}")
    range_adds = set(git("diff", "--name-only", "--diff-filter=A", f"{base}..{head}"))
    frames = {p for p in range_adds if p.startswith(FRAME_DIR) and p.endswith(".png")}
    if len(frames) != 1:
        errors.append(f"batch must add exactly one frame PNG in {FRAME_DIR}; found {len(frames)}")
    touched_frames = {p for p in changed if p.startswith(FRAME_DIR)}
    for frame in touched_frames:
        if frame.endswith(".png") and frame not in range_adds:
            errors.append(f"batch modifies/removes historical frame {frame}; history is immutable")
        if frame.endswith(".txt") and frame not in range_adds:
            errors.append(f"batch modifies/removes historical measurement {frame}; history is immutable")
    if any(p.startswith("MDK/") or p.startswith("mdk_3ds/romfs/") for p in changed):
        errors.append("batch touches protected/local game bytes or staged RomFS")
    for frame in frames:
        sidecar = str(Path(frame).with_suffix(".txt"))
        if sidecar not in range_adds:
            errors.append(f"frame {frame} has no measurement sidecar {sidecar}")
        else:
            errors.extend(validate_png_pair(frame, sidecar))
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
        "base..commit-a": ["src/a.cpp", "docs/frames/a.png", "docs/frames/a.txt"],
        "base..commit-b": ["src/a.cpp", "src/b.cpp", "docs/frames/a.png", "docs/frames/a.txt", "docs/frames/b.png", "docs/frames/b.txt"],
        "commit-a..commit-a": ["src/a.cpp", "docs/frames/a.png", "docs/frames/a.txt"],
        "commit-a..commit-b": ["src/b.cpp", "docs/frames/b.png", "docs/frames/b.txt"],
    }
    additions = {
        "base..commit-a": ["docs/frames/a.png", "docs/frames/a.txt"],
        "base..commit-b": ["docs/frames/a.png", "docs/frames/a.txt", "docs/frames/b.png", "docs/frames/b.txt"],
        "commit-a..commit-a": ["docs/frames/a.png", "docs/frames/a.txt"],
        "commit-a..commit-b": ["docs/frames/b.png", "docs/frames/b.txt"],
    }

    def fake_git(*args: str) -> list[str]:
        if args[:2] == ("rev-list", "--first-parent"):
            return commits
        if args[:2] == ("rev-list", "--parents"):
            return [" ".join(parents[args[-1]])]
        if args[:3] == ("diff", "--name-only", "--diff-filter=A"):
            return additions.get(args[-1], [])
        if args[:2] == ("diff", "--name-only"):
            return changes.get(args[-1], [])
        raise AssertionError(f"unexpected git args: {args}")

    with patch(__name__ + ".git", side_effect=fake_git), patch(__name__ + ".validate_png_pair", return_value=[]):
        chain_errors = validate_commit_chain("base", "commit-b", False)
        combined_errors = validate_range("base", "commit-b", False)
        failed_chain = validate_commit_chain("base", "commit-c", False)
    if chain_errors:
        print(f"BATCH INTEGRITY SELF-TEST: FAIL: valid independent commits rejected: {chain_errors}")
        return 1
    if not any("found 2" in error for error in combined_errors):
        print("BATCH INTEGRITY SELF-TEST: FAIL: combined multi-frame range was not rejected")
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
    parser.add_argument("--each-commit", action="store_true", help="validate each first-parent commit separately; preserves per-commit frame requirements")
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
        "ralph/builder.md": ["READ-ONLY behavioural", "Never weaken a gate", "Phase 2", "EVERY COMMIT", "owner-protected"],
        "SPEC-COMPLETION.md": ["behavioural oracle", "post-process gap sweep", "frame-golden harness", "original `./MDK/MDKD3D.EXE`"],
        "docs/frames/README.md": ["visual history", "publish_frame.sh", "oracle is never published"],
        "ralph-pi.sh": ["gates_batch_integrity.py", "publish_frame.sh", "--base", "REVIEWER FAILED", "--staged", "--each-commit"],
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
    if (ROOT / "tools/retired/ralph-muse.sh").is_file() and not (ROOT / "logs/ralph/disabled").exists():
        errors.append("retired Muse loop is unparked; refuse mixed swarm/multiple writers")

    if args.staged:
        staged = set(git("diff", "--cached", "--name-only"))
        staged_adds = set(git("diff", "--cached", "--name-only", "--diff-filter=A"))
        frames = {p for p in staged_adds if p.startswith(FRAME_DIR) and p.endswith(".png")}
        if not frames:
            errors.append("staged batch has no newly added docs/frames/*.png")
        for frame in frames:
            sidecar = str(Path(frame).with_suffix(".txt"))
            if sidecar not in staged_adds:
                errors.append(f"staged frame {frame} has no staged measurement sidecar {sidecar}")
            else:
                errors.extend(validate_png_pair(frame, sidecar))
        if not args.owner_approved:
            for path in sorted(staged & PROTECTED):
                errors.append(f"staged change to protected contract requires owner approval: {path}")
        if any(p.startswith("MDK/") or p.startswith("mdk_3ds/romfs/") for p in staged):
            errors.append("staged changes include protected/local game bytes or RomFS")
        changed_frames = {p for p in staged if p.startswith(FRAME_DIR)}
        if any(p.endswith((".png", ".txt")) and p not in staged_adds for p in changed_frames):
            errors.append("staged batch modifies a historical frame or measurement")

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
