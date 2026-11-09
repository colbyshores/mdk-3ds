# MDK C++ Port - Loop Context (Pi harness)

This document provides context for the Pi harness to understand the current state of the MDK C++ port.

## Project Overview
- Converting Metal Dungeon (MDK) from C# to C++ 
- C# source in `src/Mdk.Game/**` is the read-only behavioral oracle
- Phase 2: Script VM opcode porting is the main focus
- Target: Both Linux and 3DS (devkitARM)

## Current State (as of 2026-10-31)
- **Script VM Coverage**: 211/238 opcodes ported (88.7%)
- **Remaining**: 27 opcodes deferred per oracle boundary (C# doesn't implement them)
- **All gates passing**: `make -C tests/scripts` and `bash tools/check.sh` pass
- **Both targets build**: Linux and 3DS
- **Frame parity**: P5.0 frame-golden harness active, ~1.240% gap from oracle

## Key Files to Understand
- `SPEC-COMPLETION.md` - Mission order and oracle boundary
- `docs/CONTINUATION.md` - Current priority order and verified state
- `docs/PROGRESS_LOG.md` - Detailed batch history
- `docs/OPCODE_COVERAGE.md` - Opcode porting status
- `AGENTS.md` - Operating contract and rules

## Agent Architecture (owner directive 2026-10-31, updated 2026-11-01)
Three separated roles — the implementer is never the sole authority on its own
correctness:
- **Implementer / remediator**: Qwen (`litellm/qwen` → `openai/qwen38-flash-next`, family
  `qwen`). No fallback builder route (operator decision 2026-11-05): when qwen is
  unavailable the loop waits/backs off rather than switching models. Read-only
  review is performed by a different family.
- **Adversarial reviewer**: DeepSeek V4.1 Flash TH
  (`litellm/deepseek-v4.1-flash-th` → `openai/deepseek-v4.1-flash:free`, family
  `deepseek`), with `litellm/deepseek-v4.1-flash` (`deepseek/deepseek-flash` at
  api.deepseek.com, family `deepseek`) as fallback. Both reviewer routes are
  cross-family from the builder route (`qwen`), so no independence
  exception is needed. Read-only; structured findings; verdict gates remediation.
  Protocol: `ralph/validator.md`.
- **Verification authority**: the harness gates — `make -C tests/scripts`,
  `bash tools/check.sh`, `make -C mdk_3ds`, plus the batch-integrity gate. Objective
  pass/fail, independent of any model's opinion.
- Workflow: Qwen implements → harness gates verify → DeepSeek reviews adversarially
  (risk-based, see below) → Qwen remediates (fix confirmed, rebut invalid with
  evidence) → repeat.
- Oracle boundary enforced - only port what C# implements
- Every commit must include frame record in docs/frames/
- Gates must pass before committing

## Risk-based review policy (owner directive 2026-11-01)
Paid DeepSeek review is NOT invoked every iteration. The builder declares
`Risk: low|medium|high` in docs/CONTINUATION.md; the orchestrator decides:
- low  -> skip review (routine, localized, tests pass, no interface/behavior change)
- medium -> batch; review every 5 completed iterations or before a milestone
- high  -> review immediately
Failure-triggered: 2+ consecutive gate failures, uncertain root cause, test
weakening, or unexplained behavior also trigger review. Local verification suite
always runs. Track review-call count and estimated cost at each milestone.

## Important Constraints
- C# in src/Mdk.Game/** is READ-ONLY oracle
- Never guess behavior for unimplemented C# opcodes
- Follow AGENTS.md §10 (C with classes) and all hot-path rules
- Frame record required for every commit
- Must run emulator tests on DISPLAY=:99, never operator desktop
- **Prior-solution lookup (mandatory):** Before implementing, check Obsidian MCP
  for prior findings/architecture notes, and Cognee MCP for related solutions.
  If Obsidian MCP is unavailable, read the vault directly (read-only) from
  `/mnt/nas/Vaults/port-docs`. Record the lookup result in the commit message.
