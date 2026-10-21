#!/usr/bin/env python3
"""ARM11 math audit: trig-LUT coverage + fixed-point candidates.

    python3 tools/runner/audit_arm11_math.py [--json]

PORTED FROM t2k (2026-10-21). Detection mechanism for the 3DS math contract
documented in mdk_core/src/util/math_lut.h:

  TRIG   Raw libm trig in 3DS-compiled code (mdk_core/src + mdk_3ds/src --
         the 3DS Makefile globs ALL of mdk_core/src) must either sit in a
         policy exception (below) or move to mdk::fastSin/fastCos/fastAtan2.

  FIXED  Fixed-point candidates: double residue (mechanics-gated files are
         deliberate), power-of-two scale math feeding integer casts inside
         seeded-hot loops, and degree/radian conversion churn.

Policy exceptions (each cites math_lut.h "WHAT IS DELIBERATELY NOT ROUTED"):
  math_lut.cpp            LUT construction -- the libm reference itself.
  mdk_core/src/game/**   mechanics-gated DOUBLE gameplay: a ULP shift moves a
  mdk_core/src/scripts/**  gameplay boundary, so it is not LUT-ed and not
                          float-ed (AGENTS.md R4, standing exemption E-MECH-1).
                          The scope is a DIRECTORY until the gameplay port
                          lands and the per-file review narrows it; the
                          standing exemption waives R4/R6 ONLY -- never
                          R2/R3/R5/R11, which are gameplay behaviour.
Any file carrying an @vfp-exempt R6 pin is honoured for that site.

The Linux tree (mdk_linux) is reported separately as INFO: per math_lut.h the
LUT is ONE PATH on both targets, so raw trig there is equally off-policy, but
mdk_linux/** is outside AGENTS.md 8's scope -- which means it is explicitly
UNGATED, not compliant. It is reported, never gated.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gates_vfp import (  # noqa: E402
    DEFAULT_CORPUS,
    REPO_ROOT,
    Function,
    line_of,
    load_corpus,
    parse_functions,
    seed_map,
    split_loops,
    strip_comments_keep_lines,
)

THIRD_PARTY = "third_party"

# Raw libm trig: bare C names (double), f-suffixed, and std:: qualified.
TRIG_RE = re.compile(
    r"\b(?:std::)?(sin|cos|tan|asin|acos|atan|atan2|sinf|cosf|tanf"
    r"|asinf|acosf|atanf|atan2f)\s*\("
)

# The 3DS-compiled trees. The 3DS Makefile globs mdk_core/src at every depth
# it reaches, so ALL of mdk_core/src is 3DS-compiled code.
DS3_SRC_DIRS = ("mdk_core/src", "mdk_3ds/src")
PC_SRC_DIR = "mdk_linux/src"

# math_lut.h "WHAT IS DELIBERATELY NOT ROUTED THROUGH HERE".
ALLOWED_FILES: dict[str, str] = {
    "mdk_core/src/util/math_lut.cpp": "ALLOWED-LUT-CONSTRUCTION",
}

# Files whose gameplay math is ported in DOUBLE on purpose. The standing
# exemption E-MECH-1 in gates_vfp.py has the same scope, so the two agree:
# what the gate waives, the audit does not call an offender.
MECHANICS_GATED = ("mdk_core/src/game/", "mdk_core/src/scripts/")

# Data tables: the format headers carry the game's own numbers.
DATA_TABLE = ("mdk_core/src/formats/",)

# Power-of-two / quantisation scale constants that map onto Q-shifts.
QSCALE_RE = re.compile(
    r"(?:\*\s*(?:127\.5f|255\.0f|255|256\.0f|256|128\.0f|128|0\.5f|2\.0f)\b"
    r"|/\s*(?:256\.0f|256|128\.0f|128|2\.0f|2)\b)"
)
INT_CAST_RE = re.compile(r"\((?:int|u8|s8|u16|s16|u32|short|char)\s*\)|static_cast<(?:int|u8|u16|s16|u32|short)>")
DEG_CONV_RE = re.compile(r"\bRAD2DEG\b|\bDEG2RAD\b|\bM_PI\s*\*\s*/|/\s*180\.0f|\*\s*180\.0f")
DOUBLE_RE = re.compile(r"\bdouble\b")


def iter_sources(sub: str, root: Path = REPO_ROOT) -> list[Path]:
    base = root / sub
    if not root.is_dir():
        return []
    # `.inc` too: c3d/*.inc are pasted into renderer_c3d.cpp, so their trig is
    # compiled trig, and a scan that skipped them was certifying a renderer it
    # had not read. First run over the fragments added one R6 site (already
    # ALLOWED-R6-PIN) and three cold `double` declarations.
    return [
        p
        for p in sorted(base.rglob("*"))
        if p.suffix in (".cpp", ".h", ".inc") and THIRD_PARTY not in p.parts
    ]


def raw_line(raw: str, clean: str, idx: int) -> int:
    """strip_comments_keep_lines preserves line structure, so the line number
    in the cleaned text is the line number in the raw text."""
    return line_of(clean, idx)


def has_pin_above(raw: str, line: int, rule: str, max_back: int = 25) -> bool:
    """Honour an existing @vfp-exempt <rule> pin within max_back lines above."""
    lines = raw.split("\n")
    lo = max(0, line - 1 - max_back)
    window = "\n".join(lines[lo:line])
    return bool(re.search(r"@vfp-exempt\b[^*]*\b" + rule + r"\b", window))


def function_of(funcs: list[Function], index: int) -> Function | None:
    for f in funcs:
        if f.start <= index < f.end:
            return f
    return None


def load_hot(root: Path = REPO_ROOT) -> tuple[dict[str, object], str]:
    """The hot-path seed, from the corpus of `root` when it has one."""
    corpus_path = root / "tools" / "runner" / "contracts" / "hotpath_corpus.json"
    if not corpus_path.exists():
        corpus_path = DEFAULT_CORPUS
    corpus = load_corpus(corpus_path)
    seed = seed_map(corpus, {})
    return seed, "corpus"


def is_hot(seed: dict[str, object], rel: str, fname: str) -> bool:
    entry = seed.get(rel)
    if entry is None:
        return False
    syms = getattr(entry, "symbols", None) or []
    return "*" in syms or fname in syms


def audit_trig(seed: dict[str, object], root: Path = REPO_ROOT) -> tuple[list[dict], list[dict]]:
    violations: list[dict] = []
    allowed: list[dict] = []
    for sub in DS3_SRC_DIRS:
        for path in iter_sources(sub, root):
            rel = path.relative_to(root).as_posix()
            raw = path.read_text(encoding="utf-8", errors="replace")
            clean = strip_comments_keep_lines(raw)
            funcs = parse_functions(clean)
            for m in TRIG_RE.finditer(clean):
                idx = m.start()
                # Skip hits inside the fast* wrappers themselves.
                prefix = clean[max(0, idx - 6): idx + 1]
                if "fast" in prefix:
                    continue
                ln = raw_line(raw, clean, idx)
                fn = function_of(funcs, idx)
                fname = fn.name if fn else "<file scope>"
                rec = {
                    "file": rel,
                    "line": ln,
                    "function": fname,
                    "call": m.group(0).rstrip("(").strip(),
                    "hot": is_hot(seed, rel, fname),
                }
                policy = ALLOWED_FILES.get(rel)
                if policy is None and has_pin_above(raw, ln, "R6"):
                    policy = "ALLOWED-R6-PIN"
                if policy:
                    rec["policy"] = policy
                    allowed.append(rec)
                else:
                    violations.append(rec)
    return violations, allowed


def audit_trig_pc(root: Path = REPO_ROOT) -> list[dict]:
    out: list[dict] = []
    for path in iter_sources(PC_SRC_DIR, root):
        rel = path.relative_to(root).as_posix()
        clean = strip_comments_keep_lines(path.read_text(encoding="utf-8", errors="replace"))
        for m in TRIG_RE.finditer(clean):
            if "fast" in clean[max(0, m.start() - 6): m.start() + 1]:
                continue
            out.append({"file": rel, "line": line_of(clean, m.start()), "call": m.group(0).rstrip("(").strip()})
    return out


def _policy_of(rel: str) -> str:
    """The file-level policy for a `double` residue: mechanics-gated, data, or none."""
    for prefix in MECHANICS_GATED:
        if rel.startswith(prefix):
            return "MECHANICS-GATED"
    for prefix in DATA_TABLE:
        if rel.startswith(prefix):
            return "DATA-TABLE"
    return ""


def audit_fixed(seed: dict[str, object], root: Path = REPO_ROOT) -> dict:
    doubles: list[dict] = []
    qshift: list[dict] = []
    degconv: Counter = Counter()
    for sub in DS3_SRC_DIRS:
        for path in iter_sources(sub, root):
            rel = path.relative_to(root).as_posix()
            raw = path.read_text(encoding="utf-8", errors="replace")
            clean = strip_comments_keep_lines(raw)
            gate = _policy_of(rel)
            for m in DOUBLE_RE.finditer(clean):
                idx = m.start()
                ln = raw_line(raw, clean, idx)
                fn = function_of(parse_functions(clean), idx)
                pol = gate or ("HOT" if fn and is_hot(seed, rel, fn.name) else "cold")
                if pol == "HOT" and has_pin_above(raw, ln, "R4", max_back=40):
                    pol = "HOT-R4-PINNED"
                doubles.append({
                    "file": rel, "line": ln,
                    "function": fn.name if fn else "<file scope>",
                    "policy": pol,
                })
            funcs = parse_functions(clean)
            for fn in funcs:
                if not is_hot(seed, rel, fn.name):
                    continue
                for _off, loop in split_loops(fn.body):
                    loop_start = fn.start + fn.body.find(loop)
                    for stmt in re.split(r";", loop):
                        qm = QSCALE_RE.search(stmt)
                        if not (qm and INT_CAST_RE.search(stmt)):
                            continue
                        qidx = loop_start + loop.find(stmt) + qm.start()
                        qshift.append({
                            "file": rel, "line": line_of(clean, qidx),
                            "function": fn.name,
                            "expr": stmt.strip().replace("\n", " ")[:100],
                        })
                for _ in DEG_CONV_RE.finditer(fn.body):
                    degconv[f"{rel}:{fn.name}"] += 1
    return {"doubles": doubles, "qshift": qshift, "degconv": degconv}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--json", action="store_true", help="machine-readable output")
    ap.add_argument("--root", default=str(REPO_ROOT),
                    help="the repo root to scan (default: the directory this "
                         "script lives in; override it to point the gate at a "
                         "fixture tree)")
    args = ap.parse_args()

    root = Path(args.root).resolve()
    seed, source = load_hot(root)
    trig_v, trig_ok = audit_trig(seed, root)
    trig_pc = audit_trig_pc(root)
    fixed = audit_fixed(seed, root)

    if args.json:
        print(json.dumps({
            "trig_violations": trig_v, "trig_allowed": trig_ok, "trig_pc": trig_pc,
            "fixed": fixed, "source": source,
        }, indent=2))
        return 0

    print("=" * 74)
    print("ARM11 MATH AUDIT — trig LUT coverage + fixed-point candidates")
    print("=" * 74)

    print(f"\nTRIG — 3DS-compiled raw libm trig: {len(trig_v) + len(trig_ok)} sites")
    print(f"  policy exceptions (math_lut.h): {len(trig_ok)}")
    by_pol: dict[str, int] = Counter(r["policy"] for r in trig_ok)
    for pol, n in sorted(by_pol.items()):
        print(f"    {pol}: {n}")
    print(f"  VIOLATIONS (not in math_lut.h exceptions): {len(trig_v)}")
    for r in trig_v:
        hot = "HOT" if r["hot"] else "cold"
        print(f"    {r['file']}:{r['line']}  {r['call']}  in {r['function']}  [{hot}]"
              f"  -> swap to fast{'Atan2' if 'atan' in r['call'] else 'Sin'}")

    print(f"\nTRIG — Linux tree (mdk_linux, outside AGENTS.md 8's scope: reported, not gated): {len(trig_pc)} sites")

    dbl = fixed["doubles"]
    print(f"\nFIXED — double residue in 3DS-compiled code: {len(dbl)} sites")
    by_pol2: dict[str, int] = Counter(d["policy"] for d in dbl)
    for pol, n in sorted(by_pol2.items()):
        print(f"    {pol}: {n}")
    offenders = [d for d in dbl if d["policy"] not in ("MECHANICS-GATED", "DATA-TABLE", "cold", "HOT-R4-PINNED")]
    for d in offenders[:15]:
        print(f"    {d['file']}:{d['line']}  in {d['function']}  [{d['policy']}]")

    print(f"\nFIXED — Q-shift candidates (power-of-two scale + int cast in seeded-hot loops): {len(fixed['qshift'])}")
    for q in fixed["qshift"][:15]:
        print(f"    {q['file']}:{q['line']}  in {q['function']}  .. {q['expr'][:70]}")

    print("\nFIXED — degree/radian conversion churn (angle-domain candidates), top sites:")
    for k, n in sorted(fixed["degconv"].items(), key=lambda kv: -kv[1])[:8]:
        print(f"    {k}: {n}")

    # The verdict must be able to FAIL: a gate that always returns 0 reports the
    # answer the run wanted (PRISTINE-SPEC 2.3). Off-policy trig or an
    # unallowlisted double residue in 3DS-compiled code is a contract breach.
    clean = not trig_v and not offenders
    if clean:
        print("\nVERDICT: CONTRACT CLEAN")
        return 0
    print("\nVERDICT:", f"{len(trig_v)} trig site(s) off-policy"
          + (f", {len(offenders)} double offender(s) outside policy" if offenders else ""))
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
