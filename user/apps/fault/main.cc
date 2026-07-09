// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Deliberate CPU-fault gate, in its own binary because it ends the process: main
// (root thread, privileged) executes an undefined/illegal instruction so the arch
// fault reporter runs its dump and hands off to kfault_terminate (exit 132 on the
// host/QEMU targets). This is the automated fault-dump gate. Its point is not that
// a fault halts -- it is that the DUMP comes out: on a chip whose console ring is
// armed (the sim arms one), the reporter must force the synchronous writer, or the
// dump is enqueued into a ring whose drain interrupt is masked and lost. A silent
// exit-only fault would pass without it; the marker assertion is what catches that.

#include <kickos/kos.h>

// Optional pre-crash delay (ms). Default 0 -- the ctest gate boards have a stable
// console. Set (e.g. -DKICKOS_FAULT_DELAY_MS=5000) for a board whose console
// re-enumerates on reset (the ESP32-C6 self-hosted USB-Serial-JTAG): it lets the
// host re-attach AFTER the reset before the dump prints, so the dump is captured
// rather than emitted into the enumeration gap and dropped.
#ifndef KICKOS_FAULT_DELAY_MS
#define KICKOS_FAULT_DELAY_MS 0
#endif

int main(int, char**)
{
    kos_print("[fault] executing an illegal instruction (expect a fault dump)\n");
#if KICKOS_FAULT_DELAY_MS
    kos_sleep_ns(static_cast<uint64_t>(KICKOS_FAULT_DELAY_MS) * 1000000ull);
#endif
#if defined(__XTENSA__)
    __asm volatile("ill");
#elif defined(__riscv)
    __asm volatile(".word 0x00000000"); // all-zero encoding: illegal on RV32
#elif defined(__arm__) || defined(__thumb__)
    __asm volatile("udf #0");
#else
    __builtin_trap(); // host/sim: x86 ud2 -> SIGILL -> on_sigill reporter
#endif
    // Unreachable: the fault path never returns. If a target somehow does not trap,
    // this distinct line fails the gate's negative assertion rather than passing.
    kos_print("[fault] ERROR: illegal instruction did not fault\n");
    return 0;
}
