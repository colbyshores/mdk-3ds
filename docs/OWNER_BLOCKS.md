# Owner blocks

Rows in `docs/COMPLETION_LEDGER.tsv` are `owner-blocked` only when they appear in
this table. The completion gate refuses an `owner-blocked` row that is absent here,
so a batch cannot block its own work by declaring it blocked.

Each row states the exact decision required and what unblocks it. When the decision
is made, record it here and flip the ledger row to `todo` in the same commit.

| id | blocking decision | unblocks with |
|---|---|---|
| hw.cia-boot | **Owner decision 2026-11-17: CIA testing goes LAST, and it is not part of the netload hardware loop.** `netload_stub` and `hw_cycle.sh` are for `.3dsx` payloads only; the loop must never try to install or boot a CIA. **Refined 2026-11-19:** the owner first tests the `.3dsx` on their own New 3DS; the CIA pass follows that. | The owner reports their New 3DS `.3dsx` test, then starts the final CIA pass (install the `make -C mdk_3ds cia` output by the owner's chosen route, then boot it). Record the route and its evidence here and flip the row to `todo` with that acceptance, in the same commit. |
| re.phase-2-sweep | The Phase 2 post-process gap sweep needs the REA MCP server, which is disabled in `~/.config/kilo/kilo.jsonc`, and `SPEC.md` §15.4 says the sweep is a separate pass, never inside an ordinary batch. | Enable the REA MCP server and authorise a dedicated sweep phase after the C#-driven work is exhausted. This is a config flip plus an authorisation, not hardware. |

## Decided

- **spec.voice-pool** (2026-11-19): the 3DS replicates `../forsaken`'s sound system -- the DSP mixes, 8 effect voices on ndsp channels 0-7 with oldest-eviction, DSP-ADPCM music on channel 23 by a 25 ms worker -- while the 64 logical voices and their lifecycle stay shared code, because `if_sound_playing` reads them. Recorded in `SPEC.md` §8; the ledger row is `todo`.

## Not blocked: hardware

The closed-lid autonomous harness (`netload_stub`, clamshell shut by design,
`push -> run N -> self-quit -> chain-load stub -> pull log`) runs the `.3dsx` `hw.*`
rows without a human. MDK has been wired in since 2026-11-17: `hw.harness-cycle`,
`hw.pixels-rgba8` and `hw.profile-og` were closed by `ledger_close.py`, which ran
their acceptance on `old3ds-lan167`. A cycle takes ~330-400 s for the 119 MB
`.3dsx`. `hw.cia-boot` is NOT a harness row (see the table above).
`hw.sleep-resume` stays `todo`: ATM has no remote lid/sleep control today. See
Obsidian `20-runbooks/3ds-autonomous-hardware-profiling-loop.md` and
`10-projects/netload_stub/`.
