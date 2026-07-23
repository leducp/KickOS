// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-C6 GPIO10 blink: the CANONICAL per-thread peripheral-MMIO isolation
// reference on RISC-V PMP. The C6 twin of the F411 PMSA proof (f411spi), plus the
// one extra gate the C6 forces: on this core a U-mode (REE) access to an HP
// peripheral passes TWO permission units in series (TRM 16.1) -- PMP (CPU-side,
// per-hart, checked FIRST) then APM (bus-side, per security mode, checked only if
// PMP passes). The default APM posture BLOCKS all REE access to every peripheral
// (TRM 16.3.2 Note), so a U-mode driver reaches nothing until a one-time TEE-mode
// APM open. PMP still draws the genuine per-thread line ON TOP of that background
// permit -- which is exactly what K64F (coarse AIPS-only) structurally cannot do.
//
// A privileged (M-mode = TEE) bring-up shim muxes GPIO10 push-pull, opens APM
// region 1 over the GPIO Matrix block for REE0, then spawns the UNPRIVILEGED driver
// granted ONLY the 8 B PMP window over GPIO_OUT_W1TS/W1TC (0x6009_1008, size 8).
// The driver blinks by alternating writes to the two atomic set/clear registers,
// then pokes UNGRANTED GPIO_ENABLE (0x6009_1020) -- same GPIO block, APM-permitted,
// but OUTSIDE the 8 B PMP window -> PMP store fault (mcause=7) -> the kernel names
// the task ("MPU FAULT: task 'c6blink'"). The isolation proof rides the PMP fault,
// NOT APM: an APM denial does NOT trap (TRM 16.5: read returns 0 / write dropped +
// a separate HP_APM interrupt), so only PMP gives the load/store fault.
//
// Diagnostic app (kickos_add_diagnostic_app): build-only, never a production image;
// the operator flashes a RAM-only image + observes GPIO10 and the console.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/libc/fmt.h>

#include <gpio_class.h> // Rule 6 class-driver leaf: shared GPIO output-latch read

#include <stdint.h>

// This app EXISTS to prove PMP per-thread peripheral enforcement. Without it the
// ungranted poke below succeeds and the console prints the isolation-FAILURE line --
// a false verdict. Refuse to build a misleading oracle. (CMake gates it too.)
#if !KICKOS_HAVE_MPU
#error "c6blink requires enforcement: configure with -DKICKOS_HAVE_MPU=1"
#endif

namespace
{
    // ESP32-C6 TRM v1.2. Absolute addresses, no ESP-IDF/HAL.
    // GPIO Matrix block (mem map Table 5.3-2): 0x6009_1000 .. 0x6009_1FFF.
    constexpr uintptr_t GPIO_BASE = 0x60091000u;
    constexpr uintptr_t GPIO_END = 0x60091FFFu;
    constexpr uintptr_t GPIO_OUT_W1TS = GPIO_BASE + 0x08u;  // Reg 7.2 (WT set-1s)
    constexpr uintptr_t GPIO_OUT_W1TC = GPIO_BASE + 0x0Cu;  // Reg 7.3 (WT clear-1s)
    constexpr uintptr_t GPIO_ENABLE = GPIO_BASE + 0x20u;    // Reg 7.4 (dir; escalation)
    constexpr uintptr_t GPIO_ENABLE_W1TS = GPIO_BASE + 0x24u;
    constexpr uintptr_t GPIO_FUNC_OUT_SEL_CFG = GPIO_BASE + 0x554u; // Reg base, +4*n

    // IO_MUX block (mem map): 0x6009_0000. IO_MUX_GPIOn_REG = 0x0004 + 4*n (Reg 7.20).
    constexpr uintptr_t IO_MUX_BASE = 0x60090000u;
    constexpr uint32_t IO_MUX_MCU_SEL_GPIO = 1u << 12; // MCU_SEL=1 -> GPIO matrix func
    constexpr uint32_t IO_MUX_FUN_DRV_2 = 2u << 10;    // ~20 mA drive

    // HP TEE controller (mem map): 0x6009_8000. TEE_Mn_MODE_CTRL_REG = 0x0000+0x4*n
    // (Reg 16.53). HP CPU is master M0 (TRM 16.3.1). Reset 0 => U-mode security mode
    // is REE0 (TRM 16.3.1); confirm-only, no write needed while the reset holds.
    constexpr uintptr_t HP_TEE_M0_MODE_CTRL = 0x60098000u;

