// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The kernel's half seen from EL0, in its own binary because it ends the process: an
// unprivileged thread READS a word of kernel writable state, and that read must fault. What
// it witnesses is not a missing mapping but a REVOKED one, the kernel's half being mapped
// and reachable at every instant by the privileged side of the same core
// (docs/design-m6-mmu.md, F1 and T5b.3).
//
// THE ADDRESS COMES FROM THE KERNEL. App text cannot name a kernel-half symbol under this
// board's code model, and an address the app computed itself would assert the layout rather
// than a word the kernel really owns. kos_guard_addr answers with a word in kernel-side
// .bss, which is outside every arena and inside no granted range
// (arch/common/arch_ram_common.cc).
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

int main(int, char**)
{
    void* const word = kos_guard_addr();
    if (word == nullptr)
    {
        kos_print("[kernelhalf] ERROR: this board names no privileged-only word\n");
        return 1;
    }
    char msg[96];
    ksnprintf(msg, sizeof(msg), "[kernelhalf] reading 0x%lx\n",
              reinterpret_cast<unsigned long>(word));
    kos_print(msg);
    volatile uint32_t const* const p = static_cast<volatile uint32_t const*>(word);
    uint32_t const seen = *p;
    ksnprintf(msg, sizeof(msg), "[kernelhalf] ERROR: read 0x%lx from the kernel's half\n",
              static_cast<unsigned long>(seen));
    kos_print(msg);
    return 1;
}
