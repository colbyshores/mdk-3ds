#!/usr/bin/env python3
"""
gates_frames.py -- the frame golden gate (SPEC.md 11.3, 15.2).

    python3 tools/runner/gates_frames.py [--json] [--keep]

WHY THIS GATE EXISTS. The VM corridor is safe to run unsupervised because the
golden dump proves every opcode batch against the C# oracle. The renderer had no
equivalent proof: a reviewer can say a frame looks wrong, but a reviewer is not
a frame oracle, and "looks wrong" cannot be re-checked by the next machine. This
gate is that missing proof.

WHAT THE REFERENCE IS, AND WHAT IT IS NOT. `tests/frames/reference.sha256`
holds two hashes per pinned frame and they are NOT interchangeable:

    oracle=   the C# renderer's bytes, captured by tests/frames/
              capture_oracle.sh. This is the reference: the original look.
              Nothing about the C++ build decides it.
    cpp=      the C++ build's own bytes at the moment the manifest was
              regenerated. This is a REGRESSION BASELINE, not a fidelity
              reference, and it must never be quoted as one.

The C++ frame does not equal the oracle frame yet -- the measured gap is in the
manifest as bad_percent/max_delta/mean_delta, and closing it is P5. So the
byte-exact gate runs against the baseline, and the oracle gap is reported on
every run and re-measured whenever the oracle pixels are on disk.

That split is deliberate and it is the same argument AGENTS.md 2 makes about the
waived corpus-age clocks: a gate that is red from the day it is written trains
everyone to skip the gate, and a skipped gate enforces nothing. What keeps this
from becoming a rubber stamp is that the baseline is BYTE-EXACT -- any change to
the picture, in any direction, is a hash change and a FAIL -- so the only way
through the gate is to regenerate the manifest, and regenerating it re-measures
the oracle gap and puts that number in the diff for the reviewer to read.
Raising bad_percent is an accepted regression and has to look like one.

WHAT IS CHECKED, per pinned frame:

    1. DETERMINISM  -- two captures of the same pinned scene are byte-identical.
       Without this the rest is meaningless, so it is checked first and it is
       checked live, not assumed.
    2. REGRESSION   -- the captured bytes match the recorded cpp= hash.
    3. FIDELITY     -- if the oracle pixels are present (see below), the live
       gap must not exceed the recorded ceiling.

NO ORACLE PIXELS ARE COMMITTED. They are ROM-derived art and the game install
is gitignored, so the .bmp files live in a scratch directory and only their
hashes are in the repo. Check 3 therefore runs only when that directory is
still around: `MDK_FRAME_REFERENCE_DIR` (default
${TMPDIR:-/tmp}/mdk-frame-reference). When it is missing, check 3 reports that
it could not run instead of pretending it passed.

SKIPS ARE SKIPS. No manifest, no built `build-linux/mdk`, or no game data means
there is nothing to capture; the gate says which and exits 0, the same way
gates_smdh reports "no built .cia". A skip is recorded as a skip.

Exit code 1 on any failed check.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import asdict, dataclass, field
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
MANIFEST_REL = "tests/frames/reference.sha256"
CAPTURE_REL = "tests/frames/capture_cpp.sh"
COMPARE_REL = "tests/frames/compare_frames.py"
DEFAULT_REFERENCE_DIR = Path(os.environ.get("TMPDIR", "/tmp")) / "mdk-frame-reference"

FIELD = re.compile(r"(\w+)=(\S+)")


@dataclass
class Frame:
    name: str
    level: str
    size: str
    seed: str
    frame: str
    oracle: str
    cpp: str
    bad_percent: float
    max_delta: int
    mean_delta: float


@dataclass
class Result:
    name: str
    captured: bool = False
    deterministic: bool = False
    matches_baseline: bool = False
    fidelity_checked: bool = False
    fidelity_ok: bool = True
    gap_percent: float | None = None
    sha: str = ""
    notes: list[str] = field(default_factory=list)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def parse_manifest(path: Path) -> list[Frame]:
    frames: list[Frame] = []
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        fields = dict(FIELD.findall(line))
        if "name" not in fields or "cpp" not in fields:
            continue
        frames.append(Frame(
            name=fields["name"], level=fields.get("level", "3"),
            size=fields.get("size", "320x240"), seed=fields.get("seed", "1"),
            frame=fields.get("frame", "1"), oracle=fields.get("oracle", ""),
            cpp=fields["cpp"],
            bad_percent=float(fields.get("bad_percent", "100")),
            max_delta=int(float(fields.get("max_delta", "255"))),
            mean_delta=float(fields.get("mean_delta", "255")),
        ))
    return frames


def capture(frame: Frame, out: Path, env: dict[str, str]) -> tuple[bool, str]:
    """Run the pinned C++ capture into `out`. Returns (ok, detail)."""
    run = dict(env)
    run["MDK_FRAME_LEVEL"] = frame.level
    run["MDK_FRAME_SIZE"] = frame.size
    run["MDK_FRAME_SEED"] = frame.seed
    run["MDK_FRAME_CAPTURE"] = frame.frame
    proc = subprocess.run([str(REPO_ROOT / CAPTURE_REL), str(out)],
                          capture_output=True, text=True, env=run, cwd=REPO_ROOT)
    if proc.returncode != 0 or not out.is_file() or out.stat().st_size == 0:
        tail = (proc.stderr or proc.stdout or "").strip().splitlines()
        return False, tail[-1] if tail else f"capture exited {proc.returncode}"
    return True, ""


def measure_gap(reference: Path, actual: Path) -> tuple[float, int, float] | None:
    proc = subprocess.run(
        [sys.executable, str(REPO_ROOT / COMPARE_REL), str(reference), str(actual),
         "--measure"], capture_output=True, text=True, cwd=REPO_ROOT)
    if proc.returncode != 0:
        return None
    parts = proc.stdout.split()
    if len(parts) != 3:
        return None
    return float(parts[0]), int(float(parts[1])), float(parts[2])


def main() -> int:
    parser = argparse.ArgumentParser(description="the frame golden gate")
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--keep", metavar="DIR", default="",
                        help="keep the captured frames in DIR instead of a temp dir")
    args = parser.parse_args()

    def note(text: str) -> None:
        if not args.json:
            print(f"note: {text}")

    manifest = REPO_ROOT / MANIFEST_REL
    if not manifest.is_file():
        print("VERDICT: SKIPPED")
        print("note: no tests/frames/reference.sha256; run tests/frames/regenerate_reference.sh")
        return 0

    binary = REPO_ROOT / "build-linux" / "mdk"
    if not binary.is_file():
        print("VERDICT: SKIPPED")
        print("note: build-linux/mdk is not built, so there is no frame to capture")
        return 0

    data_dir = Path(os.environ.get("MDK_DATA_DIR", str(REPO_ROOT / "MDK")))
    if not (data_dir / "TRAVERSE").is_dir():
        print("VERDICT: SKIPPED")
        print(f"note: no game data in {data_dir} (set MDK_DATA_DIR); the pinned scene "
              "cannot be loaded")
        return 0

    frames = parse_manifest(manifest)
    if not frames:
        print("VERDICT: SKIPPED")
        print("note: the manifest has no pinned frames")
        return 0

    reference_dir = Path(os.environ.get("MDK_FRAME_REFERENCE_DIR", str(DEFAULT_REFERENCE_DIR)))
    keep_oracle = reference_dir.is_dir()
    if not keep_oracle:
        note(f"oracle pixels not in {reference_dir}: the fidelity ceiling cannot be "
             "re-measured live; the regression check still runs")

    outdir = Path(args.keep) if args.keep else Path(tempfile.mkdtemp(prefix="mdk-frame-gate."))
    outdir.mkdir(parents=True, exist_ok=True)

    results: list[Result] = []
    failures = 0
    for frame in frames:
        res = Result(name=frame.name)
        first = outdir / f"{frame.name}.a"
        second = outdir / f"{frame.name}.b"
        ok, detail = capture(frame, first, dict(os.environ))
        if not ok:
            res.notes.append(f"capture failed: {detail}")
            results.append(res)
            failures += 1
            continue
        res.captured = True
        res.sha = sha256(first)

        ok2, detail2 = capture(frame, second, dict(os.environ))
        res.deterministic = ok2 and sha256(second) == res.sha
        if not res.deterministic:
            res.notes.append("two captures of the same pinned scene differ "
                             f"({'second capture failed: ' + detail2 if not ok2 else 'bytes differ'})")

        res.matches_baseline = res.sha == frame.cpp
        if not res.matches_baseline:
            res.notes.append(f"the frame changed: {res.sha[:16]} != recorded {frame.cpp[:16]}")

        oracle_pixels = reference_dir / frame.name
        if keep_oracle and oracle_pixels.is_file():
            gap = measure_gap(oracle_pixels, first)
            if gap is None:
                res.notes.append("the oracle frame exists but could not be measured")
            else:
                res.fidelity_checked = True
                res.gap_percent = gap[0]
                if gap[0] > frame.bad_percent or gap[1] > frame.max_delta \
                        or gap[2] > frame.mean_delta + 1e-9:
                    res.fidelity_ok = False
                    res.notes.append(
                        f"the gap to the oracle grew: {gap[0]:.3f}%/{gap[1]}/{gap[2]:.4f} "
                        f"vs the recorded ceiling {frame.bad_percent:.3f}%/"
                        f"{frame.max_delta}/{frame.mean_delta:.4f}")
        else:
            res.notes.append(f"gap to the oracle is recorded at {frame.bad_percent:.3f}% "
                             "of pixels and was not re-measured")

        if not (res.deterministic and res.matches_baseline and res.fidelity_ok):
            failures += 1
        results.append(res)

    if not args.keep:
        shutil.rmtree(outdir, ignore_errors=True)

    if args.json:
        print(json.dumps([asdict(r) for r in results], indent=2))
    else:
        for res in results:
            status = "PASS" if (res.captured and res.deterministic
                                and res.matches_baseline and res.fidelity_ok) else "FAIL"
            print(f"{status} {res.name} sha={res.sha[:16]}")
            for n in res.notes:
                print(f"  note: {n}")

    if failures:
        print(f"VERDICT: FAILED ({failures} frame(s) failed)")
        return 1
    print("VERDICT: CONTRACT CLEAN")
    return 0


if __name__ == "__main__":
    sys.exit(main())
