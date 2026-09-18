// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// satp access and fences for the RV64 map editor.
// FENCE w,w orders explicit stores. SFENCE.VMA orders translation reads
// and invalidates cached translations.

#ifndef KICKOS_ARCH_RISCV_RV64IMAC_SYSOPS_RV64IMAC_H
#define KICKOS_ARCH_RISCV_RV64IMAC_SYSOPS_RV64IMAC_H

#include <stdint.h>

// Host tests provide these operations through rv64_sysops_seam.cc.
#ifndef KICKOS_RV64_SYSOPS_FROM_SEAM
#define KICKOS_RV64_SYSOPS_FROM_SEAM 0
#endif

extern "C"
{

#if !KICKOS_RV64_SYSOPS_FROM_SEAM

    inline uint64_t kickos_rv64_read_satp(void)
    {
        uint64_t satp = 0;
        __asm volatile("csrr %0, satp" : "=r"(satp));
        return satp;
    }

    // Write only; callers supply any required translation fence.
    inline void kickos_rv64_write_satp(uint64_t satp)
    {
        __asm volatile("csrw satp, %0" ::"r"(satp) : "memory");
    }

    inline void kickos_rv64_fence_w_w(void)
    {
        __asm volatile("fence w, w" ::: "memory");
    }

    // rs2=zero covers all ASIDs and global kernel/window entries.
    inline void kickos_rv64_sfence_page(uint64_t va)
    {
        __asm volatile("sfence.vma %0, zero" ::"r"(va) : "memory");
    }

    inline void kickos_rv64_sfence_all(void)
    {
        __asm volatile("sfence.vma zero, zero" ::: "memory");
    }

#else

    uint64_t kickos_rv64_read_satp(void);
    void kickos_rv64_write_satp(uint64_t satp);
    void kickos_rv64_fence_w_w(void);
    void kickos_rv64_sfence_page(uint64_t va);
    void kickos_rv64_sfence_all(void);

#endif
}

#endif
