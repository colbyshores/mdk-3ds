# MDK C++ port — performance and platform contract

Revision: 2026-10-21 (initial). Derived from `../t2k/AGENTS.md`, which is the
proved contract for the same house style on the same hardware.

Design language, fidelity ladder, and the rest of the doctrine live in
`docs/DOCTRINE.md`. This file records MDK's ARM11 performance rules, gameplay
safeguards, C++ house style, and 3DS platform requirements.

**The original 3DS is the performance target.** MDK is a 1997 Direct3D game
running on a 1997 3DS screen: the 3DS build shows the **original look** only
(pixel-faithful to `MDKD3D.EXE`), and it targets the original 3DS's 268 MHz
ARM11 with L2 disabled. Record the actual console, clock, cache settings, and
stereoscopic mode for every capture. A New-3DS clocked run is not an OG pass.

**Project policy and hardware facts are different.** R2/R3 and the language bans
are deliberately conservative rules for this engine. They are not proof that
every prohibited construct is always slow on every 3DS workload. When adapting
this contract, establish the new project's profile and invariants.

**A new target is not covered by the Linux waiver.** The corpus is a 3DS
capture. A hot-path list derived from a 3DS frame says nothing about a
desktop-Vulkan budget, so it is not merely stale for the Linux build, it is
inapplicable. `mdk_linux/**` is outside §8's scope, which means a Linux
renderer there is explicitly *ungated*, not compliant.

---

## 0. Working loop (autonomous) — do not stop after every turn

The port is driven by the ralph-wiggum loop (`SPEC.md` §13): pick the next
unfinished increment, work it to its checked exit criterion, and **keep going**.
Stopping at the end of a turn to report and wait is a contract violation unless
the increment is genuinely blocked (no path to the exit criterion, or a decision
only the user can make). A verified checkpoint is not a reason to stop.

**The per-increment loop, in order, every increment:**

1. Implement the increment.
2. Run the checks: the relevant tests/gates (`tests/`, `tools/check.sh`) AND the
   adversarial validator (`@adversarial-validator`) over the changed code.
3. Fix what the validator and gates raise.
4. **Commit** — only after the validator and gates are clean.
5. Move to the next increment immediately.

Commit is gated on the agent check, not on the turn ending. Never commit un-checked
work, and never end a turn with verified, committed work by merely summarising —
start the next increment. If a turn must end mid-increment, say what is in flight
and continue on the next one rather than treating the summary as a stop.

---

## 1. What the ARM11 actually taxes

Floating-point arithmetic is supported in hardware. Costs depend on the
operation, dependencies, register pressure, and memory behavior:

1. **Transfers, conversions, and dependencies.** These are distinct operations,
   not a universal 20+ cycle penalty. ARM11 MPCore TRM §19.11, Table 19-16:

   | Operation | Manual mnemonic | Throughput interval | Latency |
   |---|---|---:|---:|
   | ARM → VFP single-register transfer | `FMSR` | 1 cycle | 4 cycles |
   | VFP → ARM single-register transfer | `FMRS` | 1 cycle | 2 cycles |
   | Integer ↔ float conversion | `FSITO` / `FTOSI` and variants | 1 cycle | 8 cycles |
   | Float comparison | `FCMP` and variants | 1 cycle | 4 cycles |
   | Copy comparison flags to ARM | `FMSTAT` | 1 cycle | 2 cycles |

   Throughput measures when another instruction can start; latency concerns
   result availability. Do not add table entries and call the sum a measured
   loop cost. Inspect generated instructions and profile the actual loop.
2. **Subnormal handling.** VFP11 can require support-code handling in
   full-compliance mode; FZ changes underflow/subnormal behavior, and DN
   selects default NaNs. These are deliberate numerical choices, not
   transparent optimizations. See R1.
3. **Register pressure.** There are 32 single-precision registers, overlapping
   16 double-precision registers. Doubles and long live ranges can increase
   spills; count generated loads/stores rather than source variables.
