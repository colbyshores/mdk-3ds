---
description: Run the adversarial validator (luna) over the current change and report falsification attempts. Use after every phase, or whenever a claim about correctness, fidelity or performance needs to be disproved.
agent: adversarial-validator
model: litellm/luna
subtask: true
---

Adversarially validate the current working tree of this repository.

$ARGUMENTS

Context you need:

- `SPEC.md` is the design contract. The code must match it.
- `AGENTS.md` is the performance and platform contract. The gates in
  `tools/runner/` enforce parts of it; the rest is review-only.
- `src/` is the C# port of the same game and is the BEHAVIOURAL ORACLE. It
  is read-only and must not be modified. For any question of the form "is
  this right?", the answer is what the C# does.
- `../t2k/` is the sibling project whose house style, build system and
  performance gates this port inherits.
- `../forsaken/3DS_PORT_NOTES.md` holds the D3D→PICA200 conversion rules.
  MDK is a Direct3D game, so they apply verbatim.

What to do:

1. Work out what changed. `git status --short` and `git diff` against HEAD.
   If the tree is clean, review the most recent phase's files instead and
   say so.
2. For each changed piece, construct the strongest possible attempt to break
   it, in the priority order the agent definition gives: fidelity to the C#
   oracle, the 3DS traps, the performance contract, the language contract,
   memory and lifetime, and the build (does the new file compile on BOTH
   targets?).
3. For each finding, give the file, the quoted code, the mechanism, and the
   EVIDENCE — a failing input, a disassembly, a doc quote or the C# quote.
   A finding without evidence is an opinion, and you must label it UNPROVEN
   if you cannot produce one.
4. Report, explicitly, what you tried to break and could not. That section
   is what stops the writer re-litigating a sound path.

Do not fix anything. You have no edit permission by design. Report.
