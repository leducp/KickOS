// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// STM32F411 SPI1 loopback driver: the CANONICAL per-thread peripheral-MMIO
// isolation reference on ARMv7-M PMSA (task #9 Stage 5). Unlike K64F, where the
// SYSMPU is bus-slave-side and peripherals are gated coarsely by the AIPS bridge
// (k64drv proved a peripheral window grant is INERT), the PMSA MPU is CPU-side and
// covers peripheral space, so a granted DEV window IS a genuine per-thread
// capability (reprogrammed every switch-in by arch_mpu_apply).
//
// main muxes PA5/6/7 (SCK/MISO/MOSI) to AF5 and holds PE3 high through kos_pinmux_set,
// its only hardware access and no direct MMIO at all, then spawns the
// UNPRIVILEGED driver granted ONLY the 32 B SPI1 register window (0x4001_3000, DEV
// R|W no-X) + the SPI1 IRQ (35, tier-1). The driver's first act is
// kos_periph_enable(SPI1), authorised by possession of that window, and it configures
// SPI1 as a software-NSS master itself. The clock-enable (RCC) and pin-mux (GPIOA)
// registers are the escalation surfaces and stay OUT of the window: keeping them out
// is what makes the window a real capability. The driver runs a physical PA7->PA6
// loopback (rx == tx per word), then pokes an UNGRANTED peripheral (GPIOB) which on
// PMSA MUST fault MemManage, the per-thread isolation result the fleet was missing.
//
// PMSA peripheral enforcement is only build/link-validated to date; this driver
// ALSO first-proves it on F411 silicon. Diagnostic app (kickos_add_diagnostic_app):
// build-only, never a production image; the operator flashes + validates.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/libc/fmt.h>

#include <stdint.h>

// This app EXISTS to prove PMSA per-thread peripheral enforcement. Without it the
// MPU is a no-op, the ungranted poke below succeeds, and the console prints the
// isolation-FAILURE line: a false "PMSA does not gate peripherals" verdict. Refuse
// to build a misleading oracle. (CMake also gates the app to enforcement builds.)
#if !KICKOS_HAVE_MPU
#error "f411spi requires enforcement: configure with -DKICKOS_HAVE_MPU=1"
#endif

namespace
{
    // RM0383: GPIO (8.4), SPI1 (20.5). Absolute addresses, no CMSIS.

    // kos_pinmux_set port index on stm32f411: (port base - GPIOA_BASE) / 0x400.
    constexpr uint32_t PORT_A = 0u;
    constexpr uint32_t PORT_E = 4u;
    constexpr uint32_t PIN_SCK = 5u;
    constexpr uint32_t PIN_MISO = 6u;
    constexpr uint32_t PIN_MOSI = 7u;

    // The F411E-DISCO onboard gyro (L3GD20/I3G4250D) chip-select is PE3 ("CS_I2C/SPI",
    // UM1842 pin table). It shares SPI1 with PA5/6/7 and its SDO drives PA6/MISO, so it
    // MUST be held deselected (PE3 high) or it fights the PA7->PA6 loopback jumper.
    constexpr uint32_t PIN_GYRO_CS = 3u;

    // stm32f411 func encoding: bits[1:0] are the MODER field (00 in, 01 out, 10 AF,
    // 11 analog), bits[7:4] the AF number, bit 8 presets an output high. OSPEEDR/PUPDR
    // are not reachable and stay at reset; at /64 (~1.3 MHz) the reset slew carries SCK.
    constexpr uint32_t MUX_OUTPUT = 0x01u;
    constexpr uint32_t MUX_OUT_HIGH = 0x100u;
    constexpr uint32_t MUX_AF5 = 0x52u; // AF5 = SPI1

    // SPI1 register window granted to the driver (RM0383 memory map: SPI1 @
    // 0x4001_3000). 32 B is the minimal PMSA-encodable window (pow2 >= 32, base
    // 32-aligned) covering CR1/CR2/SR/DR at 0x00/0x04/0x08/0x0C.
    constexpr uintptr_t SPI1_BASE = 0x40013000u;
    constexpr uint32_t SPI1_WINDOW = 32u;
    constexpr uint32_t CR1_OFFSET = 0x00u;
    constexpr uint32_t CR2_OFFSET = 0x04u;
    constexpr uint32_t SR_OFFSET = 0x08u;
    constexpr uint32_t DR_OFFSET = 0x0Cu;

