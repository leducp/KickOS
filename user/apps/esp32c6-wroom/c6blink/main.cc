// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-C6 GPIO10 blink: per-thread peripheral isolation on RISC-V PMP. A U-mode access to an HP
// peripheral passes PMP first, then the bus-side APM (TRM 16.1), and only PMP traps: an APM denial
// reads 0 or drops the write and raises a separate interrupt (TRM 16.5). The task's entry muxes
// GPIO10 through kos_pinmux_set, probes kos_periph_enable as the window's holder, drives and reads
// back the pad through the 64-byte pin bank its composition grants, then a child thread holding no
// window probes the same call and writes GPIO10's matrix out-sel, the escalation surface
// kos_pinmux_set owns, in the same block but outside the window: PMP kills the child and the task
// ends.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/errno.h>
#include <kickos/libc/fmt.h>

#include <gpio_class.h>

#include <stdint.h>
#include <stdlib.h>

// Without enforcement the ungranted write lands and prints an isolation-failure line.
#if !KICKOS_HAVE_MPU
#error "c6blink requires enforcement: build the board's base variant, not its flat one"
#endif

namespace
{
    // arch_pinmux_set `func` word, mirrored from arch/riscv/chip/esp32c6/regs/ (io_mux.h, gpio.h):
    // [15:0] the IO_MUX_GPIOn_REG word (Reg 7.20), [23] arm the matrix out-sel write, [31:24] the
    // out-sel signal index.
    constexpr uint32_t IO_MUX_MCU_SEL_GPIO = 1u << 12;
    constexpr uint32_t IO_MUX_FUN_DRV_2 = 2u << 10;
    constexpr uint32_t IO_MUX_FUN_IE = 1u << 9; // pad input path, for the GPIO_IN readback
    constexpr uint32_t PINMUX_MATRIX_EN = 1u << 23;
    constexpr uint32_t PINMUX_OUT_SEL_S = 24;
    constexpr uint32_t OUT_SEL_SIMPLE = 128u; // bit n of GPIO_OUT drives the pad (TRM 7.4.1)

    // GPIO10 is a non-strapping header pin (strapping 8/9/15, USB-JTAG 12/13, console 16/17).
    constexpr int BLINK_PIN = 10;
    constexpr uint32_t WINDOW_BYTES = 0x40u;
    constexpr uint32_t W1TS_OFFSET = 0x08u;
    constexpr uint32_t W1TC_OFFSET = 0x0Cu;
    constexpr uint32_t ENABLE_W1TS_OFFSET = 0x24u;
    constexpr uint32_t IN_OFFSET = 0x3Cu; // the pad, readable on a push-pull output
    // GPIO_FUNCn_OUT_SEL_CFG (Reg 7.13), past the pin bank in the same block.
    constexpr uintptr_t OUT_SEL_OFFSET = 0x554u + 4u * BLINK_PIN;

    constexpr int DRIVER_BLINKS = 10;
    constexpr uint64_t HALF_PERIOD_NS = 250000000ull;

    inline volatile uint32_t& r32(uintptr_t a)
    {
        return *reinterpret_cast<volatile uint32_t*>(a);
    }

    void verdict(char const* what, int rc, int want)
    {
        char const* v = "FAIL";
        if (rc == want)
        {
            v = "PASS";
        }
        char m[80];
        ksnprintf(m, sizeof(m), "[c6blink] %s %s rc %d (want %d)\n", v, what, rc, want);
        kos::print(m);
    }

    // Holds no window: the base arrives as a value, never dereferenced as memory.
    void poker(void* arg)
    {
        uintptr_t const win = reinterpret_cast<uintptr_t>(arg);
        // The refusal contract: the call never reaches the chip for a thread not holding the
        // window.
        verdict("periph_enable non-holder", kos_periph_enable(win), -KOS_EPERM);

        char m[80];
        ksnprintf(m, sizeof(m), "[c6blink] poking UNGRANTED out-sel @ 0x%lx (expect MPU FAULT)\n",
                  static_cast<unsigned long>(win + OUT_SEL_OFFSET));
        kos::print(m);
        r32(win + OUT_SEL_OFFSET) = OUT_SEL_SIMPLE;

        kos::print("[c6blink] UNGRANTED ACCESS DID NOT FAULT (PMP not enforcing)\n");
    }
}

extern "C" void c6blink_main(kos_self_t const* self)
{
    kos_window_t const window = kos_grant_mmio(self, "/dev/gpio");
    uintptr_t const win = reinterpret_cast<uintptr_t>(kos_window_addr(window));
    if (win == 0u or kos_window_size(window) < WINDOW_BYTES)
    {
        kos::print("[c6blink] ERROR: no /dev/gpio window\n");
        exit(1);
    }

    // Both mux stages in one mediated call: the IO_MUX pad on the matrix with a driver, and the
    // matrix out-sel at 128 so GPIO_OUT and GPIO_ENABLE drive the pad.
    int const mux = kos_pinmux_set(0, BLINK_PIN,
                                   IO_MUX_MCU_SEL_GPIO | IO_MUX_FUN_DRV_2 | IO_MUX_FUN_IE |
                                       PINMUX_MATRIX_EN | (OUT_SEL_SIMPLE << PINMUX_OUT_SEL_S));
    if (mux != 0)
    {
        char m[48];
        ksnprintf(m, sizeof(m), "[c6blink] ERROR: pinmux rc %d\n", mux);
        kos::print(m);
        exit(1);
    }

    // The holder reaches arch_periph_enable, whose fallback answers -KOS_ENOSYS.
    verdict("periph_enable holder", kos_periph_enable(win), -KOS_ENOSYS);

    uint32_t const bit = 1u << BLINK_PIN;
    r32(win + ENABLE_W1TS_OFFSET) = bit;
    uint32_t const out = kickos::esp32c6::driver::gpio_out_read(win);
    char rb[56];
    ksnprintf(rb, sizeof(rb), "[c6blink] GPIO_OUT readback 0x%lx\n", static_cast<unsigned long>(out));
    kos::print(rb);
    kos::print("[c6blink] blinking GPIO10 via the 64 B pin bank\n");

    // GPIO_IN is the pad, not the latch: on a bare header pin the only proof the pin moved.
    bool ok = true;
    for (int i = 0; i < DRIVER_BLINKS; i++)
    {
        r32(win + W1TS_OFFSET) = bit;
        kos_sleep_ns(HALF_PERIOD_NS);
        int const hi = static_cast<int>((r32(win + IN_OFFSET) >> BLINK_PIN) & 1u);
        r32(win + W1TC_OFFSET) = bit;
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

    auto const child = kos::thread::create(poker, reinterpret_cast<void*>(win), "c6poke", 10);
    if (not child.valid())
    {
        char m[48];
        ksnprintf(m, sizeof(m), "[c6blink] ERROR: poker spawn rc %d\n", child.error());
        kos::print(m);
        exit(1);
    }
    (void)child.join();
    exit(1);
}
