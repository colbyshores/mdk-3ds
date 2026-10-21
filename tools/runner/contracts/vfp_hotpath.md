# VFP Hot-Path Contract — MDK

**Target:** Nintendo 3DS (ARM11 MPCore, VFP11 / VFPv2)
**Scope:** all MDK source compiled into the 3DS build — shared and
3DS-specific alike. See §7.
**Status:** enforceable — agents MUST report violations by rule ID

This is MDK's copy of the contract the gates enforce. The authoritative text is
`AGENTS.md`; this file is the machine-facing companion to it — the rule texts the
gates implement, the exemption grammar they police, and the standing exemptions
and hot-site candidates they are pointed at.

Ported from `../t2k/tools/runner/contracts/vfp_hotpath.md` (2026-10-21). The
rule set is inherited; the paths, the flags and the hit set are MDK's.

---

## 0. Rationale

The ARM11 MPCore has a real hardware FPU (VFP11, VFPv2, IEEE 754). Raw
floating-point arithmetic is not the problem. Three things are:

1. **The coprocessor boundary.** Moving a value between the ARM integer
   register file and the VFP register file costs 20+ cycles. Anything that
   forces a round trip per iteration dominates the loop.
2. **Trap-to-support-code on exceptional values.** Denormals stall hard
   unless flush-to-zero is enabled. Decay loops generate denormals by
   construction.
3. **Register file size.** 32 single-precision registers. Doubles consume
   two. Spills are memory traffic on a core with a small cache.

This contract targets those three things specifically. It does **not**
prohibit floating-point math, and it does **not** prohibit mixing integer
and float types in the same function.

---

## 1. Definitions

**HOT PATH** — a function or loop body that satisfies at least one of:
- Executes per-frame at a rate ≥ 1× frame count, AND appears in the top 15
  entries of a captured profile, OR
- Executes ≥ 256 times per frame regardless of profile rank, OR
- Is explicitly annotated `/* @hotpath */`

Nothing in this contract applies to code that is not HOT PATH. See §5.

**BOUNDARY CROSSING** — any operation that moves a value between the ARM
integer register file and the VFP register file, or that requires a VFP
status flag to be read by ARM-side control flow. Includes:
- `vmov` / `vmrs` between register files
- float→int and int→float conversion (`vcvt`)
- float comparison whose result drives a branch
- float value used as an array index

**COLD PATH** — everything else. Init, level load, menu logic, texture
generation, parse, teardown.

---

## 2. Rules

Severity: **MUST** = build-blocking violation. **SHOULD** = flag for review.

### R1 — FPSCR configuration (MUST)

Flush-to-Zero and Default-NaN MUST be set in `FPSCR` on every thread that
executes float math, at thread entry, before any hot path runs.

MDK's helpers live in `mdk_3ds/src/platform/vfp_fpscr.h`:
`mdk::vfpSetFzDnMain()` (main) and `mdk::vfpThreadFzDn()` (a worker).
The main call site is `mdk_3ds/src/main_3ds.cpp`; the audio worker's is the
first line of the audio thread in `mdk_3ds/src/audio/audio_3ds.cpp`.

Every FPSCR helper and every call site MUST carry a comment stating the VFPU
rationale: denormals stall the VFP11 unless FZ is set, decay loops generate
denormals by construction, FPSCR is per-thread so each worker needs its own
setup. Uncommented FPSCR code is a MUST finding — the next reader must not
have to reverse-engineer why the asm is there.

A new 3DS worker thread that touches floats MUST get the same helper at
entry. Copy the existing comment; do not invent a second protocol.

### R2 — No float→int conversion in a hot loop body (MUST)

### R3 — No float comparison driving control flow in a hot loop (MUST)

### R4 — No double precision in hot paths (MUST)

### R5 — Loop counters and indices MUST be integer

### R6 — No transcendentals in hot paths (MUST)

Presentation / camera / mesh / panel trig goes through `mdk::fastSin` /
`fastCos` / `fastAtan2` in `mdk_core/src/util/math_lut.h`. The table is
**one path on both targets**: a `#if __3DS__` LUT / `#else` libm split makes
host harnesses prove the desktop arithmetic and say nothing about the 3DS.

### R7 — Division and sqrt SHOULD be avoided in hot paths

