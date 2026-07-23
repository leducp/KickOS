// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RX72M (RXv3) GPIO blink: the per-thread peripheral-MMIO isolation reference on
// the RX MPU. The RX twin of the F411 PMSA proof (f411spi) and the C6 PMP proof
// (c6blink). The RX MPU is CPU-side and checks EVERY access to the whole address
// space 0000_0000h-FFFF_FFFFh in user mode -- including the peripheral/SFR aperture
// (UM r01uh0804ej0120 sec.17.1 + Table 17.1); supervisor is never checked. So a
// granted MMIO window IS a genuine per-thread capability, unlike K64F where the
// SYSMPU cannot gate peripherals (k64drv proved that grant inert).
//
// A privileged bring-up shim muxes P80/LED6 as a GPIO output (PMR=GPIO, PDR=out --
// the escalation surfaces), then spawns the UNPRIVILEGED driver granted ONLY the
// 16 B Port Output Data Register block (PODR, 0008_C020h..0008_C02Fh; UM sec.22.3.2)
// -- the RX MPU 16-byte page, exact cover, encodable. The driver blinks LED6 by
// writing PORT8.PODR (0008_C028h, in-window), then pokes UNGRANTED PORT8.PDR
// (0008_C008h, the direction register -- an escalation surface outside the window)
// -> RX access exception (fixed vector +0x54) with MPESTS.DMPER set, MPDEA holding
// the faulting address -> the kernel names the task ("MPU FAULT: task 'rxdrv'").
//
// LED6 (P80, active-low, board UM r12uz0098ej0110 Table 5-9) is the CPU Card's only
// user LED; the console (SCI6, 115200 8N1) is the authoritative oracle either way.
//
// Diagnostic app (kickos_add_diagnostic_app): build-only, never a production image;
// the operator flashes a RAM image + observes LED6 and the console.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/libc/fmt.h>

#include <port_class.h> // Rule 6 class-driver leaf: shared PORT output-latch read

#include <stdint.h>

// This app EXISTS to prove RX MPU per-thread peripheral enforcement. Without it the
// ungranted poke below succeeds and the console prints the isolation-FAILURE line --
// a false verdict. Refuse to build a misleading oracle. (CMake gates it too.)
#if !KICKOS_HAVE_MPU
#error "rxdrv requires enforcement: configure with -DKICKOS_HAVE_MPU=1"
#endif

namespace
{
    // RX72M UM r01uh0804ej0120 sec.22 (I/O ports). Absolute SFR addresses, no vendor
    // SDK -- consistent with the chip backend's clean-room register map.
    // PODR block: one byte per port from PORT0 @0008_C020h (the block runs past PORTF
    // to PORTQ, sec.22.3.2); the granted window below covers only the first 16 bytes.
    // PDR  block: one byte per port, PORT0 @0008_C000h .. (sec.22.3.1).
    constexpr uintptr_t PODR_BLOCK = 0x0008C020u; // PORT0.PODR (window base)
    constexpr uintptr_t PORT8_PODR = 0x0008C028u; // LED6 output data
    constexpr uintptr_t PORT8_PDR = 0x0008C008u;  // LED6 direction (ungranted; escalation)
    constexpr uintptr_t PORT8_PMR = 0x0008C068u;  // LED6 mode (GPIO vs peripheral)
    constexpr uint8_t LED6 = 1u << 0;             // P80, active-low (board Table 5-9)

    // 16 B PODR window granted to the driver: base 0008_C020h (16-aligned), size 16
    // == the RX MPU page (UM sec.17.1.2, RSPAGEn/REPAGEn address[31:4]) -> exact
    // cover, encodable by arch_mpu_region_encodable. Covers PODR for PORT0..PORTF;
    // PORT8.PODR sits at window+0x08.
    constexpr uintptr_t PODR_WINDOW_BASE = PODR_BLOCK;
    constexpr uint32_t PODR_WINDOW = 16u;
    constexpr uint32_t PODR8_OFFSET = PORT8_PODR - PODR_BLOCK; // 0x08

    constexpr int DRIVER_BLINKS = 10;
    constexpr uint64_t HALF_PERIOD_NS = 250000000ull; // ~2 Hz blink

    inline volatile uint8_t& r8(uintptr_t a)
    {
        return *reinterpret_cast<volatile uint8_t*>(a);
    }

