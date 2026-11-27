// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// QEMU `virt` for AArch64 (Cortex-A53, EL1 bare metal): PL011 UART at 0x0900_0000 (or the
// second at 0x0904_0000 on an own-image node given /dev/uart1), GICv2 or GICv3, and the
// architected generic timer as the timebase. Run with `-cpu cortex-a53`.

#include <kickos/arch/arch.h>
#include <arch_reboot_tail.h>
#include <kickos/diag.h>
#include <kickos/arch/console_poll.h>
#include "crt_tail.h"
#include <kickos/console_tx.h>

#include <kickos/arch/amp_shared.h>

#include <chip_layout.h>
#include <kickos/chip_limits.h>
#include <board_pins.h>
#include <kickos/chip_mmap.h>
#include <kickos/config/limits.h>
#include <kickos/sys/abi.h>
#include <kickos/sys/atomic.h>

#include "a53.h"
#include "gic.h"
#if KICKOS_ARM64_GIC_VERSION == 3
#include "gicv3.h"
#else
#include "gicv2.h"
#endif
#include "smp_bringup.h"

#include <stdint.h>

// An own-image node given no console touches no UART on any path.
#if KICKOS_AMP_OWN_IMAGE && !KICKOS_CONSOLE_CHIP
#define VIRT_ARM64_CONSOLE 0
#else
#define VIRT_ARM64_CONSOLE 1
#endif

// The EL refusal path is linked into the identity-mapped boot span (virt_arm64.ld) and runs
// with SCTLR_EL1.M clear: it names the PL011 by its physical base rather than through dev_va
// and may not call out of the section, ld's veneer for an out-of-range branch still targeting
// the callee's high address.
#define KICKOS_BOOT_TEXT __attribute__((section(".text.init"), noinline, used))
#define KICKOS_BOOT_RODATA __attribute__((section(".rodata.init"), used))

namespace kickos
{
    void kprintf(char const* fmt, ...);
    void kputs(char const* s);
}

extern "C"
{
    void kfault_terminate(void) __attribute__((noreturn));
    void kpanic_enter(void);

#if KICKOS_NUM_CORES > 1
    // The first instruction a released core executes. It is linked in the identity-mapped
    // span (.text.init): PSCI hands a core a physical entry address and it arrives MMU off.
    void kickos_armv8a_secondary_entry(void);

    // Set by a core once it executes its own code: arrived, as against the SUCCESS PSCI
    // returns for a core it merely started.
    extern kickos::Atomic<uint8_t, kickos::Order::RELAXED> kickos_armv8a_core_online[];

#if KICKOS_KERNEL_CORES > 1
    void kickos_doorbell_reachable(void);
#endif
#if defined(KICKOS_WITNESS_DOORBELL) && KICKOS_WITNESS_DOORBELL
    void kickos_doorbell_boot_check(void);
#endif
#endif
}

namespace
{
    using kickos::arm64::r32p;

#if defined(KICKOS_AMP_CONSOLE_UART)
#if KICKOS_AMP_CONSOLE_UART == 0
    constexpr uintptr_t UART_BASE = kickos::virt_arm64::mmap::UART0_BASE;
#elif KICKOS_AMP_CONSOLE_UART == 1
    constexpr uintptr_t UART_BASE = kickos::virt_arm64::mmap::UART1_BASE;
#else
#error "KICKOS_AMP_CONSOLES names a UART qemu-arm64 does not have: it has /dev/uart0 and /dev/uart1"
#endif
#else
    constexpr uintptr_t UART_BASE = KICKOS_BOARD_CONSOLE_BASE;
    static_assert(UART_BASE == kickos::virt_arm64::mmap::UART0_BASE
                      or UART_BASE == kickos::virt_arm64::mmap::UART1_BASE,
                  "the board's console is not a UART of this machine");
#endif
    constexpr uintptr_t UART_DR = UART_BASE + 0x00;
    constexpr uintptr_t UART_FR = UART_BASE + 0x18;
    constexpr uint32_t UART_FR_TXFF = 1u << 5;
    constexpr uint32_t UART_FR_BUSY = 1u << 3;

    // A bound, not a timing: the flush and the writer sit on the panic and shutdown paths,
    // where a wedged UART must cost a dropped tail rather than a hang.
    constexpr uint32_t UART_POLL_BOUND = 100000;

#if VIRT_ARM64_CONSOLE
    int pl011_tx_slot_free(void) { return (*r32p(UART_FR) & UART_FR_TXFF) == 0; }
    void pl011_tx_push(uint8_t b) { *r32p(UART_DR) = static_cast<uint32_t>(b); }
    void pl011_tx_irq_enable(void) {}
    void pl011_tx_irq_disable(void) {}

