# Owner blocks

Rows in `docs/COMPLETION_LEDGER.tsv` are `owner-blocked` only when they appear in
this table. The completion gate refuses an `owner-blocked` row that is absent here,
so a batch cannot block its own work by declaring it blocked.

Each row states the exact decision required and what unblocks it. When the decision
is made, record it here and flip the ledger row to `todo` in the same commit.

| id | blocking decision | unblocks with |
|---|---|---|
| spec.voice-pool | `SPEC.md:809` says the 3DS voice pool shrinks to 24 and the audio queue to 40 ms; the port ships the C# oracle's 64 voices and an 80 ms queue (`src/Mdk.Engine/Audio/AudioDevice.cs:73 MaxVoices = 64`). The builder may not amend a protected spec. | Either amend `SPEC.md` §8 to match the oracle, or authorise a per-target 3DS cap with a stated reason. One line; no code change either way. |
| re.phase-2-sweep | The Phase 2 post-process gap sweep needs the REA MCP server, which is disabled in `~/.config/kilo/kilo.jsonc`, and `SPEC-COMPLETION.md:146` says "No RE work enters a normal porting batch". | Enable the REA MCP server and authorise a dedicated sweep phase after the C#-driven work is exhausted. This is a config flip plus an authorisation, not hardware. |

## Not blocked: hardware

The closed-lid autonomous harness (`netload_stub`, clamshell shut by design,
`push -> run N -> self-quit -> chain-load stub -> pull log`, ~107-108 s per 90 s
run) makes the `hw.*` rows executable without a human, so they are `todo`, not
`owner-blocked`. See Obsidian
`20-runbooks/3ds-autonomous-hardware-profiling-loop.md` and
`10-projects/netload_stub/`. MDK is not yet wired into it: that is the
`3ds.harness-hooks` (host-verifiable) and `hw.harness-cycle` (on-device) pair.
