// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// QEMU `virt` for RV64: NS16550A UART at 0x1000_0000, the CLINT at 0x0200_0000 and the
// SiFive test finisher at 0x0010_0000. Run with:
// qemu-system-riscv64 -M virt -bios none -nographic -kernel <elf>.
//
// THE MMIO ADDRESSES BELOW ARE VIRTUAL, and they are the kernel's own alias of a physical
// register rather than its identity: KICKOS_RV64_VA_BASE plus the physical address, the same
// delta the kernel window uses (startup.S). Every leaf that names them is U-clear, so an
// access below is reachable from supervisor mode only.
//
// THE TIMEBASE IS Sstc AND THAT IS A CHIP FACT, NOT AN ARCH ONE. This core carries the
// extension, so supervisor code owns its own comparator: arch_timer_arm writes stimecmp
// directly and no machine-mode shim stays resident. startup.S sets menvcfg.STCE and
// mcounteren.TM in machine mode, without which both CSR accesses below trap.
//
// A PART WITHOUT Sstc SUPPLIES ITS OWN PATH FROM HERE: the C906 reaches firmware by
// ECALL-from-S, and startup.S leaves medeleg bit 9 UNDELEGATED, so that ecall goes to
// machine mode where firmware wants it instead of into stvec. The trap entry spends no
// cause on it.
//
// Virtual board, no pads; arch_pinmux_set is left to the declining ENOSYS fallback.

#include <kickos/arch/arch.h>
#include <kickos/arch/clk_q32.h> // KICKOS_NS_PER_SEC (canonical 1e9 ns/sec)
#include <kickos/arch/rv64_doorbell.h>

#include <stdint.h>

#include "boot_layout.ld.h"

namespace kickos
{
    int kmain(int argc, char** argv);
}

extern "C"
{
    // arch/riscv/rv64imac: stvec, the trap stack and the supervisor-mode confirmation.
    void kickos_rv64_init(void);

    // startup.S: the one root, already in satp at the configured paging mode.
    extern uint64_t kickos_rv64_root[];

    // startup.S: the transient window's level-0 table, whose leaves the map editor writes.
    extern uint64_t kickos_rv64_window_l0[];

    // arch/riscv/rv64imac/aspace_rv64imac.cc: the boot root, the window, the kernel window's
    // output range and the physical extent this platform implements, handed over before the
    // first space exists.
    void kickos_rv64_aspace_boot(uint64_t* boot_root, uint64_t* window_leaves,
                                 uintptr_t window_va, uintptr_t window_delta,
                                 arch_phys_addr_t pa_lo, arch_phys_addr_t pa_hi,
                                 unsigned phys_bits);

    // Linker-script symbols (virt_rv64.ld).
    extern uintptr_t _sidata, _sdata, _edata, _sbss, _ebss;
    extern void (*__init_array_start[])();
    extern void (*__init_array_end[])();

    // The app's zero range (virt_rv64.ld), in the app's own VIRTUAL addresses. Reset_Handler
    // runs before any space exists, so nothing maps that half yet and these are reached
    // through the kernel's own alias of the same frames instead.
    extern uintptr_t __kickos_appbss_start[], __kickos_appbss_end[];

    // Nominal core clock (Hz). Matches the `virt` mtime rate.
    uint32_t SystemCoreClock = 10000000u;
}

namespace
{
    // LINK-TIME WORDS: kernel text must not materialise an app-half address, and a missed one
    // links SILENTLY rather than failing, medlow's absolute reach covering the app's base
    // (tests/static/check_riscv_kernel_apphalf.sh). VOLATILE is what keeps each a word rather
    // than a value the optimiser folds back into the reference it came from.
    uintptr_t* const volatile g_appbss_lo = __kickos_appbss_start;
    uintptr_t* const volatile g_appbss_hi = __kickos_appbss_end;

    inline volatile uint8_t* r8p(uintptr_t a) { return reinterpret_cast<volatile uint8_t*>(a); }
    inline volatile uint32_t* r32p(uintptr_t a) { return reinterpret_cast<volatile uint32_t*>(a); }

    // EVERY DEVICE IS NAMED AT THE KERNEL'S OWN ALIAS OF ITS PHYSICAL ADDRESS, one uniform
    // delta with the kernel window beside it (startup.S maps physical 0 there). Identity would
    // put the device leaf in a level-2 slot of the LOW half, which every space copies from the
    // boot root, and that would refuse a per-space mapping anywhere in the first gigabyte.
    // The physical values below are what a grant names.
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

