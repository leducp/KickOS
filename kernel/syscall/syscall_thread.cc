// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Thread and task lifecycle syscalls.

#include <kickos/ampshare.h>
#include <kickos/arch/arch.h>
#include <kickos/aspace.h>
#include <kickos/cap.h>
#include <kickos/config.h>
#include <kickos/console_tx.h>
#include <kickos/domain.h>
#include <kickos/grant.h>
#include <kickos/image.h>
#include <kickos/instance.h>
#include <kickos/irq.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/kruntime.h>
#include <kickos/ramown.h>
#include <kickos/sched.h>
#include <kickos/sync.h>
#include <kickos/task.h>
#include <kickos/thread.h>
#include <kickos/time.h>
#include <kickos/ustack.h>

#include <kickos/sys/abi.h>
#include <kickos/sys/errno.h>
#include <kickos/tls.h>

#include "syscall_internal.h"

namespace kickos
{
    namespace
    {
        constexpr uint64_t CREATE_FOREVER = UINT64_MAX;

        // The monotonic instant a create of `timeout_us` stops waiting: 0 never waits.
        uint64_t create_until(uint32_t timeout_us)
        {
            if (timeout_us == KOS_TIMEOUT_NONE)
            {
                return CREATE_FOREVER;
            }
            if (timeout_us == 0u)
            {
                return 0;
            }
            return ktime_now() + static_cast<uint64_t>(timeout_us) * 1000u;
        }
    }

    int grant_device_window_admit(uintptr_t base, size_t size)
    {
        if (size == 0 or base + size < base)
        {
            return -KOS_EINVAL;
        }
#if KICKOS_HAVE_ASPACE
        // Whole granules inside an aperture the chip states, which holds no RAM: a window over
        // RAM would hand the task whatever frames sit there.
        if (not grant_window_aperture_ok(base, size))
        {
            return -KOS_EINVAL;
        }
        if (grant_hits_reserved(base, size))
        {
            return -KOS_EPERM; // the kernel's own device
        }
        if (not grant_memtype_at(ARCH_MAP_DEVICE, static_cast<arch_phys_addr_t>(base), size))
        {
            return -KOS_ENOTSUP;
        }
#else
        // Inside an aperture the chip states, which holds no RAM, as a translating build asks.
        if (not arch_mpu_region_encodable(base, size) or not grant_window_in_aperture(base, size))
        {
            return -KOS_EINVAL;
        }
        if (not grant_region_admissible(base, size, ARCH_MPU_R | ARCH_MPU_W | ARCH_MPU_DEV, true))
        {
            return -KOS_EPERM; // reserved block / bit-band alias
        }
#endif
        return 0;
    }

    namespace
    {
        // Give back what a spawn took before thread_create committed: the kernel's stack and
        // the thread slot. noinline: its frame lands on the armv7m SVC chain the trap red zone
        // measures.
        //
        // kstack_owned, not the pointer, says whether the stack was the kernel's: the app's own
        // allocator hands out frame-pool frames too, so the pool cannot tell the two apart.
        __attribute__((noinline)) void spawn_unwind(Kernel& k, ThreadAttr const& attr,
                                                    Task* tk, void* stack, size_t bytes,
                                                    int slot)
        {
#if KICKOS_HAVE_ASPACE
            (void)k;
            // The windows went in after the stack, and go back before it. Both were mapped
            // inside this spawn's one bracket, so no waiter parked on them.
            aspace_window_unmap_holder(domain_space(task_domain(tk)),
                                       domain_ranges_mut(task_domain(tk)),
                                       static_cast<uint16_t>(slot + 1));
            if (attr.kstack_owned)
            {
                ustack_free(task_domain(tk),
                            reinterpret_cast<uintptr_t>(stack), bytes);
            }
#else
            (void)tk;
            (void)bytes;
            if (attr.kstack_owned)
            {
                k.threads.stack_push(stack);
            }
#endif
            k.threads.release(slot);
        }

#if KICKOS_HAVE_ASPACE
        // Maps the admitted windows into tk's space where the kernel chooses, held by `slot`: a
        // device at its own frames, a memory window at the frames of the spawner's reservation,
        // whose domain it holds until unmapped. spawn_unwind takes back a partial list.
        int spawn_map_windows(Task* tk, kos_window const* list, uint16_t n, int slot)
        {
            Domain* const into = task_domain(tk);
            Domain* const own = thread_domain(sched::current());
            for (uint16_t i = 0; i < n; i++)
            {
                kos_window const& w = list[i];
                if (w.kind == KOS_WINDOW_PORTS)
                {
                    continue; // the switch loads a port window; nothing is mapped
                }
                arch_phys_addr_t pa = w.base;
                uint32_t rights = ARCH_MAP_R | ARCH_MAP_W;
                enum arch_map_memtype type = ARCH_MAP_DEVICE;
                Domain* donor = nullptr;
                if (w.kind == KOS_WINDOW_MEMORY)
                {
                    pa = aspace_frame_of(w.base);
                    type = ARCH_MAP_NORMAL;
                    donor = own;
                    if ((w.flags & KOS_WINDOW_RO) != 0)
                    {
                        rights = ARCH_MAP_R;
                    }
                    if ((w.flags & KOS_WINDOW_UNCACHED) != 0)
                    {
                        type = ARCH_MAP_NOCACHE;
                    }
                }
                int const rc = aspace_window_map(domain_payer(into), domain_space(into),
                                                 domain_ranges_mut(into), pa, w.size, rights,
                                                 type, static_cast<uint16_t>(slot + 1), i, donor);
                if (rc != 0)
                {
                    return rc;
                }
            }
            return 0;
        }
#endif

        // Whether an entry of `kind` before entry i of a staged window list overlaps it.
        bool earlier_overlaps(kos_window const* list, uint16_t i, uint8_t kind)
        {
            uintptr_t const last = list[i].base + list[i].size - 1u;
            for (uint16_t j = 0; j < i; j++)
            {
                if (list[j].kind == kind
                    and grant_ranges_overlap(list[i].base, last, list[j].base,
                                             list[j].base + list[j].size - 1u))
                {
                    return true;
                }
            }
            return false;
        }

        // Admits entry i of a staged window list, the entries before it already admitted, or
        // answers why not; the child joins `into`. Caller holds IrqLock. noinline, as
        // spawn_unwind: its locals would otherwise sit in the armv7m SVC chain's widest frame.
        __attribute__((noinline)) int window_admit(kos_window const* list, uint16_t i,
                                                   Task const* into)
        {
            kos_window const& w = list[i];
            Thread* const c = sched::current();
            if (w.size == 0 or w.base + w.size < w.base)
            {
                return -KOS_EINVAL;
            }
            if (w.kind == KOS_WINDOW_PORTS)
            {
#if KICKOS_ARCH_HAS_PORTS
                // Exclusive as a device window is, and inside an aperture the chip states, which
                // holds none of the kernel's ports and nothing that writes memory.
                if (w.flags != 0 or w.base + w.size > 0x10000u)
                {
                    return -KOS_EINVAL;
                }
                if (not cap_check_authority(c, AUTH_MEMORY))
                {
                    return -KOS_EPERM;
                }
                if (not port_aperture_ok(w.base, w.size))
                {
                    return -KOS_EINVAL;
                }
                if (not port_window_free(w.base, w.size)
                    or earlier_overlaps(list, i, KOS_WINDOW_PORTS))
                {
                    return -KOS_EBUSY;
                }
                return 0;
#else
                return -KOS_ENOTSUP;
#endif
            }
            if (w.kind == KOS_WINDOW_MEMORY)
            {
                if ((w.flags & ~(KOS_WINDOW_RO | KOS_WINDOW_UNCACHED)) != 0)
                {
                    return -KOS_EINVAL;
                }
                // One window per source base: a list names a block once.
                for (uint16_t j = 0; j < i; j++)
                {
                    if (list[j].kind == KOS_WINDOW_MEMORY and list[j].base == w.base)
                    {
                        return -KOS_EINVAL;
                    }
                }
#if KICKOS_HAVE_ASPACE
                // One reservation of the spawner's own, named whole, which the child's space
                // maps again where the kernel chooses; a handed-off one is another space's. The
                // spawn's own task data is checked where the window is mapped, after it.
                size_t const g = arch_aspace_granule();
                VirtualRanges const* const own = domain_ranges(thread_domain(c));
#if KICKOS_AMP_SHARE
                int share_rc = 0;
                if (amp_share_window(own, w, &share_rc))
                {
                    return share_rc;
                }
#endif
                VirtualRange const* e = nullptr;
                if (own != nullptr)
                {
                    e = own->at_base(w.base);
                }
                if (e == nullptr or not vr_caller_nameable(e) or (e->flags & VR_BORROWED) != 0
                    or e->pages != (w.size + g - 1u) / g)
                {
                    return -KOS_EPERM;
                }
                uint8_t type = ARCH_MAP_NORMAL;
                if ((w.flags & KOS_WINDOW_UNCACHED) != 0)
                {
                    if (not grant_memtype_at(ARCH_MAP_NOCACHE, aspace_frame_of(w.base),
                                             static_cast<size_t>(e->pages) * g))
                    {
                        return -KOS_ENOTSUP;
                    }
                    type = ARCH_MAP_NOCACHE;
                }
                if (not aspace_frames_type_ok(aspace_frame_of(w.base), e->pages, type, nullptr))
                {
                    return -KOS_EBUSY; // mapped elsewhere with another memory type
                }
                return 0;
#else
                uint32_t const attr = window_memory_attr(w.flags);
                int const arc = ram_region_admit(c, w.base, w.size, attr, RAM_ADMIT_GRANT);
                if (arc != 0)
                {
                    return arc;
                }
                arch_mpu_region data = {};
                Domain const* const dom = task_domain(into);
                if (dom != domain_kernel() and domain_region_count(dom) > 0)
                {
                    data = *domain_region_at(dom, 0);
                }
                if (data.size != 0 and ((data.attr ^ attr) & ARCH_MPU_NOCACHE) != 0
                    and grant_ranges_overlap(w.base, w.base + ram_grant_extent(w.size) - 1u,
                                             data.base, data.base + data.size - 1u))
                {
                    return -KOS_EBUSY;
                }
                return 0;
#endif
            }
            if (w.kind != KOS_WINDOW_DEVICE or w.flags != 0)
            {
                return -KOS_EINVAL;
            }
            if (not cap_check_authority(c, AUTH_MEMORY))
            {
                return -KOS_EPERM;
            }
            int const drc = grant_device_window_admit(w.base, w.size);
            if (drc != 0)
            {
                return drc;
            }
            // A bus master reaches memory by physical address past every unit.
            if (grant_window_bus_master(w.base, w.size)
                and not cap_check_authority(c, AUTH_BUS_MASTER))
            {
                return -KOS_EPERM;
            }
            if (not dev_window_free(w.base, w.size))
            {
                return -KOS_EBUSY; // already held: no stealing
            }
            if (console_window_withheld(w.base, w.size, into)
                or earlier_overlaps(list, i, KOS_WINDOW_DEVICE))
            {
                return -KOS_EBUSY; // the console's, or held twice within one list
            }
            return 0;
        }


