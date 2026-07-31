// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-C6 GPIO10 blink: the CANONICAL per-thread peripheral-MMIO isolation
// reference on RISC-V PMP. The C6 twin of the F411 PMSA proof (f411spi), plus the
// one extra gate the C6 forces: on this core a U-mode (REE) access to an HP
// peripheral passes TWO permission units in series (TRM 16.1): PMP (CPU-side,
// per-hart, checked FIRST) then APM (bus-side, per security mode, checked only if
// PMP passes). The chip layer opens APM for REE0 once at boot (arch_init), so the
// background permit is already in place here; PMP draws the genuine per-thread line
// ON TOP of it, which is exactly what K64F (coarse AIPS-only) structurally cannot do.
//
// main only prints and spawns (the fleet pattern, see apps/common/gpioblink): the
// mux goes through kos_pinmux_set, which the kernel mediates on both the IO_MUX pad
// and the GPIO matrix out-sel, and EVERY GPIO MMIO write happens inside the spawned
// UNPRIVILEGED driver holding the pin bank as a 64 B PMP window. So this app runs
// unchanged with a privileged or an unprivileged root.
//
// The driver sets its own direction, blinks, then pokes UNGRANTED
// GPIO_FUNC10_OUT_SEL_CFG (0x6009_157C): same GPIO block, APM-permitted, but
// OUTSIDE the 64 B PMP window -> PMP store fault (mcause=7) -> the kernel names the
// task ("MPU FAULT: task 'c6blink'"). That register is the matrix escalation surface
// arch_pinmux_set now owns, so the negative test proves the driver cannot re-route
// its pad behind pinmux's back. The isolation proof rides the PMP fault, NOT APM: an
// APM denial does NOT trap (TRM 16.5: read returns 0 / write dropped + a separate
// HP_APM interrupt), so only PMP gives the load/store fault.
//
// Diagnostic app (kickos_add_diagnostic_app): build-only, never a production image;
// the operator flashes a RAM-only image + observes GPIO10 and the console.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/errno.h>
#include <kickos/libc/fmt.h>

#include <gpio_class.h> // Rule 6 class-driver leaf: shared GPIO output-latch read

#include <stdint.h>

// This app EXISTS to prove PMP per-thread peripheral enforcement. Without it the
// ungranted poke below succeeds and the console prints the isolation-FAILURE line:
// a false verdict. Refuse to build a misleading oracle. (CMake gates it too.)
#if !KICKOS_HAVE_MPU
#error "c6blink requires enforcement: configure with -DKICKOS_HAVE_MPU=1"
#endif

namespace
{
    // ESP32-C6 TRM v1.2. Absolute addresses, no ESP-IDF/HAL.
    // GPIO Matrix block (mem map Table 5.3-2): 0x6009_1000 .. 0x6009_1FFF.
    constexpr uintptr_t GPIO_BASE = 0x60091000u;
    constexpr uintptr_t GPIO_FUNC_OUT_SEL_CFG = GPIO_BASE + 0x554u; // Reg base, +4*n

    // arch_pinmux_set `func` word, mirrored from arch/riscv/chip/esp32c6/regs/
    // (io_mux.h + gpio.h PINMUX_*); a cross-tree include from user/ does not resolve.
    // [15:0] the IO_MUX_GPIOn_REG word (Reg 7.20), [23] arm the matrix out-sel write,
    // [31:24] the out-sel signal index.
    constexpr uint32_t IO_MUX_MCU_SEL_GPIO = 1u << 12; // MCU_SEL=1 -> GPIO matrix func
    constexpr uint32_t IO_MUX_FUN_DRV_2 = 2u << 10;    // ~20 mA drive
    constexpr uint32_t IO_MUX_FUN_IE = 1u << 9;        // pad input path (GPIO_IN readback)
    constexpr uint32_t PINMUX_MATRIX_EN = 1u << 23;
    constexpr uint32_t PINMUX_OUT_SEL_S = 24;
    constexpr uint32_t OUT_SEL_SIMPLE = 128u; // bit n of GPIO_OUT drives the pad (TRM 7.4.1)