    // SPI_CR1 (RM0383 20.5.1). CPOL/CPHA/DFF/LSBFIRST all 0 => mode 0, 8-bit, MSB.
    constexpr uint32_t CR1_MSTR = 1u << 2;       // master
    constexpr uint32_t CR1_SPE = 1u << 6;        // SPI enable
    constexpr uint32_t CR1_SSI = 1u << 8;        // internal NSS level (high => not deselected)
    constexpr uint32_t CR1_SSM = 1u << 9;        // software NSS (loopback needs no real CS)
    constexpr uint32_t CR1_BR_DIV64 = 0x5u << 3; // 84 MHz APB2 / 64 ~= 1.3 MHz
    // SPI_CR2 (20.5.2) / SPI_SR (20.5.3).
    constexpr uint32_t CR2_RXNEIE = 1u << 6; // RX-buffer-not-empty interrupt enable
    constexpr uint32_t SR_RXNE = 1u << 0;
    constexpr uint32_t SR_TXE = 1u << 1;

    constexpr int SPI1_IRQ = 35; // RM0383 vector table: SPI1 global interrupt

    // Ungranted peripheral for the negative test (RM0383 memory map): GPIOB base,
    // outside the 32 B SPI1 window. On PMSA an unprivileged access MUST MemManage.
    constexpr uintptr_t GPIOB_BASE = 0x40020400u;

    constexpr uint32_t POLL_TIMEOUT = 1000000u;

    inline volatile uint32_t& r32(uintptr_t a)
    {
        return *reinterpret_cast<volatile uint32_t*>(a);
    }

    void mux_pin(char const* what, uint32_t port, uint32_t pin, uint32_t func)
    {
        int rc = kos_pinmux_set(port, pin, func);
        if (rc != 0)
        {
            // Name the pin: a refused mux leaves that signal on the wrong function and
            // the loopback verdict below becomes meaningless rather than absent.
            char m[64];
            ksnprintf(m, sizeof(m), "[f411spi] ERROR: pinmux %s failed rc %d\n", what, rc);
            kos::print(m);
        }
    }