        int task_create_admit(Thread* c, void* mem_base, size_t mem_size, uint32_t mem_attr)
        {
            if (not cap_check_authority(c, AUTH_TASKS))
            {
                return -KOS_EPERM;
            }
            if (mem_base != nullptr and mem_size != 0)
            {
                uintptr_t const base = reinterpret_cast<uintptr_t>(mem_base);
                if (base + mem_size < base)
                {
                    return -KOS_EINVAL;
                }
#if not KICKOS_HAVE_ASPACE
                return ram_region_admit(c, base, mem_size,
                                        ARCH_MPU_R | ARCH_MPU_W | (mem_attr & ARCH_MPU_NOCACHE),
                                        RAM_ADMIT_GRANT);
#else
                (void)mem_attr;
#endif
            }
            return 0;
        }

#if KICKOS_PRESYNC
        // A call working outside the lock reads only what its first round copied, never user
        // memory.
        PresyncRecord const* spawn_record()
        {
            PresyncRecord const* const r = presync_record();
            if (r == nullptr or not r->active)
            {
                return nullptr;
            }
            return r;
        }
#endif

        // The name pointer inside the copy is still user memory. user_readable_ok, because an
        // app global lies in no granted region on a backend modelling no static-data region.
        int spawn_params_in(uintptr_t pu, kos_thread_params* out)
        {
#if KICKOS_PRESYNC
            PresyncRecord const* const r = spawn_record();
            if (r != nullptr)
            {
                if (not r->has_params)
                {
                    return -KOS_EFAULT;
                }
                *out = r->params;
                return 0;
            }
#endif
            if (not user_readable_ok(pu, sizeof(*out)))
            {
                return -KOS_EFAULT;
            }
            // Can still refuse after user_readable_ok: the granted-range record and the page
            // tables may disagree (kickos/aspace.h).
            if (not kaccess_from_user(out, user_space_of(sched::current()), pu, sizeof(*out)))
            {
                return -KOS_EFAULT;
            }
            return 0;
        }

        // `p`'s window count and pointer already admitted.
        int spawn_windows_in(kos_thread_params const* p, kos_window* out)
        {
            uint16_t const n = p->window_count;
#if KICKOS_PRESYNC
            PresyncRecord const* const r = spawn_record();
            if (r != nullptr)
            {
                if (not r->has_windows)
                {
                    return -KOS_EFAULT;
                }
                for (uint16_t i = 0; i < n; i++)
                {
                    out[i] = r->windows[i];
                }
                return 0;
            }
#endif
            uintptr_t const wu = reinterpret_cast<uintptr_t>(p->windows);
            if (not user_readable_ok(wu, sizeof(kos_window) * n)
                or not kaccess_from_user(out, user_space_of(sched::current()), wu,
                                         sizeof(kos_window) * n))
            {
                return -KOS_EFAULT;
            }
            return 0;
        }

        // The priority a spawn seats: the caller's own base where it asks to inherit it.
        uint8_t spawn_prio(kos_thread_params const* p)
        {
            if ((p->flags & KOS_THREAD_INHERIT_PRIO) != 0u)
            {
                return sched::current()->base_prio;
            }
            return p->prio;
        }

        // The stack a spawn names, for a child joining `into`. Validated before the slot claim,
        // or a bad stack leaks a slot.
        int spawn_stack_admit(kos_thread_params const* p, Task const* into)
        {
            if (p->stack_base != nullptr)
            {
                uintptr_t const base = reinterpret_cast<uintptr_t>(p->stack_base);
                // The floor binds what is left once the carve comes off the block.
                if (p->stack_size < KICKOS_MIN_STACK_SIZE + tls_pool_carve_size(task_image(into))
                    or (base & (KICKOS_STACK_ALIGN - 1)) != 0
                    or (p->stack_size & (KICKOS_STACK_ALIGN - 1)) != 0
                    or base + p->stack_size < base)
                {
                    return -KOS_EINVAL;
                }
#if KICKOS_HAVE_ASPACE
                // A range granted R|W in the space the child will run in, which is not the
                // caller's for a create's entry thread. The image is excluded: a stack carved out
                // of an app global would sit in its static data.
                VirtualRanges const* const cr = domain_ranges(task_domain(into));
                VirtualRange const* e = nullptr;
                if (cr != nullptr)
                {
                    e = cr->find(base, p->stack_size);
                }
                if (cr != nullptr
                    and (not vr_caller_nameable(e) or e->state != VirtualState::Granted
                         or (e->rights & (ARCH_MAP_R | ARCH_MAP_W))
                                != (ARCH_MAP_R | ARCH_MAP_W)))
                {
                    return -KOS_EPERM;
                }
                // The kernel seats the child's thread-local block through this alias only, and
                // a granted range no pool frame backs (the AMP share) has none.
                if (cr != nullptr and ustack_kptr(base) == nullptr)
                {
                    return -KOS_EPERM;
                }
#else
                // An out-of-arena stack_base would grant an R|W window over peripheral or kernel
                // SRAM.
                int const arc = ram_stack_admit(sched::current(), base, p->stack_size);
                if (arc != 0)
                {
                    return arc;
                }
#endif
            }
            return 0;
        }

        // Leaves the admitted window list in window_stage. `staged`: window_stage holds the list
        // already, in place of p->windows.
        int spawn_windows_admit(kos_thread_params const* p, Task const* into, bool staged)
        {
            // A window is the thread's own region, carried by no task or domain. The call's
            // admission and the commit in thread_create run inside one IrqLock, so the pair is
            // atomic. Snapshotted in one pass and admitted from the copy, as the grant list is.
            if (p->window_count > 0)
            {
                uint16_t const nwin = p->window_count;
                kos_window* const wbuf = kernel().window_stage;
                if (nwin > KICKOS_MAX_THREAD_WINDOWS)
                {
                    return -KOS_ENOMEM;
                }
                if (not staged)
                {
                    uintptr_t const wu = reinterpret_cast<uintptr_t>(p->windows);
                    if (p->windows == nullptr or (wu & (alignof(kos_window) - 1)) != 0)
                    {
                        return -KOS_EINVAL;
                    }
                    int const crc = spawn_windows_in(p, wbuf);
                    if (crc != 0)
                    {
                        return crc;
                    }
                }
                for (uint16_t i = 0; i < nwin; i++)
                {
                    int const wrc = window_admit(wbuf, i, into);
                    if (wrc != 0)
                    {
                        return wrc;
                    }
                }
            }
#if KICKOS_MEMORY_ENFORCED and not KICKOS_HAVE_ASPACE
            // A cacheable stack under one of this spawn's own non-cacheable windows is incoherent;
            // spawn_stack_admit asked what is held already.
            if (p->stack_base != nullptr
                and not stack_type_free(reinterpret_cast<uintptr_t>(p->stack_base),
                                        arch_ram_region_size(p->stack_size), kernel().window_stage,
                                        p->window_count))
            {
                return -KOS_EBUSY;
            }
#endif
            return 0;
        }

        // The flags, the authority word and the grant list's shape; its entries are the locked
        // pass's.
        int spawn_authority_admit(kos_thread_params const* p)
        {
            if ((p->flags & ~(KOS_THREAD_ESSENTIAL | KOS_THREAD_INHERIT_PRIO)) != 0u)
            {
                return -KOS_EINVAL;
            }
            if (p->authority != 0)
            {
                if ((p->authority & ~CAP_AUTH_ALL) != 0)
                {
                    // Refused, never masked off. The authority word has its own numbering, so this
                    // catches only bits above the defined authorities, not an object right.
                    return -KOS_EINVAL;
                }
                if (not cap_check_authority(sched::current(), p->authority))
                {
                    return -KOS_EPERM;
                }
            }
            int const ncaps = static_cast<int>(p->cap_count);
            if (ncaps > 0)
            {
                // Delegated cap i lands at child index KICKOS_CAP_FIRST_DYNAMIC + i by default,
                // above the kernel's reserved slots, and cap.h asserts those indices fit the child
                // width; a caller-named destination is refused at the child table's bound.
                if (ncaps > KICKOS_MAX_SPAWN_GRANTS)
                {
                    return -KOS_EINVAL;
                }
                uintptr_t const cu = reinterpret_cast<uintptr_t>(p->caps);
                if (p->caps == nullptr or (cu & (alignof(kos_cap_grant) - 1)) != 0)
                {
                    return -KOS_EINVAL;
                }
            }
            return 0;
        }

        // Shared by the plan and the locked pass, before any claim. Leaves the admitted window
        // list in window_stage. Caller holds IrqLock.
        int spawn_admit(kos_thread_params const* p, Task const* into, bool staged = false)
        {
            // prio indexes the ready lists and drives a 1u<<prio bitmap shift, so an
            // out-of-range value is an OOB write and UB. Priority 0 is idle's alone.
            uint8_t const prio = spawn_prio(p);
            if (prio < KICKOS_PRIO_MIN or prio > KICKOS_PRIO_MAX)
            {
                return -KOS_EINVAL;
            }
            int rc = spawn_stack_admit(p, into);
            if (rc == 0)
            {
                rc = spawn_windows_admit(p, into, staged);
            }
            if (rc == 0)
            {
                rc = spawn_authority_admit(p);
            }
            if (rc != 0)
            {
                return rc;
            }
            return 0;
        }

        // kill_tag_of never answers KILL_TAG_NONE, so an orphan (a child whose spawner's slot
        // changed hands) matches nobody. Caller holds IrqLock.
        bool caller_spawned(Thread const* t, Thread const* c)
        {
            if (c == nullptr or t->spawner_tag == ThreadPool::KILL_TAG_NONE)
            {
                return false;
            }
            return t->spawner_tag == kernel().threads.kill_tag_of(c);
        }
    }

#if KICKOS_PRESYNC
    namespace
    {
        void presync_windows_in(Thread const* c, PresyncRecord* r, kos_window const* list,
                                uint16_t n)
        {
            uintptr_t const wu = reinterpret_cast<uintptr_t>(list);
            size_t const bytes = sizeof(kos_window) * n;
            if (n == 0 or n > KICKOS_MAX_THREAD_WINDOWS or wu == 0
                or (wu & (alignof(kos_window) - 1)) != 0 or not user_readable_ok(wu, bytes))
            {
                return;
            }
            r->has_windows = kaccess_from_user(r->windows, user_space_of(c), wu, bytes);
        }

#if KICKOS_ARCH_ALIAS_DCACHE
        void presync_windows_note(Thread const* c, PresyncRecord const* r, uint16_t n)
        {
            VirtualRanges const* const own = domain_ranges(thread_domain(c));
            for (uint16_t i = 0; i < n; i++)
            {
                kos_window const& w = r->windows[i];
                if (w.kind != KOS_WINDOW_MEMORY)
                {
                    continue;
                }
                enum arch_map_memtype type = ARCH_MAP_NORMAL;
                if ((w.flags & KOS_WINDOW_UNCACHED) != 0)
                {
                    type = ARCH_MAP_NOCACHE;
                }
                aspace_reservation_note(own, w.base, w.size, type);
            }
        }
#endif
    }

