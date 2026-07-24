// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// STM32F411 SPI1 loopback driver: the CANONICAL per-thread peripheral-MMIO
// isolation reference on ARMv7-M PMSA (task #9 Stage 5). Unlike K64F -- where the
// SYSMPU is bus-slave-side and peripherals are gated coarsely by the AIPS bridge
// (k64drv proved a peripheral window grant is INERT) -- the PMSA MPU is CPU-side and
// covers peripheral space, so a granted DEV window IS a genuine per-thread
// capability (reprogrammed every switch-in by arch_mpu_apply).
//
// A privileged bring-up shim clock-enables + AF5-muxes PA5/6/7 (SCK/MISO/MOSI) and
// configures SPI1 as a software-NSS master; it then spawns the UNPRIVILEGED driver
// granted ONLY the 32 B SPI1 register window (0x4001_3000, DEV R|W no-X) + the SPI1
// IRQ (35, tier-1). The clock-enable (RCC) and pin-mux (GPIOA) registers are the
// escalation surfaces and stay OUT of the window -- keeping them out is what makes
// the window a real capability. The driver runs a physical PA7->PA6 loopback
// (rx == tx per word), then pokes an UNGRANTED peripheral (GPIOB) which on PMSA
// MUST fault MemManage -- the per-thread isolation result the fleet was missing.
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
// isolation-FAILURE line -- a false "PMSA does not gate peripherals" verdict. Refuse
// to build a misleading oracle. (CMake also gates the app to enforcement builds.)
#if !KICKOS_HAVE_MPU
#error "f411spi requires enforcement: configure with -DKICKOS_HAVE_MPU=1"
#endif

namespace
{
    // RM0383: RCC (6.3), GPIOA (8.4), SPI1 (20.5). Absolute addresses, no CMSIS.
    constexpr uintptr_t RCC_BASE = 0x40023800u;
    constexpr uintptr_t RCC_AHB1ENR = RCC_BASE + 0x30u;
    constexpr uintptr_t RCC_APB2ENR = RCC_BASE + 0x44u; // 6.3.11, off 0x44
    constexpr uint32_t AHB1ENR_GPIOAEN = 1u << 0;
    constexpr uint32_t APB2ENR_SPI1EN = 1u << 12; // 6.3.11: bit 12 SPI1EN

    constexpr uint32_t AHB1ENR_GPIOEEN = 1u << 4;

    constexpr uintptr_t GPIOA_BASE = 0x40020000u;
    constexpr uintptr_t GPIOA_MODER = GPIOA_BASE + 0x00u;
    constexpr uintptr_t GPIOA_OSPEEDR = GPIOA_BASE + 0x08u;
    constexpr uintptr_t GPIOA_AFRL = GPIOA_BASE + 0x20u; // pins 0..7, 4 bits/pin

    // GPIOE: the F411E-DISCO onboard gyro (L3GD20/I3G4250D) chip-select is PE3
    // ("CS_I2C/SPI", UM1842 pin table). It shares SPI1 with PA5/6/7 and its SDO
    // drives PA6/MISO, so it MUST be held deselected (PE3 high) or it fights the
    // PA7->PA6 loopback jumper for MISO.
    constexpr uintptr_t GPIOE_BASE = 0x40021000u;
    constexpr uintptr_t GPIOE_MODER = GPIOE_BASE + 0x00u;
    constexpr uintptr_t GPIOE_BSRR = GPIOE_BASE + 0x18u;

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

