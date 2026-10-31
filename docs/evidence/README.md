# Evidence captures — pictures of paths the pinned frame cannot show

`docs/frames/` is the pinned regression record: level 3, 320x240, seed 1,
**frame 1**, one pair per batch, and `tests/frames/reference.sha256` treats its
gap as a ceiling that may only shrink. That pin is the right regression baseline
and it is not negotiable — but it is one fixed moment, and a subsystem that only
exists after frame 1 cannot be seen in it. The object pass is the standing
example: at frame 1 neither the C++ nor the C# has a script object, so every
batch that draws objects leaves the pinned picture byte-identical, and a
reviewer is right to say that a byte-identical record proves nothing about the
path it was meant to show.

This directory holds those captures.

## Rules

- **The pinned pair still ships, every batch, unchanged in meaning.**
  `tools/publish_frame.sh` writes `docs/frames/` and the loop's integrity gate
  requires exactly one new pair there per batch. Nothing here replaces it, and
  nothing here is compared against `reference.sha256`.
- **One pair per claim, named `<date>-<scene>-<claim>.png` with a `.txt`
  beside it.** A picture without its numbers is not evidence.
- **The sidecar records what the frame gate records and more**: the scene and
  frame, the exact command, the display used (AGENTS.md P7 — a capture with no
  display recorded does not count), the revision it was built from, and the
  oracle comparison with the oracle capture's own hash, so a later batch can
  prove it is comparing against the same reference.
- **The oracle's pixels are never committed here either.** They are ROM-derived
  and stay in the scratch directory (`tests/frames/capture_oracle.sh`). Only the
  C++ build's own output is tracked, as in `docs/frames/`.
- **Never edit a published pair.** Same rule as the frame record: a capture is
  evidence of what the build produced at that revision.