    // QEMU `virt` CLINT: the machine timer and the per-hart software interrupt. The timebase
    // here is the time/stimecmp CSR pair; the msip words are the cross-hart doorbell, and
    // boot_layout.ld.h is where the address lives because startup.S needs it too.
    constexpr uintptr_t CLINT_PA = KICKOS_RV64_CLINT_PA;

    // QEMU `virt` interrupt controller, named for arch_reserved_blocks and driven by nothing
    // here, the console being polled and the timebase a CSR pair.
    constexpr uintptr_t PLIC_PA = 0x0c000000;

    // The `virt` timebase, which both mtime and the time CSR are driven from.
    constexpr uint64_t TIME_HZ = 10000000ull;
    constexpr uint64_t NS_PER_TICK = kickos::KICKOS_NS_PER_SEC / TIME_HZ;
    static_assert(kickos::KICKOS_NS_PER_SEC % TIME_HZ == 0,
                  "a timebase that does not divide a second exactly makes NS_PER_TICK lossy");

    // stimecmp is an absolute compare and STIP tracks `time >= stimecmp` continuously, so
    // all-ones is the one value that asserts nothing for the life of the image.
    constexpr uint64_t STIMECMP_NEVER = ~static_cast<uint64_t>(0);

#if KICKOS_NUM_CORES > 1
    // Bring-up bound, sized far over rather than tuned: under emulation without icount the
    // guest clock tracks HOST time, so a contended host spends this budget while the guest
    // barely executes.
    constexpr uint64_t RV64_BRINGUP_WAIT_NS = 5ull * 1000ull * 1000ull * 1000ull;

    char const NO_ARRIVAL[] = "KickOS: hart never reached the supervisor park: hart ";
    char const SMP_HEAD[] = "# smp: ";
    char const SMP_TAIL[] = " core(s) online\n";
    char const SMP_NL[] = "\n";
    static_assert(KICKOS_NUM_CORES <= 9, "the core count is printed as one decimal digit");
#endif

    // QEMU `virt` SiFive test finisher: the only way this machine stops itself with a
    // status a harness can read. Named in boot_layout.ld.h, the pre-translation prologue
    // refusing an unimplemented paging mode through the same register.
    constexpr uintptr_t TEST_FINISHER = DEV_VA + KICKOS_RV64_TEST_FINISHER_PA;
    constexpr uint32_t FINISHER_PASS = KICKOS_RV64_FINISHER_PASS;
    constexpr uint32_t FINISHER_FAIL = KICKOS_RV64_FINISHER_FAIL;
}

extern "C"
{

#if KICKOS_NUM_CORES > 1
// startup.S's .smpboot carve: one doubleword per hart, and nothing else writes them.
extern volatile uint64_t kickos_rv64_hart_release[KICKOS_NUM_CORES];

extern "C" void kfault_terminate(void) __attribute__((noreturn));

// Lets every parked hart out of _start's release loop and waits, bounded, for each to reach the
// supervisor park. THE RELEASE WORD IS THE WHOLE PROTOCOL: startup.S left every secondary
// spinning on it, so nothing before this point runs on more than one hart.
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
                arch_console_write(NO_ARRIVAL, sizeof(NO_ARRIVAL) - 1);
                char const digit = static_cast<char>('0' + index);
                arch_console_write(&digit, 1);
                arch_console_write(SMP_NL, sizeof(SMP_NL) - 1);
                kfault_terminate();
            }
        }
    }

    arch_console_write(SMP_HEAD, sizeof(SMP_HEAD) - 1);
    char const count = static_cast<char>('0' + KICKOS_NUM_CORES);
    arch_console_write(&count, 1);
    arch_console_write(SMP_TAIL, sizeof(SMP_TAIL) - 1);
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
#if defined(KICKOS_ENABLE_SELFTEST) && KICKOS_KERNEL_CORES > 1
    kickos_rv64_doorbell_selfcheck();
#endif
#endif
}

// --- Tickless clock: the time CSR (10 MHz) -> ns ----------------------------
// A pure read, as the seam requires. The counter is 64-bit at this XLEN, so it comes back in
// one csrr with no high-half re-read, and ticks*100 needs 58 years to overflow.
uint64_t arch_clock_now(void)
{
    uint64_t t = 0;
    __asm volatile("csrr %0, time" : "=r"(t));
    return t * NS_PER_TICK;
}

// --- One-shot next-event timer: stimecmp (absolute) -------------------------
// Idempotent, so no armed-deadline dedup is owed. Dividing rather than multiplying is what
// keeps a UINT64_MAX deadline from overflowing. A deadline already past leaves the compare
// met, which asserts STIP now.
void arch_timer_arm(uint64_t deadline_ns)
{
    uint64_t const ticks = deadline_ns / NS_PER_TICK;
    __asm volatile("csrw stimecmp, %0" ::"r"(ticks) : "memory");
}

