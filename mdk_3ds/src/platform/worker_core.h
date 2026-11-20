#pragma once

// ============================================================================
// worker_core.h -- which CPU core MDK's worker threads run on.
//
// OWNER DECISION (2026-11-19): the async file reader (async_file_reader.cpp)
// and the DSP audio worker (audio_dsp_3ds.cpp) run on CORE 1 on BOTH the Old
// 3DS and the New 3DS, as ../t2k does for its audio (t2k_3ds/src/audio/
// music_3ds.cpp and sfx_3ds.cpp: APT_SetAppCpuTimeLimit(30), then
// threadCreate(..., AUDIO_CORE = 1, ...), falling back to -2). Before this they
// were created on -2, the process's default core -- core 0, the core the main
// thread renders and simulates on -- so every reader wake and every 25 ms music
// tick was taken out of the frame. On core 1 neither can preempt the main
// thread at all.
//
// NOT t2k's texture-worker rule (04_setup.inc: `isNew ? 2 : 1`): the owner
// wants core 1 on the New 3DS too, which leaves its core 2 free.
//
// CORE 1 IS THE SYSCORE. An application gets time there only after
// APT_SetAppCpuTimeLimit grants a share (workerCoreInit, below, once at
// startup); without the grant a thread created there may be refused or starved.
// Each creation site therefore tries kWorkerCore and, if threadCreate refuses
// it, kWorkerCoreFallback -- the old placement, so a refusal costs the frame
// time the move was meant to save but never the worker -- and logs the core it
// got.
//
// THE A/B (AGENTS.md P4: an affinity change is measured, not assumed): a
// harness build with `make AUTOQUIT=90 WORKERCORE=0` defines
// MDK_HW_WORKERCORE_DEFAULT, which puts both workers back on -2 and skips the
// time-limit call -- the placement before this change, for the owner's console
// comparison.
// ============================================================================

#include <3ds/types.h>

namespace mdk {

#ifdef MDK_HW_WORKERCORE_DEFAULT
constexpr int kWorkerCore = -2;  // the A/B: the process's default core (core 0)
#else
constexpr int kWorkerCore = 1;   // the syscore, on Old and New 3DS alike
#endif
// The fallback when kWorkerCore is refused: the process's default core.
constexpr int kWorkerCoreFallback = -2;
// t2k's grant: the workers' per-tick work is small (a 25 ms music refill, an SD
// read that mostly waits on the FS service), so 30% of core 1 is ample.
constexpr u32 kSyscoreTimeLimitPercent = 30;

// Once, in main, after APT is up (libctru's startup) and before any worker
// starts: grants kSyscoreTimeLimitPercent of core 1 (idempotent; harmless on
// the New 3DS) and logs the result once. Skipped in the WORKERCORE=0 A/B. In a
// harness build it also probes whether cores 2 and 3 accept a thread (a no-op
// thread, joined), so a New 3DS's log says which extra cores an app may use.
void workerCoreInit();

}  // namespace mdk
