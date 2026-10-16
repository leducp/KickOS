// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// x86_64 ring 3: what makes an unprivileged level reachable at all, and the fast syscall
// pair's registers.
//
// On this firmware every entry along the walk to the image, to its data and to conventional
// memory carries the user bit CLEAR, and the permission ANDs down the walk. The kernel leaves
// it that way: the firmware's map is the kernel half, supervisor-only, and a task reaches only
// what its own space maps in the user half (docs/design-m10-kernel-share.md section 1).
// The other direction is SMEP and SMAP, set on every core whose CPUID offers them and never
// lifted: no kernel path reads, writes or runs a user page.

#include <kickos/arch/desc.h>
#include <kickos/arch/arch.h>
#include <kickos/arch/regs.h>
#include <kickos/arch/ring3.h>
#include <kickos/chip_com1.h>

#include <stddef.h>
#include <stdint.h>

// switch.S. HIDDEN is load-bearing: -fpie emits a global-offset-table load for the address of
// an external function and `ld -m i386pep` neither builds that table nor relaxes the form
// (tools/check-x86_64-no-got.sh).
extern "C" __attribute__((visibility("hidden"))) void kickos_x86_64_syscall_entry(void);

namespace kickos::x86_64
{
    namespace
    {
        using namespace kickos::q35;

        constexpr uint32_t msr_efer = 0xc0000080;
        constexpr uint32_t msr_star = 0xc0000081;
        constexpr uint32_t msr_lstar = 0xc0000082;
        constexpr uint32_t msr_fmask = 0xc0000084;
        constexpr uint32_t msr_gs_base = 0xc0000101;
        constexpr uint32_t msr_kernel_gs_base = 0xc0000102;

        constexpr uint64_t efer_sce = 1ull << 0;

        constexpr uint64_t cr4_smep = 1ull << 20;
        constexpr uint64_t cr4_smap = 1ull << 21;
        static_assert((cr4_smep | cr4_smap) == cr4_guard, "ring3.h names the same two bits");
        // CPUID leaf 7 subleaf 0, EBX (Intel SDM Vol 2A, CPUID).
        constexpr uint32_t leaf7_ebx_smep = 1u << 7;
        constexpr uint32_t leaf7_ebx_smap = 1u << 20;

        // Asked of the core that sets the bits: the boot core's floor check says nothing of an AP.
        uint64_t guard_offered(void)
        {
            uint32_t a = 0;
            uint32_t b = 0;
            uint32_t c = 0;
            uint32_t d = 0;
            __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0u), "c"(0u));
            if (a < 7)
            {
                return 0;
            }
            __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7u), "c"(0u));
            uint64_t bits = 0;
            if ((b & leaf7_ebx_smep) != 0)
            {
                bits |= cr4_smep;
            }
            if ((b & leaf7_ebx_smap) != 0)
            {
                bits |= cr4_smap;
            }
            return bits;
        }

        alignas(64) cpu_block g_cpu[KICKOS_KERNEL_CORES] = {};

        // The boot core's CR4 as ring3_init read it back.
        uint64_t g_boot_cr4 = 0;

        cpu_block& local_cpu(void)
        {
            return g_cpu[arch_cpu_id()];
        }

        static_assert(sizeof(cpu_block) == KICKOS_X86_64_CPU_SIZE,
                      "switch.S addresses the block with literal displacements");
        static_assert(offsetof(cpu_block, kernel_sp) == KICKOS_X86_64_CPU_KERNEL_SP,
                      "switch.S loads the kernel stack at CPU_KERNEL_SP");
        static_assert(offsetof(cpu_block, user_rsp) == KICKOS_X86_64_CPU_USER_RSP,
                      "switch.S parks the caller's stack pointer at CPU_USER_RSP");
        static_assert(offsetof(cpu_block, sw_start) == KICKOS_X86_64_CPU_SW_START,
                      "switch.S timestamps the swap in this core's block");
        static_assert(offsetof(cpu_block, core_id) == KICKOS_X86_64_CPU_CORE_ID,
                      "arch_cpu_id reads the core ID from this GS offset");
        static_assert(KICKOS_X86_64_SEL_USER_CODE == sel_user_code,
                      "switch.S stamps the user code selector as an immediate");
        static_assert(KICKOS_X86_64_SEL_USER_DATA == sel_user_data,
                      "switch.S stamps the user stack selector as an immediate");

        // Bit 9 of the flag mask is the interrupt flag, and SYSCALL CLEARS every flag the
        // mask names (AMD APM Vol 3, SYSCALL). Without that bit the entry's first
        // instructions run with interrupts LIVE on a stack pointer the caller chose.
        constexpr uint64_t rflags_if = 1ull << 9;
        constexpr uint64_t fmask =
            (1ull << 8)      // trap: no single-step through the entry
            | rflags_if      // the one this step exists for
            | (1ull << 10)   // direction: the psABI wants it clear at a call boundary
            | (1ull << 12) | (1ull << 13) // I/O privilege level: 0 while privileged
            | (1ull << 14)   // nested task
            | (1ull << 18)   // alignment check
            | (1ull << 19) | (1ull << 20); // virtual interrupt, virtual interrupt pending
        constexpr uint64_t fmask_required =
            (1ull << 8) | rflags_if | (1ull << 10) | (3ull << 12) | (1ull << 18);
        static_assert((fmask & fmask_required) == fmask_required,
                      "the syscall entry must start with TF, IF, DF, AC and IOPL clear");
    }

    void ring3_cpu_init(void)
    {
        // Both GS bases are per-processor, as are the syscall MSRs.
        uint32_t id = 0;
#if KICKOS_NUM_CORES > 1
        id = boot_apic_id();
#endif
        g_cpu[id].core_id = id;
        write_msr(msr_gs_base, reinterpret_cast<uint64_t>(&g_cpu[id]));
        write_msr(msr_kernel_gs_base, 0);

        uint64_t const star = (static_cast<uint64_t>((sel_user_data & ~3u) - 8) << 48)
                              | (static_cast<uint64_t>(sel_kernel_code) << 32);
        write_msr(msr_star, star);
        write_msr(msr_lstar, reinterpret_cast<uint64_t>(&kickos_x86_64_syscall_entry));
        write_msr(msr_fmask, fmask);
        write_msr(msr_efer, read_msr(msr_efer) | efer_sce);
        write_cr4(read_cr4() | guard_offered());
    }

    void ring3_init(void)
    {
        // The gs pair. IA32_KERNEL_GS_BASE holds the per-core pointer while a thread runs at
        // ring 3 and swapgs is what brings it back; WRMSR is privileged, so ring 3 can change
        // the base it is holding but never the one the entry gets.
        ring3_cpu_init();
        g_boot_cr4 = read_cr4();
    }

    bool ring3_guard_matches_boot(void)
    {
        return guard_matches(g_boot_cr4, read_cr4());
    }

    void cpu_set_kernel_sp(uint64_t top)
    {
        local_cpu().kernel_sp = top;
    }

    uint64_t cpu_kernel_sp(void)
    {
        return local_cpu().kernel_sp;
    }
}

// Every core holds these: ap_main refuses one whose CR4 does not match the boot core's.
extern "C" char const* arch_uaccess_guard(void)
{
    using namespace kickos::x86_64;
    uint64_t const guard = g_boot_cr4 & cr4_guard;
    if (guard == cr4_guard)
    {
        return "smep smap";
    }
    if (guard == cr4_smep)
    {
        return "smep";
    }
    if (guard == cr4_smap)
    {
        return "smap";
    }
    return "none";
}
