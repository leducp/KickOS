// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The user-memory access funnel: the confused-deputy floor (a privileged
// syscall must never dereference a pointer the CALLER could not itself reach)
// plus the kernel<->user byte-access seam. Split out of syscall.cc because every
// domain (IPC, threads, cap objects) and the dispatch share these. External
// linkage; declared in syscall_internal.h. The MMU era reimplements exactly this
// set (per arch: translate / copy across address spaces), so it is the single
// place a kernel-side user-pointer dereference lives.

#include <kickos/arch/arch.h>
#include <kickos/sched.h>

#include <kickos/sys/abi.h>

#include "syscall_internal.h"

namespace kickos
{
    // Confused-deputy floor: syscall_dispatch runs privileged (it bypasses the
    // MPU), so it must never dereference a user pointer the CALLER could not
    // itself reach. A range passes iff it lies within one region the current
    // thread is granted -- app code/rodata (RX) + static data (RW) + its domain
    // data + its own stack (thread.cc composition) -- with the required access.
    // Privileged callers (kernel domain, trusted) bypass. Struct + out-pointer
    // args are caller STACK locals; a read buffer / name string may instead point
    // into the app's code/rodata/.data -- see user_readable_ok for how those are
    // recognized on every backend (real MPU regions on HW, the host image on sim).
    bool user_range_ok(uintptr_t ptr, size_t len, uint32_t need)
    {
        Thread* c = sched::current();
        if (c == nullptr)
        {
            return false;
        }
        if (c->privileged)
        {
            return true;
        }
        if (len == 0)
        {
            return true;
        }
        uintptr_t const end = ptr + len;
        if (end < ptr)
        {
            return false; // address-space wrap
        }
        for (size_t i = 0; i < c->region_count; i++)
        {
            arch_mpu_region const& r = c->regions[i];
            if ((r.attr & need) != need)
            {
                continue;
            }
            uintptr_t const rend = r.base + r.size;
            if (rend >= r.base and ptr >= r.base and end <= rend)
            {
                return true;
            }
        }
        return false;
    }

    // A user-supplied READ buffer (console output, a name string) the kernel
    // dereferences privileged. It passes iff it lies within a region the caller is
    // granted (app code/rodata/.data + domain data + stack) OR, where the backend
    // does not model code/rodata as a region, within the app's readable code/data
    // extent (arch_user_text_readable): the host image on the sim, flash/ROM on a
    // non-enforcing MCU. A pointer into no granted region and outside that extent
    // (another domain's arena page, kernel memory, a wild pointer) is rejected, so
    // an unprivileged caller cannot launder it out through the kernel. Privileged
    // callers and len==0 pass via user_range_ok.
    bool user_readable_ok(uintptr_t ptr, size_t len)
    {
        if (user_range_ok(ptr, len, ARCH_MPU_R))
        {
            return true;
        }
        return arch_user_text_readable(ptr, len);
    }

    // A user-supplied WRITE buffer / out-pointer the kernel stores into privileged
    // (an endpoint recv buffer, a clock_now result). It passes iff it lies within a
    // region the caller is granted WRITE. No arch_user_text_readable twin: code/rodata
    // is never a legitimate write target, so an out-pointer into it is rejected here
    // even though it would read back fine. Privileged callers and len==0 pass via
    // user_range_ok.
    bool user_writable_ok(uintptr_t ptr, size_t len)
    {
        return user_range_ok(ptr, len, ARCH_MPU_W);
    }

    // The kernel<->user byte-access seam. IDENTITY today: a validated user
    // address is directly kernel-dereferenceable (one physical space), so these
    // are plain copies. The MMU era reimplements exactly these (per arch:
    // translate / copy across address spaces); every kernel-side user-pointer
    // dereference funnels here, so that becomes one function to change, not a
    // hunt across syscalls. Callers MUST validate first (user_range_ok /
    // user_readable_ok / user_writable_ok) -- this is the ACCESS, not the check.
    // Byte loops, not memcpy: freestanding, and the arch rewrite hooks here.
    void kaccess_from_user(void* kdst, uintptr_t usrc, size_t n)
    {
        char* d = static_cast<char*>(kdst);
        char const* s = reinterpret_cast<char const*>(usrc);
        for (size_t i = 0; i < n; i++)
        {
            d[i] = s[i];
        }
    }

    void kaccess_to_user(uintptr_t udst, void const* ksrc, size_t n)
    {
        char* d = reinterpret_cast<char*>(udst);
        char const* s = static_cast<char const*>(ksrc);
        for (size_t i = 0; i < n; i++)
        {
            d[i] = s[i];
        }
    }

    // Bounded byte copy (<= KOS_EP_MSG_MAX). Both endpoints do it under IrqLock,
    // one side of which is a PARKED peer's user buffer (see endpoint.h): the
    // waker's own MPU regions are loaded, so it reaches the peer's memory only via
    // privileged background access (arch contract, design section 3.1). This is
    // the user<->user peer of the kaccess_*_user seam above (both ends are user
    // memory, not one kernel side): the ONE endpoint access the MMU era rewrites
    // as a cross-aspace copy. All endpoint payload movement funnels here already.
    void ep_copy(uintptr_t dst, uintptr_t src, size_t n)
    {
        char* d = reinterpret_cast<char*>(dst);
        char const* s = reinterpret_cast<char const*>(src);
        for (size_t i = 0; i < n; i++)
        {
            d[i] = s[i];
        }
    }

    // Deliver a receiver's kos_recv_info (badge + reply_cap) into its parked out-ptr,
    // or nothing when it asked for none (info-less recv, out == 0). reply_cap == -1
    // marks a plain send; a real handle marks a call. Validated for 8 bytes at recv.
    void write_recv_info(uintptr_t out, uint32_t badge, int32_t reply_cap)
    {
        if (out == 0)
        {
            return;
        }
        kos_recv_info info;
        info.badge = badge;
        info.reply_cap = reply_cap;
        kaccess_to_user(out, &info, sizeof(info));
    }
}