// Disarm has to mean no callback fires. STIP follows the comparator with no latch of its own,
// so parking it deasserts the pending bit and no sip write is owed.
void arch_timer_disarm(void)
{
    __asm volatile("csrw stimecmp, %0" ::"r"(STIMECMP_NEVER) : "memory");
}

#if KICKOS_NUM_CORES > 1
// --- The cross-hart doorbell's raise ----------------------------------------
// A write of 1 to a hart's CLINT msip word raises a MACHINE software interrupt on it, which
// mideleg cannot delegate (measured: writing all ones reads back 0x3666, bit 3 clear). The
// machine-mode trampoline in startup.S clears the word and re-raises it as mip.SSIP.
//
// SUPERVISOR MODE MAY DO THIS WRITE. PMP entry 0 grants the whole space and QEMU's CLINT gates
// on no privilege, both measured on this machine, so the send needs no machine-mode leg.
//
// THE DENSE CORE INDEX IS THE HART INDEX HERE: startup.S seats each hart's row from its own
// mhartid, so msip[index] is that core's word. A part whose ids are not dense would owe a
// published map instead.
void kickos_rv64_doorbell_send(uint32_t cores)
{
    if (cores == 0)
    {
        return;
    }
    // THE SUCCESSOR IS A DEVICE STORE, SO THE SUCCESSOR SET MUST NAME THE I/O DOMAIN. RISC-V's
    // FENCE carries separate predecessor and successor bits for memory (R, W) and for device
    // (I, O) accesses, and the CLINT sits in an I/O PMA: a successor set of `w` names memory
    // writes only and so orders nothing against the msip store below. Predecessor `rw` is this
    // hart's own memory work, the tables and the request cell it published; successor `ow`
    // names the device write that raises the doorbell, and the memory writes that follow it.
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

// Rule 7. Only the CLINT is here: the console is the UART a driver may be granted, and the
// timebase and the translation controls are CSRs, so none of those is nameable by a grant.
// THE PLIC IS ABSENT FROM THIS LIST AND THAT IS A GAP, not a judgement that it is safe: no
// driver on this board names any base, so an entry whose size nothing has checked against the
// machine would be a refusal nobody can witness. The size to use is the APERTURE from PLIC_PA
// to UART0_PA and not the register file's own length, which follows the hart count and lives
// in a device tree this port does not parse.
//
// PHYSICAL, not the alias the kernel reads a register through: what a grant names on a
// translating backend is an output address.
size_t arch_reserved_blocks(struct arch_reserved_block* out, size_t max)
{
    static struct arch_reserved_block const blocks[] = {
        {CLINT_PA, 0x10000u}, // msip + mtimecmp + mtime, all machine-mode
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

// The UART comes out of QEMU's reset already usable at the machine's default baud, so the
// polled path needs no bring-up. Bounded because the arch_console_write_sync fallback
// aliases this body onto the panic path, where a wedged UART must cost a dropped tail.
void arch_console_write(char const* buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        uint32_t spin = 0;
        while ((*r8p(UART_LSR) & UART_LSR_THRE) == 0 and spin < UART_POLL_BOUND)
        {
            spin++;
        }
        *r8p(UART_THR) = static_cast<uint8_t>(buf[i]);
    }
}

// THRE says the holding register can take a byte; TEMT says the FIFO and the shift register
// are both idle, which is what arch_shutdown and a clock retune actually ask. UNWITNESSABLE
// HERE, and kept anyway: QEMU's NS16550A hands each byte to its chardev on the register write
// and reports TEMT set with it, so a truncation cannot be produced. Taking the fallback
// instead would ASSERT that this console cannot outrun a shutdown, which the 16-byte TX FIFO
// and the shift register make false on the part.
void arch_console_flush_sync(void)
{
    uint32_t spin = 0;
    while ((*r8p(UART_LSR) & UART_LSR_TEMT) == 0 and spin < UART_POLL_BOUND)
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
    *r32p(TEST_FINISHER) = word;
    // If the finisher is absent, mask interrupts and park.
    __asm volatile("csrci sstatus, 0x2" ::: "memory");
    while (true)
    {
        __asm volatile("wfi");
    }
}

// --- C-runtime bring-up -----------------------------------------------------
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
    for (void (**fn)() = __init_array_start; fn != __init_array_end; fn++)
    {
        (*fn)();
    }
    arch_init();
    kickos::kmain(0, nullptr);
    arch_shutdown(0);
}

}
