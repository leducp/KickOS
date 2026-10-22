// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// QEMU `virt` for RV64: NS16550A UART at 0x1000_0000, the CLINT at 0x0200_0000 and the
// SiFive test finisher at 0x0010_0000. Run with:
// qemu-system-riscv64 -M virt -bios none -nographic -kernel <elf>.
//
// THE MMIO ADDRESSES BELOW ARE VIRTUAL: KICKOS_RV64_VA_BASE plus the physical address, the
// same delta the kernel window uses (startup.S). Every leaf that names them is U-clear.
//
// THE TIMEBASE IS Sstc, A CHIP FACT: supervisor code owns its comparator and arch_timer_arm
// writes stimecmp directly. startup.S sets menvcfg.STCE and mcounteren.TM in machine mode,
// without which both CSR accesses below trap.
//
// startup.S leaves medeleg bit 9 undelegated, so a part without Sstc can reach firmware by
// ECALL-from-S.

#include <kickos/arch/arch.h>
#include <arch_reboot_tail.h>
#include <kickos/diag.h>
#include <kickos/arch/console_poll.h>
#include "crt_tail.h"
#include <kickos/console_tx.h>
#include <kickos/io/mmio.h>
#include <kickos/sys/abi.h>
#include <kickos/arch/clk_q32.h> // KICKOS_NS_PER_SEC (canonical 1e9 ns/sec)
#include <kickos/chip_limits.h>
#include <kickos/arch/doorbell_protocol.h>
#include <kickos/arch/rv64_doorbell.h>

#include <stdint.h>

#include "boot_layout.ld.h"

extern "C"
{
    void kickos_rv64_init(void);

    extern uint64_t kickos_rv64_root[];

    extern uint64_t kickos_rv64_window_l0[];

    void kickos_rv64_aspace_boot(uint64_t* boot_root, uint64_t* window_leaves,
                                 uintptr_t window_va, uintptr_t window_delta,
                                 arch_phys_addr_t pa_lo, arch_phys_addr_t pa_hi,
                                 unsigned phys_bits);

    extern uintptr_t _sidata, _sdata, _edata, _sbss, _ebss;

    // The app's zero range, in the app's own VIRTUAL addresses: no space maps that half yet, so
    // Reset_Handler reaches it through the kernel's alias of the same frames.
    extern uintptr_t __kickos_appbss_start[], __kickos_appbss_end[];
}

namespace
{
    // LINK-TIME WORDS: kernel text must not materialise an app-half address, and a missed one
    // links SILENTLY, medlow's absolute reach covering the app's base
    // (tests/static/check_riscv_kernel_apphalf.sh). VOLATILE keeps each a word the optimiser
    // cannot fold back into the reference it came from.
    uintptr_t* const volatile g_appbss_lo = __kickos_appbss_start;
    uintptr_t* const volatile g_appbss_hi = __kickos_appbss_end;

    // EVERY DEVICE IS NAMED AT THE KERNEL'S OWN ALIAS OF ITS PHYSICAL ADDRESS, one uniform
    // delta with the kernel window beside it (startup.S). Identity would put the device leaf in
    // a level-2 slot of the LOW half, which every space copies from the boot root, and refuse a
    // per-space mapping anywhere in the first gigabyte. The physical values are what a grant
    // names.
    constexpr uintptr_t DEV_VA = KICKOS_RV64_VA_BASE;

    // QEMU `virt` NS16550A. Byte registers: THR at +0, LSR at +5 with bit 5 = holding
    // register empty and bit 6 = transmitter empty (holding register AND shift register).
    constexpr uintptr_t UART0_PA = KICKOS_RV64_UART0_PA;
    constexpr uintptr_t UART0_BASE = DEV_VA + UART0_PA;
    constexpr uintptr_t UART_THR = UART0_BASE + 0;
    constexpr uintptr_t UART_LSR = UART0_BASE + 5;
    constexpr uint8_t UART_LSR_THRE = 1u << 5;
    constexpr uint8_t UART_LSR_TEMT = 1u << 6;
    constexpr uint32_t UART_POLL_BOUND = KICKOS_RV64_UART_POLL_BOUND;

    int ns16550_tx_slot_free(void) { return (r8(UART_LSR) & UART_LSR_THRE) != 0; }
    void ns16550_tx_push(uint8_t b) { r8(UART_THR) = b; }
    void ns16550_tx_irq_enable(void) {}
    void ns16550_tx_irq_disable(void) {}

    console_tx_backend const ns16550_console_backend = {
        ns16550_tx_slot_free, ns16550_tx_push, ns16550_tx_irq_enable, ns16550_tx_irq_disable,
        &kickos_console_tx_storage};

    // QEMU `virt` CLINT: the per-hart msip words are the cross-hart doorbell. The address is in
    // boot_layout.ld.h because startup.S needs it too.
    constexpr uintptr_t CLINT_PA = KICKOS_RV64_CLINT_PA;

