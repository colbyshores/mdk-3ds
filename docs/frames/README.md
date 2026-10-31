# Frame records — the visual history of the port

Owner directive 2026-10-29: **every batch that moves the picture leaves a
picture in the commit history.** This directory is that record.

Each file is the pinned frame — level 3, 320x240, seed 1, frame 1, captured by
`tests/frames/capture_cpp.sh` and published by `tools/publish_frame.sh`:

```
<YYYY-MM-DD>-<label>.png    the frame
<YYYY-MM-DD>-<label>.txt    the measured C++-to-oracle gap at that moment
```

Read them as a sequence and you are watching the port converge on the original
look. The `.txt` beside each frame matters as much as the picture: a frame with
no number cannot say whether it moved toward the oracle or away from it, and
`tests/frames/reference.sha256` treats that gap as a ceiling that may only
shrink.

## Rules

- **Publish after the gates, before the commit.** The frame must come from a
  tree whose build is consistent, and from the same revision the batch commits.
- **The oracle is never published here.** The C# reference frame lives in a
  scratch directory (`tests/frames/capture_oracle.sh`) because it is
  ROM-derived pixel data and must not be committed. Only the C++ port's own
  frames — a derived, low-resolution progress record — are tracked.
- **Never hand-edit or replace a published frame to make a comparison look
  better.** A frame is evidence of what the build produced at that revision;
  the gate (`tools/runner/gates_frames.py`) is what decides whether it is
  acceptable.
- **What `captured: … at <rev>` means.** The tool can only name the revision it
  ran on top of, which for a batch's own frame is that batch's **parent** — a
  commit cannot contain its own hash, and the pair is committed WITH the code it
  shows. The pixels are the working tree's, i.e. the batch's own output. Do not
  read `at <rev>` as "these pixels came from <rev>"; where a sidecar needs to say
  more (a batch whose frame is not its own output, say), it says so in its own
  text. Review 2026-10-31 raised this; the note is the fix, not a re-capture.
- The game's own data and executable stay gitignored at `/MDK/` and are never
  committed, here or anywhere.

## How a batch publishes one

```sh
tools/publish_frame.sh <short-label>     # e.g. sky-axes, hud-aspect
git add docs/frames && git commit ...    # with the batch
```

The ralph loop (`ralph-pi.sh`) checks every committing iteration: if the batch
changed render-relevant paths and left no frame here, the loop publishes one
itself and commits it. A model that forgets cannot break the visual record.

## When the pinned frame cannot show the batch

Because the pin is one fixed moment, a subsystem that only exists after frame 1
cannot be seen in it — the object pass is the standing example, since at frame 1
neither tree has a script object and every object batch leaves this directory's
picture byte-identical. That is a property of the pin, not a licence to leave no
picture: the capture that DOES show the path goes in `docs/evidence/` (see its
README), and the sidecar here says in its own text why the pinned picture did
not move, with the measurement that shows it. The pin itself, and the ceiling in
`reference.sha256`, do not move.
