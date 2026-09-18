// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Kernel access to user memory. Validate permissions before dereferencing,
// and identify the owning space for each user range.

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
        // Require the exact device base from the thread's ownership record.
        // A sub-block grant must not resolve to the enclosing block.
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

    // Require both read and write validation, including architecture fallbacks.
    // A read-only reply destination must be rejected.
    bool user_readable_and_writable_ok(uintptr_t ptr, size_t read_len, size_t write_len)
    {
        Thread* const c = sched::current();
        bool read_seen = false;
        bool write_seen = false;
        if (c != nullptr)
        {
            if (c->privileged)
            {
                return true;
            }
            read_seen = (read_len == 0);
            write_seen = (write_len == 0);
            // A wrapping extent asks the set nothing and is still offered to the arch hook.
            bool read_asks = (not read_seen and ptr + read_len >= ptr);
            bool write_asks = (not write_seen and ptr + write_len >= ptr);
            for (arch_mpu_region const& r : c->mpu)
            {
                if (not read_asks and not write_asks)
                {
                    break;
                }
                uintptr_t const rend = r.base + r.size;
                if (rend < r.base or ptr < r.base)
                {
                    continue;
                }
                if (read_asks and (r.attr & ARCH_MPU_R) == ARCH_MPU_R and ptr + read_len <= rend)
                {
                    read_seen = true;
                    read_asks = false;
                }
                if (write_asks and (r.attr & ARCH_MPU_W) == ARCH_MPU_W
                    and ptr + write_len <= rend)
                {
                    write_seen = true;
                    write_asks = false;
                }
            }
#if KICKOS_HAVE_ASPACE
            if (read_asks or write_asks)
            {
                VirtualRanges const* const ranges = current_ranges(c);
                if (ranges != nullptr)
                {
                    if (read_asks and ranges->covers(ptr, read_len, map_rights_of(ARCH_MPU_R)))
                    {
                        read_seen = true;
                    }
                    if (write_asks and ranges->covers(ptr, write_len, map_rights_of(ARCH_MPU_W)))
                    {
                        write_seen = true;
                    }
                }
            }
#endif
        }
        if (not read_seen and not arch_user_text_readable(ptr, read_len))
        {
            return false;
        }
        if (not write_seen and not arch_user_data_writable(ptr, write_len))
        {
            return false;
        }
        return true;
    }

    // Callers must validate permissions first. Null space means a directly
    // accessible address. Reject overlap because kmemcpy copies forward only.

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

        // Acquire each granule separately and hold both ends during the copy.
        // Failure leaves the already-copied prefix in place.
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

    // Copy one aligned pointer-sized word with one acquire. It cannot cross
    // a granule boundary. Use memcpy to preserve effective-type rules (reent.h).
    bool kaccess_word_to_user(struct arch_aspace* dspace, uintptr_t udst, void const* kword)
    {
        if ((udst & static_cast<uintptr_t>(sizeof(void*) - 1u)) != 0)
        {
            return false;
        }
#if KICKOS_HAVE_ASPACE
        if (dspace != nullptr)
        {
            void* const d = arch_aspace_acquire(dspace, udst);
            if (d == nullptr)
            {
                return false;
            }
            __builtin_memcpy(__builtin_assume_aligned(d, sizeof(void*)), kword, sizeof(void*));
            arch_aspace_release(dspace, udst);
            return true;
        }
#else
        (void)dspace;
#endif
        __builtin_memcpy(__builtin_assume_aligned(reinterpret_cast<void*>(udst), sizeof(void*)),
                         kword, sizeof(void*));
        return true;
    }

    // Compare overlap only within the same space. Return failure rather than
    // asserting: user code can request overlap, and fault reporting uses this path.
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