    // HP_APM controller (mem map): 0x6009_9000. Region regs (TRM Reg 16.1-16.4):
    //   FILTER_EN  @ 0x0000        (reset 0x01: region 0 on)
    //   REGIONn_ADDR_START @ 0x0004 + 0xC*n
    //   REGIONn_ADDR_END   @ 0x0008 + 0xC*n  (reset 0xFFFFFFFF)
    //   REGIONn_ATTR       @ 0x000C + 0xC*n; REE0 bits: R0_X=b0, R0_W=b1, R0_R=b2
    constexpr uintptr_t HP_APM_BASE = 0x60099000u;
    constexpr uintptr_t HP_APM_FILTER_EN = HP_APM_BASE + 0x0000u;
    constexpr uintptr_t HP_APM_R1_ADDR_START = HP_APM_BASE + 0x0004u + 0x0Cu; // n=1
    constexpr uintptr_t HP_APM_R1_ADDR_END = HP_APM_BASE + 0x0008u + 0x0Cu;   // n=1
    constexpr uintptr_t HP_APM_R1_ATTR = HP_APM_BASE + 0x000Cu + 0x0Cu;       // n=1
    constexpr uint32_t APM_R0_W = 1u << 1;
    constexpr uint32_t APM_R0_R = 1u << 2;
    constexpr uint32_t APM_REGION1_EN = 1u << 1;

    // Driver pin + granted window. GPIO10 (net IO10) is a non-strapping header pin
    // (strapping: 8/9/15; USB-JTAG: 12/13; console UART: 16/17). 8 B PMP NAPOT over
    // W1TS(0x08)+W1TC(0x0C): size 8 == PMP min, pow2, base 8-aligned -> encodable as
    // one entry (arch_mpu_region_encodable). RW-NX; PMP has no device-memory type.
    constexpr int BLINK_PIN = 10;
    constexpr uintptr_t GPIO_MMIO_WINDOW_BASE = GPIO_OUT_W1TS; // 0x6009_1008
    constexpr uint32_t GPIO_MMIO_WINDOW = 8u;
    constexpr uint32_t W1TS_OFFSET = 0x00u; // W1TS at window+0
    constexpr uint32_t W1TC_OFFSET = 0x04u; // W1TC at window+4

    constexpr int DRIVER_BLINKS = 10;
    constexpr uint64_t HALF_PERIOD_NS = 250000000ull; // ~2 Hz blink

    inline volatile uint32_t& r32(uintptr_t a)
    {
        return *reinterpret_cast<volatile uint32_t*>(a);
    }

    // UNPRIVILEGED driver: granted app code+data (auto) + the 8 B GPIO window (spawn
    // MMIO grant). No file-scope mutable state under enforcement: the window base
    // arrives as the thread arg VALUE (never dereferenced as memory), buffers live on
    // the granted stack. IRQ-less (GPIO blink); a kos_sleep_ns toggle loop, not an IRQ.
    void blink_driver(void* arg)
    {
        uintptr_t const win = reinterpret_cast<uintptr_t>(arg); // GPIO window base
        volatile uint32_t* w1ts = reinterpret_cast<volatile uint32_t*>(win + W1TS_OFFSET);
        volatile uint32_t* w1tc = reinterpret_cast<volatile uint32_t*>(win + W1TC_OFFSET);
        uint32_t const bit = 1u << BLINK_PIN;

        kos::print("[c6blink] blinking GPIO10 via the 8 B PMP window\n");

        for (int i = 0; i < DRIVER_BLINKS; i++)
        {
            *w1ts = bit; // drive high (in-window; atomic set, no RMW of the shared latch)
            kos_sleep_ns(HALF_PERIOD_NS);
            *w1tc = bit; // drive low (in-window; atomic clear)
            kos_sleep_ns(HALF_PERIOD_NS);

            char s[48];
            ksnprintf(s, sizeof(s), "[c6blink] blink %d\n", i + 1);
            kos::print(s);
        }

        // Negative test (the per-thread isolation proof): poke UNGRANTED GPIO_ENABLE
        // -- same GPIO block, APM-permitted for REE0, but OUTSIDE the 8 B PMP window.
        // PMP is checked FIRST and is fail-closed -> store access fault, mcause=7,
        // mtval=0x6009_1020 -> kickos_rv_fault_report routes it (from_user and mcause 7)
        // to "MPU FAULT: task 'c6blink'". Announce-before-poke; terminal, so it is LAST.
        // An APM denial would NOT trap (TRM 16.5), so the proof rides the PMP fault.
        kos::print("[c6blink] poking UNGRANTED GPIO_ENABLE @ 0x60091020 (expect MPU FAULT)\n");
        r32(GPIO_ENABLE) = bit;

        // Only reached if PMP did NOT enforce -- an isolation failure, not a pass.
        kos::print("[c6blink] UNGRANTED ACCESS DID NOT FAULT (PMP not enforcing)\n");
        while (true)
        {
            kos_sleep_ns(1000000000ull);
        }
    }
}

