// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// STM32F411 SPI1 loopback through a PA7-to-PA6 jumper, run by the task's entry over the SPI1
// window and line its composition grants. It muxes its own pins, clocks SPI1 through
// kos_periph_enable, echoes four words, then reads GPIOB, which nothing grants, and must fault.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/libc/fmt.h>

#include <stdint.h>
#include <stdlib.h>

// Without enforcement the ungranted read lands and prints an isolation-failure line.
#if !KICKOS_HAVE_MPU
#error "f411spi requires enforcement: build the board's base variant, not its flat one"
#endif

namespace
{
    // kos_pinmux_set port index on stm32f411: (port base - GPIOA_BASE) / 0x400.
    constexpr uint32_t PORT_A = 0u;
    constexpr uint32_t PORT_E = 4u;
    constexpr uint32_t PIN_SCK = 5u;
    constexpr uint32_t PIN_MISO = 6u;
    constexpr uint32_t PIN_MOSI = 7u;

    // The on-board gyro's chip select (UM1842). Its SDO drives PA6, so it must be held
    // deselected or it fights the jumper.
    constexpr uint32_t PIN_GYRO_CS = 3u;

    // stm32f411 func encoding: bits[1:0] MODER, bits[7:4] the AF number, bit 8 presets an output
    // high.
    constexpr uint32_t MUX_OUTPUT = 0x01u;
    constexpr uint32_t MUX_OUT_HIGH = 0x100u;
    constexpr uint32_t MUX_AF5 = 0x52u; // AF5 = SPI1

    constexpr uint32_t WINDOW_BYTES = 0x10u;
    constexpr uint32_t CR1_OFFSET = 0x00u;
    constexpr uint32_t CR2_OFFSET = 0x04u;
    constexpr uint32_t SR_OFFSET = 0x08u;
    constexpr uint32_t DR_OFFSET = 0x0Cu;

    // SPI_CR1 (RM0383 20.5.1): mode 0, 8-bit, MSB first.
    constexpr uint32_t CR1_MSTR = 1u << 2;
    constexpr uint32_t CR1_SPE = 1u << 6;
    constexpr uint32_t CR1_SSI = 1u << 8;
    constexpr uint32_t CR1_SSM = 1u << 9;
    constexpr uint32_t CR1_BR_DIV64 = 0x5u << 3; // 84 MHz APB2 / 64 ~= 1.3 MHz
    constexpr uint32_t CR2_RXNEIE = 1u << 6;
    constexpr uint32_t SR_TXE = 1u << 1;

    // GPIOB (RM0383 memory map): kernel-owned, so outside every window a task holds.
    constexpr uintptr_t GPIOB_BASE = 0x40020400u;

    constexpr uint32_t POLL_TIMEOUT = 1000000u;

    [[noreturn]] void give_up(char const* what, int rc)
    {
        char m[80];
        ksnprintf(m, sizeof(m), "[f411spi] ERROR: %s rc %d\n", what, rc);
        kos::print(m);
        exit(1);
    }

    void mux_pin(char const* what, uint32_t port, uint32_t pin, uint32_t func)
    {
        int const rc = kos_pinmux_set(port, pin, func);
        if (rc != 0)
        {
            give_up(what, rc);
        }
    }
}

