// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RX72M (RXv3) GPIO blink: per-thread peripheral-MMIO isolation on the RX MPU. That MPU
// is CPU-side and checks EVERY user-mode access to the whole address space
// 0000_0000h-FFFF_FFFFh, including the peripheral/SFR aperture (UM r01uh0804ej0120
// sec.17.1 + Table 17.1); supervisor is never checked. So a granted MMIO window IS a
// genuine per-thread capability. This app's own window grant is LOAD-BEARING, because it
// authorises the kos_periph_enable in the spawned driver.
//
// main only prints and spawns: the mux goes through kos_pinmux_set, which the kernel
// mediates on both the MPC PmnPFS function select and the PORTm.PMR peripheral-vs-GPIO
// switch, and EVERY port MMIO access happens inside the spawned UNPRIVILEGED driver
// holding an 80 B window. So this app runs unchanged with a privileged or an
// unprivileged root.
//
// The driver sets its own direction (PDR), blinks LED6 (PODR), reads the pad back
// (PIDR), then pokes UNGRANTED PORT8.PMR (0008_C068h), the mux escalation surface
// arch_pinmux_set owns, OUTSIDE the window -> RX access exception (fixed vector +0x54)
// with MPESTS.DMPER set and MPDEA holding the address -> rxv3 opted into fault
// isolation, so the thread is KILLED ("=== THREAD FAULT === thread 'rxdrv' killed") and
// the system continues. The negative test therefore proves the driver cannot re-mux its
// own pin behind pinmux's back.
//
// LED6 (P80, active-low, board UM r12uz0098ej0110 Table 5-9) is the CPU Card's only user
// LED; the console (SCI6, 115200 8N1) is the authoritative oracle.
//
// Diagnostic app (kickos_add_diagnostic_app): the operator flashes a RAM image and
// observes LED6 and the console.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/errno.h>
#include <kickos/libc/fmt.h>

#include <port_class.h> // Rule 6 class-driver leaf: shared PORT output-latch read

#include <stdint.h>

// Anti-vacuity: without enforcement the ungranted poke below succeeds and the console
// prints the isolation-FAILURE line, which is a false verdict.
#if !KICKOS_HAVE_MPU
#error "rxdrv requires enforcement: build the board's base variant, not its flat one"
#endif

namespace
{
    // RX72M UM r01uh0804ej0120 sec.22 (I/O ports). Absolute SFR addresses, no vendor
    // SDK, consistent with the chip backend's clean-room register map. Each block is
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
    // page is 16 B and takes any 16-multiple size, UM sec.17.1.2). It spans PDR, PODR and
    // PIDR through PORTF, ending at 0008_C04Fh, one full register row short of the PMR
    // block at 0008_C060h: direction and drive are in, the FUNCTION SELECTORS (PMR here,
    // the MPC PFS file at 0008_C140h) stay out. Covering PDR/PODR for every port is an
    // over-grant the RX register layout forces (ports are interleaved inside each block
    // rather than blocked per port), and not an escalation: a pin at PMR=1 ignores
    // PDR/PODR entirely (UM Table 23.47), so the console pins cannot be touched through
    // this window.
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
    // the granted stack.
    void blink_driver(void* arg)
    {
        uintptr_t const win = reinterpret_cast<uintptr_t>(arg); // port block base
        uintptr_t const pdr = win + PDR_OFFSET + PORT8;
        uintptr_t const podr = win + PODR_OFFSET + PORT8;
        uintptr_t const pidr = win + PIDR_OFFSET + PORT8;

        // Possession probe, positive arm. This thread holds `win` as a live ARCH_MPU_DEV
        // region whose base is EXACTLY `win`, so caller_holds_mmio_block passes and the
        // call reaches arch_periph_enable. chip_rx72m answers for RIIC0/1/2 only and
        // refuses this port block with -KOS_EINVAL, so no register is touched. First act,
        // so the capture shows it ahead of any MMIO.
        int const pe = kos_periph_enable(win);
        int const pe_want = -KOS_EINVAL;
        char const* pe_verdict = "FAIL";
        if (pe == pe_want)
        {
            pe_verdict = "PASS";
        }
        char pe_msg[64];
        ksnprintf(pe_msg, sizeof(pe_msg), "[rxdrv] %s periph_enable holder rc %d (want %d)\n",
                  pe_verdict, pe, pe_want);
        kos::print(pe_msg);

        // Direction, in-window; the pin is already muxed to general I/O. Set before the
        // first drive, and drive high (LED off) first so the pin does not glitch on.
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
        // sec.22.3.3): the console-visible oracle that the pin really moved, and the
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

        // Negative test (the per-thread isolation proof): poke UNGRANTED PORT8.PMR, the
        // pin-function switch, OUTSIDE the 80 B window. The RX MPU is CPU-side and checked
        // on every user access, so this operand write faults BEFORE the bus -> access
        // exception (fixed vector +0x54), MPESTS.DMPER set, MPDEA=0008_C068h. Since rxv3
        // opted into fault isolation this KILLS the thread ("=== THREAD FAULT === thread
        // 'rxdrv' killed"); the address is ABOVE this thread's stack base, so it takes the
        // kill path and not the overflow escalation in kickos_isr_fault. A plain store, so
        // that the report names the escalating write: an RMW would fault on its READ half
        // and be reported as a read. Announce-before-poke; terminal, so it is LAST.
        kos::print("[rxdrv] poking UNGRANTED PORT8.PMR @ 0x0008C068 (expect MPU FAULT)\n");
        r8(PORT8_PMR) = LED6;

        // Only reached if the MPU did NOT enforce: an isolation failure, not a pass.
        kos::print("[rxdrv] UNGRANTED ACCESS DID NOT FAULT (MPU not enforcing)\n");
        while (true)
        {
            kos_sleep_ns(1000000000ull);
        }
    }
}