int main(int, char**)
{
    // Privileged bring-up (this main runs privileged = M-mode = TEE): the one-time
    // unsafe setup the unprivileged driver must NOT be able to do. IO_MUX, GPIO_ENABLE
    // / FUNC_OUT_SEL, and APM/TEE are the escalation surfaces and stay OUT of the
    // driver's 8 B window -- keeping them out is what makes the window a capability.
    // The ROM leaves the GPIO Matrix clocked (its own boot log muxes pads), so no PCR
    // gating here -- and PCR (0x6009_6000) is itself an escalation surface left closed.

    // GPIO10 push-pull output: IO_MUX pad on the GPIO matrix function + a driver, the
    // GPIO matrix out-sel = 128 (bit n of GPIO_OUT_REG/GPIO_ENABLE_REG drives the pad;
    // TRM 7.4.1 "simple GPIO output" + Reg 7.x reset 0x80), output enable via
    // GPIO_ENABLE. Direction is set BEFORE the driver can touch the output latch.
    r32(IO_MUX_BASE + 0x0004u + 0x4u * BLINK_PIN) = IO_MUX_MCU_SEL_GPIO | IO_MUX_FUN_DRV_2;
    r32(GPIO_FUNC_OUT_SEL_CFG + 0x4u * BLINK_PIN) = 128u; // simple GPIO output
    r32(GPIO_ENABLE_W1TS) = 1u << BLINK_PIN;              // output enable for GPIO10

    // APM open (TEE-mode only; TRM 16.3.2 Note). REE0 = U-mode security mode by reset
    // (TEE_M0_MODE_CTRL reset 0, TRM 16.3.1) -- confirm-only, no write. Region 0 stays
    // the reset catch-all (START=0, END=0xFFFFFFFF, ATTR=0) that DENIES all REE modes
    // everywhere; region 1 grants REE0 R/W over just the GPIO block, and the overlap
    // resolves to the permit (TRM 16.3.2.3). So U-mode gains R/W to exactly the GPIO
    // block while every other peripheral (IO_MUX/PCR/APM/TEE) stays APM-closed to REE.
    (void)HP_TEE_M0_MODE_CTRL; // reset default (REE0) holds; documented no-write
    r32(HP_APM_R1_ADDR_START) = GPIO_BASE;
    r32(HP_APM_R1_ADDR_END) = GPIO_END;
    r32(HP_APM_R1_ATTR) = APM_R0_R | APM_R0_W;             // REE0 read+write, no execute
    r32(HP_APM_FILTER_EN) = r32(HP_APM_FILTER_EN) | APM_REGION1_EN;
    // HP_APM_FUNC_CTRL (0x00C4) resets to 0xF (M0-M3 enforcing) -- left as-is.
    __asm volatile("fence" ::: "memory"); // settle the APM config before the driver runs

    // Read back the GPIO output latch through the shared class leaf (Rule 6):
    // baseline output state before the unprivileged driver drives GPIO10. Pure read.
    uint32_t const out = kickos::esp32c6::classdrv::gpio_out_read(GPIO_BASE);
    char rb[48];
    ksnprintf(rb, sizeof(rb), "[c6blink] GPIO_OUT readback 0x%lx\n",
              static_cast<unsigned long>(out));
    kos::print(rb);

    // Spawn the UNPRIVILEGED driver granted ONLY the 8 B W1TS/W1TC window. No IRQ.
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
