// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// QEMU `virt` for AArch64 (Cortex-A53, EL1 bare metal): PL011 UART at 0x0900_0000, GICv2,
// and the architected generic timer as the timebase. `-cpu cortex-a53` is required, the
// machine defaulting to a core that refuses an A64 image.

#include <kickos/arch/arch.h>

#include <kickos/arch/clk_q32.h> // KICKOS_NS_PER_SEC (canonical 1e9 ns/sec)
#include <kickos/chip_limits.h>  // KICKOS_MAX_IRQ: this GIC's interrupt-ID count
#include <kickos/sys/atomic.h>

#include "gic.h"         // arch/arm64/common: the architected half of this machine's controller
#include "gicv2.h"       // arch/arm64/common: which controller this machine has, and where
#include "smp_bringup.h" // arch/arm64/common: ARM64_BRINGUP_WAIT_NS

#include <fatal_status.ld.h>

#include <stdint.h>

// THE EL REFUSAL PATH IS LINKED INTO THE IDENTITY-MAPPED BOOT SPAN (virt_arm64.ld).
// startup.S branches to it with SCTLR_EL1.M still 0, where no walk resolves a high-half
// address, so it names the PL011 by its physical base rather than through dev_va and may
// not call out of the section: ld's veneer for an out-of-range branch still targets the
// callee's high address.
#define KICKOS_BOOT_TEXT __attribute__((section(".text.init"), noinline, used))
#define KICKOS_BOOT_RODATA __attribute__((section(".rodata.init"), used))

namespace kickos
{
    int kmain(int argc, char** argv);
}

extern "C"
{
    // Linker-script symbols (virt_arm64.ld).
    extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss;
    // VA - PA for the kernel's half; see dev_va below.
    extern unsigned char __kickos_arm64_va_base[];
    // The app's .bss lives in the low window, outside _sbss.._ebss, so the boot zeroing
    // covers it separately. Its .data needs no copy: the low window links VMA == LMA.
    extern uint32_t __kickos_appbss_start, __kickos_appbss_end;
    extern void (*__init_array_start[])();
    extern void (*__init_array_end[])();

    // Nominal core clock (Hz).
    uint32_t SystemCoreClock = 0;

    void kfault_terminate(void) __attribute__((noreturn));

#if KICKOS_NUM_CORES > 1
    // The first instruction a released core executes, supplied by the armv8a backend. It has
    // to be linked in the identity-mapped span (virt_arm64.ld's .text.init at ORIGIN(BOOT)):
    // PSCI hands a core a PHYSICAL entry address and that core arrives with its MMU off.
    void kickos_armv8a_secondary_entry(void);

    // Set by a core once it is executing its own code, which is what separates ARRIVED from
    // the SUCCESS PSCI returns for a core it merely started.
    extern kickos::Atomic<uint8_t, kickos::Order::RELAXED> kickos_armv8a_core_online[];

    // The armv8a doorbell-and-lock bring-up check. Terminates the image on a raise that goes
    // unanswered.
    void kickos_armv8a_doorbell_selfcheck(void);
#endif
}

namespace
{
    // EVERY DEVICE REGISTER IS REACHED THROUGH THE KERNEL'S OWN HALF. The device gigabyte
    // is mapped at PA + __kickos_arm64_va_base by TTBR1, which every address space shares;
    // TTBR0 carries a per-process root that maps no device at all, so a low literal here
    // would translate against whatever process happened to be running.
    inline uintptr_t dev_va(uintptr_t pa)
    {
        return pa + reinterpret_cast<uintptr_t>(__kickos_arm64_va_base);
    }

    inline volatile uint32_t* r32p(uintptr_t a)
    {
        return reinterpret_cast<volatile uint32_t*>(dev_va(a));
    }

    // QEMU `virt` PL011.
    constexpr uintptr_t UART0_BASE = 0x09000000;
    constexpr uintptr_t UART_DR = UART0_BASE + 0x00;
    constexpr uintptr_t UART_FR = UART0_BASE + 0x18;
    constexpr uint32_t UART_FR_TXFF = 1u << 5;
    constexpr uint32_t UART_FR_BUSY = 1u << 3;

    // A BOUND, not a timing: arch.h requires the flush to be bounded because it sits on the
    // panic and shutdown paths, where a wedged UART must cost a dropped tail rather than a
    // hang. The same reasoning binds the writer below.
    constexpr uint32_t UART_POLL_BOUND = 100000;

