// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// What kernel/syscall/syscall_mem.cc and the fastpath's own arm leave undefined on top of
// karch_seam.cc's set. Re-derive it rather than trusting this list:
//
//   nm --undefined-only <the objects> | comm -23 - <their defined symbols>

#include <kickos/arch/arch.h>

extern "C"
{
    // Under the fastpath macro only, and reached by switch_book for a thread the fastpath
    // parked. Recorded so an arm can read the result the restore would have popped.
    uint32_t g_fast_result = 0;
    uint32_t g_fast_result_stores = 0;

    void arch_ctx_set_syscall_result(struct arch_context*, uint32_t result)
    {
        g_fast_result = result;
        g_fast_result_stores++;
    }

    // The two admission extents. FALSE, so nothing in this gate is admitted by the image's
    // own static ranges: every buffer an arm hands the fastpath is kernel storage, which
    // ep_copy reaches with no space at all.
    bool arch_user_text_readable(uintptr_t, size_t)
    {
        return false;
    }

    bool arch_user_data_writable(uintptr_t, size_t)
    {
        return false;
    }

    int arch_periph_enable(uintptr_t)
    {
        return 0;
    }
}