### R8 — Fixed-point domains MUST NOT implicitly promote

### R9 — VFP register pressure SHOULD stay under budget

### R10 — Build flags MUST be verified, not assumed

`mdk_3ds/Makefile` must carry

```
ARCH := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
```

plus `-fno-math-errno` and `-fno-strict-aliasing` in the common flags, and
must NOT carry `-ffast-math`, `-Ofast`, or a soft/softfp float ABI. An
explicit `-mfpu=vfp` is permitted; any other `-mfpu=` is not. The flags MUST
reach the compile and link commands — a literal `ARCH` line alone is not
verification. `-fsingle-precision-constant` stays per-object; widening it
retypes every unsuffixed float literal in the tree.

### R11 — Integer division/modulo by a runtime value (SHOULD)

ARMv6 has **no integer divide instruction**. A runtime `%` or `/` on integers
is a `bl __aeabi_idivmod` / `__aeabi_idiv`: tens of cycles plus a call barrier
that spills the loop's live registers — categorically worse than R7's float
division, since VFPv2 has hardware `vdiv`. A compile-time-constant divisor is
NOT a finding (GCC emits a magic-number multiply).

`((x % n) + n) % n` is two calls and is usually wrapping a value at most one
period out of range; where that bound is provable, a conditional add/subtract
is an exact integer identity. Screened by `tools/runner/audit_r11_idiv.py`.

### T1 — Solve, don't search (temperature-independent)

Do not iterate toward a value that has a closed form. If the condition is
linear (or otherwise directly invertible) in the unknown, invert it.

### T2 — No float comparison in a LOOP CONDITION (temperature-independent)

`while (x > limit) x -= step;` is a termination hazard before it is a
performance one: floating-point accumulation decides the iteration count, so
the loop can run one iteration more or fewer than intended, and with a small
enough step or an unlucky value it may not terminate at all. Use an integer
iteration count, or solve it (T1).

T1/T2 apply in COLD paths too. The standing exemption for **bounded
normalisation** (a wrap near range, whose closed form is not bit-identical to
repeated subtraction) is `/* @vfp-exempt T1 */` / `/* @vfp-exempt T2 */` —
pin it, do not fix it. The rule is for code being WRITTEN, not a licence to
rewrite ported math that already works.

---

## 3. Explicit non-rules

The following are **NOT** violations and MUST NOT be reported:

- An integer loop counter driving float arithmetic in the body. This is
  correct and idiomatic.
- Mixed int and float types in the same *function*, where the hot loop
  itself stays in one domain.
- Float math in COLD PATH code of any shape.
- Float comparisons outside a loop.
- Any pattern in code the profile does not show as hot.

---

## 4. Exemptions

An exemption is granted inline:

```c
/* @vfp-exempt R3 — predicate depends on per-vertex data that cannot be
   hoisted; measured cost 0.4ms, accepted. Verified 2026-01-01. */
```

Grammar:

```
/* @vfp-exempt R<id>[,R<id>...] — <reason>. measured <cost-or-n/a>. Verified <YYYY-MM-DD>. */
```

The grammar is policed by `gates_vfp.py` (rule `R-EXEMPT`): a MUST rule
without a `measured` cost is a finding, a malformed pin is a finding, and a
pin older than the pin-age clock is a finding. An exemption is a reviewed
decision, not a TODO.

### 4.1 Standing exemptions (corpus, not source comments)

AGENTS.md §5 names four standing categories. They are machine-readable in
`gates_vfp.py`'s `STANDING_EXEMPTIONS` table.

| ID | Scope | Rule | Standing | Why |
|---|---|---|---|---|
| E-LUT-1 | `mdk_core/src/util/math_lut.h` : `fastSin`, `fastCos` | R2,R3,R5 | yes | Range reduction; the leaf `(int)` truncation and sign fixup are load-bearing, and VFPv2 has no float-result floor instruction |
| E-LUT-2 | `mdk_core/src/util/math_lut.h` : `fastAtan2` | R2,R3,R5,R7 | yes | Octant fold requires float predicates; one `vdiv` is the atan2 |
| E-LUT-3 | `mdk_core/src/util/math_lut.cpp` : `mathLutInit` | R4,R6 | yes | COLD table build; libm is the reference itself |
| E-MECH-1 | `mdk_core/src/game/**`, `mdk_core/src/scripts/**` | R4,R6 | yes | Mechanics-gated DOUBLE gameplay: a ULP shift moves a gameplay boundary, so it is not LUT-ed and not `float`-ed. **Never** waives R2/R3/R5/R11 |
| E-TEX-1 | `mdk_core/src/**/textures*.cpp` | R2–R9 | yes | The procedural texture DSL **defines asset output**: the same change moves texture bytes |

