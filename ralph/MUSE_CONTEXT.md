# MDK C++ Port - Progress Context for Meta Muse CLI

This document provides context for Meta Muse CLI to understand the current state of the MDK C++ port.

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

## Ralph Loop Configuration
- Worker chain: qwen (local) first, then minimax, then glm variants, then luna
- Reviewer: Glyph Cluster (kilo/stealth/glyph-cluster) as primary, qwen/luna as fallbacks
- Oracle boundary enforced - only port what C# implements
- Every commit must include frame record in docs/frames/
- Gates must pass before committing

## Important Constraints
- C# in src/Mdk.Game/** is READ-ONLY oracle
- Never guess behavior for unimplemented C# opcodes
- Follow AGENTS.md §10 (C with classes) and all hot-path rules
- Frame record required for every commit
- Must run emulator tests on DISPLAY=:99, never operator desktop