    // The refusal path's own writer, with the PL011 at the address the bus sees: the MMU
    // is off, so dev_va's high alias translates through nothing.
    KICKOS_BOOT_TEXT void boot_console_write(char const* buf, size_t n)
    {
        volatile uint32_t* fr = reinterpret_cast<volatile uint32_t*>(UART_FR);
        volatile uint32_t* dr = reinterpret_cast<volatile uint32_t*>(UART_DR);
        for (size_t i = 0; i < n; i++)
        {
            uint32_t spin = 0;
            while ((*fr & UART_FR_TXFF) != 0 and spin < UART_POLL_BOUND)
            {
                spin++;
            }
            *dr = static_cast<uint32_t>(static_cast<unsigned char>(buf[i]));
        }
    }

    // File scope, so they are .rodata for certain: as locals the compiler is free to
    // stage them on the stack through memcpy, and this path runs before the C runtime.
    // In the boot span because the reader runs there.
    KICKOS_BOOT_RODATA char const BAD_EL_HEAD[] = "KickOS: qemu-arm64 entered at EL";
    KICKOS_BOOT_RODATA char const BAD_EL_TAIL[] = ", and this port implements EL1 only\n";
    char const BAD_CNTFRQ[] =
        "KickOS: qemu-arm64 CNTFRQ_EL0 does not divide a second exactly\n";

    // AArch64 semihosting: `hlt #0xF000` with the operation in x0 and the parameter in x1.
    // ALWAYS inlined: a caller in the boot span can call nothing outside it.
    inline __attribute__((always_inline)) long semihost(long op, void* arg)
    {
        register long x0 __asm("x0") = op;
        register void* x1 __asm("x1") = arg;
        __asm volatile("hlt #0xF000" : "+r"(x0) : "r"(x1) : "memory");
        return x0;
    }

    constexpr long SYS_EXIT = 0x18;
    constexpr uint64_t ADP_Stopped_ApplicationExit = 0x20026u;

    // GICv2 on QEMU `virt`. The distributor is global, the CPU interface is this core's.
    constexpr uintptr_t GICD_BASE = 0x08000000;
    constexpr uintptr_t GICC_BASE = 0x08010000;

    // EL1 physical timer, an architecturally assigned PPI. It is NOT a kernel IRQ line:
    // kickos_isr_timer takes no line and the timer is in no dispatch table.
    constexpr int PPI_EL1_PHYS_TIMER = 30;

    constexpr uint64_t CNTP_CTL_ENABLE = 1u << 0;

    // ns per tick of the architected counter, from CNTFRQ_EL0 at bring-up. QEMU virt reports
    // 62.5 MHz, so this is exactly 16 and the conversions are lossless.
    uint64_t g_ns_per_tick = 0;

