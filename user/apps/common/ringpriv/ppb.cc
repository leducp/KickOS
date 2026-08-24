// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// PRIVILEGE-RING gate, fault arm: an unprivileged thread cannot READ a privileged-only
// region, witnessed through a real trap. Its own binary, since the trap ends the process.
//
// This arm is INDEPENDENT OF THE MPU, which is what lets a board with no MPU witness a
// confinement fault at all. Two ARM ARM facts carry it (ARM DDI 0403E.e):
//   - "Unprivileged access to the PPB causes BusFault errors unless otherwise stated"
//     (B3.1.1, the PPB being 0xE0000000-0xE0100000, which contains the System Control
//     Space at 0xE000E000).
//   - The MPU never arbitrates that. ValidateAddress() computes
//     `isPPBaccess = (address<31:20> == '111000000000')` and takes the default system
//     address map on a hit, BEFORE it ever consults MPU_CTRL.ENABLE (B3.5.3); B3.5.1
//     says the same in prose: "Accesses to the Private Peripheral Bus (PPB) always use
//     the default system address map".
// So the trap below fires with the MPU absent, present-and-disabled, or enforcing.
//
// The target is SCB->CPUID, read-only with no side effects, so an unexpectedly SUCCESSFUL
// read cannot perturb the machine it just failed to confine. STIR (0xE000EF00) would not
// serve: a privileged CCR.USERSETMPEND can open that one SCS register to unprivileged
// access, which would make a refusal there contingent on a control bit and not on privilege.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/libc/fmt.h>
#include <kickos/sys/emit.h>

using kickos::emit;

namespace
{
    constexpr uintptr_t SCB_CPUID = 0xE000ED00u;

    uint32_t load32(uintptr_t addr)
    {
        return *reinterpret_cast<volatile uint32_t*>(addr);
    }
}

int main(int, char**)
{
    // Positive control, and it must precede the trap: the identical volatile 32-bit load
    // aimed at memory this thread does hold must succeed. Without it, "the next load
    // faulted" could mean loads are broken rather than that the PPB is refused. A stack
    // local, so this image adds no .bss.
    uint32_t const own = 0xA5A5A5A5u;
    uintptr_t const own_addr = reinterpret_cast<uintptr_t>(&own);
    uint32_t const got = load32(own_addr);
    char msg[128];
    if (got != own)
    {
        emit("[ringppb] ERROR: the control load misread this thread's own stack\n");
        return 1;
    }
    emit("[ringppb] ok - control: a 32-bit volatile load of held memory succeeded\n");

    // The privileged half of the same access is witnessed by the fault report itself:
    // kickos_armv7m_fault_report reads CFSR/HFSR out of this very PPB page
    // (arch/arm/armv7m/arch_armv7m.cc) while privileged, so the dump that follows could
    // not be printed at all unless privileged PPB reads work.
    ksnprintf(msg, sizeof(msg),
              "[ringppb] root: reading privileged-only SCB->CPUID at 0x%x "
              "(expect BusFault)\n",
              static_cast<unsigned int>(SCB_CPUID));
    emit(msg);

    // The announce above must be ON THE WIRE before the fault, not merely queued. A chip
    // with a buffered console (stm32f302, xmc4800: arch_console_write is console_tx_write,
    // a ring the UART TX-empty ISR drains) has only pushed a few bytes by now, and
    // kpanic_enter masks IRQs before it flushes, so anything still in the ring is at the
    // mercy of the reporter completing. Without this wait a board that dies AT the read is
    // indistinguishable from one that dies in the reporter: both emit a few bytes and stop.
    kos_sleep_ns(100000000ull); // 100 ms: ~1150 byte-times at 115200, the ring is
                                // far smaller

    uint32_t const cpuid = load32(SCB_CPUID);

    // Reached only if the PPB did NOT refuse the access; "NOT confined" is the gate's
    // FAIL marker.
    ksnprintf(msg, sizeof(msg),
              "[ringppb] CPUID read completed: 0x%x - the ring is NOT confined\n",
              static_cast<unsigned int>(cpuid));
    emit(msg);
    return 0;
}
