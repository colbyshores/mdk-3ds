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
WORKER_MODELS="litellm/qwen litellm/deepseek-v4.1-flash-th"
REVIEWER_FALLBACKS="litellm/deepseek-v4.1-flash-nvidia litellm/minimax-m3.1-flash-nvidia litellm/luna"
```

Both model IDs were verified live on the proxy before being written. **grok is
retired by operator decision** and must not be re-added to any chain.

Two consequences recorded so nobody rediscovers them the hard way:

- When the builder falls back to `deepseek-v4.1-flash-th`, the reviewer's own
  family rule skips both DeepSeek variants, so review lands on minimax or luna.
- Still outstanding, and **not** done here: the shared router
  `/opt/pi/extensions/litellm-router.ts` (root-owned) still carries grok in
  `FALLBACK` and `TS_REVIEW_CHAIN`, and `perfect_dark` and `tetrisphere-godot`
  still reference `litellm/grok` in their own chains. Removing the `grok` model
  from `~/.config/kilo/kilo.jsonc` before those are fixed would make their
  reviews fail to resolve, and this loop's design blocks rather than skips an
  unresolvable mandatory reviewer. Fix the other two projects first.