4. **Code footprint.** Excess inlining can increase instruction-cache pressure.
   This is an argument for measuring code size and locality, not a claim that
   templates themselves are slow. §10 retains the separate house-style
   restriction.

This contract does **not** prohibit floating-point math or mixing integer and
float values in the same function.

Full rule text, exemption grammar, standing exemptions, and the report schema:
`tools/runner/contracts/vfp_hotpath.md`. The corpus of known hot functions is
`tools/runner/contracts/hotpath_corpus.json`. Gates:
`python3 -m tools.runner.gates_vfp`. Trig/fixed-point audit:
`python3 tools/runner/audit_arm11_math.py`.

---

## 2. HOT PATH

A function or loop body is HOT PATH if it:

- runs per-frame at ≥ 1× frame count AND is in the top 15 of a captured
  profile, OR
- runs ≥ 256 times per frame regardless of rank, OR
- is annotated `/* @hotpath */`.

**THE CORPUS IS DERIVED FROM A PROFILE, NEVER FROM MEMORY.** "Top 15 of a
captured profile" is only meaningful against a CURRENT capture: code that was
cold when the list was written becomes hot when a caller changes. Regenerate the
corpus from the profile you are actually optimising against.

Init, level load, menu logic, texture decoding, parsing, and teardown are
normally COLD. Do not "clean up" them to satisfy hot-only math restrictions.
If work moves into a per-frame path, reassess it against the criteria above.

### The staleness clocks are waived while the corpus is empty

The corpus age limit (`CORPUS_MAX_AGE_DAYS = 45`) and the exemption-pin age
limit (`PIN_MAX_AGE_DAYS = 180`) both exist to **demand a re-measurement**.
MDK's port has not yet taken an OG-profile capture, so both clocks would be
permanently red — and a permanently red gate is worse than a stale corpus,
because it trains the reader to skip the gate, and §2's whole argument depends
on the reader believing the gate.

So both clocks are waived from the port's start by `PROJECT_FROZEN_AT` in
`tools/runner/gates_vfp.py`. This is not a bug and must not be "tidied" back.

**What is NOT waived:** every substantive rule still runs — R1–R11, T1/T2,
exemption shape, the `measured`/`Verified` pin grammar, register pressure.
Editing hot-path code still fails on its merits. Only the *age* checks are
silent. `PASS` after the freeze therefore means "clean under the freeze", not
"freshly measured", and the gate says so on its own output.

**To resume perf work**, set `VFP_FROZEN_AT=""` and take a fresh capture.

---

## 3. MUST rules (build-blocking)

### R1 — Verify FPSCR FZ + DN on every float thread

Flush-to-Zero (bit 24) and Default-NaN (bit 25) MUST be enabled for MDK's
floating-point threads before application float work. This initialization
requirement applies regardless of hot/cold classification.

Current upstream libctru's `initThreadVars()` sets both bits, and its main
startup calls that initializer. Verify the version and thread-entry path
actually linked by this project; do not assume all historical releases or
custom thread entry points have the same initialization. Main-thread state
alone is not evidence of a worker's state.

Keep MDK's explicit helpers/read-back as defensive enforcement. FZ/DN change
numerical semantics: do not toggle them within gameplay or assume they
preserve subnormal results or NaN payloads.

Shipped helpers (do not delete, do not `#ifdef` out, do not skip the
read-back on main):

| Thread | Helper | Call site |
|---|---|---|
| main | `mdk::vfpSetFzDnMain()` in `mdk_3ds/src/main_3ds.cpp` (helper in `mdk_3ds/src/platform/vfp_fpscr.h`) | start of `main`, before `gfxInitDefault`; failure `return 1` immediately, never `goto quit` |
| audio worker | `mdk::vfpThreadFzDn()` (helper in `mdk_3ds/src/platform/vfp_fpscr.h`) | first line of the audio thread — PENDING: no audio source exists yet; the row states the required pattern for when it lands |

