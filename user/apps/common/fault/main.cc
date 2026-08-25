// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Deliberate CPU-fault gate, in its own binary because it ends the process: main
// (the root thread, which kmain spawns unprivileged in every posture) executes an
// undefined/illegal instruction. The claim is that the DUMP comes out: on a chip whose
// console ring is armed (the sim arms one), the reporter must force the synchronous
// writer, or the dump is enqueued into a ring whose drain interrupt is masked and lost.
// A silent exit-only fault would otherwise pass, and the marker assertion is what
// catches it. Which marker and which exit status the gate pins belongs to the posture;
// CMakeLists.txt picks them.

#include <kickos/kos.h>

// Optional pre-crash delay (ms), default 0. Set (e.g. -DKICKOS_FAULT_DELAY_MS=5000) for
// a board whose console re-enumerates on reset (the ESP32-C6 self-hosted
// USB-Serial-JTAG): it lets the host re-attach AFTER the reset before the dump prints,
// so the dump is captured rather than emitted into the enumeration gap and dropped.
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
#elif defined(__aarch64__)
    // All-zero is a permanently-undefined A64 encoding, reported with EC 0x00; the RV32 arm
    // above uses the same idiom. NOT __builtin_trap, which is `brk` on this ISA and raises a
    // DEBUG exception (EC 0x3C) instead: a different vector cause and a different report.
    __asm volatile(".inst 0x00000000");
#elif defined(__RX__)
    __asm volatile("brk"); // BRK traps through rvector[0]
#else
    __builtin_trap(); // host/sim: x86 ud2 -> SIGILL -> on_sigill reporter
#endif
    // The fault path never returns. This distinct line makes a target that did not trap
    // fail the gate's negative assertion instead of passing.
    kos_print("[fault] ERROR: illegal instruction did not fault\n");
    return 0;
}