    void spawn_presync_enter(kos_thread_params const* p)
    {
        Thread const* const c = sched::current();
        PresyncRecord* const r = presync_record();
        uintptr_t const pu = reinterpret_cast<uintptr_t>(p);
        if (c == nullptr or r == nullptr or p == nullptr
            or (pu & (alignof(kos_thread_params) - 1)) != 0
            or not user_readable_ok(pu, sizeof(r->params))
            or not kaccess_from_user(&r->params, user_space_of(c), pu, sizeof(r->params)))
        {
            return;
        }
        r->has_params = true;
        presync_windows_in(c, r, r->params.windows, r->params.window_count);
    }

    void task_spawn_presync_enter(uintptr_t pu, uint32_t timeout_us)
    {
        Thread const* const c = sched::current();
        PresyncRecord* const r = presync_record();
        if (r != nullptr)
        {
            r->task_until = create_until(timeout_us);
        }
        if (c == nullptr or r == nullptr or pu == 0
            or (pu & (alignof(kos_task_params) - 1)) != 0
            or not user_readable_ok(pu, sizeof(r->task_params))
            or not kaccess_from_user(&r->task_params, user_space_of(c), pu,
                                     sizeof(r->task_params)))
        {
            return;
        }
        r->has_task_params = true;
        presync_windows_in(c, r, r->task_params.windows, r->task_params.window_count);
    }

    void spawn_presync_plan()
    {
        Thread* const c = sched::current();
        PresyncRecord const* const r = presync_record();
        if (c == nullptr or c->task == nullptr or r == nullptr or not r->has_params)
        {
            return;
        }
        kos_thread_params const& p = r->params;
        if (spawn_admit(&p, c->task) != 0)
        {
            return;
        }
#if KICKOS_ARCH_ALIAS_DCACHE
        presync_windows_note(c, r, p.window_count);
#endif
    }

    namespace
    {
        int program_instance(Thread* c, uint32_t prog, kos_task_params const& tp, int* refused_at);

        // What a create of declaration `decl`'s instance owes a sync and stages.
        void create_plan(Thread* c, void* mem_base, size_t mem_size, uint32_t mem_attr,
                         size_t decl)
        {
            if (task_create_admit(c, mem_base, mem_size, mem_attr) != 0)
            {
                return;
            }
            presync_stage_image(decl);
#if KICKOS_ARCH_ALIAS_DCACHE
            if (mem_base == nullptr or mem_size == 0)
            {
                return;
            }
            enum arch_map_memtype type = ARCH_MAP_NORMAL;
            if ((mem_attr & ARCH_MPU_NOCACHE) != 0)
            {
                type = ARCH_MAP_NOCACHE;
            }
            aspace_reservation_note(domain_ranges(thread_domain(c)),
                                    reinterpret_cast<uintptr_t>(mem_base), mem_size, type);
#else
            (void)mem_attr;
#endif
        }
    }

    void task_spawn_presync_plan(uint32_t prog)
    {
        Thread* const c = sched::current();
        PresyncRecord const* const r = presync_record();
        if (c == nullptr or c->task == nullptr or r == nullptr or not r->has_task_params)
        {
            return;
        }
        kos_task_params const& tp = r->task_params;
        uint32_t attr = 0;
        if (not mem_flags_to_attr(tp.flags, &attr))
        {
            return;
        }
        int refused_at = -1;
        int const sel = program_instance(c, prog, tp, &refused_at);
        if (sel < 0)
        {
            return;
        }
        create_plan(c, tp.block, tp.size, attr, static_cast<size_t>(sel));
#if KICKOS_ARCH_ALIAS_DCACHE
        presync_windows_note(c, r, tp.window_count);
#endif
    }

#endif

    namespace
    {
#if not KICKOS_PRESYNC
        // noinline: a member offset folded into every field access costs each one a wide
        // encoding on Thumb.
        __attribute__((noinline)) CreateStage* create_stage()
        {
            return &kernel().create_stage;
        }
#endif

        // A create's argv: the caller's vector, or none for { name }, and the bytes its strings
        // took with their NULs when they were measured.
        // `driver`: window_stage holds the entry thread's windows already, in place of p->windows,
        // and the entry thread is built and left out of the scheduler, for the caller to add.
        struct ArgBlock
        {
            uintptr_t vector;
            char const* name;
            uint16_t argc;
            uint16_t decl;
            uint16_t bytes;
            bool driver;
        };
        static_assert(KICKOS_ARGV_MAX <= UINT16_MAX, "an ArgBlock count no longer fits its field");

        // The caller-readable prefix of [s, s + room), found by halving rather than byte by
        // byte.
        size_t user_readable_prefix(uintptr_t s, size_t room)
        {
            if (user_readable_ok(s, room))
            {
                return room;
            }
            size_t lo = 0;
            size_t hi = room;
            while (hi - lo > 1u)
            {
                size_t const mid = lo + (hi - lo) / 2u;
                if (user_readable_ok(s, mid))
                {
                    lo = mid;
                }
                else
                {
                    hi = mid;
                }
            }
            return lo;
        }

        constexpr size_t NAME_CHUNK = 8u;

        // The length of the caller's string at `s`, read a chunk at a time: -KOS_EFAULT, or
        // -KOS_EINVAL where `room` bytes hold no NUL. With `own`, -KOS_ENOENT where the string
        // is not `own`.
        int user_string_len(uintptr_t s, size_t room, char const* own)
        {
            char chunk[NAME_CHUNK];
            size_t at = 0;
            while (at < room)
            {
                size_t want = room - at;
                if (want > NAME_CHUNK)
                {
                    want = NAME_CHUNK;
                }
                size_t const n = user_readable_prefix(s + at, want);
                if (n == 0 or not kaccess_from_user(chunk, user_space_of(sched::current()), s + at, n))
                {
                    return -KOS_EFAULT;
                }
                for (size_t j = 0; j < n; j++)
                {
                    if (own != nullptr and chunk[j] != own[at + j])
                    {
                        return -KOS_ENOENT;
                    }
                    if (chunk[j] == '\0')
                    {
                        return static_cast<int>(at + j);
                    }
                }
                if (n < want)
                {
                    return -KOS_EFAULT;
                }
                at = at + n;
            }
            return -KOS_EINVAL;
        }

        // struct kos_task_args and the strings behind it, rounded so SP stays aligned below.
        size_t arg_block_size(ArgBlock const& a)
        {
            size_t const raw = (4u + a.argc) * sizeof(uintptr_t) + a.bytes;
            return (raw + KICKOS_STACK_ALIGN - 1u) & ~static_cast<size_t>(KICKOS_STACK_ALIGN - 1u);
        }

        // `k` is where the kernel writes the block, `va` where the thread reads it, `block` the
        // task's shared block. Each string is copied straight from the caller in one copy, within
        // the bytes it measured at.
        int arg_block_write(ArgBlock const& a, unsigned char* k, uintptr_t va, uintptr_t block)
        {
            uintptr_t* const words = reinterpret_cast<uintptr_t*>(k);
            words[0] = a.argc;
            words[1] = a.decl;
            words[2] = block;
            size_t const strings = (4u + a.argc) * sizeof(uintptr_t);
            size_t at = 0;
            for (uint32_t i = 0; i < a.argc; i++)
            {
                words[3u + i] = va + strings + at;
                char* const into = reinterpret_cast<char*>(k + strings + at);
                size_t len = 0;
                if (a.vector == 0)
                {
                    while (a.name[len] != '\0')
                    {
                        len++;
                    }
                    kmemcpy(into, a.name, len);
                }
                else
                {
                    uintptr_t su = 0;
                    uintptr_t const slot = a.vector + i * sizeof(uintptr_t);
                    if (not user_readable_ok(slot, sizeof(su))
                        or not kaccess_from_user(&su, user_space_of(sched::current()), slot, sizeof(su)))
                    {
                        return -KOS_EFAULT;
                    }
                    int const n = user_string_len(su, a.bytes - at, nullptr);
                    if (n < 0 or not kaccess_from_user(into, user_space_of(sched::current()), su,
                                                       static_cast<size_t>(n)))
                    {
                        return -KOS_EFAULT;
                    }
                    len = static_cast<size_t>(n);
                }
                into[len] = '\0';
                at = at + len + 1u;
            }
            words[3u + a.argc] = 0;
            return 0;
        }
    }

    // Drops the references the first `n` grants of a spawn took.
    static void spawn_refs_undo(uint8_t const* kind, int const* obj, int n)
    {
        for (int ci = 0; ci < n; ci++)
        {
            obj_ref_undo(static_cast<CapType>(kcap_grant_type(kind[ci])), obj[ci],
                         kcap_grant_rights(kind[ci]));
        }
    }

    // The width a spawn seats: its params', or its task's where they ask none.
    static uint32_t spawn_cap_width(kos_thread_params const* p, Task const* tk)
    {
        if (p->cap_width != 0)
        {
            return p->cap_width;
        }
        return task_cap_width(tk);
    }

