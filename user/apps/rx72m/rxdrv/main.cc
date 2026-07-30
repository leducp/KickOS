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
// main only prints and spawns (the fleet pattern -- see apps/common/gpioblink): the
// mux goes through kos_pinmux_set, which the kernel mediates on both the MPC PmnPFS
// function select and the PORTm.PMR peripheral-vs-GPIO switch, and EVERY port MMIO
// access happens inside the spawned UNPRIVILEGED driver holding an 80 B window. So
// this app runs unchanged with a privileged or an unprivileged root.
//
// The driver sets its own direction (PDR), blinks LED6 (PODR), reads the pad back
// (PIDR), then pokes UNGRANTED PORT8.PMR (0008_C068h) -- the mux escalation surface
// arch_pinmux_set now owns, OUTSIDE the window -> RX access exception (fixed vector
// +0x54) with MPESTS.DMPER set and MPDEA holding the address -> the kernel names the
// task ("MPU FAULT: task 'rxdrv'"). So the negative test proves the driver cannot
// re-mux its own pin behind pinmux's back.
//
// LED6 (P80, active-low, board UM r12uz0098ej0110 Table 5-9) is the CPU Card's only
// user LED; the console (SCI6, 115200 8N1) is the authoritative oracle either way.
//
// Diagnostic app (kickos_add_diagnostic_app): build-only, never a production image;
// the operator flashes a RAM image + observes LED6 and the console.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/errno.h>
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
    // SDK -- consistent with the chip backend's clean-room register map. Each block is
    // one byte per port, contiguous from PORT0: PDR @0008_C000h (sec.22.3.1), PODR
    // @0008_C020h (22.3.2), PIDR @0008_C040h (22.3.3), PMR @0008_C060h (22.3.4).
    constexpr uintptr_t PORT_BASE = 0x0008C000u;
    constexpr uintptr_t PORT8_PMR = 0x0008C068u; // LED6 mode (ungranted; escalation)
    constexpr uint32_t PORT8 = 8u;
    constexpr uint32_t P80 = 0u;
    constexpr uint8_t LED6 = 1u << P80; // P80, active-low (board Table 5-9)

    // arch_pinmux_set `func` word, mirrored from arch/rx/chip/rx72m/regs/mpc.h
    // (PINMUX_*); a cross-tree include from user/ does not resolve. [7:0] the PmnPFS
    // byte, [8] the PORTm.PMR bit value, [9] arm the PFS write.
    constexpr uint32_t PINMUX_PMR = 1u << 8;
    constexpr uint32_t PINMUX_PFS_EN = 1u << 9;
    constexpr uint32_t PFS_PSEL_HIZ = 0x00u; // PSEL=000000b, ISEL=ASEL=0: general I/O

    // Console pin the kernel owns for life: PB1/TXD6 (PORTB = port index 0x0B). The
    // refusal check below asks for PSEL=000000b with PMR=0, i.e. exactly the write
    // that would dark the console if the backend let it through.
    constexpr uint32_t PORTB = 0x0Bu;
    constexpr uint32_t PB1 = 1u;

    // 80 B window granted to the driver: base 0008_C000h (16-aligned), size 0x50
    // (16-multiple) -> exact cover, encodable by arch_mpu_region_encodable (the RX MPU
    // page is 16 B and needs no power-of-two size, UM sec.17.1.2). It spans PDR, PODR
    // and PIDR up to PORTF, ending at 0008_C04Fh -- 16 B, one full register row, short
    // of the PMR block at 0008_C060h, which is the point: direction and drive are in,
    // the FUNCTION SELECTORS (PMR here, the MPC
    // PFS file at 0008_C140h) stay out. Covering PDR/PODR for every port is an
    // unavoidable over-grant -- the RX interleaves ports inside each register block
    // instead of blocking per port -- but not an escalation: a pin at PMR=1 ignores
    // PDR/PODR entirely (UM Table 23.47), so the console pins cannot be touched
    // through this window.
    constexpr uintptr_t PORT_WINDOW_BASE = PORT_BASE;
    constexpr uint32_t PORT_WINDOW = 0x50u;
    constexpr uint32_t PDR_OFFSET = 0x00u;
    constexpr uint32_t PODR_OFFSET = 0x20u;
    constexpr uint32_t PIDR_OFFSET = 0x40u;

    constexpr int DRIVER_BLINKS = 10;
    constexpr uint64_t HALF_PERIOD_NS = 250000000ull; // ~2 Hz blink

    inline volatile uint8_t& r8(uintptr_t a)
    {
        return *reinterpret_cast<volatile uint8_t*>(a);
    }

    // UNPRIVILEGED driver: granted app code+data (auto) + the 80 B port window (spawn
    // MMIO grant). No file-scope mutable state under enforcement: the window base
    // arrives as the thread arg VALUE (never dereferenced as memory), buffers live on
    // the granted stack. IRQ-less (GPIO blink); a kos_sleep_ns toggle loop.
    void blink_driver(void* arg)
    {
        uintptr_t const win = reinterpret_cast<uintptr_t>(arg); // port block base
        uintptr_t const pdr = win + PDR_OFFSET + PORT8;
        uintptr_t const podr = win + PODR_OFFSET + PORT8;
        uintptr_t const pidr = win + PIDR_OFFSET + PORT8;

        // Possession probe, positive arm. This thread holds `win` as a live ARCH_MPU_DEV
        // region whose base is EXACTLY `win`, so caller_holds_mmio_block passes and the
        // call reaches arch_periph_enable. No rx72m backend defines that symbol, so the
        // weak default (kernel/time/clock_select.cc) answers -KOS_ENOSYS, which means the
        // kernel touched no register: the window state below is untouched either way.
        // First act, so the capture shows it ahead of any MMIO.
        int const pe = kos_periph_enable(win);
        int const pe_want = -KOS_ENOSYS;
        char const* pe_verdict = "FAIL";
        if (pe == pe_want)
        {
            pe_verdict = "PASS";
        }
        char pe_msg[64];
        ksnprintf(pe_msg, sizeof(pe_msg), "[rxdrv] %s periph_enable holder rc %d (want %d)\n",
                  pe_verdict, pe, pe_want);
        kos::print(pe_msg);

        // Direction, in-window: the pin is already muxed to general I/O, so this is the
        // driver owning a pin it was granted, not an escalation. Set before the first
        // drive, and drive high (LED off) first so the pin does not glitch on.
        r8(podr) = static_cast<uint8_t>(r8(podr) | LED6);
        r8(pdr) = static_cast<uint8_t>(r8(pdr) | LED6);

        // Output-latch baseline through the shared class leaf (Rule 6). Pure read, and
        // in-window: the PODR block is at win+0x20.
        uint8_t const odr = kickos::rx::driver::port_odr_read(win + PODR_OFFSET, PORT8);
        char rb[48];
        ksnprintf(rb, sizeof(rb), "[rxdrv] PORT8 PODR readback 0x%x\n", odr);
        kos::print(rb);
        kos::print("[rxdrv] blinking LED6 (P80) via the 80 B port window\n");

        // PIDR is the PAD, not the latch, and reads regardless of PDR/PMR (UM
        // sec.22.3.3) -- the console-visible oracle that the pin really moved, and the
        // only proof the mux landed if LED6 is not in the operator's line of sight.
        bool ok = true;
        for (int i = 0; i < DRIVER_BLINKS; i++)
        {
            r8(podr) = static_cast<uint8_t>(r8(podr) & ~LED6); // P80 low => LED on
            kos_sleep_ns(HALF_PERIOD_NS);
            int const lo = static_cast<int>((r8(pidr) >> P80) & 1u);
            r8(podr) = static_cast<uint8_t>(r8(podr) | LED6); // P80 high => LED off
            kos_sleep_ns(HALF_PERIOD_NS);
            int const hi = static_cast<int>((r8(pidr) >> P80) & 1u);

            char s[64];
            ksnprintf(s, sizeof(s), "[rxdrv] blink %d pad=0/%d pad=1/%d\n", i + 1, lo, hi);
            kos::print(s);
            if (lo != 0 or hi != 1)
            {
                ok = false;
            }
        }
        if (ok)
        {
            kos::print("[rxdrv] PASS (pad tracked the drive on every cycle)\n");
        }
        else
        {
            kos::print("[rxdrv] FAIL (pad did not track the drive)\n");
        }

        // Negative test (the per-thread isolation proof): poke UNGRANTED PORT8.PMR --
        // the pin-function switch, OUTSIDE the 80 B window. The RX MPU is CPU-side and
        // checked on every user access, so this operand write faults BEFORE the bus ->
        // access exception (fixed vector +0x54), MPESTS.DMPER set, MPDEA=0008_C068h ->
        // kickos_rx_fault_report routes it (cause 0x54 and PSW.PM set) to "MPU FAULT:
        // task 'rxdrv'". A plain store, not a read-modify-write: an RMW faults on its
        // READ half and the report would name a read rather than the escalation.
        // Announce-before-poke; terminal, so it is LAST.
        kos::print("[rxdrv] poking UNGRANTED PORT8.PMR @ 0x0008C068 (expect MPU FAULT)\n");
        r8(PORT8_PMR) = LED6;

        // Only reached if the MPU did NOT enforce -- an isolation failure, not a pass.
        kos::print("[rxdrv] UNGRANTED ACCESS DID NOT FAULT (MPU not enforcing)\n");
        while (true)
        {
            kos_sleep_ns(1000000000ull);
        }
    }
}

