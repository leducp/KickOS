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

#include <kickos/arch/desc.h>
#include <kickos/arch/arch.h>
#include <kickos/arch/pe_sections.h>
#include <kickos/arch/regs.h>
#include <kickos/arch/ring3.h>
#include <kickos/chip_com1.h>

#include <stddef.h>
#include <stdint.h>

extern "C" void kfault_terminate(void) __attribute__((noreturn));

// switch.S. HIDDEN is load-bearing: -fpie emits a global-offset-table load for the address of
// an external function and `ld -m i386pep` neither builds that table nor relaxes the form
// (tools/check-x86_64-no-got.sh).
extern "C" __attribute__((visibility("hidden"))) void kickos_x86_64_syscall_entry(void);

// The linker's own symbol for the image's first byte. Taken PC-relative, so it is the address
// firmware LOADED the image at; the PE carries an empty base-relocation directory, so nothing
// else here may be an absolute either.
extern "C" __attribute__((visibility("hidden"))) char __ImageBase[];

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

        alignas(64) cpu_block g_cpu[KICKOS_KERNEL_CORES] = {};

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

        uintptr_t g_image_base = 0;
        uint32_t g_image_size = 0;
        uint8_t const* g_sections = nullptr;
        unsigned g_nsections = 0;

        [[noreturn]] void refuse(char const* what)
        {
            com1_puts("\nx86_64 ring3: ");
            com1_puts(what);
            com1_puts("\n");
            kfault_terminate();
        }

        // The PE32+ headers, at the image's first byte because the loader maps them with it.
        void read_own_headers(void)
        {
            uint8_t const* const image = reinterpret_cast<uint8_t const*>(__ImageBase);
            g_image_base = reinterpret_cast<uintptr_t>(image);
            uint32_t const pe_offset = *reinterpret_cast<uint32_t const*>(image + 0x3c);
            uint8_t const* const pe = image + pe_offset;
            if (pe[0] != 'P' or pe[1] != 'E' or pe[2] != 0 or pe[3] != 0)
            {
                refuse("this image carries no PE signature, so it cannot describe itself");
            }
            g_nsections = *reinterpret_cast<uint16_t const*>(pe + 6);
            uint16_t const optional_size = *reinterpret_cast<uint16_t const*>(pe + 20);
            g_image_size = *reinterpret_cast<uint32_t const*>(pe + 24 + 56);
            g_sections = pe + 24 + optional_size;
        }
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
    }

    void ring3_init(void)
    {
        uint64_t const cr4 = read_cr4();
        if ((cr4 & cr4_smep) != 0)
        {
            refuse("supervisor-mode execution prevention is on and no arm here enables it");
        }
        if ((cr4 & cr4_smap) != 0)
        {
            refuse("supervisor-mode access prevention is on and no path here lifts it");
        }

        read_own_headers();

        // The gs pair. IA32_KERNEL_GS_BASE holds the per-core pointer while a thread runs at
        // ring 3 and swapgs is what brings it back; WRMSR is privileged, so ring 3 can change
        // the base it is holding but never the one the entry gets.
        ring3_cpu_init();
    }

    void cpu_set_kernel_sp(uint64_t top)
    {
        local_cpu().kernel_sp = top;
    }

    uint64_t cpu_kernel_sp(void)
    {
        return local_cpu().kernel_sp;
    }

    bool image_range_mapped(uintptr_t ptr, size_t len, bool need_write)
    {
        return pe_range_mapped(g_image_base, g_image_size, g_sections, g_nsections, ptr, len,
                               need_write);
    }
}