    // `p` is the kernel's copy. `kname` is the thread's name in kernel memory, or null for the
    // default; `args` the argv block the stack's top takes, with p->arg the task's shared block
    // it names, or null. `into` is the task a create's entry thread starts, which comes back in
    // *child out of the scheduler for its create to start; null for a thread of the caller's own
    // task, whose handle goes to the caller's word at `out` before the thread exists, so a
    // refused write is a refused spawn.
    static int spawn_locked(kos_thread_params const* p, char const* kname, ArgBlock const* args,
                            Task* into, uintptr_t out, Held lock, Thread** child)
    {
        Thread* const spawner = sched::current();
        Task* tk = into;
        if (tk == nullptr)
        {
            tk = spawner->task;
        }
        bool const driver_entry = args != nullptr and args->driver;
        int const arc = spawn_admit(p, tk, driver_entry);
        if (arc != 0)
        {
            return arc;
        }
        // The whole grant list is validated before anything is claimed. Sized by the grant
        // bound: these live on the caller's stack, which can be 1 KiB.
        int deleg_obj[KICKOS_MAX_SPAWN_GRANTS];
        // Kind and rights packed in one byte (kcap_grant_pack): this frame sits on the syscall
        // descent the trap red zone measures, and a third wide array would move the SVC
        // reservation a whole 64-byte step on every board that enforces the class.
        uint8_t deleg_kind[KICKOS_MAX_SPAWN_GRANTS];
        // A badge travels with the copy. Unbadged, the child's notification copy would raise bit
        // 0 and its endpoint copy would send as no client at all, neither a narrowing.
        uint32_t deleg_badge[KICKOS_MAX_SPAWN_GRANTS];
        uint16_t deleg_dest[KICKOS_MAX_SPAWN_GRANTS];
        int const ncaps = static_cast<int>(p->cap_count);

        if (ncaps > 0)
        {
            uintptr_t const cu = reinterpret_cast<uintptr_t>(p->caps);
            // user_readable_ok, as for the params struct: the array may be a global.
            if (not user_readable_ok(cu, sizeof(kos_cap_grant) * static_cast<size_t>(ncaps)))
            {
                return -KOS_EFAULT;
            }
            // Snapshotted in one pass and validated from the copy, so no double fetch. The copy
            // sits in the arrays it is validated into: the source handle in deleg_badge and the
            // mask in deleg_kind until the check of its own entry overwrites them.
            for (int ci = 0; ci < ncaps; ci++)
            {
                kos_cap_grant g;
                if (not kaccess_from_user(&g, user_space_of(spawner),
                                          cu + static_cast<size_t>(ci) * sizeof(kos_cap_grant),
                                          sizeof(kos_cap_grant)))
                {
                    return -KOS_EFAULT;
                }
                deleg_badge[ci] = g.source_cap;
                deleg_kind[ci] = g.rights_mask;
                deleg_dest[ci] = 0;
            }
            if (p->cap_dest != nullptr)
            {
                uintptr_t const du = reinterpret_cast<uintptr_t>(p->cap_dest);
                // Must precede the copy: a misaligned halfword load traps in the kernel on a
                // strict-align arch.
                if ((du & (alignof(uint16_t) - 1)) != 0)
                {
                    return -KOS_EINVAL;
                }
                if (not user_readable_ok(du, sizeof(uint16_t) * static_cast<size_t>(ncaps)))
                {
                    return -KOS_EFAULT;
                }
                for (int ci = 0; ci < ncaps; ci++)
                {
                    if (not kaccess_from_user(&deleg_dest[ci], user_space_of(spawner),
                                              du + static_cast<size_t>(ci) * sizeof(uint16_t),
                                              sizeof(uint16_t)))
                    {
                        return -KOS_EFAULT;
                    }
                }
            }
            for (int ci = 0; ci < ncaps; ci++)
            {
                kos_cap_t const source = deleg_badge[ci];
                uint8_t const mask = deleg_kind[ci];
                CapEntry* se = cap_lookup(spawner, source);
                if (se == nullptr or not cap_obj_live(*se))
                {
                    return -KOS_EBADF;
                }
                if ((se->rights & CAP_TRANSFER) != CAP_TRANSFER)
                {
                    return -KOS_EACCES;
                }
                // Narrow-only, with one exception: an endpoint cap carrying the handout right
                // may seat CAP_WAIT in the child without holding it.
                uint8_t grantable = se->rights;
                if (static_cast<CapType>(se->type) == CapType::CAP_ENDPOINT
                    and (se->rights & CAP_HANDOUT) != 0)
                {
                    grantable = static_cast<uint8_t>(grantable | CAP_WAIT);
                }
                if ((mask & grantable) != mask)
                {
                    return -KOS_EACCES;
                }
                deleg_obj[ci] = cap_obj(*se);
                deleg_kind[ci] = kcap_grant_pack(se->type, mask);
                deleg_badge[ci] = cap_badge(*se);
                // An absent array, or a 0 entry, means default placement, and 0 is the only
                // reserved index, so no destination names a reserved slot.
                static_assert(KOS_CAP_FIRST_DYNAMIC == 1,
                              "a second reserved index needs a destination refusal here");
                unsigned dest = deleg_dest[ci];
                if (dest == 0u)
                {
                    dest = static_cast<unsigned>(KOS_SPAWN_DELEGATED_CAP0) +
                           static_cast<unsigned>(ci);
                }
                deleg_dest[ci] = static_cast<uint16_t>(dest);
            }
            // Defaulted destinations included: a second install on the same slot would silently
            // overwrite the first and leak its reference.
            for (int ci = 1; ci < ncaps; ci++)
            {
                for (int cj = 0; cj < ci; cj++)
                {
                    if (deleg_dest[ci] == deleg_dest[cj])
                    {
                        return -KOS_EINVAL;
                    }
                }
            }
        }
        Kernel& k = kernel();
        // An ended task takes no member, live members or none: a restart is a new task.
        if (task_ended(tk))
        {
            return -KOS_EBUSY;
        }
        // A priority inheritance boost is not bounded by this ceiling (sched::set_prio).
        uint8_t const prio = spawn_prio(p);
        if (prio > task_prio_ceiling(tk))
        {
            return -KOS_EPERM;
        }
#if KICKOS_KERNEL_CORES > 1
        // 0 asks for the task's default set, not for the whole machine.
        uint32_t requested = p->core_mask;
        if (requested == 0)
        {
            requested = task_default_cores(tk);
        }
        uint32_t seated_cores = 0;
        int const crc = sched_admit_mask(requested, task_core_set(tk), MaskBound::INTERSECT,
                                         &seated_cores);
        if (crc != 0)
        {
            return crc;
        }
#endif
        uint32_t const width = spawn_cap_width(p, tk);
        if (width < static_cast<uint32_t>(KOS_CAP_FIRST_DYNAMIC + ncaps))
        {
            return -KOS_EINVAL;
        }
        if (not task_admit(tk, BudgetKind::CAP_SLOTS, kcap_width_for(width)))
        {
            return -KOS_EAGAIN;
        }
        if (kcap_chunks_for(width) > cap_dir_length())
        {
            return -KOS_EINVAL;
        }
        int const i = k.threads.alloc(tk);
        if (i == SLOT_OVER_BUDGET)
        {
            return -KOS_EAGAIN;
        }
        if (i < 0)
        {
            return -KOS_ENOMEM;
        }

        ThreadAttr attr;
        attr.name = "user";
        if (kname != nullptr)
        {
            attr.name = kname;
        }
        attr.prio = prio;
        attr.policy = Policy::FIFO;
        if (p->policy == KOS_POLICY_RR)
        {
            attr.policy = Policy::RR;
        }
        attr.quantum_ns = p->quantum_ns;
        attr.windows = kernel().window_stage;
        attr.window_count = p->window_count;
        attr.task = tk;
        attr.spawner_tag = k.threads.kill_tag_of(spawner);
        // The first member a non-member seats.
        if (tk != spawner->task and task_member_count(tk) == 0)
        {
            attr.task_role = TASK_ROLE_ENTRY;
        }
        else if ((p->flags & KOS_THREAD_ESSENTIAL) != 0u)
        {
            attr.task_role = TASK_ROLE_ESSENTIAL;
        }
#if KICKOS_KERNEL_CORES > 1
        attr.core_mask = seated_cores;
#endif

        // Both sources failing must release the slot just claimed, or the spawn leaks a TCB
        // and burns the prior occupant's join handle.
        void* stack = p->stack_base;
        size_t stack_size = p->stack_size;
        if (p->stack_base == nullptr)
        {
#if KICKOS_HAVE_ASPACE
            // Frames in the task's space, its guard's included: the arena is linked in the
            // kernel's half, which EL0 cannot reach.
            UserStack const us = ustack_alloc(task_domain(tk),
                                              KICKOS_USER_STACK_SIZE);
            stack = reinterpret_cast<void*>(us.base);
            stack_size = us.bytes;
            if (stack == nullptr)
            {
                spawn_unwind(k, attr, tk, stack, stack_size, i);
                if (us.over_budget)
                {
                    return -KOS_EAGAIN;
                }
                return -KOS_ENOMEM;
            }
            attr.kstack_owned = true;
#else
            stack = k.threads.stack_pop();
            if (stack != nullptr)
            {
                // A recycled block still holds the dead thread's locals, and the new thread's
                // region covers it. arch_ram_alloc's blocks come out of .bss and are never
                // freed, so only the free list can carry a former owner. Cleared under the
                // spawn's one bracket: a gap here lets a slay claim this spawner's resume, and
                // the block it popped would then never go back.
                kmemset(stack, 0, KICKOS_USER_STACK_SIZE);
            }
            else
            {
                stack = arch_ram_alloc(KICKOS_USER_STACK_SIZE);
            }
            if (stack == nullptr)
            {
                spawn_unwind(k, attr, tk, stack, stack_size, i);
                return -KOS_ENOMEM;
            }
            stack_size = KICKOS_USER_STACK_SIZE;
            attr.kstack_owned = true;
#endif
        }
#if KICKOS_HAVE_ASPACE
        int const wrc = spawn_map_windows(tk, kernel().window_stage, p->window_count, i);
        if (wrc != 0)
        {
            spawn_unwind(k, attr, tk, stack, stack_size, i);
            return wrc;
        }
#endif
        // Where the thread pointer is SP masked down to KICKOS_TLS_STRIDE, a caller-supplied
        // block that is not strided, or that spans more than one stride, would hand this thread
        // a pointer into a neighbour's thread_local storage.
        size_t const image = task_image(tk);
        if (not tls_stack_admissible(reinterpret_cast<uintptr_t>(stack), stack_size, image))
        {
            spawn_unwind(k, attr, tk, stack, stack_size, i);
            return -KOS_EINVAL;
        }
        // The argv block comes off the top, the carve off the bottom.
        void* arg = p->arg;
        if (args != nullptr)
        {
            size_t const bytes = arg_block_size(*args);
            if (stack_size < bytes + KICKOS_MIN_STACK_SIZE + tls_pool_carve_size(image))
            {
                spawn_unwind(k, attr, tk, stack, stack_size, i);
                return -KOS_EINVAL;
            }
            unsigned char* home = static_cast<unsigned char*>(stack);
#if KICKOS_HAVE_ASPACE
            home = static_cast<unsigned char*>(ustack_kptr(reinterpret_cast<uintptr_t>(stack)));
#endif
            uintptr_t const va = reinterpret_cast<uintptr_t>(stack) + stack_size - bytes;
            int const arc_write = arg_block_write(*args, home + (stack_size - bytes), va,
                                                  reinterpret_cast<uintptr_t>(p->arg));
            if (arc_write != 0)
            {
                spawn_unwind(k, attr, tk, stack, stack_size, i);
                return arc_write;
            }
            attr.arg_bytes = static_cast<uint16_t>(bytes);
            arg = reinterpret_cast<void*>(va);
        }
#if KICKOS_MEMORY_ENFORCED and KICKOS_HAVE_MPU and not KICKOS_HAVE_ASPACE
        attr.regions = thread_regions_stage(task_domain(tk), attr, stack, stack_size);
        // On PMSAv8 any overlap faults the child's exception entry and the kernel's own reads of
        // its frame.
        if (not attr.regions->overlaps_expressible())
        {
            spawn_unwind(k, attr, tk, stack, stack_size, i);
            return -KOS_EINVAL;
        }
#endif
        // Taken before the reference loop so one unwind path serves both failures.
        attr.cap_dir = cap_directory(i);
        // Read again rather than held across the claim: a word live here is a word of this frame,
        // which the trap red zone prices.
        if (not cap_slab_attach(attr.cap_dir, spawn_cap_width(p, tk), &attr.cap_free_head,
                                &attr.cap_width))
        {
            spawn_unwind(k, attr, tk, stack, stack_size, i);
            return -KOS_ENOMEM;
        }
        // Bounded by the run the child actually gets, and checked before any reference is
        // taken so the only unwind owed here is the run.
        for (int ci = 0; ci < ncaps; ci++)
        {
            if (deleg_dest[ci] >= attr.cap_width)
            {
                cap_slab_detach(attr.cap_dir, &attr.cap_free_head, &attr.cap_width);
                spawn_unwind(k, attr, tk, stack, stack_size, i);
                return -KOS_EINVAL;
            }
        }

        // The grant list is a take against the child's task, admitted before any reference is
        // taken. Without it a task at its ceiling passes its objects into a second task and
        // takes its whole ceiling over again.
        if (not task_object_admit_grants(tk, deleg_kind, deleg_obj, ncaps))
        {
            cap_slab_detach(attr.cap_dir, &attr.cap_free_head, &attr.cap_width);
            spawn_unwind(k, attr, tk, stack, stack_size, i);
            return -KOS_EAGAIN; // the destination task holds its ceiling of one of the pools
        }

        // Every fallible step precedes thread_create, after which the unwind would also owe the
        // task reference and the child's seated caps. The handle write is the last of them: a
        // refusal after it would leave the caller a name for nothing.
        for (int ci = 0; ci < ncaps; ci++)
        {
            if (obj_ref_inc(static_cast<CapType>(kcap_grant_type(deleg_kind[ci])), deleg_obj[ci],
                            kcap_grant_rights(deleg_kind[ci])))
            {
                continue;
            }
            spawn_refs_undo(deleg_kind, deleg_obj, ci);
            cap_slab_detach(attr.cap_dir, &attr.cap_free_head, &attr.cap_width);
            spawn_unwind(k, attr, tk, stack, stack_size, i);
            return -KOS_EOVERFLOW; // an object refcount is at its ceiling
        }
        if (into == nullptr)
        {
            kos_thread_t const named = k.threads.handle_for(i);
            if (not kaccess_to_user(user_space_of(spawner), out, &named, sizeof(named)))
            {
                spawn_refs_undo(deleg_kind, deleg_obj, ncaps);
                cap_slab_detach(attr.cap_dir, &attr.cap_free_head, &attr.cap_width);
                spawn_unwind(k, attr, tk, stack, stack_size, i);
                return -KOS_EFAULT;
            }
        }
        thread_create(&k.threads.slots()[i], p->entry, arg, stack, stack_size, attr);
        // Nothing below here may fail: the validation above guarantees each install succeeds,
        // and the reference loop already holds a reference for every cap seated here.
        Thread* const made = &k.threads.slots()[i];
        task_run_join(tk, i);
        cap_install_defaults(made, lock);
        cap_seat_authority(made, p->authority);
        for (int ci = 0; ci < ncaps; ci++)
        {
            cap_install_at(made, static_cast<int>(deleg_dest[ci]), deleg_obj[ci],
                           static_cast<CapType>(kcap_grant_type(deleg_kind[ci])),
                           kcap_grant_rights(deleg_kind[ci]), deleg_badge[ci]);
        }
        // Stays inside the lock: a spawner slain between an allocated child and sched::add never
        // returns to its continuation (switch_book redirects a CANCEL_SLAY thread to the exit
        // stub), leaving a fully built INACTIVE orphan that nothing frees.
        if (into == nullptr)
        {
            sched::add(made, lock);
        }
        presync_commit();
        *child = made;
        return 0;
    }