    uint64_t counter_now(void)
    {
        uint64_t t = 0;
        // The ISB is the architected ordered read: without it the counter may be sampled
        // out of order with the surrounding instructions, and every deadline derives from it.
        __asm volatile("isb; mrs %0, cntpct_el0" : "=r"(t));
        return t;
    }

#if KICKOS_NUM_CORES > 1
    // --- Secondary release, PSCI ------------------------------------------------
    //
    // QEMU `virt` holds every core but the first inside its own PSCI implementation, so a
    // secondary's first instruction is the one CPU_ON names and the release is the kernel's to
    // initiate.
    //
    // THE CONDUIT IS A CHIP CONSTANT. QEMU's generated `/psci` node carries it in the `method`
    // property, readable with `-machine dumpdtb=`: "hvc" under `-M virt`, "smc" under
    // `-M virt,virtualization=on`. This port parses no device tree, and startup.S admits a
    // handover at EL1 alone, which is the configuration whose method is "hvc". The i.MX8MP
    // runs EL3 firmware that owns PSCI behind `smc`.
    //
    // SMCCC: the function identifier in x0, the arguments in x1..x3, the result back in x0.
    // x4..x17 are the callee's scratch for a caller that negotiates no SMCCC version.
    long psci_hvc(uint32_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
    {
        register uint64_t x0 __asm("x0") = fn;
        register uint64_t x1 __asm("x1") = a1;
        register uint64_t x2 __asm("x2") = a2;
        register uint64_t x3 __asm("x3") = a3;
        __asm volatile("hvc #0"
                       : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
                       :
                       : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13",
                         "x14", "x15", "x16", "x17", "memory");
        return static_cast<long>(x0);
    }

    // The `/psci` node's own `cpu_on` property reads 0xC4000003, the SMC64 identifier: the
    // 64-bit variant is the one whose entry-point argument is 64 bits wide.
    constexpr uint32_t PSCI_FN64_CPU_ON = 0xC4000003u;
    constexpr long PSCI_SUCCESS = 0;

    // MPIDR_EL1 affinity (DDI 0487 M.b section D24.2.137): Aff0 [7:0], Aff1 [15:8],
    // Aff2 [23:16], Aff3 [39:32]. Bit 31 is RES1, bit 30 U, bit 24 MT and bits [29:25] and
    // [63:40] RES0; a PSCI target_cpu carries zero at all of them.
    constexpr uint64_t MPIDR_AFFINITY = 0x000000FF00FFFFFFull;

    void console_hex(uint64_t v, int digits)
    {
        char buf[16];
        for (int i = 0; i < digits; i++)
        {
            unsigned const nib = static_cast<unsigned>((v >> (4 * (digits - 1 - i))) & 0xFu);
            char c = static_cast<char>('0' + nib);
            if (nib >= 10)
            {
                c = static_cast<char>('a' + (nib - 10));
            }
            buf[i] = c;
        }
        arch_console_write(buf, static_cast<size_t>(digits));
    }

    char const BAD_ENTRY[] = "KickOS: qemu-arm64 secondary entry is not identity-linked: 0x";
    char const BAD_CPU_ON[] = "KickOS: qemu-arm64 PSCI CPU_ON refused core ";
    char const BAD_CPU_ON_TAIL[] = ", status 0x";
    char const RELEASE_NL[] = "\n";
    char const NO_ARRIVAL[] = "KickOS: qemu-arm64 secondary never reached its entry: core ";
    // A TAP comment, so the line is legal ahead of a plan on the boards that announce one.
    char const SMP_ONLINE[] = "# smp: ";
    char const SMP_ONLINE_TAIL[] = " core(s) online\n";

    // Bounded so a core that never arrives refuses rather than hangs. The counter is live
    // before the release, so the bound means the same on a part of any clock speed.
    constexpr uint64_t SECONDARY_ARRIVAL_NS = kickos::ARM64_BRINGUP_WAIT_NS;

    // A core short of KICKOS_NUM_CORES is FATAL: the count sizes every per-core array, so
    // continuing would hand threads to a core that is not running.
    void release_secondaries(void)
    {
        uint64_t mpidr = 0;
        __asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));

        // A released core fetches from a PHYSICAL address with its MMU off, so a high-half
        // entry is refused here: CPU_ON would answer SUCCESS and the core would fetch where no
        // walk of its own resolves, which presents as a core that never started.
        uintptr_t const entry = reinterpret_cast<uintptr_t>(&kickos_armv8a_secondary_entry);
        if (entry >= reinterpret_cast<uintptr_t>(__kickos_arm64_va_base))
        {
            arch_console_write(BAD_ENTRY, sizeof(BAD_ENTRY) - 1);
            console_hex(entry, 16);
            arch_console_write(RELEASE_NL, 1);
            kfault_terminate();
        }

        // Orders every write the primary published for a secondary ahead of the release. A
        // released core reads with its caches off until its own entry turns them on, so data
        // handed over that way owes a clean to the point of coherency. QEMU models no cache,
        // so that hazard has no witness here.
        __asm volatile("dsb sy" ::: "memory");

        // target_cpu is an AFFINITY value, not an index: a part packing eight cores to a
        // cluster gives index 8 the affinity 0x100. Aff0 alone varies here and the running core
        // supplies the rest, which is exact on this board, QEMU's generated /cpus/cpu@N `reg`
        // reading 0, 1, 2, 3 at `-smp 4`. An affinity naming no core is refused by PSCI.
        uint64_t const cluster = mpidr & MPIDR_AFFINITY & ~uint64_t(0xFF);
        for (uint32_t index = 1; index < KICKOS_NUM_CORES; index++)
        {
            // context_id in x3, which PSCI hands the entry point in x0. Zero: a secondary
            // reads its identity out of MPIDR_EL1.
            long const rc = psci_hvc(PSCI_FN64_CPU_ON, cluster | index, entry, 0);
            if (rc != PSCI_SUCCESS)
            {
                arch_console_write(BAD_CPU_ON, sizeof(BAD_CPU_ON) - 1);
                console_hex(index, 2);
                arch_console_write(BAD_CPU_ON_TAIL, sizeof(BAD_CPU_ON_TAIL) - 1);
                console_hex(static_cast<uint64_t>(rc), 16);
                arch_console_write(RELEASE_NL, 1);
                kfault_terminate();
            }
        }