    constexpr uint64_t TIME_HZ = KICKOS_CHIP_TIMEBASE_HZ;
    constexpr uint64_t NS_PER_TICK = kickos::KICKOS_NS_PER_SEC / TIME_HZ;
    static_assert(kickos::KICKOS_NS_PER_SEC % TIME_HZ == 0,
                  "a timebase that does not divide a second exactly makes NS_PER_TICK lossy");

    // stimecmp is an absolute compare and STIP tracks `time >= stimecmp` continuously, so
    // all-ones is the one value that asserts nothing for the life of the image.
    constexpr uint64_t STIMECMP_NEVER = ~static_cast<uint64_t>(0);

#if KICKOS_NUM_CORES > 1
    // Sized far over rather than tuned: under emulation without icount the guest clock tracks
    // HOST time, so a contended host spends this budget while the guest barely executes.
    constexpr uint64_t RV64_BRINGUP_WAIT_NS = 5ull * 1000ull * 1000ull * 1000ull;

    char const NO_ARRIVAL[] = "KickOS: hart never reached the supervisor park: hart %u\n";
#endif

    // QEMU `virt` SiFive test finisher: the only way this machine stops itself with a status a
    // harness can read.
    constexpr uintptr_t TEST_FINISHER = DEV_VA + KICKOS_RV64_TEST_FINISHER_PA;
    constexpr uint32_t FINISHER_PASS = KICKOS_RV64_FINISHER_PASS;
    constexpr uint32_t FINISHER_FAIL = KICKOS_RV64_FINISHER_FAIL;
    constexpr uint32_t FINISHER_RESET = 0x7777;
}

namespace kickos
{
    void kprintf(char const* fmt, ...);
}