    static int spawn_masked(kos_thread_params const* p, uintptr_t out)
    {
        IrqLock lock;
        if (p == nullptr)
        {
            return -KOS_EINVAL;
        }
        // The misalignment reject must precede the typed copy: on a strict-align arch a
        // misaligned word load traps in the kernel.
        uintptr_t const pu = reinterpret_cast<uintptr_t>(p);
        if ((pu & (alignof(kos_thread_params) - 1)) != 0)
        {
            return -KOS_EINVAL;
        }
#if KICKOS_PRESYNC
        kos_thread_params params;
#else
        kos_thread_params& params = create_stage()->params;
#endif
        int const prc = spawn_params_in(pu, &params);
        if (prc != 0)
        {
            return prc;
        }
        // Each source byte is checked caller-readable before the privileged copy: the kernel
        // must neither fault on a bad name pointer nor leak another domain's page through it.
        // A string with no NUL stops at the first unreachable byte.
        char namebuf[KICKOS_THREAD_NAME_MAX];
        char const* kname = nullptr;
        if (params.name != nullptr)
        {
            uintptr_t const np = reinterpret_cast<uintptr_t>(params.name);
            size_t ni = 0;
            bool name_ok = false;
            for (; ni + 1 < sizeof(namebuf); ni++)
            {
                if (not user_readable_ok(np + ni, 1))
                {
                    break;
                }
                if (not kaccess_from_user(&namebuf[ni], user_space_of(sched::current()),
                                          np + ni, 1))
                {
                    break; // readable a moment ago, gone now
                }
                if (namebuf[ni] == '\0')
                {
                    name_ok = true;
                    break;
                }
            }
            if (ni + 1 >= sizeof(namebuf))
            {
                name_ok = true; // filled to the cap with readable bytes, no NUL yet
            }
            namebuf[ni] = '\0';
            if (name_ok)
            {
                kname = namebuf;
            }
        }
        Thread* child = nullptr;
        return spawn_locked(&params, kname, nullptr, nullptr, out, lock, &child);
    }

    int thread_create_call(kos_thread_params const* p, uintptr_t out)
    {
        return spawn_masked(p, out);
    }

    namespace
    {
        // The refusals both kinds share, under the caller's lock: 0 with *out the live target.
        int cancel_target(Thread* c, kos_thread_t thread, Thread** out)
        {
            Thread* const t = kernel().threads.resolve(thread);
            if (t == nullptr)
            {
                return -KOS_EBADF;
            }
            // Nothing left to cancel.
            if (t->state == ThreadState::EXITED or t->state == ThreadState::INACTIVE)
            {
                return -KOS_EBADF;
            }
            if (t == c)
            {
                return -KOS_EINVAL; // ending yourself is kos_exit; this path must return
            }
            // Cancelling idle ends the scheduler's fallback.
            if (sched::is_idle(t))
            {
                return -KOS_EINVAL;
            }
            if (not caller_spawned(t, c))
            {
                return -KOS_EPERM;
            }
            *out = t;
            return 0;
        }
    }

    // A kill is cooperative: the target dies at its next syscall entry, in its own exit_current,
    // and a thread that never enters the kernel again survives. A slay claims the target's
    // resume (switch_to rebuilds its context into kickos_thread_slay_exit before arch_switch), so
    // it never returns to userspace and never gets the window in which a driver would have
    // quieted its device; it still runs its own cap_teardown, in its own context.
    //
    // No timer is needed to reach a spinning victim: on one core a target distinct from the
    // caller is READY, BLOCKED or refused, and both live states are claimed at the resume.
    int thread_cancel_call(kos_thread_t thread, uintptr_t kind, uint32_t timeout_us)
    {
        uint8_t cancel = CANCEL_KILL;
        if (kind == KOS_CANCEL_SLAY)
        {
            cancel = CANCEL_SLAY;
        }
        else if (kind != KOS_CANCEL_KILL)
        {
            return -KOS_EINVAL;
        }
        Thread* const c = sched::current();
        if (timeout_us == 0u)
        {
            IrqLock lock;
            Thread* t = nullptr;
            int const rc = cancel_target(c, thread, &t);
            if (rc != 0)
            {
                return rc;
            }
            thread_cancel_kind(t, cancel, lock);
            return 0;
        }
        uint32_t epoch = 0;
        {
            IrqLock lock;
            // Ahead of the park and of the cancel below: an exit taken between them would leave
            // the victim uncancelled with the caller gone.
            ParkToken const ask = park_cancel_pending(c);
            if (ask.cancelled())
            {
                sched::exit_current(KOS_EXIT_CANCELLED, sched::EXIT_RETURN, &lock);
            }
            Thread* t = nullptr;
            int const rc = cancel_target(c, thread, &t);
            if (rc != 0)
            {
                return rc;
            }
            // Parked first, so the cancel takes no pass: from a running caller it could run the
            // victim to its exit, whose sweep would then find nobody parked on it.
            epoch = park_queueless(ask)(c, WAIT_JOIN, t, lock);
            ktime_deadline_arm(c, timeout_us, lock);
            thread_cancel_kind(t, cancel, lock);
            sched::reschedule(nullptr, lock);
        }
        wq_confirm_resume(c, epoch); // the lock is released across this: see sync.h
        // 0 (the target is gone and swept), -KOS_ETIMEDOUT (cancelled but not yet gone), or
        // -KOS_ECANCELED (the caller was itself cancelled, e.g. by the victim's group cancel).
        return static_cast<int>(c->wait_result);
    }

#if KICKOS_KERNEL_CORES > 1
    // Also what keeps KOS_AFFINITY_QUERY, bit 31, from ever naming a core.
    static_assert(KICKOS_KERNEL_CORES <= 31, "a core mask must never read as an error");

    int thread_affinity_call(kos_thread_t thread, uint32_t core_mask)
    {
        IrqLock lock;
        Thread* const c = sched::current();
        Thread* const t = kernel().threads.resolve(thread);
        if (t == nullptr)
        {
            return -KOS_EBADF;
        }
        // Nothing left to place.
        if (t->state == ThreadState::EXITED or t->state == ThreadState::INACTIVE)
        {
            return -KOS_EBADF;
        }
        // Self needs no clause of its own: a thread is in its own group.
        if (not task_same_group(c, t))
        {
            return -KOS_EPERM;
        }
        if (core_mask == KOS_AFFINITY_QUERY)
        {
            return static_cast<int>(t->affinity);
        }
        // 0 asks for the task's default set, as at spawn.
        uint32_t requested = core_mask;
        if (requested == 0)
        {
            requested = task_default_cores(t->task);
        }
        uint32_t effective = 0;
        int const arc = sched_admit_mask(requested, task_core_set(t->task),
                                         MaskBound::INTERSECT, &effective);
        if (arc != 0)
        {
            return arc;
        }
        sched::set_affinity(t, effective);
        return static_cast<int>(effective);
    }

    int task_cores(kos_task_t task)
    {
        IrqLock lock;
        Thread* const c = sched::current();
        Task const* t = c->task;
        if (task != KOS_TASK_NONE)
        {
            t = task_resolve(task);
            if (t == nullptr)
            {
                return -KOS_EBADF;
            }
            if (not task_created_by(t, kernel().threads.kill_tag_of(c)))
            {
                return -KOS_EPERM;
            }
        }
        return static_cast<int>(task_core_set(t));
    }
#endif

    int thread_set_priority(uintptr_t priority)
    {
        // prio indexes the ready lists and a 1u<<prio bitmap shift, as at spawn.
        if (priority < KICKOS_PRIO_MIN or priority > KICKOS_PRIO_MAX)
        {
            return -KOS_EINVAL;
        }
        uint8_t const p = static_cast<uint8_t>(priority);
        IrqLock lock;
        Thread* const c = sched::current();
        if (p > task_prio_ceiling(c->task))
        {
            return -KOS_EPERM;
        }
        c->base_prio = p;
        // A boost the caller holds stays: the funnel answers the base or the boost above it.
        uint8_t const was = c->prio;
        uint8_t const now = thread_effective_prio(c);
        if (now != was)
        {
            sched::set_prio(c, now, lock);
            if (now < was)
            {
                sched::reschedule(nullptr, lock);
            }
        }
        return 0;
    }