    // Driver pin + granted window. GPIO10 (net IO10) is a non-strapping header pin
    // (strapping: 8/9/15; USB-JTAG: 12/13; console UART: 16/17). 64 B PMP NAPOT at the
    // block base: pow2, base 64-aligned -> encodable as one entry
    // (arch_mpu_region_encodable). RW-NX; PMP has no device-memory type. The window is
    // the pin BANK: output latch (OUT/W1TS/W1TC), direction (ENABLE/ENABLE_W1TS),
    // input and strap, and nothing else. The matrix out-sel (+0x554) and the per-pin
    // config/interrupt registers (+0x74..) stay outside, which is what makes it a
    // capability rather than the whole block.
    constexpr int BLINK_PIN = 10;
    constexpr uintptr_t GPIO_MMIO_WINDOW_BASE = GPIO_BASE;
    constexpr uint32_t GPIO_MMIO_WINDOW = 64u;
    constexpr uint32_t W1TS_OFFSET = 0x08u;
    constexpr uint32_t W1TC_OFFSET = 0x0Cu;
    constexpr uint32_t ENABLE_W1TS_OFFSET = 0x24u;
    constexpr uint32_t IN_OFFSET = 0x3Cu; // Reg 7.8: PAD input, readable on a PP output

    constexpr int DRIVER_BLINKS = 10;
    constexpr uint64_t HALF_PERIOD_NS = 250000000ull; // ~2 Hz blink

    inline volatile uint32_t& r32(uintptr_t a)
    {
        return *reinterpret_cast<volatile uint32_t*>(a);
    }

    // UNPRIVILEGED driver: granted app code+data (auto) + the 64 B GPIO window (spawn
    // MMIO grant). No file-scope mutable state under enforcement: the window base
    // arrives as the thread arg VALUE (never dereferenced as memory), buffers live on
    // the granted stack. IRQ-less (GPIO blink); a kos_sleep_ns toggle loop, not an IRQ.
    void blink_driver(void* arg)
    {
        uintptr_t const win = reinterpret_cast<uintptr_t>(arg); // GPIO block base
        volatile uint32_t* w1ts = reinterpret_cast<volatile uint32_t*>(win + W1TS_OFFSET);
        volatile uint32_t* w1tc = reinterpret_cast<volatile uint32_t*>(win + W1TC_OFFSET);
        uint32_t const bit = 1u << BLINK_PIN;

        // Possession probe, positive arm. This thread holds `win` as a live ARCH_MPU_DEV
        // region whose base is EXACTLY `win`, so caller_holds_mmio_block passes and the
        // call reaches arch_periph_enable. No esp32c6 backend defines that symbol, so the
        // fallback TU (arch/common) answers -KOS_ENOSYS, which means the
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
        ksnprintf(pe_msg, sizeof(pe_msg), "[c6blink] %s periph_enable holder rc %d (want %d)\n",
                  pe_verdict, pe, pe_want);
        kos::print(pe_msg);

        // Direction, in-window: the pad is already muxed to the matrix and the matrix to
        // the output latch, so this is the driver owning a pin it was granted, not an
        // escalation. Set before the first drive.
        r32(win + ENABLE_W1TS_OFFSET) = bit;

        // Output-latch baseline through the shared class leaf (Rule 6). Pure read, and
        // in-window: GPIO_OUT is at +0x04.
        uint32_t const out = kickos::esp32c6::driver::gpio_out_read(win);
        char rb[56];
        ksnprintf(rb, sizeof(rb), "[c6blink] GPIO_OUT readback 0x%lx\n",
                  static_cast<unsigned long>(out));
        kos::print(rb);
        kos::print("[c6blink] blinking GPIO10 via the 64 B PMP window\n");

        // GPIO_IN is the PAD, not the latch: it is the only console-visible oracle that
        // the pin really moved (GPIO10 is a bare header pin, no LED).
        bool ok = true;
        for (int i = 0; i < DRIVER_BLINKS; i++)
        {
            *w1ts = bit; // drive high (in-window; atomic set, no RMW of the shared latch)
            kos_sleep_ns(HALF_PERIOD_NS);
            int const hi = static_cast<int>((r32(win + IN_OFFSET) >> BLINK_PIN) & 1u);
            *w1tc = bit; // drive low (in-window; atomic clear)
            kos_sleep_ns(HALF_PERIOD_NS);
            int const lo = static_cast<int>((r32(win + IN_OFFSET) >> BLINK_PIN) & 1u);

            char s[64];
            ksnprintf(s, sizeof(s), "[c6blink] blink %d pad=1/%d pad=0/%d\n", i + 1, hi, lo);
            kos::print(s);
            if (hi != 1 or lo != 0)
            {
                ok = false;
            }
        }
        if (ok)
        {
            kos::print("[c6blink] PASS (pad tracked the drive on every cycle)\n");
        }
        else
        {
            kos::print("[c6blink] FAIL (pad did not track the drive)\n");
        }

        // Negative test (the per-thread isolation proof): poke UNGRANTED
        // GPIO_FUNC10_OUT_SEL_CFG: same GPIO block, APM-permitted for REE0, but
        // OUTSIDE the 64 B window. PMP is checked FIRST and is fail-closed -> store
        // access fault, mcause=7, mtval=0x6009_157C -> kickos_rv_fault_report routes it
        // (from_user and mcause 7) to "MPU FAULT: task 'c6blink'". Announce-before-poke;
        // terminal, so it is LAST. An APM denial would NOT trap (TRM 16.5).
        kos::print("[c6blink] poking UNGRANTED out-sel @ 0x6009157c (expect MPU FAULT)\n");
        r32(GPIO_FUNC_OUT_SEL_CFG + 0x4u * BLINK_PIN) = OUT_SEL_SIMPLE;

        // Only reached if PMP did NOT enforce: an isolation failure, not a pass.
        kos::print("[c6blink] UNGRANTED ACCESS DID NOT FAULT (PMP not enforcing)\n");
        while (true)
        {
            kos_sleep_ns(1000000000ull);
        }
    }
}