Every helper and call site MUST keep a rationale comment: decay can reach
subnormals; FZ/DN are the chosen numerical mode; each thread's state must be
established; modern libctru already initializes it on supported entry paths.
Unexplained FPSCR asm is a MUST finding.

A new 3DS worker thread that touches floats MUST get the same helper at
entry. Copy the existing comment; do not invent a second protocol.

### R2 — No float→int conversion in a hot loop body

Do not introduce float→int casts, float→int `vcvt` sequences, `lroundf`, or
array-index-from-float inside a hot loop without an exemption. Hoist an
invariant conversion or keep a naturally integer domain integer. An ordinary
integer cast is not a violation; not every `vcvt` is a float→int conversion.

This is a conservative regression rule, not a fixed cycle estimate. Inspect the
generated code and preserve rounding, range, and gameplay semantics. Never
integerize a continuous quantity solely to satisfy a scan.

**MDK's hit set.** The BSP box sweep (`Bsp::SweepBox`), the script VM's
operand fetches, and the sprite decoder's coordinate work are the places where
a float→int conversion would be tempting. The BSP sweep's results are
**gameplay** — they decide where Kurt stands — so a rounding change is a
behaviour change, not a rounding nit.

### R3 — No float comparison driving control flow in a hot loop

A `vcmp` followed by `vmrs` makes ARM control flow depend on VFP comparison
flags; it is not a float→integer→float conversion round trip. This rule
restricts new occurrences in hot loops pending profiling and a fidelity review.
It is not a universal claim that every float comparison needs replacing.

Gameplay swept windows (Kurt's feet against a floor, the item pickups, the
damage volumes) are the hit set. Integerizing them can change the game.
Preserve their `@vfp-exempt R3` pins; do not "fix" them. Here, R3 concerns
loop control and decisions inside hot loops; T2 separately addresses loop
termination.

### R4 — No double precision in hot paths

Unexempted `double` arithmetic and promotions that force double operations
are forbidden in hot paths. Use `float` and `f`-suffixed literals; prefer
`(float)M_PI * 0.00017f` to `(float)(M_PI * 0.00017)`.
`std::sin(float)` has a float overload, so the function name alone is not
proof of double arithmetic. R6 independently restricts hot-path libm
transcendentals. Perf accumulators are `float` on purpose.

Gameplay trig that is **mechanics-gated DOUBLE** is a standing exemption — a
ULP shift there moves a gameplay boundary. Do not LUT it. Do not `float` it.

### R5 — Loop counters and indices MUST be integer

Unsigned induction where the value is a lattice id. Do not reintroduce
`(uint32_t)j` casts inside those loops.

### R6 — No libm transcendentals in hot paths

Presentation / camera / mesh / panel trig goes through `mdk::fastSin` /
`fastCos` / `fastAtan2` in `mdk_core/src/util/math_lut.h`. The table is **one
path on both targets**. Do not resurrect a `#if __3DS__` LUT / `#else` libm
split — that split made host harnesses prove the desktop arithmetic and say
nothing about the 3DS.

Do not route through `__builtin_floorf` for range reduction. VFPv2 has no
dedicated float-result floor instruction. The leaf `(int)` truncation and sign
fixup in `fastSin` is load-bearing. Do not "clean" it.

Call `mathLutInit()` from every new entry point (including host harnesses)
before any `fast*`. Uninitialized tables are zero; every trig call returns 0.

### R10 — Build flags follow the devkitPro baseline and are verified

Use the supported devkitARM toolchain and the devkitPro application template
as the baseline. Its current architecture flags are:

```makefile
ARCH := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
```

These settings MUST reach the relevant compile and link commands. Equivalent
Makefile organization is allowed; checking a literal `ARCH` line alone is not
verification. Preserve the template's other applicable compilation and
packaging settings.