    namespace
    {
        // Declaration `decl`'s instance in its own record, which the declaration gives its
        // ceiling and cores. Caller holds IrqLock.
        int task_build(Thread* c, void* mem_base, size_t mem_size, uint32_t mem_attr,
                       size_t decl, Held lock, Task** out)
        {
            int const arc = task_create_admit(c, mem_base, mem_size, mem_attr);
            if (arc != 0)
            {
                return arc;
            }
            kos_task_decl const* const d = image_decl(decl);
            if (d == nullptr)
            {
                return -KOS_EINVAL;
            }
            // Both halves admitted against the creator's grant, which the task inherits whole,
            // so the narrowing below cannot refuse.
            if (d->ceiling > task_prio_ceiling(c->task))
            {
                return -KOS_EPERM;
            }
            uint32_t cores = d->core_mask;
#if KICKOS_KERNEL_CORES > 1
            if (cores != 0)
            {
                int const crc = sched_admit_mask(d->core_mask, task_core_set(c->task),
                                                 MaskBound::SUBSET, &cores);
                if (crc != 0)
                {
                    return crc;
                }
            }
#endif
            int derr = 0;
            Task* const t = task_create_declared(decl, image_of_decl(decl),
                                                 kernel().threads.kill_tag_of(c), mem_base,
                                                 mem_size, mem_attr, thread_domain(c), &derr,
                                                 lock);
            if (t == nullptr)
            {
                return -derr;
            }
            task_sched_inherit(t, c->task);
            (void)task_sched_narrow(t, d->ceiling, cores);
            *out = t;
            return 0;
        }

        int task_params_in(uintptr_t pu, kos_task_params* out)
        {
            bool copied = false;
#if KICKOS_PRESYNC
            PresyncRecord const* const r = spawn_record();
            if (r != nullptr)
            {
                if (not r->has_task_params)
                {
                    return -KOS_EFAULT;
                }
                *out = r->task_params;
                copied = true;
            }
#endif
            if (not copied
                and ((pu & (alignof(kos_task_params) - 1)) != 0
                     or not user_readable_ok(pu, sizeof(*out))
                     or not kaccess_from_user(out, user_space_of(sched::current()), pu,
                                              sizeof(*out))))
            {
                return -KOS_EFAULT;
            }
            if (out->rsv0 != 0)
            {
                return -KOS_EINVAL;
            }
            return 0;
        }

        // The authority bits declaration `d` needs of its creator: its own, and those its devices,
        // lines and console are claimed with.
        uint32_t decl_authority(kos_task_decl const& d)
        {
            uint32_t need = d.authority;
            kos_decl_grant const* const g = image_decl_grants(&d);
            for (uint16_t k = 0; k < d.grant_count; k++)
            {
                if (g[k].kind == KOS_DECL_LINE)
                {
                    need = need | AUTH_IRQ;
                }
                else
                {
                    need = need | AUTH_MEMORY;
                }
            }
            if ((d.flags & KOS_DECL_CONSOLE) != 0)
            {
                need = need | AUTH_CONSOLE;
            }
            return need;
        }

        // Whether the caller may start declaration `d`.
        int decl_admit(Thread* c, kos_task_decl const& d)
        {
            if ((d.flags & KOS_DECL_ON_REQUEST) == 0 and not kernel().threads.is_root(c))
            {
                return -KOS_EPERM;
            }
            // Refused, never narrowed: the declaration states what the program needs.
            uint32_t const need = decl_authority(d);
            if (need != 0 and not cap_check_authority(c, need))
            {
                return -KOS_EPERM;
            }
            if (d.ceiling > task_prio_ceiling(c->task))
            {
                return -KOS_EPERM;
            }
#if KICKOS_KERNEL_CORES > 1
            uint32_t cores = 0;
            if (d.core_mask != 0)
            {
                return sched_admit_mask(d.core_mask, task_core_set(c->task), MaskBound::SUBSET,
                                        &cores);
            }
#endif
            return 0;
        }

        // 0 where the caller may start declaration `i` now, else the refusal a create naming it
        // answers.
        int decl_startable(Thread* c, size_t i)
        {
            int const rc = decl_admit(c, *image_decl(i));
            if (rc != 0)
            {
                return rc;
            }
            return task_decl_state(i);
        }

        // How near refusal `rc` leaves its instance to a start: a retry, its death, never.
        int refusal_rank(int rc)
        {
            if (rc == -KOS_EAGAIN)
            {
                return 2;
            }
            if (rc == -KOS_EBUSY)
            {
                return 1;
            }
            return 0;
        }

        // The declaration over the program of image `program` a create starts, by index, or
        // the refusal, with the declaration it refuses in *refused_at: every image running that
        // program holds its own declarations.
        int instance_select(Thread* c, kos_task_params const& tp, size_t program, int* refused_at)
        {
            size_t const n = image_decl_count();
            if (tp.instance != nullptr)
            {
                for (size_t i = 0; i < n; i++)
                {
                    if (not image_same_program(image_of_decl(i), program))
                    {
                        continue;
                    }
                    kos_task_decl const* const d = image_decl(i);
                    int const is = user_string_len(reinterpret_cast<uintptr_t>(tp.instance),
                                                   KICKOS_ARGV_MAX, image_decl_name(d));
                    if (is == -KOS_ENOENT)
                    {
                        continue;
                    }
                    if (is < 0)
                    {
                        return is;
                    }
                    int const rc = decl_startable(c, i);
                    if (rc != 0)
                    {
                        *refused_at = static_cast<int>(i);
                        return rc;
                    }
                    return static_cast<int>(i);
                }
                return -KOS_ENOENT;
            }
            // None startable: the refusal of the instance nearest to a start.
            int refused = -KOS_ENOENT;
            for (size_t i = 0; i < n; i++)
            {
                if (not image_same_program(image_of_decl(i), program))
                {
                    continue;
                }
                int const rc = decl_startable(c, i);
                if (rc == 0)
                {
                    return static_cast<int>(i);
                }
                if (refused == -KOS_ENOENT or refusal_rank(rc) > refusal_rank(refused))
                {
                    refused = rc;
                    *refused_at = static_cast<int>(i);
                }
            }
            if (refused == -KOS_EBUSY)
            {
                return -KOS_ENOMEM; // every instance the caller may start is live
            }
            return refused;
        }

        // The declaration a create naming program capability `prog` instantiates: its index,
        // or a refusal, as instance_select. Caller holds IrqLock.
        int program_instance(Thread* c, uint32_t prog, kos_task_params const& tp, int* refused_at)
        {
            int err = 0;
            void const* const program = cap_resolve_e(c, prog, CapType::CAP_PROGRAM, 0, &err);
            if (program == nullptr)
            {
                return -err;
            }
            if (not cap_check_authority(c, AUTH_TASKS))
            {
                return -KOS_EPERM;
            }
            int image = 0;
            while (image_program(image) != nullptr and image_program(image) != program)
            {
                image++;
            }
            return instance_select(c, tp, static_cast<size_t>(image), refused_at);
        }

        // The caller's capability that delegated capability `at` of the create's list names, of
        // `type`.
        int create_source(Thread* c, kos_task_params const& tp, uint32_t at, CapType type,
                          uint32_t* out)
        {
            uintptr_t const gu = reinterpret_cast<uintptr_t>(tp.caps) + at * sizeof(kos_cap_grant);
            if (at >= tp.cap_count or tp.caps == nullptr
                or (gu & (alignof(kos_cap_grant) - 1)) != 0)
            {
                return -KOS_EINVAL;
            }
            kos_cap_grant g;
            if (not user_readable_ok(gu, sizeof(g))
                or not kaccess_from_user(&g, user_space_of(c), gu, sizeof(g)))
            {
                return -KOS_EFAULT;
            }
            CapEntry const* const e = cap_lookup(c, g.source_cap);
            if (e == nullptr or static_cast<CapType>(e->type) != type)
            {
                return -KOS_EINVAL;
            }
            *out = g.source_cap;
            return 0;
        }

        // The endpoint behind delegated capability `at`, which the declaration names ready.
        int ready_endpoint(Thread* c, kos_task_params const& tp, uint32_t at, int* obj)
        {
            uint32_t cap = 0;
            int const rc = create_source(c, tp, at, CapType::CAP_ENDPOINT, &cap);
            if (rc != 0)
            {
                return rc;
            }
            *obj = cap_lookup(c, cap)->obj;
            return 0;
        }

        // The entry thread's destination of each delegated capability, as spawn_locked places it.
        int create_dests(Thread* c, kos_task_params const& tp, uint16_t* out)
        {
            if (tp.cap_count > KICKOS_MAX_SPAWN_GRANTS)
            {
                return -KOS_EINVAL;
            }
            uintptr_t const du = reinterpret_cast<uintptr_t>(tp.cap_dest);
            if (tp.cap_dest != nullptr
                and ((du & (alignof(uint16_t) - 1)) != 0
                     or not user_readable_ok(du, sizeof(uint16_t) * tp.cap_count)
                     or not kaccess_from_user(out, user_space_of(c), du,
                                              sizeof(uint16_t) * tp.cap_count)))
            {
                return -KOS_EFAULT;
            }
            for (uint8_t i = 0; i < tp.cap_count; i++)
            {
                if (tp.cap_dest == nullptr or out[i] == 0)
                {
                    out[i] = static_cast<uint16_t>(KOS_SPAWN_DELEGATED_CAP0 + i);
                }
            }
            return 0;
        }

        // Whether line slot `slot` of grant `k` is a free index of the entry thread's table,
        // `width` wide.
        bool line_slot_free(uint16_t slot, uint32_t width, kos_decl_grant const* g, uint16_t k,
                            uint16_t const* dest, uint8_t n)
        {
            if (slot < KOS_SPAWN_DELEGATED_CAP0 or slot >= width)
            {
                return false;
            }
            for (uint8_t i = 0; i < n; i++)
            {
                if (dest[i] == slot)
                {
                    return false;
                }
            }
            for (uint16_t j = 0; j < k; j++)
            {
                if (g[j].kind == KOS_DECL_LINE and g[j].slot == slot)
                {
                    return false;
                }
            }
            return true;
        }

        // The notification the declaration's lines are bound to, its object and its delivery
        // core, or the refusal.
        int lines_notify(Thread* c, kos_task_params const& tp, kos_task_decl const& d,
                         Notification** n, int* obj, uint32_t* core)
        {
            uint32_t note = 0;
            if (d.notify == 0)
            {
                return -KOS_EINVAL;
            }
            int rc = create_source(c, tp, d.notify - 1u, CapType::CAP_NOTIFY, &note);
            if (rc != 0)
            {
                return rc;
            }
            *n = static_cast<Notification*>(cap_resolve_e(c, note, CapType::CAP_NOTIFY, CAP_SIGNAL, &rc));
            if (*n == nullptr)
            {
                return -rc;
            }
            *obj = cap_lookup(c, note)->obj;
#if KICKOS_KERNEL_CORES > 1
            if (d.core_mask == 0 or (d.core_mask & (d.core_mask - 1u)) != 0)
            {
                return -KOS_EINVAL;
            }
            *core = static_cast<uint32_t>(__builtin_ctz(d.core_mask));
#else
            *core = kickos_kernel_core();
#endif
            return 0;
        }

