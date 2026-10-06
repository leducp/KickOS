// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Kernel access to user memory. Validate permissions before dereferencing,
// and identify the owning space for each user range.

#include <kickos/arch/arch.h>
#include <kickos/aspace.h>
#include <kickos/domain.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/kruntime.h>
#include <kickos/mpuset.h>
#include <kickos/sched.h>
#include <kickos/task.h>
#include <kickos/thread.h>

#include <kickos/sys/abi.h>
#include <kickos/sys/atomic.h>

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

    namespace
    {
        // A device region on a translating backend records the window's PHYSICAL base, which is
        // not where its holder reaches it: the range list answers for that address.
        bool region_answers_pointers(arch_mpu_region const& r)
        {
#if KICKOS_HAVE_ASPACE
            return (r.attr & ARCH_MPU_DEV) == 0;
#else
            (void)r;
            return true;
#endif
        }

        // Whether a region of c's set without W overlaps [ptr, end), so the set answers no
        // write there: a region backend obeys one of the overlapping regions, PMSAv7 the last,
        // the PMP the first and PMSAv8 none, so a read-only window over a writable block is
        // never a way to have the kernel write it. A translating backend's rights are its
        // mapping's, which the range list carries.
        bool read_only_overlaps(Thread const* c, uintptr_t ptr, uintptr_t end)
        {
#if KICKOS_HAVE_ASPACE
            (void)c;
            (void)ptr;
            (void)end;
#else
            for (arch_mpu_region const& r : c->mpu)
            {
                if ((r.attr & ARCH_MPU_W) == 0 and ptr < r.base + r.size and end > r.base)
                {
                    return true;
                }
            }
#endif
            return false;
        }
    }

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
        if ((need & ARCH_MPU_W) != 0 and read_only_overlaps(c, ptr, end))
        {
            return false;
        }
        for (arch_mpu_region const& r : c->mpu)
        {
            if ((r.attr & need) != need or not region_answers_pointers(r))
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
        // Require the exact base of a device window the thread holds, its device regions being
        // the ownership record. A sub-block grant must not resolve to the enclosing block.
        size_t mmio_block_of(Thread const* c, uintptr_t base)
        {
            for (arch_mpu_region const& r : c->mpu)
            {
                if ((r.attr & ARCH_MPU_DEV) != 0 and r.base == base)
                {
                    return r.size;
                }
            }
            return 0;
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

    namespace
    {
        // The window at `index` in c's spawn list, by the place recorded with it at the spawn:
        // in the port set where the arch has ports, in the VR_WINDOW range where a space
        // translates, else in the region set. False past the list. Caller holds IrqLock.
        bool thread_window_at(Thread const* c, uint32_t index, kos_window* w)
        {
#if KICKOS_ARCH_HAS_PORTS
            for (uint8_t r = 0; r < c->ctx.port_count; r++)
            {
                if (((c->ctx.port_places >> (2u * r)) & 3u) != index)
                {
                    continue;
                }
                w->base = c->ctx.ports[r].base;
                w->size = static_cast<uint32_t>(c->ctx.ports[r].last - c->ctx.ports[r].base) + 1u;
                w->kind = KOS_WINDOW_PORTS;
                return true;
            }
#endif
#if KICKOS_HAVE_ASPACE
            VirtualRanges const* const ranges = domain_ranges(task_domain(c->task));
            uint16_t const holder = static_cast<uint16_t>(kernel().threads.index_of(c) + 1);
            for (size_t i = 0; ranges != nullptr and i < VirtualRanges::capacity(); i++)
            {
                VirtualRange const* const e = ranges->at(i);
                if (e == nullptr or (e->flags & VR_WINDOW) == 0 or e->holder != holder
                    or e->place != index)
                {
                    continue;
                }
                w->base = e->base;
                w->size = static_cast<uint32_t>(e->pages * arch_aspace_granule());
                w->kind = KOS_WINDOW_DEVICE;
                if (e->memtype != static_cast<uint8_t>(ARCH_MAP_DEVICE))
                {
                    w->kind = KOS_WINDOW_MEMORY;
                    if ((e->rights & ARCH_MAP_W) == 0)
                    {
                        w->flags = static_cast<uint8_t>(w->flags | KOS_WINDOW_RO);
                    }
                    if (e->memtype == static_cast<uint8_t>(ARCH_MAP_NOCACHE))
                    {
                        w->flags = static_cast<uint8_t>(w->flags | KOS_WINDOW_UNCACHED);
                    }
                }
                return true;
            }
            return false;
#else
            uint8_t flags = 0;
            arch_mpu_region const* const r = c->mpu.window(index, &flags);
            if (r == nullptr)
            {
                return false;
            }
            w->base = r->base;
            w->size = static_cast<uint32_t>(r->size);
            w->kind = KOS_WINDOW_DEVICE;
            if ((r->attr & ARCH_MPU_DEV) == 0)
            {
                w->kind = KOS_WINDOW_MEMORY;
                w->flags = flags;
            }
            return true;
#endif
        }
    }

    int window_get_call(uintptr_t index, uintptr_t out)
    {
        IrqLock lock;
        Thread* const c = sched::current();
        if (c == nullptr or out == 0 or (out & (alignof(kos_window) - 1u)) != 0)
        {
            return -KOS_EINVAL;
        }
        if (not user_writable_ok(out, sizeof(kos_window)))
        {
            return -KOS_EFAULT;
        }
        // Zeroed whole, padding included, which the copy below hands to the caller.
        kos_window w;
        kmemset(&w, 0, sizeof(w));
        uint32_t const at = static_cast<uint32_t>(index);
        if (at != index or not thread_window_at(c, at, &w))
        {
            return -KOS_EINVAL;
        }
        if (not kaccess_to_user(user_space_of(c), out, &w, sizeof(w)))
        {
            return -KOS_EFAULT;
        }
        return 0;
    }

    int port_reg_write_call(uintptr_t base, uintptr_t offset, uintptr_t value)
    {
#if KICKOS_ARCH_HAS_PORTS
        if (offset > 0xffffu or base > 0xffffu - offset or value > 0xffu)
        {
            return -KOS_EINVAL;
        }
        IrqLock lock;
        Thread* const c = sched::current();
        uintptr_t const port = base + offset;
        bool held = c != nullptr and c->privileged;
        for (uint8_t r = 0; c != nullptr and r < c->ctx.port_count; r++)
        {
            if (port >= c->ctx.ports[r].base and port <= c->ctx.ports[r].last)
            {
                held = true;
            }
        }
        if (not held)
        {
            return -KOS_EPERM;
        }
        return arch_port_reg_write(static_cast<uint16_t>(port), static_cast<uint8_t>(value));
#else
        (void)base;
        (void)offset;
        (void)value;
        return -KOS_ENOSYS;
#endif
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
            if (write_asks and read_only_overlaps(c, ptr, ptr + write_len))
            {
                write_asks = false; // the set answers no write here, as user_range_ok says
            }
            for (arch_mpu_region const& r : c->mpu)
            {
                if (not read_asks and not write_asks)
                {
                    break;
                }
                uintptr_t const rend = r.base + r.size;
                if (rend < r.base or ptr < r.base or not region_answers_pointers(r))
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
#if defined(KICKOS_ENABLE_SELFTEST) and KICKOS_ALIAS_DCACHE
        // One per core, written with interrupts masked: no increment is lost, and none is an RMW.
        Atomic<uint32_t, Order::RELAXED> g_alias_syncs[KICKOS_KERNEL_CORES];
#endif

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
#elif KICKOS_ARCH_ARENA_DCACHE
        // Whether [a, a + n) meets a non-cacheable region of the owner's set, which the kernel
        // reaches through the cacheable background map whenever that set is not the one loaded.
        bool region_uncached(UserOwner owner, uintptr_t a, size_t n)
        {
            if (owner == nullptr)
            {
                return false;
            }
            uintptr_t const last = a + n - 1u;
            for (arch_mpu_region const& r : owner->mpu)
            {
                if ((r.attr & (ARCH_MPU_NOCACHE | ARCH_MPU_DEV)) == ARCH_MPU_NOCACHE
                    and r.size != 0 and a <= r.base + (r.size - 1u) and r.base <= last)
                {
                    return true;
                }
            }
            return false;
        }
#endif

#if KICKOS_ALIAS_DCACHE
        // The dropping ahead of the write is owed too: a partial-line write merges into a stale
        // line, and the clean after it writes the stale bytes back.
        void chunk_copy(void* d, bool d_uncached, void const* s, bool s_uncached, size_t n)
        {
            if (s_uncached)
            {
                alias_sync(s, n);
            }
            if (d_uncached)
            {
                alias_sync(d, n);
            }
            kmemcpy(d, s, n);
            if (d_uncached)
            {
                alias_sync(d, n);
            }
        }
#endif

        // Acquire each granule separately and hold both ends during the copy.
        // Failure leaves the already-copied prefix in place.
        bool access_copy(UserOwner dspace, uintptr_t dst, UserOwner sspace, uintptr_t src,
                         size_t n)
        {
#if KICKOS_HAVE_ASPACE
            while (n != 0)
            {
                size_t chunk = n;
                void* d = reinterpret_cast<void*>(dst);
                bool d_uncached = false;
                if (dspace != nullptr)
                {
#if KICKOS_ALIAS_DCACHE
                    d = arch_aspace_acquire(dspace, dst, &d_uncached);
#else
                    d = arch_aspace_acquire(dspace, dst, nullptr);
#endif
                    if (d == nullptr)
                    {
                        return false;
                    }
                    chunk = granule_chunk(dst, chunk);
                }
                void const* s = reinterpret_cast<void const*>(src);
                bool s_uncached = false;
                if (sspace != nullptr)
                {
#if KICKOS_ALIAS_DCACHE
                    s = arch_aspace_acquire(sspace, src, &s_uncached);
#else
                    s = arch_aspace_acquire(sspace, src, nullptr);
#endif
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
#if KICKOS_ALIAS_DCACHE
                chunk_copy(d, d_uncached, s, s_uncached, chunk);
#else
                (void)d_uncached;
                (void)s_uncached;
                kmemcpy(d, s, chunk);
#endif
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
#elif KICKOS_ARCH_ARENA_DCACHE
            if (n != 0)
            {
                chunk_copy(reinterpret_cast<void*>(dst), region_uncached(dspace, dst, n),
                           reinterpret_cast<void const*>(src), region_uncached(sspace, src, n),
                           n);
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

#if KICKOS_ALIAS_DCACHE
    void alias_sync(void const* p, size_t n)
    {
#if defined(KICKOS_ENABLE_SELFTEST)
        arch_irq_state_t const s = arch_irq_save();
        Atomic<uint32_t, Order::RELAXED>& mine = g_alias_syncs[kickos_kernel_core()];
        mine.store(mine.load() + 1u);
        arch_irq_restore(s);
#endif
        arch_dcache_invalidate(const_cast<void*>(p), n);
    }
#endif

#if KICKOS_ARCH_ARENA_DCACHE and not KICKOS_HAVE_ASPACE
    void grant_sync(MpuSet const* held, uintptr_t base, size_t size, uint32_t attr)
    {
        constexpr uint32_t TYPE = ARCH_MPU_NOCACHE | ARCH_MPU_DEV;
        bool owed = (attr & TYPE) == ARCH_MPU_NOCACHE;
        if (not owed and held != nullptr)
        {
            for (arch_mpu_region const& r : *held)
            {
                if (r.base == base and r.size == size and (r.attr & TYPE) == ARCH_MPU_NOCACHE)
                {
                    owed = true;
                }
            }
        }
        if (owed)
        {
            alias_sync(reinterpret_cast<void const*>(base), size);
        }
    }
#endif

#if defined(KICKOS_ENABLE_SELFTEST)
    uint32_t alias_sync_count()
    {
        uint32_t sum = 0;
#if KICKOS_ALIAS_DCACHE
        for (Atomic<uint32_t, Order::RELAXED> const& n : g_alias_syncs)
        {
            sum += n.load();
        }
#endif
        return sum;
    }
#endif

#if KICKOS_HAVE_ASPACE
    UserOwner user_space_of(Thread const* t)
    {
        if (t == nullptr)
        {
            return nullptr;
        }
        return domain_space(task_domain(t->task));
    }
#elif KICKOS_ARCH_ARENA_DCACHE
    UserOwner user_space_of(Thread const* t)
    {
        return t;
    }
#endif

#if KICKOS_HAVE_ASPACE or KICKOS_ARCH_ARENA_DCACHE
    UserOwner ipc_buf_space(Thread const* t)
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
    bool kaccess_from_user(void* kdst, UserOwner sspace, uintptr_t usrc, size_t n)
    {
        return access_copy(nullptr, reinterpret_cast<uintptr_t>(kdst), sspace, usrc, n);
    }

    // ksrc is kernel storage, udst is user memory: disjoint for the same reason.
    bool kaccess_to_user(UserOwner dspace, uintptr_t udst, void const* ksrc, size_t n)
    {
        return access_copy(dspace, udst, nullptr, reinterpret_cast<uintptr_t>(ksrc), n);
    }

    // Copy one aligned pointer-sized word with one acquire. It cannot cross
    // a granule boundary. Use memcpy to preserve effective-type rules (reent.h).
    bool kaccess_word_to_user(UserOwner dspace, uintptr_t udst, void const* kword)
    {
        if ((udst & static_cast<uintptr_t>(sizeof(void*) - 1u)) != 0)
        {
            return false;
        }
#if KICKOS_ALIAS_DCACHE
        void* d = reinterpret_cast<void*>(udst);
        bool uncached = false;
#if KICKOS_HAVE_ASPACE
        if (dspace != nullptr)
        {
            d = arch_aspace_acquire(dspace, udst, &uncached);
            if (d == nullptr)
            {
                return false;
            }
        }
#else
        uncached = region_uncached(dspace, udst, sizeof(void*));
#endif
        if (uncached)
        {
            alias_sync(d, sizeof(void*));
        }
        __builtin_memcpy(__builtin_assume_aligned(d, sizeof(void*)), kword, sizeof(void*));
        if (uncached)
        {
            alias_sync(d, sizeof(void*));
        }
#if KICKOS_HAVE_ASPACE
        if (dspace != nullptr)
        {
            arch_aspace_release(dspace, udst);
        }
#endif
        return true;
#else
#if KICKOS_HAVE_ASPACE
        if (dspace != nullptr)
        {
            void* const d = arch_aspace_acquire(dspace, udst, nullptr);
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
#endif
    }

    // Compare overlap only within the same space, which is every pair where the backend does
    // not translate. Return failure rather than asserting: user code can request overlap, and
    // fault reporting uses this path.
    bool ep_copy(UserOwner dspace, uintptr_t dst, UserOwner sspace, uintptr_t src, size_t n)
    {
        bool one_space = true;
#if KICKOS_HAVE_ASPACE
        one_space = dspace == sspace;
#endif
        if (one_space and dst + n > src and src + n > dst)
        {
            return false;
        }
        return access_copy(dspace, dst, sspace, src, n);
    }

    // `out` == 0 is an info-less recv, which answers true. KCAP_INVALID marks a plain send
    // and a real handle marks a call.
    bool write_recv_info(UserOwner ospace, uintptr_t out, uint32_t badge, uint32_t reply_cap)
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