    console_tx_backend const pl011_console_backend = {
        pl011_tx_slot_free, pl011_tx_push, pl011_tx_irq_enable, pl011_tx_irq_disable,
        &kickos_console_tx_storage};
#endif

    KICKOS_BOOT_TEXT void boot_console_write(char const* buf, size_t n)
    {
#if VIRT_ARM64_CONSOLE
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
#else
        (void)buf;
        (void)n;
#endif
    }

    // File scope: as locals the compiler may stage them on the stack through memcpy, and this
    // path runs before the C runtime. In the boot span because the reader runs there.
    KICKOS_BOOT_RODATA char const BAD_EL_HEAD[] = "KickOS: qemu-arm64 entered at EL";
    KICKOS_BOOT_RODATA char const BAD_EL_TAIL[] = ", and this port implements EL1 only\n";
    char const BAD_CNTFRQ[] =
        "KickOS: qemu-arm64 CNTFRQ_EL0 does not divide a second exactly\n";

    using kickos::virt_arm64::mmap::GICD_BASE;
#if KICKOS_ARM64_GIC_VERSION == 3
    // Two 64 KB frames per core, RD_base then SGI_base, contiguous from the first core.
    using kickos::virt_arm64::mmap::GICR_BASE;
    constexpr uintptr_t GICR_STRIDE = 0x20000;
    // `virt` instantiates a redistributor per core `-smp` gave it, so this is the machine's
    // core count, not the count this image drives. On a die the two are unrelated.
    constexpr int GICR_COUNT = KICKOS_DOORBELL_CORES;
#else
    using kickos::virt_arm64::mmap::GICC_BASE;
#endif

    // QEMU `virt` holds every core but the first inside its own PSCI implementation: a
    // secondary's first instruction is the one CPU_ON names, and a reset is SYSTEM_RESET.
    //
    // The conduit is a chip constant: QEMU's `/psci` node says "hvc" under `-M virt` and "smc"
    // under `-M virt,virtualization=on`, and startup.S admits a handover at EL1 alone, the
    // configuration whose method is "hvc".
    //
    // SMCCC: the function identifier in x0, the arguments in x1..x3, the result back in x0;
    // x4..x17 are the callee's scratch.
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

    constexpr uint32_t PSCI_FN_SYSTEM_RESET = 0x84000009u;

// Built for a shared kernel releasing its secondaries and for a node releasing its peers.
#if (KICKOS_NUM_CORES > 1 || KICKOS_AMP_OWN_IMAGE)
    // The SMC64 identifier: the variant whose entry-point argument is 64 bits wide.
    constexpr uint32_t PSCI_FN64_CPU_ON = 0xC4000003u;
    constexpr long PSCI_SUCCESS = 0;

    // MPIDR_EL1 affinity (DDI 0487 M.b section D24.2.137): Aff0 [7:0], Aff1 [15:8],
    // Aff2 [23:16], Aff3 [39:32]; a PSCI target_cpu carries zero in every other bit.
    constexpr uint64_t MPIDR_AFFINITY = 0x000000FF00FFFFFFull;

    // A two-digit index and a sixteen-digit status: the formatter takes no width.
    char const BAD_ENTRY[] = "KickOS: qemu-arm64 secondary entry is not identity-linked: 0x%lx\n";
    char const BAD_CPU_ON[] = "KickOS: qemu-arm64 PSCI CPU_ON refused core %x%x, status 0x%lx\n";

#if KICKOS_AMP_OWN_IMAGE
    char const BAD_NODE_ON[] = "KickOS: PSCI CPU_ON refused for AMP node %x%x, status 0x%lx\n";

    constexpr uintptr_t AMP_SLICE[KICKOS_AMP_NODES] = KICKOS_AMP_SLICE_LIST;

    // The sum of the slices below `node`, in the form a uniform list folds to node * AMP_SLICE[0].
    constexpr uintptr_t amp_node_offset(uint32_t node)
    {
        uintptr_t offset = static_cast<uintptr_t>(node) * AMP_SLICE[0];
        for (uint32_t k = 1; k < KICKOS_AMP_NODES; k++)
        {
            if (k < node)
            {
                offset += AMP_SLICE[k] - AMP_SLICE[0];
            }
        }
        return offset;
    }
    static_assert(amp_node_offset(KICKOS_AMP_NODE_ID) == KICKOS_AMP_IMAGE_OFFSET
                      and amp_node_offset(KICKOS_AMP_NODES) == KICKOS_AMP_SLICES_SIZE,
                  "the slice list and the geometry this image is linked at disagree");