- Keep `-mtp=soft`: it selects runtime-helper access to the thread-local
  storage pointer (`__aeabi_read_tp`). This is separate from the floating-point
  ABI; do not remove it as a supposed float optimization.
- Explicit `-mfpu=vfp` is permitted; GCC defines it as VFPv2. Verify effective
  settings with the installed devkitARM compiler rather than assuming generic
  ARM compiler defaults.
- `-mtpcs-leaf-frame` is **not required by this contract**. It concerns Thumb
  Procedure Call Standard leaf frames, not enabling the VFP. If retained,
  document the actual debugging/stack-frame requirement.
- Keep `-fno-math-errno` where applied. It allows optimization of functions
  such as `sqrtf` without preserving `errno` reporting. It is not what enables
  hardware floating-point `/`. Code must not depend on math functions setting
  `errno` under this policy.
- Do not add `-ffast-math` or `-Ofast`, which enables it. Preserve the
  fidelity-sensitive arithmetic and the narrow per-object
  `-fsingle-precision-constant` scope in §10. Manual reciprocal hoists still
  need rounding/fidelity review.
- **MDK additionally requires `-fno-strict-aliasing`** in BOTH builds. The
  format parsers read binary data through `memcpy` into typed locals, and the
  3DS's ARM11 is unforgiving about aliasing assumptions that a desktop
  optimizer may take.

For build-setting changes, record the toolchain/library versions, inspect a
clean build's commands and representative disassembly, and run the affected
hardware checks. Updating this text does not change the Makefile or its gates;
reconcile them in the repository. Do not loosen a check merely to obtain green.

---

## 3b. TEMPERATURE-INDEPENDENT rules (apply in COLD paths too)

Unlike the hot-only numeric restrictions, T1/T2 also apply in cold code,
subject to the bounded-normalization exemptions below. They address needless
iterative work and termination hazards. R1, R10, and the other explicitly
wider-scope requirements remain in force.

**They exist because widening §3 to all floats was considered and REJECTED.**
There are thousands of float declarations across the two trees against a
handful of `@vfp-exempt` pins, so blanket scope is roughly an order of
magnitude of annotation burden whose overwhelming majority would be
rubber-stamped — and a rule exempted thousands of times trains reflexive
stamping, which is worse than no rule because it looks like scrutiny. The
defect that prompted the proposal would ALSO NOT HAVE BEEN CAUGHT by it.

### T1 — Solve, don't search

Do not iterate toward a value that has a closed form. If the condition is
linear (or otherwise directly invertible) in the unknown, invert it.

It is temperature-independent because it is not a cycle argument: a search where
a solution exists is harder to read, has an approximate answer, and hides its
termination condition in a step size. **Fixed point would not have fixed it** —
a fixed-point descending loop is equally wrong.

### T2 — No float comparison in a LOOP CONDITION, at any temperature

`while (x > limit) x -= step;` is a termination hazard before it is a
performance one: floating-point accumulation decides the iteration count, so
the loop can run one iteration more or fewer than intended, and with a small
enough step or an unlucky value it may not terminate at all. Use an integer
iteration count, or solve it (T1).

This is NARROWER than R3 on purpose. R3 covers float comparisons driving
control flow generally and stays hot-only, because a swept-window `if` is a
legitimate gameplay predicate that must not be integerised. T2 is only about
the CONDITION OF A LOOP.

### The standing exemption for BOUNDED NORMALISATION — pin it, do not fix it

A wrap like this HAS a closed form (`fmod`, or `x - k*floor(x/k + 0.5)`), so it
reads as a T1 violation, and it is a float loop condition, so it reads as T2.
Both readings can be wrong in practice:

- the input is already near range, so it runs one or two iterations — there is
  no search, and no termination hazard in practice;
- ported gameplay math is **gated**: a closed form is not bit-identical to
  repeated subtraction, and a ULP there moves gameplay;
- the texture/asset DSL **defines asset output** — the same change moves
  texture bytes.