// Root muxes its own port pins, then grants the PORT window to a worker. main never
// returns.
KICKOS_APP_AUTHORITY(KOS_AUTH_MEMORY | KOS_AUTH_PINMUX);

int main(int, char**)
{
    // Possession probe from root: root holds no ARCH_MPU_DEV region, so this is the
    // REFUSAL contract (caller_holds_mmio_block refuses, the chip backend is never
    // consulted). The kernel wrote nothing, so it runs safely before the mux.
    int const pe_want = -KOS_EPERM;
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

    // P80 to general I/O, both mux stages in one mediated call: PmnPFS PSEL=000000b and
    // PORT8.PMR bit 0 clear (UM sec.23.4.1 steps 1-6, PWPR unlock included). No raw MMIO
    // here, so this runs identically with root privileged or not.
    int const mux = kos_pinmux_set(PORT8, P80, PINMUX_PFS_EN | PFS_PSEL_HIZ);
    char m[64];
    ksnprintf(m, sizeof(m), "[rxdrv] pinmux P80 -> general I/O rc %d\n", mux);
    kos::print(m);

    // Mediation check: the same call aimed at the console's TXD6 pin must be REFUSED.
    // If the refusal ever broke, this very write is the one that darks the console,
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

    // The driver ends on the negative test's fault, and a fault cancels the faulting
    // thread's whole TASK: spawned plain it would join root's task and take root with it,
    // leaving no survivor to keep the board up. Root holds the handle for the life of the
    // image, since it never reaches a point past the driver.
    kos_task_t victim = KOS_TASK_NONE;
    if (kos_task_create(nullptr, 0, 0, &victim) != 0)
    {
        // The console is the only oracle at the bench: without this line a failed spawn
        // and a dead board read the same.
        kos::print("[rxdrv] ERROR: no task slot for the driver\n");
    }
    else
    {
        auto drv = kos::thread::create(blink_driver,
                                       reinterpret_cast<void*>(PORT_WINDOW_BASE),
                                       "rxdrv", 10, KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                       /*mem=*/nullptr, /*mem_size=*/0,
                                       /*stack=*/nullptr, /*stack_size=*/0,
                                       /*mmio=*/reinterpret_cast<void*>(PORT_WINDOW_BASE),
                                       PORT_WINDOW,
                                       /*caps=*/nullptr, /*cap_count=*/0,
                                       /*authority=*/0, /*cap_dest=*/nullptr, victim);
        if (not drv.valid())
        {
            kos::print("[rxdrv] ERROR: driver spawn failed\n");
        }
    }

    // Sleep park when the semaphore could not be created: an unmintable handle would
    // spin a hot loop of failing sem_wait syscalls.
    kos_cap_t idle = KOS_CAP_NONE;
    (void)kos_sem_create(0, &idle);
    while (true)
    {
        if (idle == KOS_CAP_NONE)
        {
            kos_sleep_ns(1000000000ull);
            continue;
        }
        kos_sem_wait(idle);
    }
}