    // The partition primary starts every other node at its image base, the partition's base plus
    // the slices below it.
    //
    // Called after the window is seated: a peer released before the shared region is zeroed
    // would publish into bytes its starter is about to clear.
    void release_partition_peers(void)
    {
        if (KICKOS_AMP_NODE_ID != 0)
        {
            return;
        }
        uint64_t mpidr = 0;
        __asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
        uint64_t const cluster = mpidr & MPIDR_AFFINITY & ~uint64_t(0xFF);

        // Every write this node made for a peer, before the release that lets it read them.
        __asm volatile("dsb sy" ::: "memory");

        for (uint32_t node = 0; node < KICKOS_AMP_NODES; node++)
        {
            if (node == KICKOS_AMP_NODE_ID)
            {
                continue;
            }
            uintptr_t const entry = static_cast<uintptr_t>(KICKOS_AMP_PARTITION_BASE)
                                    + amp_node_offset(node);
            uint64_t const core = kickos_amp_node_core(node);
            long const rc = psci_hvc(PSCI_FN64_CPU_ON, cluster | core, entry, 0);
            if (rc != PSCI_SUCCESS)
            {
                kpanic_enter();
                kickos::kprintf(BAD_NODE_ON, node >> 4, node & 0xFu, rc);
                kfault_terminate();
            }
        }
    }
#endif

#endif
#if KICKOS_NUM_CORES > 1
    char const NO_ARRIVAL[] = "KickOS: qemu-arm64 secondary never reached its entry: core %x%x\n";

    constexpr uint64_t SECONDARY_ARRIVAL_NS = kickos::ARM64_BRINGUP_WAIT_NS;

    // A core short of KICKOS_NUM_CORES is fatal: the count sizes every per-core array.
    void release_secondaries(void)
    {
        uint64_t mpidr = 0;
        __asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));

        // A released core fetches from a physical address with its MMU off: CPU_ON would answer
        // SUCCESS for a high-half entry and the core would never start.
        uintptr_t const entry = reinterpret_cast<uintptr_t>(&kickos_armv8a_secondary_entry);
        if (entry >= reinterpret_cast<uintptr_t>(__kickos_arm64_va_base))
        {
            kpanic_enter();
            kickos::kprintf(BAD_ENTRY, entry);
            kfault_terminate();
        }

        // Orders every write the primary published ahead of the release. A released core reads
        // with its caches off, so data handed over owes a clean to the point of coherency.
        __asm volatile("dsb sy" ::: "memory");

        // target_cpu is an affinity value, not an index: Aff0 alone varies here and the running
        // core supplies the rest, which is exact on this board (`reg` 0..3 at `-smp 4`).
        uint64_t const cluster = mpidr & MPIDR_AFFINITY & ~uint64_t(0xFF);
        for (uint32_t index = 1; index < KICKOS_NUM_CORES; index++)
        {
            // context_id is zero: a secondary reads its identity out of MPIDR_EL1.
            long const rc = psci_hvc(PSCI_FN64_CPU_ON, cluster | index, entry, 0);
            if (rc != PSCI_SUCCESS)
            {
                kpanic_enter();
                kickos::kprintf(BAD_CPU_ON, index >> 4, index & 0xFu, rc);
                kfault_terminate();
            }
        }

        // Released is not arrived: a core handed a bad entry, stack or translation is started
        // and never reaches its own code. The online byte is the arrival witness, read from
        // zero because every core publishes its own.
        for (uint32_t index = 0; index < KICKOS_NUM_CORES; index++)
        {
            uint64_t const deadline = arch_clock_now() + SECONDARY_ARRIVAL_NS;
            while (kickos_armv8a_core_online[index] == 0)
            {
                if (arch_clock_now() > deadline)
                {
                    kpanic_enter();
                    kickos::kprintf(NO_ARRIVAL, index >> 4, index & 0xFu);
                    kfault_terminate();
                }
                __asm volatile("yield" ::: "memory");
            }
        }

        // The positive statement a gate reads, a TAP comment so it is legal ahead of a plan.
        kickos::kprintf("# smp: %x core(s) online\n", static_cast<unsigned>(KICKOS_NUM_CORES));

#if KICKOS_KERNEL_CORES > 1
        kickos_doorbell_reachable();
#endif
#if defined(KICKOS_WITNESS_DOORBELL) && KICKOS_WITNESS_DOORBELL
        // After arrival: the check needs every core's interface live and its target bit
        // published.
        kickos_doorbell_boot_check();
#endif
    }
#endif
}

static_assert(KICKOS_LAYOUT_LINE_TIMER_EL1_PHYS == kickos::arm64::PPI_EL1_PHYS_TIMER,
              "the timer the GIC dispatch takes is not the line the chip file gives the kernel");