So: `@vfp-exempt T1` / `@vfp-exempt T2` and move on. **The rule is for code
being WRITTEN, not a licence to rewrite ported math that already works.**

---

## 4. SHOULD rules

- **R7** — Division and sqrt: VFPv2 has hardware `vdiv`/`vsqrt`. Prefer
  reciprocal-once outside the loop over per-element divides. Do not table them.
- **R8** — Fixed-point domains must not implicitly promote back to float
  mid-loop.
- **R9** — Stay inside the 32 S-register file. Doubles and live ranges that
  force spills are the failure mode.
- **R11 — INTEGER division or modulo by a RUNTIME value in a hot loop.**
  **This ARM11 target has no integer divide instruction.** Runtime integer
  division/remainder generally lowers to software helpers such as
  `__aeabi_idiv` / `__aeabi_idivmod`. A **compile-time-constant** divisor is
  not automatically a finding: compilers can use multiplication, shifts, masks.
  - `((x % n) + n) % n` can require **two** remainder operations/helper calls;
    confirm the emitted code. For positive `n`, where the value is proven to be
    within `-n <= v < 2*n` and the arithmetic cannot overflow,
    `if (v < 0) v += n; else if (v >= n) v -= n;` gives the exact wrapped
    integer result. Prove the bound and representation safety at each site.
  - **MDK's hot sites:** the script VM's operand decoding, the BSP node
    traversal, the palette's `index % frames` for animated textures, and the
    debris pool's wraparound. The screen is
    `python3 tools/runner/audit_r11_idiv.py`. It is a SCREEN, NOT A PROOF —
    "is this integer" and "is this hot" are both inferred, so a hit is a
    candidate to confirm against a profile and a clean run means "nothing
    obvious", never "nothing present".

---

## 5. Exemptions

Inline only, this grammar, no paraphrases:

```
/* @vfp-exempt R<id>[,R<id>...] — <reason>. measured <cost-or-n/a>. Verified <YYYY-MM-DD>. */
```

An exemption records a reviewed reason to preserve a site, including changes
to pixels, hit tests, rounding, asset output, or a measured tradeoff. It is
not a TODO. Do not strip pins to "clean up" a file. Do not add hot-only R-rule
pins to cold code; the existing T1/T2 exemptions can apply there. Preserve the
established `@vfp-exempt T1` / `@vfp-exempt T2` forms and check parser
support before changing annotation syntax.

Do not rewrite an exempted gameplay predicate without a new measured
OG-profile capture that says the rewrite is a win *and* a fidelity verdict
that says the hit set is unchanged.

Standing exemptions (corpus, not source comments) are in
`tools/runner/contracts/vfp_hotpath.md` §4: LUT range reduction, LUT
construction, mechanics-gated DOUBLE, procedural texture DSL.

---

## 6. What this pass already did — do not reopen

*(empty — the port has not yet closed its first performance pass.)*

**Hardware support and macro blind spots:** VFPv2 supports basic double
arithmetic in hardware. With the appropriate target/ABI settings, operations
can use `vmul.f64`/`vdiv.f64`/`vcvt.f64.f32` without software emulation.
Register pressure and operation-specific timings still matter.

Do not declare the current binary clear from a text scan.

---

## 7. Explicit non-rules

Do **not** report:

- An integer loop counter driving float arithmetic in the body.
- Mixed int and float in the same *function* when the hot loop stays in one
  domain.
- Ordinary float arithmetic in COLD PATH as a hot-path performance finding.
  R1's initialization requirement and §3b's rules still apply.
- Float comparisons outside a loop as R3/T2 findings.
- Hot-only math findings in code that meets none of §2's three criteria.

Do **not**:

- Introduce `#ifdef` renderer forks. Backend split is by file
  (`renderer_c3d.cpp` vs `renderer_vk.cpp`), not ifdef.
- Touch `mdk_core/src/util/math_lut.h` as part of a "quick perf pass".
- Measure New-clocked and call it OG.
- Treat a green `.3dsx` as a perf pass. OG profile, looked-at frame.

