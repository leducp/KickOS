// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Deliberate CPU-fault gate, in its own binary because it ends the process: main
// (the default composition's task, unprivileged in every posture) executes an
// undefined/illegal instruction, or with KICKOS_FAULT_DIVIDE a 32- or 64-bit divide by zero.
// The claim is that the DUMP comes out: on a chip whose console ring is armed (the sim arms
// one), the reporter must force the synchronous writer, or the dump is enqueued into a ring
// whose drain interrupt is masked and lost.
// A silent exit-only fault would otherwise pass, and the marker assertion is what
// catches it. Which marker and which exit status the gate pins belongs to the posture;
// CMakeLists.txt picks them.

#include <kickos/kos.h>
#include <kickos/sys/trap.h>

#include <stdint.h>

// Optional pre-crash delay (ms), default 0. Set (e.g. -DKICKOS_FAULT_DELAY_MS=5000) for
// a board whose console re-enumerates on reset (the ESP32-C6 self-hosted
// USB-Serial-JTAG): it lets the host re-attach AFTER the reset before the dump prints,
// so the dump is captured rather than emitted into the enumeration gap and dropped.
#ifndef KICKOS_FAULT_DELAY_MS
#define KICKOS_FAULT_DELAY_MS 0
#endif

#if defined(KICKOS_FAULT_DIVIDE)
#if KICKOS_FAULT_DIVIDE == 32
typedef int32_t fault_div_t;
#else
typedef int64_t fault_div_t;
#endif

// UBSan would report the division before it reached the hardware or the run-time ABI.
__attribute__((noinline, no_sanitize("integer-divide-by-zero"))) static fault_div_t
divide_by_zero()
{
    fault_div_t volatile dividend = 7;
    fault_div_t volatile divisor = 0;
    return dividend / divisor;
}

// A quotient nothing reads lets the compiler drop the division.
static fault_div_t volatile g_quotient;

int main(int, char**)
{
    kos_print("[fault] executing a divide by zero (expect a fault dump)\n");
    g_quotient = divide_by_zero();
    kos_print("[fault] ERROR: the divide by zero did not fault\n");
    return 0;
}
#else
int main(int, char**)
{
    kos_print("[fault] executing an illegal instruction (expect a fault dump)\n");
#if KICKOS_FAULT_DELAY_MS
    kos_sleep_ns(static_cast<uint64_t>(KICKOS_FAULT_DELAY_MS) * 1000000ull);
#endif
#if defined(__RX__)
    __asm volatile("brk"); // BRK traps through rvector[0]
#else
    KOS_TRAP_ILLEGAL();
#endif
    // The fault path never returns. This distinct line makes a target that did not trap
    // fail the gate's negative assertion instead of passing.
    kos_print("[fault] ERROR: illegal instruction did not fault\n");
    return 0;
}
#endif