extern "C"
{

#if KICKOS_ARM64_GIC_VERSION == 3
struct kickos_gicv3_map const kickos_gicv3 = {
    GICD_BASE,
    GICR_BASE,
    GICR_STRIDE,
    GICR_COUNT,
    KICKOS_MAX_IRQ,
    kickos::arm64::PPI_EL1_PHYS_TIMER,
};
#else
struct kickos_gicv2_map const kickos_gicv2 = {
    GICD_BASE,
    GICC_BASE,
    KICKOS_MAX_IRQ,
    kickos::arm64::PPI_EL1_PHYS_TIMER,
};
#endif

// SYSTEM_RESET does not return (PSCI, DEN0022 5.10); a return carrying an error code
// (NOT_SUPPORTED where the firmware lacks the call) reset nothing. A firmware that returns
// anything else may still land the reset late.
int arch_reboot(uint32_t outcome)
{
    if (outcome != KOS_SHUTDOWN_REBOOT)
    {
        return -KOS_ENOSYS;
    }
    if (psci_hvc(PSCI_FN_SYSTEM_RESET, 0, 0, 0) < 0)
    {
        return -KOS_ENOSYS;
    }
    kickos::arch::reset_settle();
    kickos::arch::reboot_failed(kickos::diag::kResetNotTaken);
}

#if KICKOS_AMP_OWN_IMAGE
void arch_amp_release_peers(void)
{
    release_partition_peers();
}
#endif

void arch_init(void)
{
    uint64_t const freq = kickos_armv8a_timebase_init();
    if (freq == 0)
    {
        kpanic_enter();
        kickos::kputs(BAD_CNTFRQ);
        kfault_terminate();
    }

    // The distributor is the machine's, so under one image per node only the partition primary
    // writes it: a peer re-entering would clear GICD_CTLR and mask every shared line while the
    // primary is delivering on them. The primary runs it before releasing any peer.
    //
    // The test is the running core, not the node index: a peer's image booted alone (as the
    // qemu-arm64-amp2-n1 boot gate does) is the only kernel on the machine and owns the
    // distributor.
#if KICKOS_AMP_OWN_IMAGE
    uint64_t boot_mpidr = 0;
    __asm volatile("mrs %0, mpidr_el1" : "=r"(boot_mpidr));
    if ((boot_mpidr & MPIDR_AFFINITY) == 0)
#endif
    {
        kickos_armv8a_gic_dist_init();
    }
    kickos_armv8a_direct_map_record();
    kickos_armv8a_percore_init();

#if KICKOS_NUM_CORES > 1
    release_secondaries();
#endif

    // PSTATE.I stays SET: interrupts first reach the core through the initial thread's SPSR.
}

// The PL011 comes out of QEMU's reset enabled: the polled path needs no bring-up.
bool arch_console_write_sync(char const* buf, size_t n)
{
#if VIRT_ARM64_CONSOLE
    return kickos::console_poll::write<pl011_tx_slot_free, pl011_tx_push>(buf, n);
#else
    (void)buf;
    (void)n;
    return false;
#endif
}

console_tx_backend const* arch_console_tx_backend(int* irq_line)
{
    *irq_line = -1;
#if VIRT_ARM64_CONSOLE
    return &pl011_console_backend;
#else
    return nullptr;
#endif
}

// BUSY says the device is still clocking a byte out, which is what arch_shutdown asks. QEMU's
// PL011 hands each byte to its chardev on the register write, so BUSY never reads set there.
// The wait stays because the real FIFO and shifter can outrun a shutdown.
void arch_console_flush_sync(void)
{
#if VIRT_ARM64_CONSOLE
    uint32_t spin = 0;
    while ((*r32p(UART_FR) & UART_FR_BUSY) != 0 and spin < UART_POLL_BOUND)
    {
        spin++;
    }
#endif
}

// Runs before .data and .bss, in the boot span: it reaches neither the console nor
// kfault_terminate, which are kernel text.
KICKOS_BOOT_TEXT void kickos_arm64_bad_el(unsigned long el)
{
    kickos::arm64::bad_el_exit(el, boot_console_write, BAD_EL_HEAD, sizeof(BAD_EL_HEAD) - 1,
                               BAD_EL_TAIL, sizeof(BAD_EL_TAIL) - 1);
}

// Anchors this archive member (arch/CMakeLists.txt, "Seam fallbacks"): startup.S branches
// here, so the link extracts this object while scanning the chip archive, before the arch
// archive that carries arch_console_flush_sync's fallback.
void Reset_Handler(void)
{
    kickos::arm64::crt_ranges_init();
    // Ahead of arch_init, which publishes this core's affinity into that region. A peer's
    // call is a no-op by node index.
    arch_amp_shared_zero();
    kickos_crt_tail();
}

}