A standing row is a REVIEWED decision about a place, not a licence. R2, R3,
R5 and R11 are never waived by a category: a float→int conversion in a
gameplay loop, a swept-window predicate and a runtime integer modulo are
gameplay behaviour, not numerical noise.

### 4.2 MDK's hit set — the places these rules bite

Named by AGENTS.md R2, R3 and R11. These are the sites to check by hand when
the first capture lands, and the sites a reviewer should look at first.

| Site | Rule | Why it is gameplay, not noise |
|---|---|---|
| `Bsp::SweepBox` (BSP box sweep) | R2, R11 | The sweep's results decide where Kurt stands. A rounding change is a behaviour change, not a rounding nit. The BSP node traversal also divides/modulos at runtime |
| Script VM operand decoding (`ScriptVm::Step`) | R2, R11 | The VM's operands are fetched per opcode; `x % n` by a runtime value is `__aeabi_idivmod` |
| Palette frame index (`index % frames`, animated textures) | R11 | The classic double-wrap idiom `((x % n) + n) % n` — two runtime modulos |
| Debris pool wraparound | R11 | A pool index wrapped at runtime |
| Sprite decoder (`KurtSprite`, RLE decode) | R2 | The decoder's coordinate work is where a float→int conversion would be tempting |
| Audio mixer (`AudioDevice::Update`, 64 voices) | R2, R11 | Per-voice envelopes accumulate small values (which is also why R1's FZ/DN matters here) |
| Swept windows: Kurt's feet against a floor, item pickups, damage volumes | R3 | A legitimate gameplay predicate. **Do not integerise it** — pin it with `@vfp-exempt R3` |
| LevelView::Draw (the vertex transform) | R3, R7 | Per-vertex float work; the transform is the hot path |
| ObjectMotion::Step | R3 | Per-object motion integration |

---

## 5. Applicability

If a function is not HOT PATH, emit nothing.

## 6. Profile ingest

HOT PATH = union of `/* @hotpath */`, `hotpath_corpus.json` seed, parsed
`docs/validation/perf-*.log` top-15 stages, and static ≥256×/frame loops.

**MDK's corpus is EMPTY today** (`captured: null`): the port has taken no
OG-profile capture, so "top 15 of a captured profile" names nothing. The first
capture populates the seed and sets the date; the schema does not change.

## 7. Scope

Include: `mdk_core/src/**`, `mdk_3ds/src/**`, `mdk_3ds/Makefile`.
Exclude: `mdk_linux/**`, `mdk_core/third_party/**`, `mdk_core/tools/**`,
`mdk_3ds/tools/**`.

`mdk_linux/**` is outside §7's scope, which means a Linux renderer there is
explicitly **ungated**, not compliant: the 3DS is the performance target and
the capture is a 3DS capture.

## 8. Report schema

Every finding has `rule`, `severity`, `file`, `line`, `symbol`, `hotpath`,
`hotpath_reason`, `exemption`, `detail`.
FAIL iff any unexempted MUST finding.

## 9. The freeze

The corpus-age clock (`CORPUS_MAX_AGE_DAYS`, default 45) and the pin-age clock
(`PIN_MAX_AGE_DAYS`, default 180) exist to **demand a re-measurement**. MDK
cannot satisfy that demand yet, so both clocks are waived from the port's start
by `PROJECT_FROZEN_AT` in `tools/runner/gates_vfp.py`.

Only the clocks are waived. Every substantive rule still runs — R1–R11, T1/T2,
the exemption shape, the `measured`/`Verified` pin grammar, register pressure.
`PASS` after the freeze means "clean under the freeze", **not** "freshly
measured", and the gate says so in its own output. Set `VFP_FROZEN_AT=""` to
put the clocks back and resume perf work.