extern "C" void f411spi_main(kos_self_t const* self)
{
    // Deselect the gyro before any SCK activity: one call gates GPIOE's clock, presets PE3 high,
    // then switches it to output, so it never drives low.
    mux_pin("pinmux PE3", PORT_E, PIN_GYRO_CS, MUX_OUTPUT | MUX_OUT_HIGH);
    // Glitch-free ahead of CR1 only because CPOL=0 is CR1's reset value; a CPOL=1 variant must
    // write CR1 first.
    mux_pin("pinmux PA5/SCK", PORT_A, PIN_SCK, MUX_AF5);
    mux_pin("pinmux PA6/MISO", PORT_A, PIN_MISO, MUX_AF5);
    mux_pin("pinmux PA7/MOSI", PORT_A, PIN_MOSI, MUX_AF5);

    kos_window_t const window = kos_grant_mmio(self, "/dev/spi1");
    kos_line_t const line = kos_grant_irq(self, "irq");
    uintptr_t const win = reinterpret_cast<uintptr_t>(kos_window_addr(window));
    if (win == 0u or kos_window_size(window) < WINDOW_BYTES or line.cap == KOS_CAP_NONE)
    {
        give_up("no /dev/spi1 window or irq line", -KOS_EBADF);
    }

    // Every register write below is discarded while SPI1 is clock-gated.
    int rc = kos_periph_enable(win);
    if (rc != 0)
    {
        give_up("periph_enable(SPI1)", rc);
    }

    kos_cap_t note = KOS_CAP_NONE;
    rc = kos_notify_create(&note);
    if (rc == 0)
    {
        rc = kos_notify_bind(note);
    }
    if (rc == 0)
    {
        rc = kos_irq_bind_notify(line.cap, note);
    }
    if (rc != 0)
    {
        give_up("the line's notification", rc);
    }

    volatile uint32_t* const cr1 = reinterpret_cast<volatile uint32_t*>(win + CR1_OFFSET);
    volatile uint32_t* const cr2 = reinterpret_cast<volatile uint32_t*>(win + CR2_OFFSET);
    volatile uint32_t* const sr = reinterpret_cast<volatile uint32_t*>(win + SR_OFFSET);
    volatile uint32_t* const dr = reinterpret_cast<volatile uint32_t*>(win + DR_OFFSET);

    // SSM|SSI hold internal NSS high, or the master takes a MODF.
    *cr1 = CR1_MSTR | CR1_SSM | CR1_SSI | CR1_BR_DIV64;
    *cr2 = CR2_RXNEIE;
    *cr1 |= CR1_SPE;

    // Before the first wait: a misrouted line hangs there, and this line tells that apart from
    // a dead board.
    kos::print("[f411spi] starting loopback (blocking on the SPI1 line)\n");

    uint8_t const pattern[] = {0xA5u, 0x3Cu, 0x00u, 0xFFu};
    int fails = 0;
    for (unsigned i = 0; i < sizeof(pattern); i++)
    {
        uint32_t const tx = pattern[i];
        uint32_t spin = 0;
        while ((*sr & SR_TXE) == 0u and spin < POLL_TIMEOUT)
        {
            spin++;
        }
        if (spin == POLL_TIMEOUT)
        {
            char t[48];
            ksnprintf(t, sizeof(t), "[f411spi] TXE timeout on word %u\n", i);
            kos::print(t);
        }
        *dr = tx;

        (void)kos_notify_wait(note, 1u, KOS_TIMEOUT_NONE, nullptr);
        // The only way to clear RXNE: without it the level storms once the next wait rearms the
        // edge-claimed line.
        uint32_t const rx = *dr & 0xFFu;

        char s[64];
        char const* verdict = "PASS";
        if (rx != tx)
        {
            verdict = "FAIL";
            fails++;
        }
        ksnprintf(s, sizeof(s), "[f411spi] word %u: tx=0x%x rx=0x%x %s\n", i,
                  static_cast<unsigned>(tx), static_cast<unsigned>(rx), verdict);
        kos::print(s);
    }
    if (fails == 0)
    {
        kos::print("[f411spi] loopback PASS (all words echoed equal)\n");
    }
    else
    {
        kos::print("[f411spi] loopback FAIL (word mismatch)\n");
    }

    // Terminal: the announce must precede the read, or the console shows only the fault.
    kos::print("[f411spi] poking UNGRANTED GPIOB @ 0x40020400 (expect MPU FAULT)\n");
    uint32_t const leaked = *reinterpret_cast<volatile uint32_t*>(GPIOB_BASE);

    char s[72];
    ksnprintf(s, sizeof(s), "[f411spi] UNGRANTED ACCESS DID NOT FAULT (GPIOB=0x%x)\n",
              static_cast<unsigned>(leaked));
    kos::print(s);
    exit(1);
}
