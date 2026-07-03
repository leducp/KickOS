// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// KickOS "blink": the bare-metal bring-up smoke test. A spawned thread toggles an
// onboard LED and sleeps between edges, so a steady ~2.5 Hz blink is end-to-end
// proof of the whole path on real silicon: reset -> C runtime -> scheduler start
// -> thread spawn + context switch (main blocks, the blinker runs) -> SysTick
// one-shot sleep -> wake. It needs no UART adapter -- just eyes on the LED.
//
// The LED wiring is board-specific (direct MMIO -- allowed unprivileged on M1,
// no MPU). The chip's arch_init has already enabled the buses/clocks these ports
// need. The board is selected by a -DKICKOS_BLINK_<board> from the CMake target.

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <stdint.h>

namespace
{
    constexpr uint64_t kBlinkNs = 200000000ull; // 0.2 s per edge -> ~2.5 Hz

    inline volatile uint32_t& r32(uintptr_t a)
    {
        return *reinterpret_cast<volatile uint32_t*>(a);
    }

#if defined(KICKOS_BLINK_PICOPI)
    // Raspberry Pi Pico: GP25 via SIO (NOT the Pico W, whose LED is on the CYW43).
    constexpr uintptr_t SIO_BASE = 0xd0000000;
    constexpr uintptr_t SIO_GPIO_OUT_XOR = SIO_BASE + 0x01c;
    constexpr uintptr_t SIO_GPIO_OE_SET = SIO_BASE + 0x024;
    constexpr uintptr_t IO_GPIO25_CTRL = 0x40014000 + 0x0cc;
    constexpr uintptr_t PADS_GPIO25_CLR = 0x4001c000 + 0x3000 + 0x68;
    constexpr uint32_t LED = 1u << 25;
    void led_init()
    {
        r32(IO_GPIO25_CTRL) = 5;        // funcsel = SIO
        r32(PADS_GPIO25_CLR) = 1u << 7; // clear output-disable
        r32(SIO_GPIO_OE_SET) = LED;     // output enable
    }
    void led_toggle() { r32(SIO_GPIO_OUT_XOR) = LED; }

#elif defined(KICKOS_BLINK_F411DISCO)
    // STM32F411E-DISCO: PD12 (green LED). GPIOD MODER output; toggle via ODR.
    constexpr uintptr_t RCC_AHB1ENR = 0x40023800 + 0x30;
    constexpr uintptr_t GPIOD_MODER = 0x40020c00 + 0x00;
    constexpr uintptr_t GPIOD_ODR = 0x40020c00 + 0x14;
    constexpr uint32_t LED = 1u << 12;
    void led_init()
    {
        r32(RCC_AHB1ENR) |= (1u << 3); // GPIODEN
        uint32_t m = r32(GPIOD_MODER);
        m &= ~(0x3u << 24);            // clear MODER12
        m |= (0x1u << 24);             // general-purpose output
        r32(GPIOD_MODER) = m;
    }
    void led_toggle() { r32(GPIOD_ODR) ^= LED; }

#elif defined(KICKOS_BLINK_BLUEPILL)
    // Blue Pill (STM32F103C8): PC13 (active-low). GPIOC CRH output; toggle via ODR.
    constexpr uintptr_t RCC_APB2ENR = 0x40021000 + 0x18;
    constexpr uintptr_t GPIOC_CRH = 0x40011000 + 0x04;
    constexpr uintptr_t GPIOC_ODR = 0x40011000 + 0x0c;
    constexpr uint32_t LED = 1u << 13;
    void led_init()
    {
        r32(RCC_APB2ENR) |= (1u << 4); // IOPCEN (GPIOC)
        uint32_t crh = r32(GPIOC_CRH);
        crh &= ~(0xFu << 20);          // clear PC13 nibble
        crh |= (0x2u << 20);           // general-purpose push-pull, 2 MHz
        r32(GPIOC_CRH) = crh;
    }
    void led_toggle() { r32(GPIOC_ODR) ^= LED; }

#elif defined(KICKOS_BLINK_F302NUCLEO)
    // Nucleo-F302R8: LD2 = PA5. F3 GPIO is on AHB at 0x48000000; toggle via ODR.
    constexpr uintptr_t RCC_AHBENR = 0x40021000 + 0x14;
    constexpr uintptr_t GPIOA_MODER = 0x48000000 + 0x00;
    constexpr uintptr_t GPIOA_ODR = 0x48000000 + 0x14;
    constexpr uint32_t LED = 1u << 5;
    void led_init()
    {
        r32(RCC_AHBENR) |= (1u << 17); // IOPAEN
        uint32_t m = r32(GPIOA_MODER);
        m &= ~(0x3u << 10);            // clear MODER5
        m |= (0x1u << 10);             // general-purpose output
        r32(GPIOA_MODER) = m;
    }
    void led_toggle() { r32(GPIOA_ODR) ^= LED; }

#elif defined(KICKOS_BLINK_DUE)
    // Arduino Due: "L" LED = PB27 via PIO controller B (active-high). No toggle
    // register on the PIO, so flip based on the current output-data status.
    constexpr uintptr_t PMC_PCER0 = 0x400E0610;
    constexpr uintptr_t PIOB_BASE = 0x400E1000;
    constexpr uintptr_t PIOB_PER = PIOB_BASE + 0x00;
    constexpr uintptr_t PIOB_OER = PIOB_BASE + 0x10;
    constexpr uintptr_t PIOB_SODR = PIOB_BASE + 0x30;
    constexpr uintptr_t PIOB_CODR = PIOB_BASE + 0x34;
    constexpr uintptr_t PIOB_ODSR = PIOB_BASE + 0x38;
    constexpr uint32_t LED = 1u << 27;
    void led_init()
    {
        r32(PMC_PCER0) = 1u << 12; // clock PIOB (peripheral ID 12)
        r32(PIOB_PER) = LED;       // pin controlled by the PIO
        r32(PIOB_OER) = LED;       // output enabled
    }
    void led_toggle()
    {
        if (r32(PIOB_ODSR) & LED)
        {
            r32(PIOB_CODR) = LED;
        }
        else
        {
            r32(PIOB_SODR) = LED;
        }
    }

#elif defined(KICKOS_BLINK_XMC4800)
    // XMC4800 Relax Kit: LED1 = P5.9. IOCR8.PC9 = push-pull output; OMR toggles
    // (write PR9|PS9). XMC ports are always clocked (no per-port gate).
    constexpr uintptr_t P5_OUT = 0x48028500;
    constexpr uintptr_t P5_OMR = P5_OUT + 0x04;
    constexpr uintptr_t P5_IOCR8 = P5_OUT + 0x18;
    void led_init()
    {
        r32(P5_IOCR8) = 0x10u << 11; // PC9 = 0b10000 (output push-pull) at bits[15:11]
    }
    void led_toggle() { r32(P5_OMR) = 0x02000200u; } // PR9|PS9 -> toggle P5.9

#else
#error "blink: define KICKOS_BLINK_<board> for the target's LED"
#endif

    void blinker(void*)
    {
        while (true)
        {
            led_toggle();
            kos_sleep_ns(kBlinkNs);
        }
    }
}

int main(int, char**)
{
    kos_print("blink: heartbeat LED\n"); // harmless with no UART wired

    led_init();
    kos::thread::spawn(blinker, nullptr, "blink", 10);

    // Root parks so the blinker owns the CPU; blocking here proves the switch.
    int idle = kos_sem_create(0);
    while (true)
    {
        kos_sem_wait(idle);
    }
}
