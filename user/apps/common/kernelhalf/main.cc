// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The kernel's half seen from EL0, in its own binary because it ends the process:
// an unprivileged thread READS a word of kernel writable state, and that read must
// fault. What it witnesses is a REVOKED mapping rather than a missing one: the
// kernel's half is mapped and reachable at every instant by the privileged side of
// the same core.
//
// THE ADDRESS IS ONE THE KERNEL REALLY OWNS, never one the app computed, which would assert
// the layout rather than a word of kernel state. On armv8a and rv64imac it comes from the link:
// _sdata, the first word of the kernel's .data, named by an absolute data word because app text
// cannot reach a kernel-half symbol under this board's code model. On x86_64 an app word aimed
// at the kernel's half is a link defect the build and the boot both refuse, so the processor
// names it instead: sgdt hands ring 3 the base of the running core's descriptor table, which
// sits in the kernel's per-core block.
//
// THE ADDRESS IS ANNOUNCED BEFORE IT IS READ, so the gate can hold the dump's fault address
// against it: a fault anywhere else says something other than this read broke.
//
// A READ and not a write, because a read is the weaker demand: a half that refuses the read
// refuses the write, while a half that refuses only writes still hands EL0 every byte of
// kernel state. The value is printed on the ERROR path, so a run that does NOT fault shows
// what it was able to see.

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <kickos/libc/fmt.h>

#include <stdint.h>

#if !defined(__x86_64__)
extern "C" char _sdata[];
#endif

namespace
{
#if defined(__x86_64__)
    struct __attribute__((packed)) DescriptorTableRegister
    {
        uint16_t limit;
        uint64_t base;
    };

    // Rests on UMIP being off: with it on, sgdt in ring 3 raises #GP instead.
    uintptr_t kernel_word()
    {
        DescriptorTableRegister gdtr = {};
        __asm__ volatile("sgdt %0" : "=m"(gdtr));
        return static_cast<uintptr_t>(gdtr.base);
    }
#else
    char const* volatile g_kernel_data = _sdata;

    uintptr_t kernel_word()
    {
        return reinterpret_cast<uintptr_t>(g_kernel_data);
    }
#endif
}

int main(int, char**)
{
    uintptr_t const word = kernel_word();
    if (word == 0)
    {
        kos_print("[kernelhalf] ERROR: this board names no privileged-only word\n");
        return 1;
    }
    char msg[96];
    ksnprintf(msg, sizeof(msg), "[kernelhalf] reading 0x%lx\n", static_cast<unsigned long>(word));
    kos_print(msg);
    volatile uint32_t const* const p = reinterpret_cast<volatile uint32_t const*>(word);
    uint32_t const seen = *p;
    ksnprintf(msg, sizeof(msg), "[kernelhalf] ERROR: read 0x%lx from the kernel's half\n",
              static_cast<unsigned long>(seen));
    kos_print(msg);
    return 1;
}
