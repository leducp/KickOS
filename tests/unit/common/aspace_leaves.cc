// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The domain and arch leaves a K-seam gate at KICKOS_HAVE_ASPACE=1 links beside its own space
// seam.

#include <string.h>

#include <kickos/aspace.h>
#include <kickos/domain.h>
#include <kickos/frame_pool.h>
#include <kickos/kernel.h>
#include <kickos/sync.h>
#include <kickos/task.h>
#include <kickos/thread.h>
#include <kickos/vrange.h>

#include "kfixture.h"

namespace kickos
{
    Domain* domain_resolve(int) { return nullptr; }
    uint16_t domain_refcount(Domain const* d) { return testfix::domain_refs(d); }
    VirtualRanges* domain_ranges_mut(Domain*) { return nullptr; }
    void frame_pool_free_run(arch_phys_addr_t, size_t, size_t) {}
    void domain_pay(Domain*, Task*) {}
    void domain_unlend(Domain*, uintptr_t) {}
    void domain_unpay(Domain*) {}
    uint32_t domain_pages_paid(Task const* t)
    {
        return aspace_task_usage(task_domain(t), BudgetKind::PAGES);
    }
}

extern "C"
{
    enum arch_aspace_result arch_aspace_unmap(struct arch_aspace*, uintptr_t, size_t)
    {
        return ARCH_ASPACE_OK;
    }

    void* kmemset(void* dst, int c, size_t n) { return memset(dst, c, n); }
    size_t arch_aspace_granule(void) { return 4096u; }
    size_t arch_aspace_table_pages(struct arch_aspace*) { return 0; }
    arch_phys_addr_t arch_aspace_frame_at(struct arch_aspace*, uintptr_t) { return 0; }
    // Weak: a gate whose own seam states the offset keeps it.
    __attribute__((weak)) uintptr_t arch_aspace_user_offset(void) { return 0; }
}