    // UNPRIVILEGED driver: granted app code+data (auto), the SPI1 window (spawn MMIO
    // grant) and the SPI1 IRQ (tier-1). No file-scope mutable state under enforcement:
    // the window base arrives as the thread arg VALUE (never dereferenced as memory),
    // buffers live on the granted stack.
    void spi_driver(void* arg)
    {
        uintptr_t const win = reinterpret_cast<uintptr_t>(arg); // SPI1 window base
        volatile uint32_t* cr1 = reinterpret_cast<volatile uint32_t*>(win + CR1_OFFSET);
        volatile uint32_t* cr2 = reinterpret_cast<volatile uint32_t*>(win + CR2_OFFSET);
        volatile uint32_t* sr = reinterpret_cast<volatile uint32_t*>(win + SR_OFFSET);
        volatile uint32_t* dr = reinterpret_cast<volatile uint32_t*>(win + DR_OFFSET);

        // First act: authorised by POSSESSION of this exact window, which only this
        // thread holds. While SPI1 is gated every register write below is discarded.
        int rc = kos_periph_enable(win);
        if (rc != 0)
        {
            char m[64];
            ksnprintf(m, sizeof(m), "[f411spi] ERROR: periph_enable(SPI1) rc %d\n", rc);
            kos::print(m);
            while (true)
            {
                kos_sleep_ns(1000000000ull);
            }
        }

        int h = kos_irq_register(SPI1_IRQ);
        if (h < 0)
        {
            kos::print("[f411spi] ERROR: irq_register(SPI1) failed\n");
            while (true)
            {
                kos_sleep_ns(1000000000ull);
            }
        }

        // SPI1 master, software NSS (SSM|SSI hold internal NSS high, else MODF), mode 0,
        // 8-bit, MSB-first, /64. Configure with SPE=0, then enable.
        *cr1 = CR1_MSTR | CR1_SSM | CR1_SSI | CR1_BR_DIV64;
        *cr2 = CR2_RXNEIE; // arm RX interrupt (only source that wakes line 35)
        *cr1 |= CR1_SPE;

        // Announce before the first blocking wait: if IRQ 35 never fires (misrouted
        // line / NVIC), the driver hangs in kos_irq_wait: this line disambiguates a
        // hung-waiting-for-IRQ board from a dead one / a missing console adapter.
        kos::print("[f411spi] starting loopback (blocking on SPI1 IRQ 35)\n");

        // Known pattern; each word round-trips through the PA7->PA6 jumper equal.
        uint8_t const pattern[] = {0xA5u, 0x3Cu, 0x00u, 0xFFu};
        int fails = 0;
        for (unsigned i = 0; i < sizeof(pattern); i++)
        {
            uint32_t tx = pattern[i];

            uint32_t spin = 0;
            bool txe_timeout = false;
            while ((*sr & SR_TXE) == 0)
            {
                if (++spin > POLL_TIMEOUT)
                {
                    txe_timeout = true;
                    break;
                }
            }
            if (txe_timeout)
            {
                // Say which word wedged: else the unbounded RXNE wait below hangs mute.
                char t[48];
                ksnprintf(t, sizeof(t), "[f411spi] TXE timeout on word %u\n", i);
                kos::print(t);
            }
            *dr = tx; // load TX buffer; master starts clocking the frame out on MOSI

            kos_irq_wait(h);           // block until RXNE raises line 35; return auto-re-arms
                                       // the line (no explicit kernel ack)
            uint32_t rx = *dr & 0xFFu; // read RX: CLEARS RXNE, de-asserts the line so it
                                       // does not storm when the next wait re-arms (SPI has
                                       // no W1C flag, so the DR read is the mandatory quiesce)

            char s[64];
            char const* verdict = "PASS";
            if (rx != tx)
            {
                verdict = "FAIL";
                fails++;
            }
            ksnprintf(s, sizeof(s), "[f411spi] word %u: tx=0x%x rx=0x%x %s\n",
                      i, static_cast<unsigned>(tx), static_cast<unsigned>(rx), verdict);
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

        // Negative test (the canonical proof): touch an UNGRANTED peripheral. On PMSA
        // the CPU-side MPU faults this BEFORE any bus access -> MemManage, reported as
        // "MPU FAULT" with MMFAR=0x40020400. Announce-before-poke so the console shows
        // intent then the fault. This is terminal, so it is the LAST thing we do.
        kos::print("[f411spi] poking UNGRANTED GPIOB @ 0x40020400 (expect MPU FAULT)\n");
        uint32_t leaked = r32(GPIOB_BASE);

        // Only reached if PMSA did NOT enforce: an isolation failure, not a pass.
        char s[72];
        ksnprintf(s, sizeof(s),
                  "[f411spi] UNGRANTED ACCESS DID NOT FAULT (GPIOB=0x%x)\n",
                  static_cast<unsigned>(leaked));
        kos::print(s);
        while (true)
        {
            kos_sleep_ns(1000000000ull);
        }
    }
}

// mux_pin runs from root for the CS and the three SPI1 pins, then the SPI1 window is
// granted to a worker. Never returns, so it needs no KOS_AUTH_SYSTEM.
KICKOS_APP_AUTHORITY(KOS_AUTH_MEMORY | KOS_AUTH_PINMUX);

int main(int, char**)
{
    // Deselect the onboard gyro FIRST, before any SCK activity, so its SDO stays
    // tri-stated and the PA7->PA6 jumper owns MISO. One call: the seam gates the GPIOE
    // clock, presets PE3 high, then switches it to output, so it never drives low.
    mux_pin("PE3", PORT_E, PIN_GYRO_CS, MUX_OUTPUT | MUX_OUT_HIGH);

    // PA5/PA6/PA7 -> AF5 (SPI1). Muxing SCK before CR1 is glitch-free ONLY because
    // CPOL=0 is the CR1 reset value, so SCK's idle level is already correct at mux
    // time; a CPOL=1 variant MUST write CR1 before muxing.
    mux_pin("PA5/SCK", PORT_A, PIN_SCK, MUX_AF5);
    mux_pin("PA6/MISO", PORT_A, PIN_MISO, MUX_AF5);
    mux_pin("PA7/MOSI", PORT_A, PIN_MOSI, MUX_AF5);

    int drv = kos::thread::spawn(spi_driver, reinterpret_cast<void*>(SPI1_BASE),
                                 "f411spi", 10, KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                 /*mem=*/nullptr, /*mem_size=*/0,
                                 /*stack=*/nullptr, /*stack_size=*/0,
                                 /*mmio=*/reinterpret_cast<void*>(SPI1_BASE), SPI1_WINDOW);
    if (drv < 0)
    {
        // Console is the only oracle at the bench: a silent dead board must not be
        // mistaken for a bring-up failure, so say so.
        kos::print("[f411spi] ERROR: driver spawn failed\n");
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