        // RELEASED is not ARRIVED: CPU_ON answering SUCCESS says the core was started, and a
        // core handed a bad entry, a bad stack or a translation it cannot walk is started and
        // never reaches its own code. The online byte is the arrival witness.
        //
        // From ZERO, where the release loop above starts at one: every core publishes its own
        // arrival, so the check is the same question at every index and the running core's own
        // cell is read rather than assumed.
        for (uint32_t index = 0; index < KICKOS_NUM_CORES; index++)
        {
            uint64_t const deadline = counter_now() * g_ns_per_tick + SECONDARY_ARRIVAL_NS;
            while (kickos_armv8a_core_online[index] == 0)
            {
                if (counter_now() * g_ns_per_tick > deadline)
                {
                    arch_console_write(NO_ARRIVAL, sizeof(NO_ARRIVAL) - 1);
                    console_hex(index, 2);
                    arch_console_write(RELEASE_NL, 1);
                    kfault_terminate();
                }
                __asm volatile("yield" ::: "memory");
            }
        }

        // The positive statement a gate reads. Absence of a refusal is not a witness: a build
        // that released nobody would print nothing and boot clean.
        arch_console_write(SMP_ONLINE, sizeof(SMP_ONLINE) - 1);
        console_hex(KICKOS_NUM_CORES, 1);
        arch_console_write(SMP_ONLINE_TAIL, sizeof(SMP_ONLINE_TAIL) - 1);

        // AFTER ARRIVAL: the check needs every core's interface live and its target bit
        // published.
        kickos_armv8a_doorbell_selfcheck();
    }
#endif
}

extern "C"
{

// This machine's interrupt controller, which the GICv2 backend reads (gicv2.h).
struct kickos_gicv2_map const kickos_gicv2 = {
    GICD_BASE,
    GICC_BASE,
    KICKOS_MAX_IRQ,
    PPI_EL1_PHYS_TIMER,
};

// This core's hardware edge alone: the distributor's shared half runs once for the machine.
//
// A core reaches this with PSTATE.DAIF masked: the primary because startup.S leaves the reset
// masking alone, a secondary because that is the state PSCI hands a released core.
void kickos_armv8a_percore_init(void)
{
    // CNTP_CTL_EL0's reset value is architecturally UNKNOWN, so an already-asserted timer
    // would fire the moment this core's PPI and DAIF open.
    __asm volatile("msr cntp_ctl_el0, %0" ::"r"(uint64_t(0)));

    kickos_armv8a_gic_percore_init();
}

void arch_init(void)
{
    uint64_t freq = 0;
    __asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    // CNTFRQ_EL0 is firmware-programmed, so a board whose firmware never wrote it reads 0,
    // and a frequency that does not divide a second exactly makes g_ns_per_tick lossy.
    if (freq == 0 or (kickos::KICKOS_NS_PER_SEC % freq) != 0)
    {
        arch_console_write(BAD_CNTFRQ, sizeof(BAD_CNTFRQ) - 1);
        kfault_terminate();
    }
    g_ns_per_tick = kickos::KICKOS_NS_PER_SEC / freq;
    SystemCoreClock = static_cast<uint32_t>(freq);

    // The distributor is up before any CPU interface is, so a secondary released below finds
    // the shared half already written.
    kickos_armv8a_gic_dist_init();
    kickos_armv8a_percore_init();

#if KICKOS_NUM_CORES > 1
    release_secondaries();
#endif

    // PSTATE.I stays SET: interrupts first reach the core through the initial thread's SPSR.
}

// A pure read, as the seam requires: the counter is 64-bit and monotonic in hardware, so
// there is no wrap to extend and no anchor to keep, and ticks*16 needs 584 years to overflow.
uint64_t arch_clock_now(void)
{
    return counter_now() * g_ns_per_tick;
}

// CNTP_CVAL_EL0 is an absolute compare, so the write is idempotent and no armed-deadline
// dedup is owed. The division is what keeps a UINT64_MAX deadline from overflowing, where
// a multiply would not.
void arch_timer_arm(uint64_t deadline_ns)
{
    uint64_t const ticks = deadline_ns / g_ns_per_tick;
    __asm volatile("msr cntp_cval_el0, %0" ::"r"(ticks));
    __asm volatile("msr cntp_ctl_el0, %0" ::"r"(CNTP_CTL_ENABLE));
    // A deadline already past leaves the compare met, which asserts the timer's output now.
}

// Disarm has to mean no callback fires. Clearing ENABLE deasserts a level-driven output, so
// the GIC's pending state follows it down; ICPENDR covers a pend latched while masked.
void arch_timer_disarm(void)
{
    __asm volatile("msr cntp_ctl_el0, %0" ::"r"(uint64_t(0)));
    kickos_armv8a_gic_clear_pending(PPI_EL1_PHYS_TIMER);
}

// Rule 7. Only the GIC is here: the timebase is the architected generic timer, reached
// through system registers, and so are the translation controls, so neither is nameable by
// a grant. This machine has no clock or reset gates.
size_t arch_reserved_blocks(struct arch_reserved_block* out, size_t max)
{
    static struct arch_reserved_block const blocks[] = {
        {GICD_BASE, 0x10000u}, // distributor
        {GICC_BASE, 0x10000u}, // CPU interface
    };
    size_t n = sizeof(blocks) / sizeof(blocks[0]);
    if (n > max)
    {
        n = max;
    }
    for (size_t i = 0; i < n; i++)
    {
        out[i] = blocks[i];
    }
    return n;
}

// The PL011 comes out of QEMU's reset already enabled at the machine's default baud, so
// the polled path needs no bring-up.
void arch_console_write(char const* buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        uint32_t spin = 0;
        while ((*r32p(UART_FR) & UART_FR_TXFF) != 0 and spin < UART_POLL_BOUND)
        {
            spin++;
        }
        *r32p(UART_DR) = static_cast<uint32_t>(static_cast<unsigned char>(buf[i]));
    }
}