---

## 8. Scope

Include: `mdk_core/src/**`, `mdk_3ds/src/**`, `mdk_3ds/Makefile`.
Exclude: `mdk_linux/**`, `mdk_core/third_party/**`, `mdk_core/tools/**`,
`mdk_3ds/tools/**`.

---

## 9. CIA home-menu identity (build-blocking)

Every new CIA build MUST ship a publisher that is **not** the engine's internal
source name. The three SMDH strings are authored in the `mdk_3ds/Makefile`
identity block:

| Field | Makefile var | Value |
|---|---|---|
| short | `APP_TITLE` | `MDK` |
| long (shown on hover) | `APP_DESCRIPTION` | `Metal Dungeon` |
| publisher | `APP_AUTHOR` | `Shiny Entertainment` (a placeholder to be confirmed by the user) |

This is enforced, not assumed. `tools/runner/gates_smdh.py` reads each field
from the Makefile and re-reads the SMDH out of the BUILT `.cia` and fails if
any populated language slot disagrees. So a CIA whose publisher disagrees
cannot pass a production build. Changing `APP_UNIQUE_ID` is a separate, larger
decision (it forks the install into a second title) and is out of scope here.

---

## 10. C++ implementation constraints — "C with classes"

Scope is §8. This is the **language** half of `docs/DOCTRINE.md`. The
**numeric** half is §§3–4 and is deliberately not restated here — two copies of
R2/R4/R6/R11 is two copies to drift.

Do not introduce:

- **Virtual functions, virtual destructors, or inheritance that depends on
  virtual dispatch.** The reason is architecture, not the cycle argument §§1–4
  make: the renderer seam is chosen at **build time** by which tree is
  compiled (`renderer_c3d.cpp` vs `renderer_vk.cpp`), so a vtable would put a
  runtime decision where the build already made it.
- **RTTI** — `dynamic_cast`, `typeid`.
- **Exceptions.** Not "exceptions for control flow": exceptions at all. Both
  build systems compile `-fno-exceptions -fno-rtti`, so they do not compile.
  Setup errors use return codes / status structs, as `vfpSetFzDn()` does.
- **Project-defined templates.** Concrete types and ordinary functions. This is
  **house style, not an ARM11 performance rule** — say it that way, because the
  opposite claim is false: templates are instantiated at compile time and can
  produce code as efficient as ordinary concrete functions. The relevant ARM11
  argument here is §1 item 4, code footprint in a 16 KB L1I with L2 off, and
  that argues for restraint, not for a ban. The real reasons are readability and
  one-implementation-per-behaviour. Do not defend this rule by claiming
  templates are slow.
- **`std::function` and `std::bind`.** The project record has 0 in the tree.
  Keep free functions over `GameEngine&` and the flat C-API header
  (`render/render.h`). The ban remains MDK policy; do not defend it as a
  universal hardware fact.
- **`assert()`.** Use `static_assert` for compile-time invariants and explicit
  return codes for runtime ones. `assert` disappears under `NDEBUG` and turns
  a data bug into a silent wrong answer.

In HOT PATH (§2) also:

- **No per-frame container growth where the size is known.** Preallocate and
  reuse. Where a container genuinely grows during play, that is shipped design
  and the rule is about its CONSEQUENCE, not the growth. Do not "optimise" the
  spawn vectors into fixed arrays without checking that invariant first.

On the 3DS worker threads, **respect the stack you were given**: the audio
worker runs on a small stack. No recursion in those threads, and no large local
arrays or local containers — an overflow there is silent memory corruption in
the audio path, not a clean fault. Size locals against those numbers before
adding them.

Ordinary classes, structs, constructors, destructors, RAII, namespaces,
non-virtual member functions and local lambdas remain permitted. Use them to
make ownership and behaviour clear.

