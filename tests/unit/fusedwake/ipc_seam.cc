// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// What kernel/syscall/syscall_ipc.cc leaves undefined once the K-seam sources answer the rest,
// re-derived with
//
//   nm --undefined-only <the object> | comm -23 - <what those sources define>
//
// at two kernel cores. All six are the address layer, and kernel/syscall/syscall_mem.cc is
// deliberately NOT the answer: its verdicts read MPU regions and an address space the host
// fixture seats no board for, so every access in an arm would be refused for a reason the arm
// is not about. The arms own their buffers and pass them disjoint, so the copies below are the
// real thing and only the PERMISSION question is answered by fiat.

#include <string.h>

#include <kickos/thread.h>

#include <kickos/sys/abi.h>

#include "ipc_seam.h"

namespace kickos::testfix
{
    uintptr_t g_ipc_seam_refuse_rw = 0;
    uintptr_t g_ipc_seam_refuse_write = 0;
}

namespace kickos
{
    bool user_readable_ok(uintptr_t, size_t)
    {
        return true;
    }

    bool user_readable_and_writable_ok(uintptr_t ptr, size_t, size_t)
    {
        return testfix::g_ipc_seam_refuse_rw == 0 or ptr != testfix::g_ipc_seam_refuse_rw;
    }

    bool kaccess_from_user(void* kdst, struct arch_aspace*, uintptr_t usrc, size_t n)
    {
        if (n != 0)
        {
            memcpy(kdst, reinterpret_cast<void const*>(usrc), n);
        }
        return true;
    }

    bool kaccess_to_user(struct arch_aspace*, uintptr_t udst, void const* ksrc, size_t n)
    {
        if (testfix::g_ipc_seam_refuse_write != 0 and udst == testfix::g_ipc_seam_refuse_write)
        {
            return false;
        }
        if (n != 0)
        {
            memcpy(reinterpret_cast<void*>(udst), ksrc, n);
        }
        return true;
    }

    bool ep_copy(struct arch_aspace*, uintptr_t dst, struct arch_aspace*, uintptr_t src,
                 size_t n)
    {
        if (n != 0)
        {
            memcpy(reinterpret_cast<void*>(dst), reinterpret_cast<void const*>(src), n);
        }
        return true;
    }

    bool write_recv_info(struct arch_aspace* ospace, uintptr_t out, uint32_t badge, uint32_t cap)
    {
        if (out == 0)
        {
            return true;
        }
        kos_recv_info const info{badge, cap};
        return kaccess_to_user(ospace, out, &info, sizeof(info));
    }
}
