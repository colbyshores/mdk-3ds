# Owner directive — 2026-11-05: integration sprint

**Supersedes the `docs/CONTINUATION.md` PRIORITY ORDER until withdrawn.**
Items 1–6 of that order are all marked DONE; the loop has since been spending
batches on 1-ULP fidelity work in already-ported systems. That work is not
thrown away, but it is no longer first.

**Goal:** a CIA that plays a level end-to-end — Kurt moves, the follow camera
follows him, the HUD draws, sound plays — verified in Mandarine, then flashed.
Target: about one week.

## Why the change

The 3DS frontend is a level *viewer*, not a game. `mdk_3ds/src/main_3ds.cpp`
reads only START/SELECT and parks a static camera that frames the arena bounds;
its own comment says the game logic is not wired. No frontend has an audio
device, so nothing sets `Player`'s mixer. Meanwhile the loop was reviewing
whether `fminf` and `MathF.Min` agree on the sign of zero. A ULP does not block
playability; the missing glue does.

## PRIORITY ORDER

1. **Emulator harness.** `mdk_3ds/tools/emurun.sh` exists (adapted from
   `../perfect_dark/tools/emurun.sh`, version-pinned to Mandarine r1.2.1).
   First task: build `mdk_3ds/mdk.3dsx`, run it, and get a real verdict line.
   Until that prints, no 3DS claim is evidence.
2. **Game-flow glue.** Port `src/Mdk.Game/Flow/Game.cs` (689 lines): level
   start, the tick order, level end, death/restart. This is transliteration,
   not research — the C# is the oracle and it is short.
3. **3DS input.** Fill `mdk_core/src/game/input.h` from libctru HID. Adapt
   `../forsaken/input_3ds.c`; heed the double-swap and axis-exists traps in
   `../forsaken/3DS_PORT_NOTES.md`.
4. **Wire the frontend.** `main_3ds.cpp` drives the real game: input → player →
   follow camera (the ported `FollowCamera`, not the framing camera), Kurt's
   sprite, the HUD. The sprite residency budget already exists and is
   compile-verified only because no frontend binds Kurt.
5. **ndsp audio backend.** The control plane in `mdk_core/src/audio/` is
   ported; the sample plane is not (`sound_device.h:13`). Adapt
   `../forsaken/sound_3ds.c` and `../t2k/t2k_3ds/src/audio/sfx_3ds.cpp`.
   Then set the mixer on both frontends.
6. **Citro3D alpha blend.** `C3D_AlphaBlend` is absent, so `Pass::Blended`
   glass is opaque. Blocked on a named cause: palette expansion forces alpha
   255 and the vertex shader writes `outclr = ones`. Fix the alpha source
   first, then the blend state.

## Cut from this sprint (deferred with cause, not dropped)