// Muxes its own port pins from root, then grants the PORT window to a worker. Never
// returns, so it needs no KOS_AUTH_SYSTEM.
KICKOS_APP_AUTHORITY(KOS_AUTH_MEMORY | KOS_AUTH_PINMUX);

int main(int, char**)
{
    // Possession probe from root. The call runs in both postures and what it exercises
    // follows the posture, not the other way round: unprivileged root holds no
    // ARCH_MPU_DEV region, so this is the REFUSAL contract (caller_holds_mmio_block
    // refuses, the chip backend is never consulted); privileged root BYPASSES the
    // possession gate and this instead shows that bypass reaching the backend. Both codes
    // prove the kernel wrote nothing, so it runs safely before the mux.
#if KICKOS_ROOT_PRIVILEGED
    int const pe_want = -KOS_ENOSYS;
#else
    int const pe_want = -KOS_EPERM;
#endif
    int const pe = kos_periph_enable(PORT_WINDOW_BASE);
    char const* pe_verdict = "FAIL";
    if (pe == pe_want)
    {
        pe_verdict = "PASS";
    }
    char pe_msg[64];
    ksnprintf(pe_msg, sizeof(pe_msg), "[rxdrv] %s periph_enable root rc %d (want %d)\n",
              pe_verdict, pe, pe_want);
    kos::print(pe_msg);

    // P80 to general I/O, both mux stages in one mediated call: PmnPFS PSEL=000000b
    // and PORT8.PMR bit 0 clear (UM sec.23.4.1 steps 1-6, PWPR unlock included). No
    // raw MMIO is left here, so this runs identically with root privileged or not.
    int const mux = kos_pinmux_set(PORT8, P80, PINMUX_PFS_EN | PFS_PSEL_HIZ);
    char m[64];
    ksnprintf(m, sizeof(m), "[rxdrv] pinmux P80 -> general I/O rc %d\n", mux);
    kos::print(m);

    // Mediation check: the same call aimed at the console's TXD6 pin must be REFUSED.
    // If the refusal ever broke, this very write is the one that darks the console --
    // so a silent run stopping right here is itself the failure signal.
    int const owned = kos_pinmux_set(PORTB, PB1, PINMUX_PFS_EN | PFS_PSEL_HIZ);
    if (owned == -KOS_EBUSY)
    {
        kos::print("[rxdrv] pinmux PB1/TXD6 refused (-KOS_EBUSY): console pin is kernel-owned\n");
    }
    else
    {
        ksnprintf(m, sizeof(m), "[rxdrv] ERROR: PB1/TXD6 not refused, rc %d\n", owned);
        kos::print(m);
    }

    // Spawn the UNPRIVILEGED driver granted ONLY the 80 B port window. No IRQ.
    int drv = kos::thread::spawn(blink_driver,
                                 reinterpret_cast<void*>(PORT_WINDOW_BASE),
                                 "rxdrv", 10, KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                 /*mem=*/nullptr, /*mem_size=*/0,
                                 /*stack=*/nullptr, /*stack_size=*/0,
                                 /*mmio=*/reinterpret_cast<void*>(PORT_WINDOW_BASE),
                                 PORT_WINDOW);
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
