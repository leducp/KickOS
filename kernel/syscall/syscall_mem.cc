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
#include <kickos/domain.h>
#include <kickos/frame_pool.h>
#include <kickos/kernel.h>
#include <kickos/kruntime.h>
#include <kickos/sched.h>
#include <kickos/task.h>

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

        // The running task's granted-range list, or null where it holds no space.
        VirtualRanges const* current_ranges(Thread const* c)
        {
            return domain_ranges(task_domain(c->task));
        }
    }
#endif

    // Confused-deputy floor: syscall_dispatch runs privileged (it bypasses the
    // MPU), so it must never dereference a user pointer the CALLER could not
    // itself reach. A range passes iff it lies within one region the current
    // thread is granted, with the required access: app code/rodata (RX), static data (RW),
    // its domain data, and its own stack from the thread.cc composition.
    // Privileged callers (kernel domain, trusted) bypass. Struct + out-pointer
    // args are caller STACK locals; a read buffer / name string may instead point
    // into the app's code/rodata/.data; see user_readable_ok for how those are
    // recognized on every backend (real MPU regions on HW, the host image on sim).
    //
    // TWO SOURCES ON A TRANSLATING BACKEND, AND THEY DESCRIBE DIFFERENT THINGS. The region
    // array is what every grant path records and the only oracle a descriptor board has, so
    // the walk below stays live on both. Beside it, the address space's granted-range list
    // answers for what no region array on that backend describes: the process image, mapped
    // into the space rather than granted to a thread (docs/design-m6-mmu.md section 3.3).
    // Nothing else admits an app global there, the link-time whitelist having been retired
    // with the re-key of arch_user_text_readable.
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
        // The mapping's own memory type, which is where a translating backend records it:
        // a self-grant seats no region there, so the array below holds no entry for a block
        // this question is ever asked about.
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
            // EXACT, not a superset: a region carrying a memory type the caller did not ask
            // for is a different mapping of the block, not a wider one.
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
        // The caller's own DEV window when its base is EXACTLY `base`, else 0. Exact base,
        // not containment, so a sub-block window cannot reach a whole-block table entry
        // (K64F PIT ch2 base 0x40037120, block 0x40037000).
        //
        // READS THE THREAD'S POSSESSION RECORD, NEVER ITS REACHABLE REGIONS. Authority here
        // is thread-local by contract while the mapping that carries the window is
        // task-wide on a translating backend, so deriving one from the other would make
        // every peer in the task a holder (docs/design-m6-mmu.md F9).
        size_t mmio_block_of(Thread const* c, uintptr_t base)
        {
            if (c->dev_size == 0 or c->dev_base != base)
            {
                return 0;
            }
            return c->dev_size;
        }
    }

    // MMIO possession, the sole authorisation for arch_periph_enable. NOT
    // user_range_ok: that funnel asks whether the kernel may dereference a user
    // pointer and passes trivially on len==0. This asks whether the caller owns the
    // whole block. Privileged callers pass, as in cap_check_authority.
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
        // Compared against the size, never as base + offset + 4: the window size is the
        // only operand that cannot wrap here.
        if (offset > size)
        {
            return false;
        }
        return size - offset >= sizeof(uint32_t);
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
    // region the caller is granted WRITE or, where the backend does not model app
    // static data as a region (every no-MPU chip, and the host sim), within the app's
    // writable data extent (arch_user_data_writable). Without that arm an unprivileged
    // thread's writable set on those backends is its own stack alone, so an
    // out-pointer that lives in a global is refused -KOS_EFAULT. Privileged callers
    // and len==0 pass via user_range_ok.
    bool user_writable_ok(uintptr_t ptr, size_t len)
    {
        if (user_range_ok(ptr, len, ARCH_MPU_W))
        {
            return true;
        }
        return arch_user_data_writable(ptr, len);
    }

    // The kernel<->user byte-access seam. IDENTITY today: a validated user
    // address is directly kernel-dereferenceable (one physical space), so these
    // are plain copies. The MMU era reimplements exactly these (per arch:
    // translate / copy across address spaces); every kernel-side user-pointer
    // dereference funnels here, so that becomes one function to change, not a
    // hunt across syscalls. Callers MUST validate first (user_range_ok /
    // user_readable_ok / user_writable_ok): this is the ACCESS, never the check.
    //
    // THE SEAM DOES NOT COPY OVERLAPPING RANGES, and the primitive below is the
    // ascending-only kmemcpy, not kmemmove. A caller that needs overlap reaches for
    // kmemmove here rather than widening a range.

    // kdst is kernel storage (a stack local or a kernel global) and usrc is user
    // memory, so the two ends are disjoint by construction.
    void kaccess_from_user(void* kdst, uintptr_t usrc, size_t n)
    {
        kmemcpy(kdst, reinterpret_cast<void const*>(usrc), n);
    }

    // ksrc is kernel storage, udst is user memory: disjoint for the same reason.
    void kaccess_to_user(uintptr_t udst, void const* ksrc, size_t n)
    {
        kmemcpy(reinterpret_cast<void*>(udst), ksrc, n);
    }

    // Bounded copy (<= KOS_EP_MSG_MAX). Both endpoints do it under IrqLock,
    // one side of which is a PARKED peer's user buffer (see endpoint.h): the
    // waker's own MPU regions are loaded, so it reaches the peer's memory only via
    // privileged background access (arch contract, design section 3.1). This is
    // the user<->user peer of the kaccess_*_user seam above (both ends are user
    // memory, not one kernel side): the ONE endpoint access the MMU era rewrites
    // as a cross-aspace copy. All endpoint payload movement funnels here already.
    //
    // The two ends belong to DIFFERENT threads, one running and one parked, and a
    // thread is never both, so they cannot alias. The assert is checked in every
    // build because no preset defines KICKOS_DEBUG.
    void ep_copy(uintptr_t dst, uintptr_t src, size_t n)
    {
        KICKOS_ASSERT(dst + n <= src or src + n <= dst);
        void* d = reinterpret_cast<void*>(dst);
        void const* s = reinterpret_cast<void const*>(src);
#if KICKOS_HAVE_ASPACE
        // ONE END IS IN A SPACE THAT IS NOT THE RUNNING ONE, the peer being parked, and
        // under translation that end has no address the running walk can reach. A
        // reservation and a thread stack are frame-pool runs whose virtual address IS their
        // output address (section 3.4), so the pool answers for the same bytes and no
        // per-page translation stands in front of the copy. A buffer the pool never handed
        // out is app static data, whose per-process copy this cannot reach: that is the
        // cross-address-space seam T7 owns, and it is the caller's own space here.
        void* const dalias = frame_pool_span(static_cast<arch_phys_addr_t>(dst), n);
        if (dalias != nullptr)
        {
            d = dalias;
        }
        void* const salias = frame_pool_span(static_cast<arch_phys_addr_t>(src), n);
        if (salias != nullptr)
        {
            s = salias;
        }
#endif
        kmemcpy(d, s, n);
    }

    // Deliver a receiver's kos_recv_info (badge + reply_cap) into its parked out-ptr,
    // or nothing when it asked for none (info-less recv, out == 0). KCAP_INVALID marks
    // a plain send; a real handle marks a call. Validated for 8 bytes at recv. A timed
    // recv points `out` at the kos_recv_info NESTED in its opts struct, so this stays a
    // whole-struct copy with no uninitialised tail and no input field to preserve.
    void write_recv_info(uintptr_t out, uint32_t badge, uint32_t reply_cap)
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