extern "C"
{

#if KICKOS_NUM_CORES > 1
// startup.S's .smpboot carve: one doubleword per hart. volatile, not Atomic: Atomic refuses a
// type wider than 4 bytes.
extern volatile uint64_t kickos_rv64_hart_release[KICKOS_NUM_CORES];

extern "C" void kfault_terminate(void) __attribute__((noreturn));
extern "C" void kpanic_enter(void);

// Releases every parked hart and waits, bounded, for each to reach the supervisor park.
// Nothing before this point runs on more than one hart.
void release_secondaries(void)
{
    // Everything this hart wrote for a secondary publishes before the word that frees it.
    __asm volatile("fence rw, w" ::: "memory");
    uint32_t peers = 0;
    for (uint32_t index = 1; index < KICKOS_NUM_CORES; index++)
    {
        kickos_rv64_hart_release[index] = 1u;
        peers |= 1u << index;
    }
    // THE WORD IS THE STATE AND THE RAISE IS THE EDGE: a hart parked in WFI observes no store,
    // so the word alone would leave every secondary asleep for good.
    kickos_rv64_doorbell_send(peers);

    uint64_t const deadline = arch_clock_now() + RV64_BRINGUP_WAIT_NS;
    for (uint32_t index = 1; index < KICKOS_NUM_CORES; index++)
    {
        while (kickos_rv64_core_online_read(index) == 0u)
        {
            if (arch_clock_now() > deadline)
            {
                kpanic_enter();
                kickos::kprintf(NO_ARRIVAL, index);
                kfault_terminate();
            }
        }
    }

    kickos::kprintf("# smp: %x core(s) online\n", static_cast<unsigned>(KICKOS_NUM_CORES));
}
#endif

void arch_init(void)
{
    // stimecmp's reset value is not architecturally all-ones, so the comparator is parked
    // BEFORE kickos_rv64_init enables sie.STIE: a comparator left at zero has STIP asserted
    // already and the first sret to a thread would take a timer trap with no deadline set.
    arch_timer_disarm();

    kickos_rv64_init();

    kickos_rv64_aspace_boot(kickos_rv64_root, kickos_rv64_window_l0,
                            KICKOS_RV64_WINDOW_VA, KICKOS_RV64_VA_BASE,
                            KICKOS_RV64_DRAM_BASE,
                            KICKOS_RV64_DRAM_BASE + KICKOS_RV64_KERNEL_WINDOW_SIZE,
                            KICKOS_RV64_PHYS_ADDR_BITS);

#if KICKOS_NUM_CORES > 1
    // AFTER the boot hart's own supervisor state and address space exist: a secondary released
    // here runs a kickos_rv64_init of its own against tables this hart has already installed.
    release_secondaries();
#if KICKOS_KERNEL_CORES > 1
    kickos_doorbell_boot_check();
#endif
#endif
}

// A pure read, as the seam requires. The counter is 64-bit at this XLEN, so it comes back in
// one csrr with no high-half re-read, and ticks*100 needs 584 years to overflow.
uint64_t arch_clock_now(void)
{
    uint64_t t = 0;
    __asm volatile("csrr %0, time" : "=r"(t));
    return t * NS_PER_TICK;
}

// Dividing rather than multiplying keeps a UINT64_MAX deadline from overflowing. A deadline
// already past leaves the compare met, which asserts STIP now.
void arch_timer_arm(uint64_t deadline_ns)
{
    uint64_t const ticks = deadline_ns / NS_PER_TICK;
    __asm volatile("csrw stimecmp, %0" ::"r"(ticks) : "memory");
}

// STIP follows the comparator with no latch of its own, so parking it deasserts the pending
// bit and no sip write is owed.
void arch_timer_disarm(void)
{
    __asm volatile("csrw stimecmp, %0" ::"r"(STIMECMP_NEVER) : "memory");
}

#if KICKOS_NUM_CORES > 1
// A write of 1 to a hart's CLINT msip word raises a MACHINE software interrupt on it, which
// mideleg cannot delegate. The machine-mode trampoline in startup.S clears the word and
// re-raises it as mip.SSIP.
//
// SUPERVISOR MODE MAY DO THIS WRITE WITH NO MACHINE-MODE LEG (measurements on
// kickos_rv64_doorbell_send in arch/riscv/rv64imac/include/kickos/arch/rv64_doorbell.h).
//
// THE DENSE CORE INDEX IS THE HART INDEX HERE: startup.S seats each hart's row from its own
// mhartid, so msip[index] is that core's word.
void kickos_rv64_doorbell_send(uint32_t cores)
{
    if (cores == 0)
    {
        return;
    }
    // THE SUCCESSOR IS A DEVICE STORE, SO THE SUCCESSOR SET MUST NAME THE I/O DOMAIN: the CLINT
    // sits in an I/O PMA, and a successor set of `w` names memory writes only and orders
    // nothing against the msip store. Predecessor `rw` is this hart's own memory work;
    // successor `ow` names the device write that raises the doorbell and the memory writes
    // after it.
    __asm volatile("fence rw, ow" ::: "memory");
    for (uint32_t index = 0; index < KICKOS_NUM_CORES; index++)
    {
        if ((cores & (1u << index)) != 0)
        {
            *reinterpret_cast<volatile uint32_t*>(DEV_VA + CLINT_PA + 4u * index) = 1u;
        }
    }
}
#endif

bool arch_console_write_sync(char const* buf, size_t n)
{
    return kickos::console_poll::write<ns16550_tx_slot_free, ns16550_tx_push>(buf, n);
}

console_tx_backend const* arch_console_tx_backend(int* irq_line)
{
    *irq_line = -1; // no TX line is routed through the PLIC; the producer drains
    return &ns16550_console_backend;
}

// THRE says the holding register can take a byte; TEMT says the FIFO and the shift register
// are both idle, which is what arch_shutdown and a clock retune ask. UNWITNESSABLE HERE: QEMU's
// NS16550A hands each byte to its chardev on the register write and reports TEMT set with it.
// The wait stays because the part's 16-byte TX FIFO and shift register can outrun a shutdown.
void arch_console_flush_sync(void)
{
    uint32_t spin = 0;
    while ((r8(UART_LSR) & UART_LSR_TEMT) == 0 and spin < UART_POLL_BOUND)
    {
        spin++;
    }
}

void arch_shutdown(int status)
{
    uint32_t word = FINISHER_PASS;
    if (status != 0)
    {
        word = FINISHER_FAIL | (static_cast<uint32_t>(status) << 16);
    }
    r32(TEST_FINISHER) = word;
    __asm volatile("csrci sstatus, 0x2" ::: "memory");
    while (true)
    {
        __asm volatile("wfi");
    }
}

// The finisher's reset word restarts the machine: no SBI answers under `-bios none`.
int arch_reboot(uint32_t outcome)
{
    if (outcome != KOS_SHUTDOWN_REBOOT)
    {
        return -KOS_ENOSYS;
    }
    __asm volatile("csrci sstatus, 0x2" ::: "memory");
    r32(TEST_FINISHER) = FINISHER_RESET;
    kickos::arch::reset_settle();
    kickos::arch::reboot_failed(kickos::diag::kResetNotTaken);
}

void Reset_Handler(void)
{
    uintptr_t* src = &_sidata;
    uintptr_t* dst = &_sdata;
    while (dst < &_edata)
    {
        *dst++ = *src++;
    }
    for (uintptr_t* b = &_sbss; b < &_ebss; b++)
    {
        *b = 0;
    }
    // THE ZERO RANGE IS SPLIT AND THE COPY RANGE IS NOT. The app's .data is already at the
    // load address its mapping will point at, so nothing copies it; its .bss is NOLOAD and
    // still owes a zero, taken here through the kernel window rather than at the app virtual
    // address, which no root maps yet.
    uintptr_t const app_alias = KICKOS_RV64_APP_LOAD_DELTA + KICKOS_RV64_VA_BASE;
    uintptr_t* const abss = reinterpret_cast<uintptr_t*>(
        reinterpret_cast<uintptr_t>(g_appbss_lo) + app_alias);
    uintptr_t* const abss_end = reinterpret_cast<uintptr_t*>(
        reinterpret_cast<uintptr_t>(g_appbss_hi) + app_alias);
    for (uintptr_t* b = abss; b < abss_end; b++)
    {
        *b = 0;
    }
    kickos_crt_tail();
}

}
