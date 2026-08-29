// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The one place a kernel-side user-pointer dereference lives. A privileged syscall MUST never
// dereference a pointer the caller could not itself reach, and every access here names the
// owner of each user end.

#include <kickos/arch/arch.h>
#include <kickos/domain.h>
#include <kickos/kernel.h>
#include <kickos/kruntime.h>
#include <kickos/sched.h>
#include <kickos/task.h>
#include <kickos/thread.h>

#include <kickos/sys/abi.h>

#include "syscall_internal.h"


namespace kickos
{
#if KICKOS_HAVE_ASPACE
    namespace
    {
        // ARCH_MPU_* and ARCH_MAP_* are two vocabularies that happen to agree on their low
        // three bits. Spelled out so a later bit added to either does not silently make one
        // stand in for the other.
        uint32_t map_rights_of(uint32_t need)
        {
            uint32_t rights = 0;
            if ((need & ARCH_MPU_R) != 0)
            {
                rights |= ARCH_MAP_R;
            }
            if ((need & ARCH_MPU_W) != 0)
            {
                rights |= ARCH_MAP_W;
            }
            if ((need & ARCH_MPU_X) != 0)
            {
                rights |= ARCH_MAP_X;
            }
            return rights;
        }

        VirtualRanges const* current_ranges(Thread const* c)
        {
            return domain_ranges(task_domain(c->task));
        }
    }
#endif

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
        for (arch_mpu_region const& r : c->mpu)
        {
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
#if KICKOS_HAVE_ASPACE
        VirtualRanges const* const ranges = current_ranges(c);
        if (ranges != nullptr and ranges->covers(ptr, len, map_rights_of(need)))
        {
            return true;
        }
#endif
        return false;
    }

    bool user_range_typed_ok(uintptr_t ptr, size_t len, uint32_t need)
    {
        Thread* c = sched::current();
        if (c == nullptr or len == 0)
        {
            return false;
        }
        uintptr_t const end = ptr + len;
        if (end < ptr)
        {
            return false; // address-space wrap
        }
#if KICKOS_HAVE_ASPACE
        // A self-grant seats no region, so the array below holds no entry for a block this
        // question is ever asked about; a translating backend records the type here.
        VirtualRanges const* const ranges = current_ranges(c);
        if (ranges != nullptr)
        {
            uint8_t memtype = static_cast<uint8_t>(ARCH_MAP_NORMAL);
            if ((need & ARCH_MPU_NOCACHE) != 0)
            {
                memtype = static_cast<uint8_t>(ARCH_MAP_NOCACHE);
            }
            VirtualRange const* const e = ranges->find(ptr, len);
            if (e != nullptr and e->state == VirtualState::Granted and e->memtype == memtype
                and (e->rights & map_rights_of(need)) == map_rights_of(need))
            {
                return true;
            }
        }
#endif
        for (arch_mpu_region const& r : c->mpu)
        {
            // Exact, not a superset: a region carrying a memory type the caller did not ask
            // for is a different mapping of the block.
            if (r.attr != need)
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

    namespace
    {
        // Exact base, so a sub-block window cannot reach a whole-block table entry (K64F PIT
        // ch2 base 0x40037120, block 0x40037000). Reads the thread's possession record and
        // never its reachable regions, the mapping being task-wide on a translating backend
        // (docs/design-m6-mmu.md F9).
        size_t mmio_block_of(Thread const* c, uintptr_t base)
        {
            if (c->dev_size == 0 or c->dev_base != base)
            {
                return 0;
            }
            return c->dev_size;
        }
    }

    // The sole authorisation for arch_periph_enable: the caller must own the whole block.
    bool caller_holds_mmio_block(uintptr_t base)
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
        return mmio_block_of(c, base) != 0;
    }

    bool caller_holds_mmio_reg(uintptr_t base, uintptr_t offset)
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
        size_t const size = mmio_block_of(c, base);
        if (size == 0)
        {
            return false;
        }
        // Compared against the size, never as base + offset + 4: the window size is the only
        // operand that cannot wrap here.
        if (offset > size)
        {
            return false;
        }
        return size - offset >= sizeof(uint32_t);
    }

    bool user_readable_ok(uintptr_t ptr, size_t len)
    {
        if (user_range_ok(ptr, len, ARCH_MPU_R))
        {
            return true;
        }
        return arch_user_text_readable(ptr, len);
    }

    bool user_writable_ok(uintptr_t ptr, size_t len)
    {
        if (user_range_ok(ptr, len, ARCH_MPU_W))
        {
            return true;
        }
        return arch_user_data_writable(ptr, len);
    }