        // Stages declaration `d`'s windows in window_stage and admits its lines and console: 0, or
        // the refusal. Caller holds IrqLock. noinline, as window_admit: its locals would otherwise
        // sit in the armv7m SVC chain's widest frame.
        __attribute__((noinline)) int driver_admit(Thread* c, kos_task_params const& tp,
                                                   kos_task_decl const& d, kos_thread_params* p)
        {
            if (tp.window_count != 0)
            {
                return -KOS_EINVAL;
            }
            uint16_t dest[KICKOS_MAX_SPAWN_GRANTS] = {};
            int rc = create_dests(c, tp, dest);
            if (rc != 0)
            {
                return rc;
            }
            bool const console = (d.flags & KOS_DECL_CONSOLE) != 0;
            int freed = -1;
            if (console)
            {
                freed = console_tx_line();
            }
            kos_decl_grant const* const g = image_decl_grants(&d);
            uint16_t windows = 0;
            uint16_t lines = 0;
            for (uint16_t k = 0; k < d.grant_count; k++)
            {
                if (g[k].kind == KOS_DECL_LINE)
                {
                    rc = irq_declared_admit(g[k].line, g[k].trigger, nullptr,
                                            console and g[k].line == freed);
                    if (rc != 0)
                    {
                        return rc;
                    }
                    if (not line_slot_free(g[k].slot, d.cap_width, g, k, dest, tp.cap_count))
                    {
                        return -KOS_EINVAL;
                    }
                    lines++;
                    continue;
                }
                if (windows == KICKOS_MAX_THREAD_WINDOWS)
                {
                    return -KOS_ENOMEM;
                }
                kos_window& w = kernel().window_stage[windows];
                w.base = g[k].base;
                w.size = g[k].size;
                w.kind = KOS_WINDOW_DEVICE;
                if (g[k].kind == KOS_DECL_PORTS)
                {
                    w.kind = KOS_WINDOW_PORTS;
                }
                w.flags = 0;
                windows++;
            }
            p->windows = nullptr;
            p->window_count = windows;
            // The kernel claims each declared line into the new task, whose budget pays for it.
            if (lines > d.budgets[KOS_BUDGET_IRQ_BINDINGS])
            {
                return -KOS_EAGAIN;
            }
            if (lines != 0)
            {
                Notification* n = nullptr;
                int obj = 0;
                uint32_t core = 0;
                rc = lines_notify(c, tp, d, &n, &obj, &core);
                if (rc != 0)
                {
                    return rc;
                }
            }
            if (console)
            {
                uint32_t ep = 0;
                if (d.ready == 0)
                {
                    return -KOS_EINVAL;
                }
                rc = create_source(c, tp, d.ready - 1u, CapType::CAP_ENDPOINT, &ep);
                if (rc != 0)
                {
                    return rc;
                }
                CapEntry const* const e = cap_lookup(c, ep);
                if (kernel().endpoints.resolve(cap_obj(*e)) == nullptr)
                {
                    return -KOS_EBADF;
                }
                if ((e->rights & CAP_HANDOUT) == 0)
                {
                    return -KOS_EACCES;
                }
                if (console_handover_refused() != 0 or console_window_held_outside(nullptr))
                {
                    return -KOS_EBUSY;
                }
            }
            return 0;
        }

        // Publishes the console declaration `d` declares for task `t` through the caller's
        // capability at its ready position, and starts the handover. Caller holds IrqLock.
        __attribute__((noinline)) int driver_publish(Thread* c, kos_task_params const& tp,
                                                     kos_task_decl const& d, Task* t, Held lock)
        {
            uint32_t ep = 0;
            int const rc = create_source(c, tp, d.ready - 1u, CapType::CAP_ENDPOINT, &ep);
            if (rc != 0)
            {
                return rc;
            }
            // The last step that can fail: a refusal has to leave a working console behind.
            int const pub = cap_console_publish_through(c, cap_lookup(c, ep), t, lock);
            if (pub != 0)
            {
                return pub;
            }
            // Refuses every NEW kernel chip writer and gives up the ring, WITHOUT handing the device
            // over: a writer already counted may be mid-message with no buffered path left, and it
            // finishes synchronously before the entry thread runs.
            console_handover_begin();
            return 0;
        }

        // Claims declaration `d`'s lines into `entry`. Caller holds IrqLock.
        __attribute__((noinline)) int driver_lines(Thread* c, Thread* entry,
                                                   kos_task_params const& tp,
                                                   kos_task_decl const& d)
        {
            kos_decl_grant const* const g = image_decl_grants(&d);
            Notification* n = nullptr;
            int obj = 0;
            uint32_t core = 0;
            uint8_t line = 0;
            for (uint16_t k = 0; k < d.grant_count; k++)
            {
                if (g[k].kind != KOS_DECL_LINE)
                {
                    continue;
                }
                int rc = 0;
                if (line == 0)
                {
                    rc = lines_notify(c, tp, d, &n, &obj, &core);
                }
                if (rc == 0)
                {
                    rc = irq_declared_claim(entry, g[k].line, g[k].trigger, core, g[k].slot, n, obj,
                                            line);
                }
                if (rc != 0)
                {
                    return rc;
                }
                line++;
            }
            return 0;
        }

        // argv measured, or the instance's name where the caller passes none.
        int argv_in(Thread* c, kos_task_params const& tp, kos_task_decl const& d, ArgBlock* a)
        {
            a->name = image_decl_name(&d);
            if (tp.argv == nullptr)
            {
                size_t n = 0;
                while (a->name[n] != '\0')
                {
                    n++;
                }
                if (n + 1u > KICKOS_ARGV_MAX)
                {
                    return -KOS_EINVAL;
                }
                a->argc = 1;
                a->bytes = static_cast<uint16_t>(n + 1u);
                return 0;
            }
            uintptr_t const vu = reinterpret_cast<uintptr_t>(tp.argv);
            if ((vu & (alignof(char*) - 1)) != 0)
            {
                return -KOS_EFAULT;
            }
            a->vector = vu;
            size_t used = 0;
            uint32_t argc = 0;
            while (true)
            {
                uintptr_t su = 0;
                uintptr_t const at = vu + argc * sizeof(uintptr_t);
                if (not user_readable_ok(at, sizeof(su))
                    or not kaccess_from_user(&su, user_space_of(c), at, sizeof(su)))
                {
                    return -KOS_EFAULT;
                }
                if (su == 0)
                {
                    break;
                }
                if (used == KICKOS_ARGV_MAX or argc == KICKOS_MAX_ARGC)
                {
                    return -KOS_EINVAL;
                }
                int const len = user_string_len(su, KICKOS_ARGV_MAX - used, nullptr);
                if (len < 0)
                {
                    return len;
                }
                used = used + static_cast<size_t>(len) + 1u;
                argc++;
            }
            a->argc = static_cast<uint16_t>(argc);
            a->bytes = static_cast<uint16_t>(used);
            return 0;
        }
    }

    namespace
    {
        // What a create refused -KOS_EAGAIN can wait out before it asks again.
        enum class CreateWait : uint8_t
        {
            NONE,  // nothing that ends on its own: answered
            DEATH, // the declaration's last incarnation, which a WAIT_TASK_EMPTY park waits out
            GRACE, // a released IRQ binding or line another core's dispatch may still hold,
                   // which a WAIT_RETIRE park waits out
        };

        // What a create leaves the caller once the lock is dropped.
        struct Created
        {
            Thread* entry = nullptr;
            bool published = false;
            CreateWait wait = CreateWait::NONE;
            int16_t decl = -1; // the declaration the wait is for
        };

        // What refusal `rc` of declaration `decl`'s start by `c` waits out: nothing where the
        // refusal is the caller's own admission, which no death or grace period cures.
        CreateWait decl_wait(Thread* c, int rc, int decl)
        {
            if (rc != -KOS_EAGAIN or decl < 0
                or decl_admit(c, *image_decl(static_cast<size_t>(decl))) != 0)
            {
                return CreateWait::NONE;
            }
            if (task_decl_dying(static_cast<size_t>(decl)) != nullptr)
            {
                return CreateWait::DEATH;
            }
#if KICKOS_KERNEL_CORES > 1
            if (task_decl_retiring(static_cast<size_t>(decl)))
            {
                return CreateWait::GRACE;
            }
#endif
            return CreateWait::NONE;
        }

#if KICKOS_KERNEL_CORES > 1
        // Whether a line declaration `d` names is still retiring from its last holder.
        bool lines_retiring(kos_task_decl const& d)
        {
            if ((d.flags & KOS_DECL_DRIVER) == 0)
            {
                return false;
            }
            kos_decl_grant const* const g = image_decl_grants(&d);
            for (uint16_t k = 0; k < d.grant_count; k++)
            {
                if (g[k].kind == KOS_DECL_LINE and irq_line_retiring(g[k].line))
                {
                    return true;
                }
            }
            return false;
        }
#endif

        // What declaration `d`'s driver admission refusing `rc` waits out.
        CreateWait lines_wait(int rc, kos_task_decl const& d)
        {
#if KICKOS_KERNEL_CORES > 1
            if (rc == -KOS_EAGAIN and lines_retiring(d))
            {
                return CreateWait::GRACE;
            }
#else
            (void)rc;
            (void)d;
#endif
            return CreateWait::NONE;
        }

        // The locked pass of an instance's create, which writes the task's handle to the
        // caller's word at `uout` before its entry runs, a refused write ending the task. A
        // console create returns with `out->published` and its entry thread out of the
        // scheduler, a failure having ended the task.
        int task_create_locked(uint32_t prog, uintptr_t pu, uintptr_t uout, Created* out)
        {
            IrqLock lock;
            Thread* const c = sched::current();
#if KICKOS_PRESYNC
            // Both copies in the caller's record, off the syscall descent the trap red zone prices.
            PresyncRecord* const r = presync_record();
            if (r == nullptr)
            {
                return -KOS_EFAULT;
            }
            kos_task_params& tp = r->task_params;
            kos_thread_params& p = r->params;
#else
            CreateStage* const r = create_stage();
            kos_task_params& tp = r->task_params;
            kos_thread_params& p = r->params;
#endif
            int rc = task_params_in(pu, &tp);
            if (rc != 0)
            {
                return rc;
            }
            int refused_at = -1;
            int const sel = program_instance(c, prog, tp, &refused_at);
            if (sel < 0)
            {
                out->wait = decl_wait(c, sel, refused_at);
                out->decl = static_cast<int16_t>(refused_at);
                return sel;
            }
            kos_task_decl const& d = *image_decl(static_cast<size_t>(sel));
            bool const driver = (d.flags & KOS_DECL_DRIVER) != 0;
            int ready = -1;
            if (d.ready != 0)
            {
                rc = ready_endpoint(c, tp, d.ready - 1u, &ready);
                if (rc != 0)
                {
                    return rc;
                }
            }
            ArgBlock args{};
            args.decl = static_cast<uint16_t>(sel);
            args.driver = driver;
            rc = argv_in(c, tp, d, &args);
            if (rc != 0)
            {
                return rc;
            }
            uint32_t attr = 0;
            if (not mem_flags_to_attr(tp.flags, &attr))
            {
                return -KOS_EINVAL;
            }
            p = {};
            p.entry = reinterpret_cast<void (*)(void*)>(
                image_decl_start(static_cast<size_t>(sel)));
            p.arg = tp.block;
            p.prio = d.priority;
            p.policy = KOS_POLICY_FIFO;
            p.cap_count = tp.cap_count;
            p.caps = tp.caps;
            p.cap_dest = tp.cap_dest;
            p.windows = tp.windows;
            p.window_count = tp.window_count;
            p.stack_base = tp.stack;
            p.stack_size = tp.stack_size;
            p.authority = d.authority;
            if (driver)
            {
                rc = driver_admit(c, tp, d, &p);
                if (rc != 0)
                {
                    out->wait = lines_wait(rc, d);
                    out->decl = static_cast<int16_t>(sel);
                    return rc;
                }
            }
            Task* t = nullptr;
            rc = task_build(c, tp.block, tp.size, attr, static_cast<size_t>(sel), lock, &t);
            if (rc != 0)
            {
                return rc;
            }
            Thread* held = nullptr;
            rc = spawn_locked(&p, image_decl_name(&d), &args, t, 0, lock, &held);
            if (rc != 0)
            {
                task_drop_hold(t, lock);
                return rc;
            }
            if (d.ready != 0)
            {
                task_ready_seat(t, ready);
            }
            bool const console = driver and (d.flags & KOS_DECL_CONSOLE) != 0;
            if (console)
            {
                rc = driver_publish(c, tp, d, t, lock);
                if (rc != 0)
                {
                    task_drop_hold(t, lock);
                    return rc;
                }
                out->published = true;
            }
            if (driver)
            {
                // Past the publish, which frees the console's own line from the kernel's handler.
                rc = driver_lines(c, held, tp, d);
                if (rc != 0)
                {
                    task_drop_hold(t, lock);
                    return rc;
                }
            }
            kos_task_t const named = task_handle(t);
            if (not kaccess_to_user(user_space_of(c), uout, &named, sizeof(named)))
            {
                task_drop_hold(t, lock);
                return -KOS_EFAULT;
            }
            out->entry = held;
            if (not console)
            {
                task_mark_started(t);
                sched::add(held, lock);
            }
            return 0;
        }
    }

