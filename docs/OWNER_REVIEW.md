# Owner review — items to run by the owner at the end

Started 2026-11-16 on the owner's instruction: where a PICA200 analogue
cannot get close enough to the enhanced PC effect, or a judgement call
was parked rather than made, it is notated here instead of being faked
or silently loosened.

## PICA200 analogues (3DS enhanced look, SPEC.md 7.9)

| PC enhanced effect | PICA200 analogue | Verdict |
|---|---|---|
| Lit surfaces (hemisphere + sun) | Hardware lighting + combiner ambient/directional terms | Expected close in stills; not yet built |
| Filtered colour textures / sky / canvas | Native bilinear filtering | Expected essentially identical at 400x240 |
| Haze | Hardware fog (fog LUT) | Expected close |
| Point lights | Limited hardware lights; additive billboards beyond that | Recognizable; fewer simultaneous lights |
| Glow billboards | Additive sprites | Expected close |
| Sun shadows (2048 PCF map) | No shadow-map path; blob shadows or none | **NOT CLOSE — owner's call**: accept blob/no shadows on 3DS, or spend the fillrate on an approximation |
| Post occlusion + glow grade | No post pipeline; pre-darkened vertices or skipped | **NOT CLOSE — owner's call**: skip, or accept a cheap combiner approximation |

## Findings parked for adjudication

1. **HUD ink floor (emurun --expect hud).** The panel-location bug is
   fixed (the marker verdict passes 121/121: the harness's panel crop
   assumed a window position the seeded Qt config no longer holds, so
   it measured black while the game drew the HUD correctly). The
   remaining ink check measures 25 exact-green pixels (21 inside the
   derived region, bbox 2px past its right edge) where the derivation's
   reference capture had 134 inside. Either the digit rendering lost
   exact-green coverage since that capture, or the floor (set at a
   sixth of that capture) is scene-dependent and stale. Not loosened;
   owner's call on which side is wrong.
2. **Enhanced lit residue (Linux).** The 86.2% / 88.0-ceiling gap is a
   per-face light-distribution difference (effective-light median +6%,
   a shadowed-face tail the oracle has), isolated stage by stage with
   no single remaining defect found. Burning it down further is a
   research item, not a known fix.
3. **Emulator HUD acceptance history.** `3ds.hud-canvas` was closed on
   emitted-quads evidence; the visibility verdict only became readable
   once the panel could be located. The row's done state predates a
   working visibility check and should be re-read in that light.
4. **CIA packaging (p8.cia-build).** makerom 0.19 IS installed now
   (`~/.local/bin/makerom`), so the row is no longer blocked on the
   tool -- but `mdk_3ds/tools/build_cia.sh`'s invocation predates it
   (unknown `-name`/`-publisher` flags, an RSF passed as a directory),
   and the 0.19 RSF schema needs a dedicated pass: the AccessDesc
   preset templates are not installed, so the RSF must carry the full
   AccessControlInfo by hand (CoreVersion, SystemCallAccess with this
   version's exact svc names, kernel capabilities). Estimated half a
   day with the makerom source's examples; the row stays `todo`.
5. **Font mod consumer (slice g remainder).** Deferred by design, not
   blocked: a mod's font image assumes the oracle's single-row glyph
   atlas, while this port wraps the atlas for the PICA200's 1024px
   texture limit, so the replacement's UV layout cannot be honoured
   without unifying the layouts (a Linux-only single-row atlas when a
   font mod is present is the sketched route). No live Linux font
   consumer exists yet either (the settings menu is 3DS-only, where
   mods do not run).