    // Callers MUST validate first (user_range_ok / user_readable_ok / user_writable_ok): this
    // is the access, never the check. Each user end names its owner; a null space means the
    // address is directly kernel-dereferenceable. Overlapping ranges are not copied, the
    // primitive being the ascending-only kmemcpy.

    namespace
    {
#if KICKOS_HAVE_ASPACE
        size_t granule_chunk(uintptr_t va, size_t left)
        {
            size_t const g = arch_aspace_granule();
            size_t const room = g - static_cast<size_t>(va & static_cast<uintptr_t>(g - 1u));
            if (room < left)
            {
                return room;
            }
            return left;
        }
#endif

        // Each granule is reached through its own acquire, both ends held across the copy;
        // ARCH_ASPACE_ACQUIRE_MIN counts those holds. False leaves a PREFIX behind: the
        // granules below the one that refused are copied and released, so the destination
        // holds a head of the source over a tail of what it held.
        bool access_copy(struct arch_aspace* dspace, uintptr_t dst,
                         struct arch_aspace* sspace, uintptr_t src, size_t n)
        {
#if KICKOS_HAVE_ASPACE
            while (n != 0)
            {
                size_t chunk = n;
                void* d = reinterpret_cast<void*>(dst);
                if (dspace != nullptr)
                {
                    d = arch_aspace_acquire(dspace, dst);
                    if (d == nullptr)
                    {
                        return false;
                    }
                    chunk = granule_chunk(dst, chunk);
                }
                void const* s = reinterpret_cast<void const*>(src);
                if (sspace != nullptr)
                {
                    s = arch_aspace_acquire(sspace, src);
                    if (s == nullptr)
                    {
                        if (dspace != nullptr)
                        {
                            arch_aspace_release(dspace, dst);
                        }
                        return false;
                    }
                    chunk = granule_chunk(src, chunk);
                }
                kmemcpy(d, s, chunk);
                if (sspace != nullptr)
                {
                    arch_aspace_release(sspace, src);
                }
                if (dspace != nullptr)
                {
                    arch_aspace_release(dspace, dst);
                }
                dst += chunk;
                src += chunk;
                n -= chunk;
            }
            return true;
#else
            (void)dspace;
            (void)sspace;
            kmemcpy(reinterpret_cast<void*>(dst), reinterpret_cast<void const*>(src), n);
            return true;
#endif
        }
    }

#if KICKOS_HAVE_ASPACE
    struct arch_aspace* user_space_of(Thread const* t)
    {
        if (t == nullptr)
        {
            return nullptr;
        }
        return domain_space(task_domain(t->task));
    }

    struct arch_aspace* ipc_buf_space(Thread const* t)
    {
        if (t == nullptr)
        {
            return nullptr;
        }
        if (t->call_frame_parked != 0)
        {
            return nullptr; // the fastpath's park: ipc.buf is the caller's saved trap frame
        }
        return user_space_of(t);
    }
#endif

    // kdst is kernel storage and usrc is user memory, so the two ends are disjoint by
    // construction.
    bool kaccess_from_user(void* kdst, struct arch_aspace* sspace, uintptr_t usrc, size_t n)
    {
        return access_copy(nullptr, reinterpret_cast<uintptr_t>(kdst), sspace, usrc, n);
    }

    // ksrc is kernel storage, udst is user memory: disjoint for the same reason.
    bool kaccess_to_user(struct arch_aspace* dspace, uintptr_t udst, void const* ksrc, size_t n)
    {
        return access_copy(dspace, udst, nullptr, reinterpret_cast<uintptr_t>(ksrc), n);
    }

    // Disjointness is a (space, range) question, addresses comparing only under one owner.
    // An overlap is refused and MUST NOT become an assert again: reent_prime calls this from
    // the switch path, which the fault reporter descends into through cap_console_deliver, so
    // a panic here re-enters kputs -> kconsole_write from inside the record it was writing.
    bool ep_copy(struct arch_aspace* dspace, uintptr_t dst, struct arch_aspace* sspace,
                 uintptr_t src, size_t n)
    {
        if (dspace == sspace and dst + n > src and src + n > dst)
        {
            return false;
        }
        return access_copy(dspace, dst, sspace, src, n);
    }

    // `out` == 0 is an info-less recv, which answers true. KCAP_INVALID marks a plain send
    // and a real handle marks a call.
    bool write_recv_info(struct arch_aspace* ospace, uintptr_t out, uint32_t badge,
                         uint32_t reply_cap)
    {
        if (out == 0)
        {
            return true;
        }
        kos_recv_info info;
        info.badge = badge;
        info.reply_cap = reply_cap;
        return kaccess_to_user(ospace, out, &info, sizeof(info));
    }
}