    namespace
    {
        // Parks the caller until declaration `decl`'s last incarnation is dead, or `until`: 0 to
        // ask again, -KOS_ETIMEDOUT or -KOS_ECANCELED. noinline: the park's switch chain must
        // not sit under the create's own frame.
        __attribute__((noinline)) int create_wait_death(Thread* c, size_t decl, uint64_t until)
        {
            uint32_t epoch = 0;
            {
                IrqLock lock;
                ParkToken const ask = park_cancel_pending(c);
                if (ask.cancelled())
                {
                    sched::exit_current(KOS_EXIT_CANCELLED, sched::EXIT_RETURN, &lock);
                }
                // Asked again under the lock the park holds: the death may have come since.
                Task* const dying = task_decl_dying(decl);
                if (dying == nullptr)
                {
                    return 0;
                }
                uint64_t const now = ktime_now();
                if (now >= until)
                {
                    return -KOS_ETIMEDOUT;
                }
#if KICKOS_PRESYNC
                presync_park();
#endif
                epoch = death_park(ask)(c, dying, lock);
                death_wait_lend(dying, lock);
                if (until != CREATE_FOREVER)
                {
                    ktime_deadline_arm_at(c, ktime_floored(now, until), false, lock);
                }
                sched::reschedule(nullptr, lock);
            }
            wq_confirm_resume(c, epoch);
            return static_cast<int>(c->wait_result);
        }

#if KICKOS_KERNEL_CORES > 1
        // Parks the caller until the grace period of what declaration `decl`'s last incarnation
        // or its lines' last holder released has ended, or `until`: 0 to ask again,
        // -KOS_ETIMEDOUT or -KOS_ECANCELED. noinline as create_wait_death.
        __attribute__((noinline)) int create_wait_grace(Thread* c, size_t decl, uint64_t until)
        {
            uint32_t epoch = 0;
            {
                IrqLock lock;
                ParkToken const ask = park_cancel_pending(c);
                if (ask.cancelled())
                {
                    sched::exit_current(KOS_EXIT_CANCELLED, sched::EXIT_RETURN, &lock);
                }
                // Asked again under the lock the park holds: the grace period may have ended since.
                irq_retire_drain();
                if (not task_decl_retiring(decl) and not lines_retiring(*image_decl(decl)))
                {
                    return 0;
                }
                uint64_t const now = ktime_now();
                if (now >= until)
                {
                    return -KOS_ETIMEDOUT;
                }
#if KICKOS_PRESYNC
                presync_park();
#endif
                epoch = park_queueless(ask)(c, WAIT_RETIRE, nullptr, lock);
                irq_retire_await(c, lock);
                if (until != CREATE_FOREVER)
                {
                    ktime_deadline_arm_at(c, ktime_floored(now, until), false, lock);
                }
                sched::reschedule(nullptr, lock);
            }
            wq_confirm_resume(c, epoch);
            return static_cast<int>(c->wait_result);
        }
#endif
    }

    // An instance of the program, its entry thread started at the declared priority.
    int task_create_call(uint32_t prog, uintptr_t pu, uint32_t timeout_us, uintptr_t out)
    {
#if KICKOS_PRESYNC
        // One deadline across the rounds the dispatch sends the call through.
        uint64_t until = 0;
        PresyncRecord const* const r = spawn_record();
        if (r != nullptr)
        {
            until = r->task_until;
        }
        else
        {
            until = create_until(timeout_us);
        }
#else
        uint64_t const until = create_until(timeout_us);
#endif
        Created made;
        int rc = 0;
        while (true)
        {
            made = Created{};
            rc = task_create_locked(prog, pu, out, &made);
            if (until == 0 or made.wait == CreateWait::NONE)
            {
                break;
            }
            int waited = 0;
            if (made.wait == CreateWait::DEATH)
            {
                waited = create_wait_death(sched::current(), static_cast<size_t>(made.decl), until);
            }
#if KICKOS_KERNEL_CORES > 1
            else
            {
                waited = create_wait_grace(sched::current(), static_cast<size_t>(made.decl), until);
            }
#endif
            if (waited != 0)
            {
                return waited;
            }
#if KICKOS_PRESYNC
            // What the plan staged or found short was decided before the wait.
            if (presync_round())
            {
                return -KOS_EAGAIN;
            }
#endif
        }
        if (made.published)
        {
            // With the lock RELEASED: a writer counted before the refusal may be preempted below
            // this creator's priority. A create that failed past the publish has ended its task,
            // whose death set_user acts on.
            console_publish_drain();
            console_owner_set_user(); // strictly after the drain
            if (rc == 0)
            {
                IrqLock lock;
                task_mark_started(made.entry->task);
                sched::add(made.entry, lock);
            }
        }
        return rc;
    }

    int program_find_call(uintptr_t name, uint32_t* out_prog)
    {
        IrqLock lock;
        *out_prog = KOS_CAP_NONE; // seated before every early return
        for (int i = 0; image_program(i) != nullptr; i++)
        {
            int const is = user_string_len(name, KICKOS_ARGV_MAX, image_program_name(i));
            if (is == -KOS_ENOENT)
            {
                continue;
            }
            if (is < 0)
            {
                return is;
            }
            return cap_install(sched::current(), i, CapType::CAP_PROGRAM, CAP_TRANSFER, out_prog);
        }
        return -KOS_ENOENT;
    }

    // Every member is slain, a member running no system call included. A timeout of 0 drops the
    // creator's hold at once and returns, the slot going back once the last member's sweep is
    // done. Any other drops it only on the way out of a successful wait: dropping it first frees
    // the slot the moment the group empties, leaving `t` a dangling name in a wait edge. On a
    // timeout the hold survives with the group.
    int task_slay(kos_task_t task, uint32_t timeout_us)
    {
        Thread* const c = sched::current();
        if (timeout_us == 0u)
        {
            IrqLock lock;
            Task* const t = task_resolve(task);
            if (t == nullptr)
            {
                return -KOS_EBADF;
            }
            if (not task_created_by(t, kernel().threads.kill_tag_of(c)))
            {
                return -KOS_EPERM;
            }
            task_stop_and_drop_hold(t, lock);
            return 0;
        }
        uint32_t epoch = 0;
        {
            IrqLock lock;
            // Ahead of the resolve: the hold this caller would owe a task_drop_hold is one
            // task_orphan_created_by drops for it out of sched::exit_current.
            ParkToken const ask = park_cancel_pending(c);
            if (ask.cancelled())
            {
                sched::exit_current(KOS_EXIT_CANCELLED, sched::EXIT_RETURN, &lock);
            }
            Task* const t = task_resolve(task);
            if (t == nullptr)
            {
                return -KOS_EBADF;
            }
            if (not task_created_by(t, kernel().threads.kill_tag_of(c)))
            {
                return -KOS_EPERM;
            }
            // Already empty and swept: nothing could wake a park here. A member still sweeping
            // wakes it at the end of its sweep.
            if (task_member_count(t) == 0 and task_sweeping(t) == 0)
            {
                task_drop_hold(t, lock);
                return 0;
            }
            // Parked before the stop, as in thread_cancel_call, so the stop takes no pass: every
            // member is marked and every park ended before the reschedule below switches.
            epoch = death_park(ask)(c, t, lock);
            ktime_deadline_arm(c, timeout_us, lock);
            task_stop(t, lock);
            // After the stop, which cut every wait of the task's threads in this hold.
            death_wait_lend(t, lock);
            sched::reschedule(nullptr, lock);
        }
        wq_confirm_resume(c, epoch);
        int const rc = static_cast<int>(c->wait_result);
        if (rc == 0)
        {
            // By the handle again: once the group is dead, a create of its declaration ends this
            // hold and seats the record for its own creator, before this thread runs again.
            IrqLock lock;
            Task* const t = task_resolve(task);
            if (task_created_by(t, kernel().threads.kill_tag_of(c)))
            {
                task_drop_hold(t, lock);
            }
        }
        return rc;
    }

    // The exit sweep in sched::exit_current is the only thing that wakes this park, apart from
    // the timeout.
    int thread_join(kos_thread_t thread, uint32_t timeout_us)
    {
        Thread* const c = sched::current();
        uint32_t epoch = 0;
        {
            IrqLock lock;
            ParkToken const ask = park_cancel_pending(c);
            if (ask.cancelled())
            {
                sched::exit_current(KOS_EXIT_CANCELLED, sched::EXIT_RETURN, &lock);
            }
            Thread* const t = kernel().threads.resolve(thread);
            if (t == nullptr or t->state == ThreadState::INACTIVE)
            {
                return -KOS_EBADF;
            }
            if (t == c)
            {
                return -KOS_EDEADLK; // nothing would ever wake this park
            }
            // Its exit sweep has already run, so a park here would never be woken. Ahead of the
            // task check: an exited thread names no task, and thread_cancel_call already answers
            // its exit to any caller.
            if (t->state == ThreadState::EXITED)
            {
                return 0;
            }
            if (not caller_spawned(t, c) and (t->task == nullptr or t->task != c->task))
            {
                return -KOS_EPERM;
            }
            epoch = park_queueless(ask)(c, WAIT_JOIN, t, lock);
            ktime_deadline_arm(c, timeout_us, lock);
            sched::reschedule(nullptr, lock);
        }
        wq_confirm_resume(c, epoch); // the lock is released across this: see sync.h
        // 0 (target exited), -KOS_ETIMEDOUT (the timer arm), or -KOS_ECANCELED (the joiner
        // itself was cancelled, e.g. a task group kill; thread_abort_park handles WAIT_JOIN)
        return static_cast<int>(c->wait_result);
    }
}
