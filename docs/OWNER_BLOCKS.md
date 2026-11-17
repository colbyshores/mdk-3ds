# Owner blocks

Rows in `docs/COMPLETION_LEDGER.tsv` are `owner-blocked` only when they appear in
this table. The completion gate refuses an `owner-blocked` row that is absent here,
so a batch cannot block its own work by declaring it blocked.

Each row states the exact decision required and what unblocks it. When the decision
is made, record it here and flip the ledger row to `todo` in the same commit.

| id | blocking decision | unblocks with |
|---|---|---|
| spec.voice-pool | `SPEC.md:809` says the 3DS voice pool shrinks to 24 and the audio queue to 40 ms; the port ships the C# oracle's 64 voices and an 80 ms queue (`src/Mdk.Engine/Audio/AudioDevice.cs:73 MaxVoices = 64`). The builder may not amend a protected spec. | Either amend `SPEC.md` §8 to match the oracle, or authorise a per-target 3DS cap with a stated reason. One line; no code change either way. |
| hw.cia-boot | **Owner decision 2026-11-17: CIA testing goes LAST, and it is not part of the netload hardware loop.** `netload_stub` and `hw_cycle.sh` are for `.3dsx` payloads only; the loop must never try to install or boot a CIA. The row was `hw_cycle.sh --install`, and that mode is removed. | The owner starts the final CIA pass, a separate step after the `.3dsx` work is done (install the `make -C mdk_3ds cia` output by the owner's chosen route, then boot it). Record the route and its evidence here and flip the row to `todo` with that acceptance, in the same commit. |
| re.phase-2-sweep | The Phase 2 post-process gap sweep needs the REA MCP server, which is disabled in `~/.config/kilo/kilo.jsonc`, and `SPEC-COMPLETION.md:146` says "No RE work enters a normal porting batch". | Enable the REA MCP server and authorise a dedicated sweep phase after the C#-driven work is exhausted. This is a config flip plus an authorisation, not hardware. |

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