**Do not "tidy" `-fsingle-precision-constant` into `COMMONFLAGS`.** It is set
per-object, if at all, and widening it retypes every unsuffixed float literal
in the tree, which silently changes the **mechanics-gated DOUBLE** math R4
protects.

**Enforcement.** `python3 tools/runner/audit_c_with_classes.py` gates the
zero-tolerance constructs of this section: `virtual`, `override`,
`dynamic_cast`, `typeid`, `throw`/`try`/`catch`, `template`,
`std::function`/`std::bind`, `assert`, and the `<typeinfo>`/`<exception>`/
`<functional>`/`<cassert>` includes. The tree is at 0 of each, so the gate is
green with no baseline file — a hit is new code, and the fix is to not write
it, not to bless it. It strips comments and string literals before matching,
because this tree documents its own rules in prose ("no virtual, no vtable")
and a naive grep flags that prose as code.

What the gate still cannot see: the per-frame heap-growth rule and the worker
stack rule. Those stay review-only.

---

## 11. 3DS platform requirements

These rules cover the platform concerns needed alongside the math contract.
They apply to the 3DS backend and its shared interfaces; they do not require
Citro2D, RomFS, new workers, or a new rendering architecture where the project
does not use them. Correctness requirements apply in cold paths too.

**Enforcement status:** review and hardware validation. The existing math and
language scripts are not claimed to check P1–P6. For changes to these paths,
demonstrate compliance or record the unresolved requirement. Do not mark a
hardware check passed because compilation or a text scan succeeded.

### P1 — Application lifecycle, sleep, and resume

- The application MUST service `aptMainLoop()` regularly through its existing
  main-loop integration and exit when it returns false. Do not replace it with
  an unconditional loop or block it indefinitely on a worker.
- Use `aptHook` when application-specific suspend/resume or sleep/wake work is
  needed. Keep hook storage valid until unregistered.
- MDK MUST not advance gameplay by the wall-clock interval spent suspended.
  Rebase the frame timer on resume under the existing pause policy. Avoid
  stale queued input and verify that audio resumes without duplicate workers.
- Initialization failures and normal shutdown MUST release only resources
  successfully acquired, in dependency order. Stop producers and settle
  outstanding work before destroying their resources. Keep R1's immediate
  startup failure path; do not jump into uninitialized cleanup.

### P2 — Citro3D/Citro2D frame and state ownership

- Initialize the graphics libraries used by the selected backend before
  creating or using their resources; check fallible calls.
- Give rendering state a documented owner. Workers may prepare data, but MUST
  not concurrently mutate the shared Citro3D state without a verified
  serialization design.
- **Draw only after a successful `C3D_FrameBegin`, and pair each successful
  begin with one `C3D_FrameEnd`.** A failed/nonblocking begin does not
  authorize drawing. Select the intended target before issuing its commands.
- **All GPU state must be set explicitly every frame.** Uninitialised PICA200
  registers produce *invisible geometry* — no crash, no error, no output. The
  `C3D_AttrInfo`, `C3D_TexEnv`, depth test/write, `C3D_CullFace` and
  `C3D_DepthMap` calls are part of the frame contract, not optional tuning.
- **All uniform registers must be written every frame.** An unwritten uniform
  holds the previous frame's value or garbage.
- Preserve both-eye targets, projection, and slider behavior. Update gameplay
  once per simulation step and render the appropriate eye views; do not advance
  gameplay once per eye.

### P3 — GPU completion, cache visibility, and frame pacing

- Keep vertex/index buffers, textures, and other submitted resources valid
  until the GPU is finished with them. Do not overwrite, resize, or free an
  in-flight resource. Put the completion boundary **before** CPU reuse.
- Distinguish three obligations: CPU-thread ownership, CPU/GPU cache
  visibility, and GPU completion. A lock or cache flush alone is not a GPU
  completion fence.