// TXFF says the FIFO can take a byte; BUSY says the device is still clocking one out, which
// is what arch_shutdown and a clock retune actually ask. UNWITNESSABLE HERE, and kept anyway:
// QEMU's PL011 hands each byte to its chardev on the register write, so BUSY never reads set
// and a truncation cannot be produced. Taking the fallback instead would ASSERT that this
// console cannot outrun a shutdown, which the FIFO and shift register make false.
void arch_console_flush_sync(void)
{
    uint32_t spin = 0;
    while ((*r32p(UART_FR) & UART_FR_BUSY) != 0 and spin < UART_POLL_BOUND)
    {
        spin++;
    }
}

// The exit status is what lets the harness tell a fault from a hang: a spin here makes every
// gate that should FAIL time out instead. AArch64 SYS_EXIT takes a POINTER to a two-field
// block where AArch32 passes the reason in the register.
void arch_shutdown(int status)
{
    uint64_t block[2];
    block[0] = ADP_Stopped_ApplicationExit;
    block[1] = static_cast<uint64_t>(static_cast<unsigned>(status));
    semihost(SYS_EXIT, block);
    // Reached only when nothing is listening for semihosting calls.
    __asm volatile("msr daifset, #0xf" ::: "memory");
    while (true)
    {
        __asm volatile("wfi");
    }
}

// startup.S branches here when the handover was not at EL1. Runs before .data and .bss, so
// it touches neither, and inside the boot span, so it reaches neither arch_console_write nor
// kfault_terminate: both are kernel text and only a walk names those.
KICKOS_BOOT_TEXT void kickos_arm64_bad_el(unsigned long el)
{
    char const digit = static_cast<char>('0' + (el & 3));
    boot_console_write(BAD_EL_HEAD, sizeof(BAD_EL_HEAD) - 1);
    boot_console_write(&digit, 1);
    boot_console_write(BAD_EL_TAIL, sizeof(BAD_EL_TAIL) - 1);

    // 16-byte aligned: every access is Device-nGnRnE while the MMU is off, which faults on
    // an unaligned one whatever SCTLR_EL1.A says, and the compiler stores this pair wide.
    uint64_t block[2] __attribute__((aligned(16)));
    block[0] = ADP_Stopped_ApplicationExit;
    block[1] = KICKOS_FATAL_STATUS;
    semihost(SYS_EXIT, block);

    // Reached only when nothing is listening for semihosting calls.
    __asm volatile("msr daifset, #0xf" ::: "memory");
    while (true)
    {
        __asm volatile("wfi");
    }
}

void Reset_Handler(void)
{
    uint32_t* src = &_sidata;
    uint32_t* dst = &_sdata;
    while (dst < &_edata)
    {
        *dst++ = *src++;
    }
    for (uint32_t* b = &_sbss; b < &_ebss; b++)
    {
        *b = 0;
    }
    for (uint32_t* b = &__kickos_appbss_start; b < &__kickos_appbss_end; b++)
    {
        *b = 0;
    }
    for (void (**fn)() = __init_array_start; fn != __init_array_end; fn++)
    {
        (*fn)();
    }
    arch_init();
    kickos::kmain(0, nullptr);
    arch_shutdown(0);
}

}