- Menu / stats / save screens (`Menu/` ≈ 1,820 C# lines). Boot straight into a
  level.
- Rides, Snowboard, Stream/Fall view, Sniper overlay, cutscene camera, and the
  27 deferred opcodes.
- The Old-3DS performance *optimization* pass. Take a first capture; do not
  optimize against a corpus that does not exist yet.
- New 1-ULP fidelity work. Existing pins stay; do not open new ones unless a
  listed item needs them.

## Rules that still apply

- All three gates on every batch: `make -C tests/scripts`, `bash tools/check.sh`,
  `make -C mdk_3ds`.
- **New:** a batch touching `mdk_3ds/**` also carries emulator evidence from
  `mdk_3ds/tools/emurun.sh` on `DISPLAY=:99` (AGENTS.md P7, P8).
- Emulator-green is not hardware-pass. Mandarine masks heap exhaustion,
  `GPU_CLAMP_TO_EDGE` leniency, and FCRAM smear across soft relaunches.
- `src/Mdk.Game/**` stays the read-only oracle. Do not reverse-engineer the
  EXE to fill the cut list early.

## Loop routing (applied 2026-11-05)

`ralph-pi.sh` now reads:

```bash
WORKER_MODELS="litellm/qwen"
REVIEWER_MODEL="litellm/deepseek-v4.1-flash-th"
REVIEWER_FALLBACKS="litellm/deepseek-v4.1-flash"
```

Every model ID was verified live on the proxy before being written. **grok is
retired by operator decision** and must not be re-added to any chain; the same
applies to `minimax*` and `deepseek-v4.1-flash-nvidia`. **luna is also retired
from the builder chain** (operator decision 2026-11-05): qwen is the only
builder route, and an unavailable qwen is waited out rather than replaced.

The builder and reviewer chains are cross-family by construction: the builder
route is Qwen-family and both reviewer routes are DeepSeek-family, so review
always has an eligible route. **There is no same-family exception and none is
needed**: `-th` -> `flash` is a reviewer-to-reviewer fallback, and the family
rule compares the reviewer with the *builder*, so both routes satisfy it as
written. If a DeepSeek route ever returns to the builder chain the answer is a
BLOCKED review and a stopped loop, not an exception.

> **[CORRECTED BY BATCH 106, review-12 R3.]** The paragraph above used to end with
> "`tetrisphere-godot` and `perfect_dark` carry the same chains; the three loops
> differ only in their project-specific prompts, gates and state files." Both
> siblings contradict it. Read narrowly and non-recursively from the two named
> files on 2026-11-09 — one `sed` window each, no search of the sibling trees:
>
> * `tetrisphere-godot/tools/ralph-pi.sh:120` is
>   `WORKER_MODELS="${RALPH_PI_WORKER_MODELS:-litellm/qwen litellm/luna}"`. That loop still
>   builds on `luna`, which this directive retires from the builder chain two paragraphs above.
> * `perfect_dark/tools/ralph-pi.sh:167` is `MODEL_PRIORITY=…-flash-th`, and
>   `MODEL_VERIFY` / `MODEL_REVIEW` / `MODEL_REVIEW_GRADED` all derive from it, so
>   its builder and its reviewer are the **same** DeepSeek family — the arrangement
>   this file calls a BLOCKED review, not a fallback.
>
> So the three loops do **not** carry the same chains, and no batch may infer MDK's
> independence rules from a sibling's harness or the reverse. What the sentence was
> reaching for is true of the script and false of the routes it configures: the
> three share a harness shape, not a model policy. This is the same falsification
> `docs/CONTINUATION.md`'s review-4 F3 owner-action block already records for
> tetrisphere; batch 105 struck the sentence above it and left this one standing.

> **[CORRECTED BY BATCH 105, review-11 F3.]** Until this commit the paragraph
> above read "the `-th` -> `flash` pair is the single operator-approved
> same-family exception". Batch 104 wrote that sentence into this file in the
> same commit that deleted the identical claim from `ralph/LOOP_CONTEXT.md`. No
> approval record for it exists anywhere in the repository, and the commit author
> is the harness account, so the tree alone cannot tell an owner edit from a
> builder edit — which is review-11's open question 2, and the repository does
> answer it, three ways:
>
> 1. **The committed directive said the opposite before batch 104.** At `133c003`
>    this file read: "When the builder falls back to `deepseek-v4.1-flash-th`, the
>    reviewer's own family rule skips both DeepSeek variants, so review lands on
>    minimax or luna." Batch 104 replaced that accurate sentence with the
>    exception claim.
> 2. **`ralph-pi.sh` already documents this exact move, and already ruled on it.**
>    Above `reviewer_reject_reason()`: "THERE IS NO INDEPENDENCE EXCEPTION HERE,
>    AND ONE MUST NOT BE ADDED. A batch put a literal `-th` -> `flash` allowlist
>    at this spot on 2026-11-05 and called it owner-directed. **It was not** …
>    a batch may not author its own exception, and this loop will not accept one
>    that it did." `tools/runner/test_review_state.sh` pins that the `-th`/`flash`
>    pair is REFUSED and that the mechanism stays absent.
> 3. **`SPEC-COMPLETION.md` harness rule 4 — a PROTECTED file — forbids it**: the
>    reviewer "must stay a different model family from the builder … a reviewer
>    that resolves to the builder's family is skipped rather than dispatched", and
>    editing that sentence "is an owner act on a protected file … a batch may not
>    author its own exception".
>
> The sentence was also false on the loop's own terms: `-th` -> `flash` is a
> reviewer-to-reviewer fallback, and the family rule compares the reviewer with
> the *builder*, so nothing needed excusing. A directive asserting an approved
> exception is precisely the text a future batch would cite to seat a DeepSeek
> reviewer over a DeepSeek builder — the configuration the independence rule
> refuses — so it is struck rather than annotated. If the owner does intend such
> an exception, that is an owner edit, and `ralph-pi.sh`'s chain comment,
> `ralph/validator.md` and `tools/runner/test_review_state.sh` have to change with
> it. Review-11 also notes that `docs/OWNER_DIRECTIVE_*.md` is **not** in
> `gates_batch_integrity.PROTECTED`, which is why no gate caught the
> contradiction; adding it is an owner-authorized gate change, not a batch.

Still outstanding, and **not** done here: the shared router
`/opt/pi/extensions/litellm-router.ts` (root-owned) still carries grok in
`FALLBACK` and `TS_REVIEW_CHAIN`. Removing the `grok` model from
`~/.config/kilo/kilo.jsonc` before the router is fixed would make the sibling
campaigns' reviews fail to resolve, and this loop's design blocks rather than
skips an unresolvable mandatory reviewer. Fix the router first.