    // UNPRIVILEGED driver: granted app code+data (auto) + the 16 B PODR window (spawn
    // MMIO grant). No file-scope mutable state under enforcement: the window base
    // arrives as the thread arg VALUE (never dereferenced as memory), buffers live on
    // the granted stack. IRQ-less (GPIO blink); a kos_sleep_ns toggle loop.
    void blink_driver(void* arg)
    {
        uintptr_t const win = reinterpret_cast<uintptr_t>(arg); // PODR window base
        volatile uint8_t* podr8 = reinterpret_cast<volatile uint8_t*>(win + PODR8_OFFSET);

        kos::print("[rxdrv] blinking LED6 (P80) via the 16 B PODR window\n");

        for (int i = 0; i < DRIVER_BLINKS; i++)
        {
            *podr8 = static_cast<uint8_t>(*podr8 & ~LED6); // P80 low => LED on (active-low)
            kos_sleep_ns(HALF_PERIOD_NS);
            *podr8 = static_cast<uint8_t>(*podr8 | LED6); // P80 high => LED off
            kos_sleep_ns(HALF_PERIOD_NS);

            char s[48];
            ksnprintf(s, sizeof(s), "[rxdrv] blink %d\n", i + 1);
            kos::print(s);
        }

        // Negative test (the per-thread isolation proof): poke UNGRANTED PORT8.PDR
        // -- the pin-direction register, an escalation surface OUTSIDE the 16 B PODR
        // window. The RX MPU is CPU-side and checked on every user access, so this
        // operand write faults BEFORE the bus -> access exception (fixed vector +0x54),
        // MPESTS.DMPER set, MPDEA=0008_C008h -> kickos_rx_fault_report routes it (cause
        // 0x54 and PSW.PM set) to "MPU FAULT: task 'rxdrv'". Announce-before-poke;
        // terminal, so it is LAST.
        kos::print("[rxdrv] poking UNGRANTED PORT8.PDR @ 0x0008C008 (expect MPU FAULT)\n");
        r8(PORT8_PDR) = static_cast<uint8_t>(r8(PORT8_PDR) | LED6);

        // Only reached if the MPU did NOT enforce -- an isolation failure, not a pass.
        kos::print("[rxdrv] UNGRANTED ACCESS DID NOT FAULT (MPU not enforcing)\n");
        while (true)
        {
            kos_sleep_ns(1000000000ull);
        }
    }
}

int main(int, char**)
{
    // Privileged bring-up (this main runs privileged): the one-time unsafe setup the
    // unprivileged driver must NOT be able to do -- put P80 in GPIO mode, drive it
    // high (LED off), set it to output. PMR + PDR are the escalation surfaces and
    // stay OUT of the driver's window; keeping them out is what makes the window a
    // capability. (The kernel diag-LED init already owns P80, but configure it here
    // too so the app is self-contained.)
    r8(PORT8_PMR) = static_cast<uint8_t>(r8(PORT8_PMR) & ~LED6);  // GPIO (not peripheral)
    r8(PORT8_PODR) = static_cast<uint8_t>(r8(PORT8_PODR) | LED6); // high => LED off
    r8(PORT8_PDR) = static_cast<uint8_t>(r8(PORT8_PDR) | LED6);   // output

    // Read back PORT8's output latch through the shared class leaf (Rule 6):
    // confirms the pin drove high (LED off) before the unprivileged driver takes
    // over. PODR_BLOCK is the byte-per-port block base; index 8 == PORT8.
    uint8_t const podr8 = kickos::rx::classdrv::port_odr_read(PODR_BLOCK, 8);
    char rb[48];
    ksnprintf(rb, sizeof(rb), "[rxdrv] PORT8 PODR readback 0x%x\n", podr8);
    kos::print(rb);

    // Spawn the UNPRIVILEGED driver granted ONLY the 16 B PODR window. No IRQ.
    int drv = kos::thread::spawn(blink_driver,
                                 reinterpret_cast<void*>(PODR_WINDOW_BASE),
                                 "rxdrv", 10, KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                 /*mem=*/nullptr, /*mem_size=*/0,
                                 /*stack=*/nullptr, /*stack_size=*/0,
                                 /*mmio=*/reinterpret_cast<void*>(PODR_WINDOW_BASE),
                                 PODR_WINDOW);
    if (drv < 0)
    {
        // Console is the only oracle at the bench: a silent dead board must not be
        // mistaken for a bring-up failure, so say so.
        kos::print("[rxdrv] ERROR: driver spawn failed\n");
    }

    // Park: fall back to a sleep park if the semaphore could not be created (else a
    // -1 handle spins a hot loop of failing sem_wait syscalls).
    int idle = kos_sem_create(0);
    while (true)
    {
        if (idle < 0)
        {
            kos_sleep_ns(1000000000ull);
            continue;
        }
        kos_sem_wait(idle);
    }
}
