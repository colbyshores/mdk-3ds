#pragma once

// ============================================================================
// side_core_3ds.h -- where a worker thread runs: off the main thread's core when
// the console has one to give, on the main thread's core when it does not.
//
// THE CORES (vault 60-references/3ds-platform/arm11/03-multicore-threading.md):
//   - core 0 is the application core: the main thread, and every thread made
//     with core -2 (the exheader's default), shares it;
//   - core 1 is the system core: an application may run ONE thread there, and
//     only after APT_SetAppCpuTimeLimit has granted it a share of that core's
//     time (kSyscorePercent); a second thread there fails to be created;
//   - core 2 exists on the New 3DS only (the CIA's exheader allows it,
//     cia/mdk.rsf CanAccessCore2), with no time limit.
//
// offMainCore3ds names the core to ask for -- core 2 on a New 3DS, else core 1
// for the one thread allowed there -- and each caller makes its thread there
// with threadCreate, then again on core -2 (today's placement) when the kernel
// refuses, so a console or a launcher that grants neither still runs every
// worker. The calls stay at the call sites, entry named: the R1 gate
// (tools/runner/gates_vfp.py) reads each threadCreate's worker body.
// ============================================================================

#include <3ds.h>
#include <stdint.h>

namespace mdk {

// The share of the system core's time the application asks for before its one
// thread there (APT_SetAppCpuTimeLimit). Chosen by measurement on the Old 3DS
// (docs/WORK_LEDGER.md, 2026-11-25): the music worker's reads need little CPU.
constexpr uint32_t kSyscorePercent = 30;

// The core a worker should be made on: 2 on a New 3DS; 1 when `allowSyscore`
// (only one thread asks: the music worker) and the system granted its share of
// that core (APT_SetAppCpuTimeLimit, asked once); else -2, the default core.
int32_t offMainCore3ds(bool allowSyscore);

} // namespace mdk