// Muxes its own LED pin from root, then grants the GPIO window to a worker. Never
// returns, so it needs no KOS_AUTH_SYSTEM.
KICKOS_APP_AUTHORITY(KOS_AUTH_MEMORY | KOS_AUTH_PINMUX);

int main(int, char**)
{
    // Possession probe from root: root holds no ARCH_MPU_DEV region, so this is the
    // REFUSAL contract (caller_holds_mmio_block refuses, the chip backend is never
    // consulted). The kernel wrote nothing, so it runs safely before the mux.
    int const pe_want = -KOS_EPERM;
    int const pe = kos_periph_enable(GPIO_MMIO_WINDOW_BASE);
    char const* pe_verdict = "FAIL";
    if (pe == pe_want)
    {
        pe_verdict = "PASS";
    }
    char pe_msg[64];
    ksnprintf(pe_msg, sizeof(pe_msg), "[c6blink] %s periph_enable root rc %d (want %d)\n",
              pe_verdict, pe, pe_want);
    kos::print(pe_msg);

    // GPIO10 push-pull output, both mux stages in one mediated call: IO_MUX pad on the
    // GPIO matrix function with a driver, and the matrix out-sel = 128 so bit n of
    // GPIO_OUT/GPIO_ENABLE drives the pad (TRM 7.4.1 "simple GPIO output"). The kernel
    // refuses a kernel-owned pin on BOTH stages, which is why no raw MMIO is left here.
    // The ROM leaves the GPIO Matrix clocked (its own boot log muxes pads), so no PCR
    // gating is needed. PCR (0x6009_6000) is a reserved block anyway.
    int const mux = kos_pinmux_set(0, BLINK_PIN,
                                   IO_MUX_MCU_SEL_GPIO | IO_MUX_FUN_DRV_2 | IO_MUX_FUN_IE |
                                       PINMUX_MATRIX_EN | (OUT_SEL_SIMPLE << PINMUX_OUT_SEL_S));
    if (mux != 0)
    {
        kos::print("[c6blink] ERROR: pinmux_set failed\n");
    }

    // Spawn the UNPRIVILEGED driver granted ONLY the 64 B pin-bank window. No IRQ.
    int drv = kos::thread::spawn(blink_driver,
                                 reinterpret_cast<void*>(GPIO_MMIO_WINDOW_BASE),
                                 "c6blink", 10, KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                 /*mem=*/nullptr, /*mem_size=*/0,
                                 /*stack=*/nullptr, /*stack_size=*/0,
                                 /*mmio=*/reinterpret_cast<void*>(GPIO_MMIO_WINDOW_BASE),
                                 GPIO_MMIO_WINDOW);
    if (drv < 0)
    {
        // Console is the only oracle at the bench: a silent dead board must not be
        // mistaken for a bring-up failure, so say so.
        kos::print("[c6blink] ERROR: driver spawn failed\n");
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