- Current Citro3D waits for its previous queue in `C3D_FrameBegin()` and, with
  `C3D_FRAME_SYNCDRAW`, also performs frame synchronization. Its default
  `C3D_FrameEnd(0)` path flushes linear memory before queue execution. Preserve
  that behavior unless an alternative is measured and verified.
- Do not add redundant VBlank waits, manual swaps, per-draw waits, or cache
  flushes around an already managed frame path without identifying the need.
  Measure CPU preparation, GPU work, and waiting separately.

### P4 — Worker synchronization and shutdown

- Document ownership and lifetime for data shared by simulation, rendering,
  audio, and I/O. Use supported locks/events/atomics and an explicit handoff.
  `volatile` alone is not synchronization.
- On shutdown, signal workers, unblock their waits, and confirm completion
  before freeing shared data. For owned, joinable libctru threads, join before
  freeing the thread object.
- Apply R1 to new floating-point worker entry paths and respect §10's actual
  stack budgets. Do not increase thread count, priority, affinity, or stack
  allocation as an unmeasured performance fix.

### P5 — RomFS, SD access, and streaming

- If using RomFS, ensure it is packaged for each shipped format, mount it
  successfully with `romfsInit()` before access, and release it after its
  users finish. RomFS is read-only.
- Use explicit paths such as `romfs:/...` and `sdmc:/3ds/mdk/...` conventions.
  Do not assume a working directory or silently move user files.
- Check open/read/write/close results as applicable. Distinguish a complete
  asset from a short read. Missing assets, failed mounts, and unavailable/full
  storage need a defined recovery or error path, not an unbounded retry in the
  frame loop.
- Preload small reusable assets where appropriate. For larger data, use
  measured buffering/streaming with a bounded memory budget. Keep unpredictable
  blocking reads, asset decoding, directory scans, and verbose logging out of
  frame-critical work.
- **MDK's case-insensitive path resolution applies on the 3DS too.** The game
  asks for `MISC/MDKFONT.FTI`; the GOG tree ships `MISC/mdkfont.fti`. The
  resolver must work against a RomFS tree, not only a case-sensitive
  filesystem.
- Loading and streaming changes MUST preserve asset bytes and the current
  audio/gameplay behavior.

### P6 — Evidence for changes to platform paths

Select checks relevant to the changed behavior and record results explicitly:

| Area changed | Required evidence |
|---|---|
| Build or math | Clean build commands, tool/library versions, relevant disassembly, affected fidelity checks, and a comparable hardware capture |
| Rendering or synchronization | Busy-scene pacing, both-eye/slider behavior, buffer reuse under load, and successful resource teardown |
| Lifecycle or workers | HOME return, lid sleep/wake, repeated pause/resume, clean exit, and no stuck waits or duplicate workers |
| Filesystem or streaming | Packaged assets in each shipped format, missing/read-error handling, relevant save failures, and streaming behavior under load |

Record hardware, scene/workload, clock/cache settings, build revision, and
stereoscopic mode. Separate actual original-3DS testing from New 3DS
OG-profile testing. If actual original hardware is unavailable, state that
gap; do not claim it passed. A documentation-only revision is not a new
performance pass.

---

## 12. References

The ARM11 and VFP11 timings, the devkitPro flags, the libctru thread
initialization, the APT implementation, the Citro3D render queue, and the
devkitPro examples are the same sources t2k cites; see `../t2k/AGENTS.md` §12
for the numbered list with URLs. MDK-specific additions:

- `../forsaken/3DS_PORT_NOTES.md` — the D3D→PICA200 depth-range and
  cull-enum conversions, the hard-float ABI trap, the ARM unaligned-access
  list, and the colourkey-before-gamma bug.
- Obsidian `10-projects/forsaken/findings/citro3d-renderer-architecture-and-vertex-pipeline.md`
  — the citro3d frame lifecycle, the vertex conversion, and the texture
  format/Morton tiling rules.
- Obsidian `GX-to-PICA200/*` — the PICA200 hardware model and the pixel
  pipeline.