    // UNPRIVILEGED driver: granted app code+data (auto), the SPI1 window (spawn MMIO
    // grant) and the SPI1 IRQ (tier-1). No file-scope mutable state under enforcement:
    // the window base arrives as the thread arg VALUE (never dereferenced as memory),
    // buffers live on the granted stack.
    void spi_driver(void* arg)
    {
        uintptr_t const win = reinterpret_cast<uintptr_t>(arg); // SPI1 window base
        volatile uint32_t* cr2 = reinterpret_cast<volatile uint32_t*>(win + CR2_OFFSET);
        volatile uint32_t* sr = reinterpret_cast<volatile uint32_t*>(win + SR_OFFSET);
        volatile uint32_t* dr = reinterpret_cast<volatile uint32_t*>(win + DR_OFFSET);

        int h = kos_irq_register(SPI1_IRQ);
        if (h < 0)
        {
            kos::print("[f411spi] ERROR: irq_register(SPI1) failed\n");
            while (true)
            {
                kos_sleep_ns(1000000000ull);
            }
        }

        *cr2 = CR2_RXNEIE; // arm RX interrupt (in-window; only source that wakes line 35)

        // Announce before the first blocking wait: if IRQ 35 never fires (misrouted
        // line / NVIC), the driver hangs in kos_irq_wait -- this line disambiguates a
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
                                       // no W1C flag -- the DR read is the mandatory quiesce)

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

        // Only reached if PMSA did NOT enforce -- an isolation failure, not a pass.
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

int main(int, char**)
{
    // Privileged bring-up (this main runs privileged): clock-enable + pin-mux + SPI
    // master config -- the one-time unsafe setup the unprivileged driver must NOT be
    // able to do. RCC + GPIOA stay privileged (out of the driver's window).
    r32(RCC_AHB1ENR) |= AHB1ENR_GPIOAEN | AHB1ENR_GPIOEEN; // clock GPIOA + GPIOE
    r32(RCC_APB2ENR) |= APB2ENR_SPI1EN;                    // clock SPI1

    // Hold the onboard gyro deselected FIRST (PE3 output, driven high), before any
    // SCK activity, so its SDO stays tri-stated and the PA7->PA6 jumper owns MISO.
    uint32_t emoder = r32(GPIOE_MODER);
    emoder &= ~(0x3u << 6); // clear MODER3 (bits 6..7)
    emoder |= (0x1u << 6);  // PE3 general-purpose output
    r32(GPIOE_MODER) = emoder;
    r32(GPIOE_BSRR) = 1u << 3; // drive PE3 high => gyro CS deasserted

    // PA5/PA6/PA7 -> AF mode, AF5 (SPI1), high output speed. Muxing SCK before CR1 is
    // glitch-free ONLY because CPOL=0 is the CR1 reset value, so SCK's idle level is
    // already correct at mux time; a CPOL=1 variant MUST write CR1 before muxing.
    uint32_t moder = r32(GPIOA_MODER);
    moder &= ~(0x3Fu << 10);                             // clear MODER5/6/7 (bits 10..15)
    moder |= (0x2u << 10) | (0x2u << 12) | (0x2u << 14); // AF mode
    r32(GPIOA_MODER) = moder;
    uint32_t osp = r32(GPIOA_OSPEEDR);
    osp |= (0x3u << 10) | (0x3u << 12) | (0x3u << 14); // high speed PA5/6/7
    r32(GPIOA_OSPEEDR) = osp;
    uint32_t afrl = r32(GPIOA_AFRL);
    afrl &= ~(0xFFFu << 20);                      // clear AFRL5/6/7 (bits 20..31)
    afrl |= (5u << 20) | (5u << 24) | (5u << 28); // AF5 for PA5/6/7
    r32(GPIOA_AFRL) = afrl;

    // SPI1 master, software NSS (SSM|SSI hold internal NSS high, else MODF), mode 0,
    // 8-bit, MSB-first, /64. Configure with SPE=0, then enable.
    r32(SPI1_BASE + CR2_OFFSET) = 0u; // RXNEIE off; the driver arms it in-window
    r32(SPI1_BASE + CR1_OFFSET) = CR1_MSTR | CR1_SSM | CR1_SSI | CR1_BR_DIV64;
    r32(SPI1_BASE + CR1_OFFSET) |= CR1_SPE;

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
