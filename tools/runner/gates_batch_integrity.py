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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base", help="base commit for this batch")
    parser.add_argument("--head", default="HEAD", help="batch commit to inspect")
    parser.add_argument("--staged", action="store_true", help="validate staged commit contents before git commit")
    parser.add_argument("--owner-approved", action="store_true", help="allow protected contract edits for an explicit owner-directed change")
    args = parser.parse_args()
    errors: list[str] = []

    # Static project invariants: do not let a low-tier worker silently rewrite
    # the mission, disable its gates, or replace the configured oracle.
    required = {
        "tools/check.sh": ["tools/runner/gates_frames.py", "gates_vfp.py", "gates_core_parity.py", "gates_batch_integrity.py"],
        "ralph/builder.md": ["READ-ONLY behavioural", "Never weaken a gate", "Phase 2", "EVERY COMMIT", "owner-protected"],
        "SPEC-COMPLETION.md": ["behavioural oracle", "post-process gap sweep", "frame-golden harness", "original `./MDK/MDKD3D.EXE`"],
        "docs/frames/README.md": ["visual history", "publish_frame.sh", "oracle is never published"],
        "ralph-pi.sh": ["gates_batch_integrity.py", "publish_frame.sh", "--base", "REVIEWER FAILED", "--staged"],
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
    if (ROOT / "ralph.sh").is_file() and not (ROOT / "logs/ralph/disabled").exists():
        errors.append("Muse loop is not parked; refuse mixed swarm/multiple writers")

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
        changed = set(git("diff", "--name-only", f"{args.base}..{args.head}"))
        if not changed:
            errors.append(f"batch {args.base}..{args.head} contains no changed files")
        if not args.owner_approved:
            for path in sorted(changed & PROTECTED):
                errors.append(f"protected contract changed without owner approval: {path}")
        range_adds = set(git("diff", "--name-only", "--diff-filter=A", f"{args.base}..{args.head}"))
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
        if not args.owner_approved and changed & {
            "tools/check.sh", "tools/runner/gates_frames.py", "tests/frames/reference.sha256"
        }:
            errors.append("batch changes test/gate/fidelity ceiling; requires owner approval and explicit review")

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
