// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Thread and task lifecycle syscalls.

#include <kickos/ampshare.h>
#include <kickos/arch/arch.h>
#include <kickos/cap.h>
#include <kickos/config.h>
#include <kickos/domain.h>
#include <kickos/grant.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/kruntime.h>
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
        // Give back what a spawn took before thread_create committed: the kernel's stack, the
        // thread slot, and a task task_for built that nothing holds yet. noinline: its frame
        // lands on the armv7m SVC chain the trap red zone measures.
        //
        // kstack_owned, not the pointer, says whether the stack was the kernel's: the app's own
        // allocator hands out frame-pool frames too, so the pool cannot tell the two apart.
        //
        // The stack goes back before the task: it is mapped in the task's space, which
        // task_discard destroys.
        __attribute__((noinline)) void spawn_unwind(Kernel& k, ThreadAttr const& attr,
                                                    Task* tk, void* stack, size_t bytes,
                                                    int slot, Held held)
        {
#if KICKOS_HAVE_ASPACE
            (void)k;
            // The windows went in after the stack, and go back before the task as it does.
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
            task_discard(tk, held);
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
                int const rc = aspace_window_map(domain_space(into), domain_ranges_mut(into), pa,
                                                 w.size, rights, type,
                                                 static_cast<uint16_t>(slot + 1), i, donor);
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

        bool spawn_builds_task(kos_thread_params const& p, Thread const* spawner)
        {
            return p.task == KOS_TASK_NONE
                   and not(spawner->task != nullptr
                           and (p.privileged != 0) == spawner->privileged
                           and (p.mem_base == nullptr or p.mem_size == 0));
        }

        // Admits entry i of a staged window list, the entries before it already admitted, or
        // answers why not. A privileged child carries the whole-arena region and the
        // background map, so its windows get no descriptor to admit. Caller holds IrqLock.
        // noinline, as spawn_unwind: its locals would otherwise sit in the armv7m SVC chain's
        // widest frame.
        __attribute__((noinline)) int window_admit(kos_window const* list, uint16_t i,
                                                   kos_thread_params const* p)
        {
            bool const privileged = (p->privileged != 0);
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
                if (privileged)
                {
                    return 0;
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
                if (privileged)
                {
                    return 0;
                }
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
                    if (not arch_aspace_memtype_support(ARCH_MAP_NOCACHE))
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
#elif KICKOS_MEMORY_ENFORCED
                uint32_t const attr = window_memory_attr(w.flags);
                RamAdmit how = RAM_ADMIT_GRANT;
                if (privileged)
                {
                    how = RAM_ADMIT_PRIVILEGED;
                }
                int const arc = ram_region_admit(c, w.base, w.size, attr, how);
                if (arc != 0)
                {
                    return arc;
                }
                // The data region the child's task will have is no thread's region yet when this
                // spawn builds it: the one this spawn builds from mem_base, the named task's, or
                // the spawner's.
                arch_mpu_region data = {};
                Domain const* dom = thread_domain(c);
                if (p->task != KOS_TASK_NONE)
                {
                    dom = task_domain(task_resolve(p->task));
                }
                if (p->task == KOS_TASK_NONE and p->mem_base != nullptr and p->mem_size != 0)
                {
                    data.base = reinterpret_cast<uintptr_t>(p->mem_base);
                    data.size = arch_ram_region_size(p->mem_size);
                    data.attr = ARCH_MPU_R | ARCH_MPU_W;
                }
                else if (dom != domain_kernel() and domain_region_count(dom) > 0)
                {
                    data = *domain_region_at(dom, 0);
                }
                if (data.size != 0 and ((data.attr ^ attr) & ARCH_MPU_NOCACHE) != 0
                    and grant_ranges_overlap(w.base, w.base + arch_ram_region_size(w.size) - 1u,
                                             data.base, data.base + data.size - 1u))
                {
                    return -KOS_EBUSY;
                }
                return 0;
#else
                (void)privileged;
                (void)c;
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
#if KICKOS_HAVE_ASPACE
            // Whole granules inside an aperture the chip states, which holds no RAM: a window
            // over RAM would hand the task whatever frames sit there.
            if (not grant_window_aperture_ok(w.base, w.size))
            {
                return -KOS_EINVAL;
            }
            if (privileged)
            {
                return 0;
            }
            if (grant_hits_reserved(w.base, w.size)
                or not arch_aspace_memtype_support(ARCH_MAP_DEVICE))
            {
                return -KOS_EPERM; // the kernel's own device
            }
#else
            if (not arch_mpu_region_encodable(w.base, w.size))
            {
                return -KOS_EINVAL;
            }
            if (privileged)
            {
                return 0;
            }
            if (not grant_region_admissible(w.base, w.size,
                                            ARCH_MPU_R | ARCH_MPU_W | ARCH_MPU_DEV, true))
            {
                return -KOS_EPERM; // reserved block / bit-band alias
            }
#endif
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
            Task const* target = c->task;
            if (p->task != KOS_TASK_NONE)
            {
                target = task_resolve(p->task);
            }
            else if (spawn_builds_task(*p, c))
            {
                target = nullptr;
            }
            if (console_window_withheld(w.base, w.size, target)
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
#if KICKOS_MEMORY_ENFORCED and not KICKOS_HAVE_ASPACE
                // Whatever the caller's privilege, unlike the spawn's arm: task_create drops
                // DOM_CALLER_PRIVILEGED, so this grant becomes an unprivileged domain's region.
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

        // The stack a spawn names. Validated before the slot claim, or a bad stack leaks a slot.
        int spawn_stack_admit(kos_thread_params const* p)
        {
            if (p->stack_base != nullptr)
            {
                uintptr_t const base = reinterpret_cast<uintptr_t>(p->stack_base);
                // The floor binds what is left once the TLS carve comes off the block.
                if (p->stack_size < KICKOS_MIN_STACK_SIZE + tls_block_size()
                    or (base & (KICKOS_STACK_ALIGN - 1)) != 0
                    or (p->stack_size & (KICKOS_STACK_ALIGN - 1)) != 0
                    or base + p->stack_size < base)
                {
                    return -KOS_EINVAL;
                }
#if KICKOS_MEMORY_ENFORCED
                // The child's privilege, not the caller's: a privileged child gets the whole arena
                // plus the background region and needs no stack descriptor.
                if (p->privileged == 0)
                {
#if KICKOS_HAVE_ASPACE
                    // A range granted R|W in the space the child will run in, which is not always
                    // the caller's: a member joins a group whose space the caller does not hold. A
                    // task that does not resolve is left to the -KOS_EBADF below. The image is
                    // excluded: a stack carved out of an app global would sit in its static data.
                    Domain const* target = thread_domain(sched::current());
                    if (p->task != KOS_TASK_NONE)
                    {
                        Task const* const named = task_resolve(p->task);
                        target = nullptr;
                        if (named != nullptr)
                        {
                            target = task_domain(named);
                        }
                    }
                    VirtualRanges const* const cr = domain_ranges(target);
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
                    // A spawn bringing its own grant opens a new space whose only app memory is
                    // that grant, so the stack must lie in the same reservation or the child starts
                    // on a page its space never maps.
                    if (cr != nullptr and p->task == KOS_TASK_NONE and p->mem_base != nullptr
                        and p->mem_size != 0
                        and e != cr->find(reinterpret_cast<uintptr_t>(p->mem_base), p->mem_size))
                    {
                        return -KOS_EPERM;
                    }
#else
                    // No privileged waiver: an out-of-arena stack_base would grant an R|W window
                    // over peripheral or kernel SRAM.
                    int const arc = ram_region_admit(sched::current(), base, p->stack_size,
                                                     ARCH_MPU_R | ARCH_MPU_W, RAM_ADMIT_GRANT);
                    if (arc != 0)
                    {
                        return arc;
                    }
#endif
                }
#endif
            }
            return 0;
        }

        int spawn_grant_admit(kos_thread_params const* p)
        {
            if (p->mem_base != nullptr and p->mem_size != 0)
            {
                uintptr_t const dbase = reinterpret_cast<uintptr_t>(p->mem_base);
                // Ungated: where nothing is enforced, nothing else refuses a wrapping grant.
                if (dbase + p->mem_size < dbase)
                {
                    return -KOS_EINVAL;
                }
#if KICKOS_MEMORY_ENFORCED and not KICKOS_HAVE_ASPACE
                // A privileged child resolves the kernel domain, and a member's memory is refused
                // -KOS_EINVAL further down: neither grant becomes a region to admit.
                if (p->privileged == 0 and p->task == KOS_TASK_NONE)
                {
                    return ram_region_admit(sched::current(), dbase, p->mem_size,
                                            ARCH_MPU_R | ARCH_MPU_W, RAM_ADMIT_GRANT);
                }
#endif
            }
            return 0;
        }

        // Leaves the admitted window list in window_stage.
        int spawn_windows_admit(kos_thread_params const* p)
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
                for (uint16_t i = 0; i < nwin; i++)
                {
                    int const wrc = window_admit(wbuf, i, p);
                    if (wrc != 0)
                    {
                        return wrc;
                    }
                }
            }
#if KICKOS_MEMORY_ENFORCED and not KICKOS_HAVE_ASPACE
            // A cacheable stack under one of this spawn's own non-cacheable windows is incoherent;
            // spawn_stack_admit asked what is held already.
            if (p->stack_base != nullptr and p->privileged == 0
                and not stack_type_free(reinterpret_cast<uintptr_t>(p->stack_base),
                                        arch_ram_region_size(p->stack_size), kernel().window_stage,
                                        p->window_count))
            {
                return -KOS_EBUSY;
            }
#endif
            return 0;
        }

        // The authority word and the grant list's shape; its entries are the locked pass's.
        int spawn_authority_admit(kos_thread_params const* p)
        {
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
                // Delegated cap i lands at child index i+1 by default, index 0 being the kernel's
                // stdout slot. The default indices fit because KICKOS_MAX_SPAWN_GRANTS <
                // KICKOS_CAP_CHILD_WIDTH; a caller-named destination is refused at the child
                // table's bound.
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
        int spawn_admit(kos_thread_params const* p)
        {
            // prio indexes the ready lists and drives a 1u<<prio bitmap shift, so an
            // out-of-range value is an OOB write and UB. Priority 0 is idle's alone.
            if (p->prio < KICKOS_PRIO_MIN or p->prio > KICKOS_PRIO_MAX)
            {
                return -KOS_EINVAL;
            }
            if (p->privileged != 0 and not sched::current()->privileged)
            {
                return -KOS_EPERM;
            }
            int rc = spawn_stack_admit(p);
            if (rc == 0)
            {
                rc = spawn_grant_admit(p);
            }
            if (rc == 0)
            {
                rc = spawn_windows_admit(p);
            }
            if (rc == 0)
            {
                rc = spawn_authority_admit(p);
            }
            if (rc != 0)
            {
                return rc;
            }
#if KICKOS_HAVE_ASPACE
            // A grant handed to the new space must be one whole reservation of the spawner's.
            if (p->privileged == 0 and p->task == KOS_TASK_NONE and p->mem_base != nullptr
                and p->mem_size != 0
                and aspace_handoff_admit(domain_ranges(thread_domain(sched::current())),
                                         reinterpret_cast<uintptr_t>(p->mem_base), p->mem_size)
                        != 0)
            {
                return -KOS_EPERM;
            }
#endif
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
        uint16_t const n = r->params.window_count;
        uintptr_t const wu = reinterpret_cast<uintptr_t>(r->params.windows);
        size_t const bytes = sizeof(kos_window) * n;
        if (n == 0 or n > KICKOS_MAX_THREAD_WINDOWS or wu == 0
            or (wu & (alignof(kos_window) - 1)) != 0 or not user_readable_ok(wu, bytes))
        {
            return;
        }
        r->has_windows = kaccess_from_user(r->windows, user_space_of(c), wu, bytes);
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
        if (spawn_admit(&p) != 0)
        {
            return;
        }
        bool const builds = spawn_builds_task(p, c);
        if (builds and (p.privileged != 0 or not cap_check_authority(c, AUTH_TASKS)))
        {
            return;
        }
        if (builds)
        {
            presync_stage_image(false, domain_space(thread_domain(c)));
        }
#if KICKOS_ARCH_ALIAS_DCACHE
        VirtualRanges const* const own = domain_ranges(thread_domain(c));
        if (builds and p.mem_base != nullptr and p.mem_size != 0)
        {
            aspace_reservation_note(own, reinterpret_cast<uintptr_t>(p.mem_base), p.mem_size,
                                    ARCH_MAP_NORMAL);
        }
        for (uint16_t i = 0; i < p.window_count; i++)
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
#endif
    }

    void task_create_presync_plan(void* mem_base, size_t mem_size, uint32_t mem_attr)
    {
        Thread* const c = sched::current();
        if (c == nullptr or c->task == nullptr
            or task_create_admit(c, mem_base, mem_size, mem_attr) != 0)
        {
            return;
        }
        presync_stage_image(true, domain_space(thread_domain(c)));
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
#endif

    static int spawn_masked(kos_thread_params const* p, kos_thread_t* out_thread)
    {
        IrqLock lock;
        *out_thread = KOS_THREAD_NONE; // seated before every early return
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
        kos_thread_params params;
        int const prc = spawn_params_in(pu, &params);
        if (prc != 0)
        {
            return prc;
        }
        p = &params;
        int const arc = spawn_admit(p);
        if (arc != 0)
        {
            return arc;
        }
        // The whole grant list is validated before anything is claimed. Sized by the grant
        // bound: these, plus gbuf and dbuf below, live on the caller's stack, which can be 1 KiB.
        int deleg_obj[KICKOS_MAX_SPAWN_GRANTS];
        // Kind and rights packed in one byte (kcap_grant_pack): this frame sits on the syscall
        // descent the trap red zone measures, and a third wide array would move the SVC
        // reservation a whole 64-byte step on every board that enforces the class.
        uint8_t deleg_kind[KICKOS_MAX_SPAWN_GRANTS];
        // A CAP_NOTIFY's badge travels with the copy. Unbadged, the child's copy would raise bit
        // 0, a different bit of the same object and not a narrowing.
        uint8_t deleg_badge[KICKOS_MAX_SPAWN_GRANTS];
        // A capability table is up to KICKOS_MAX_HANDLES == 65535 slots wide.
        uint16_t deleg_dest[KICKOS_MAX_SPAWN_GRANTS];
        int const ncaps = static_cast<int>(p->cap_count);

        Thread* const spawner = sched::current();
        if (ncaps > 0)
        {
            uintptr_t const cu = reinterpret_cast<uintptr_t>(p->caps);
            // user_readable_ok, as for the params struct: the array may be a global.
            if (not user_readable_ok(cu, sizeof(kos_cap_grant) * static_cast<size_t>(ncaps)))
            {
                return -KOS_EFAULT;
            }
            // Snapshotted in one pass and validated from the copy, so no double fetch.
            kos_cap_grant gbuf[KICKOS_MAX_SPAWN_GRANTS];
            for (int ci = 0; ci < ncaps; ci++)
            {
                if (not kaccess_from_user(&gbuf[ci], user_space_of(spawner),
                                          cu + static_cast<size_t>(ci) * sizeof(kos_cap_grant),
                                          sizeof(kos_cap_grant)))
                {
                    return -KOS_EFAULT;
                }
            }
            uint16_t dbuf[KICKOS_MAX_SPAWN_GRANTS] = {};
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
                    if (not kaccess_from_user(&dbuf[ci], user_space_of(spawner),
                                              du + static_cast<size_t>(ci) * sizeof(uint16_t),
                                              sizeof(uint16_t)))
                    {
                        return -KOS_EFAULT;
                    }
                }
            }
            for (int ci = 0; ci < ncaps; ci++)
            {
                kos_cap_grant const g = gbuf[ci];
                CapEntry* se = cap_lookup(spawner, g.source_cap);
                if (se == nullptr)
                {
                    return -KOS_EBADF;
                }
                if ((se->rights & CAP_TRANSFER) != CAP_TRANSFER)
                {
                    return -KOS_EACCES;
                }
                uint8_t const mask = g.rights_mask;
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
                deleg_obj[ci] = se->obj;
                deleg_kind[ci] = kcap_grant_pack(se->type, mask);
                deleg_badge[ci] = cap_badge(*se);
                // An absent array, or a 0 entry, means default placement. 0 is free as a
                // sentinel: index 0 is the kernel's stdout slot, which cap_install_at refuses.
                unsigned dest = dbuf[ci];
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
        // Must precede the slot claim, or a task- or domain-pool exhaustion leaks a thread slot.
        // A task task_for builds is held by nobody until thread_create commits task_ref, so
        // every refusal past this point calls task_discard, a no-op where the task is already
        // somebody's.
        Task* tk = nullptr;
        if (p->task != KOS_TASK_NONE)
        {
            // A member bringing its own data grant is refused, not ignored: the group's memory
            // is the task's, and there is no domain for the grant to land in.
            if (p->privileged != 0)
            {
                return -KOS_EINVAL; // a privileged thread holds the kernel domain: no group
            }
            if (p->mem_base != nullptr and p->mem_size != 0)
            {
                return -KOS_EINVAL;
            }
            tk = task_resolve(p->task);
            if (tk == nullptr)
            {
                return -KOS_EBADF;
            }
            if (not task_created_by(tk, k.threads.kill_tag_of(spawner)))
            {
                return -KOS_EPERM;
            }
        }
        else if (not spawn_builds_task(*p, spawner))
        {
            // A spawn bringing its own data grant builds a task, since admitting it here hands
            // every sibling a region only the child asked for; so does a privilege change.
            tk = spawner->task;
        }
        else
        {
            // This spawn builds a task of its own, which is creating a task: gating
            // task_create_call alone would leave every slot to a spawn bringing a data grant.
            if (not cap_check_authority(sched::current(), AUTH_TASKS))
            {
                return -KOS_EPERM;
            }
            int derr = 0;
            uint32_t caller = 0;
            if (p->privileged != 0)
            {
                caller = DOM_CALLER_PRIVILEGED;
            }
            tk = task_for(caller, p->mem_base, p->mem_size, thread_domain(spawner), &derr);
            if (tk == nullptr)
            {
                return -derr;
            }
            task_sched_inherit(tk, spawner->task);
        }
        // An ended task takes no member, live members or none: a restart is a new task.
        if (task_ended(tk))
        {
            task_discard(tk, lock);
            return -KOS_EBUSY;
        }
        // A priority inheritance boost is not bounded by this ceiling (sched::set_prio).
        if (p->prio > task_prio_ceiling(tk))
        {
            task_discard(tk, lock);
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
            task_discard(tk, lock);
            return crc;
        }
#endif
        int const i = k.threads.alloc();
        if (i < 0)
        {
            task_discard(tk, lock);
            return -KOS_ENOMEM;
        }

        ThreadAttr attr;
        attr.name = "user";
        // Each source byte is checked caller-readable before the privileged copy: the kernel
        // must neither fault on a bad name pointer nor leak another domain's page through it.
        // A string with no NUL stops at the first unreachable byte.
        char namebuf[KICKOS_THREAD_NAME_MAX];
        if (p->name != nullptr)
        {
            uintptr_t const np = reinterpret_cast<uintptr_t>(p->name);
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
                attr.name = namebuf;
            }
        }
        attr.prio = p->prio;
        attr.policy = Policy::FIFO;
        if (p->policy == KOS_POLICY_RR)
        {
            attr.policy = Policy::RR;
        }
        attr.quantum_ns = p->quantum_ns;
        attr.privileged = (p->privileged != 0);
        attr.windows = kernel().window_stage;
        attr.window_count = p->window_count;
        attr.task = tk;
        attr.spawner_tag = k.threads.kill_tag_of(spawner);
        // The first member a non-member seats, which a task this spawn built always is.
        attr.task_entry = tk != spawner->task
                          and (p->task == KOS_TASK_NONE or task_member_count(tk) == 0);
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
            // Frames in the task's space: the arena is linked in the kernel's half, which EL0
            // cannot reach.
            UserStack const us = ustack_alloc(task_domain(tk),
                                              KICKOS_USER_STACK_SIZE);
            stack = reinterpret_cast<void*>(us.base);
            stack_size = us.bytes;
            if (stack == nullptr)
            {
                spawn_unwind(k, attr, tk, stack, stack_size, i, lock);
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
                spawn_unwind(k, attr, tk, stack, stack_size, i, lock);
                return -KOS_ENOMEM;
            }
            stack_size = KICKOS_USER_STACK_SIZE;
            attr.kstack_owned = true;
#endif
        }
#if KICKOS_HAVE_ASPACE
        if (p->privileged == 0)
        {
            int const wrc = spawn_map_windows(tk, kernel().window_stage, p->window_count, i);
            if (wrc != 0)
            {
                spawn_unwind(k, attr, tk, stack, stack_size, i, lock);
                return wrc;
            }
        }
#endif
        // Where the thread pointer is SP masked down to KICKOS_TLS_STRIDE, a caller-supplied
        // block that is not strided, or that spans more than one stride, would hand this thread
        // a pointer into a neighbour's thread_local storage.
        if (not tls_stack_admissible(reinterpret_cast<uintptr_t>(stack), stack_size))
        {
            spawn_unwind(k, attr, tk, stack, stack_size, i, lock);
            return -KOS_EINVAL;
        }
#if KICKOS_MEMORY_ENFORCED and KICKOS_HAVE_MPU and not KICKOS_HAVE_ASPACE
        attr.regions = thread_regions_stage(task_domain(tk), attr, stack, stack_size);
        // On PMSAv8 any overlap faults the child's exception entry and the kernel's own reads of
        // its frame.
        if (p->privileged == 0 and not attr.regions->overlaps_expressible())
        {
            spawn_unwind(k, attr, tk, stack, stack_size, i, lock);
            return -KOS_EINVAL;
        }
#endif
        // Taken before the reference loop so one unwind path serves both failures.
        if (not cap_slab_attach(&attr.cap_run, KICKOS_CAP_CHILD_WIDTH, &attr.cap_free_head,
                                &attr.cap_width))
        {
            spawn_unwind(k, attr, tk, stack, stack_size, i, lock);
            return -KOS_ENOMEM;
        }
        // Bounded by the run the child actually gets, and checked before any reference is
        // taken so the only unwind owed here is the run.
        for (int ci = 0; ci < ncaps; ci++)
        {
            if (deleg_dest[ci] >= attr.cap_width)
            {
                cap_slab_detach(&attr.cap_run, &attr.cap_free_head, &attr.cap_width);
                spawn_unwind(k, attr, tk, stack, stack_size, i, lock);
                return -KOS_EINVAL;
            }
        }

        // The grant list is a take against the child's task, admitted before any reference is
        // taken. Without it a task at its ceiling passes its objects into a second task and
        // takes its whole ceiling over again.
        if (not task_object_admit_grants(tk, deleg_kind, deleg_obj, ncaps))
        {
            cap_slab_detach(&attr.cap_run, &attr.cap_free_head, &attr.cap_width);
            spawn_unwind(k, attr, tk, stack, stack_size, i, lock);
            return -KOS_EAGAIN; // the destination task holds its ceiling of one of the pools
        }

        // obj_ref_inc must stay the last fallible step: after thread_create the unwind would
        // also owe the task reference and the child's already-seated caps.
        for (int ci = 0; ci < ncaps; ci++)
        {
            if (obj_ref_inc(static_cast<CapType>(kcap_grant_type(deleg_kind[ci])), deleg_obj[ci],
                            kcap_grant_rights(deleg_kind[ci])))
            {
                continue;
            }
            for (int cj = 0; cj < ci; cj++)
            {
                obj_ref_undo(static_cast<CapType>(kcap_grant_type(deleg_kind[cj])),
                             deleg_obj[cj], kcap_grant_rights(deleg_kind[cj]));
            }
            cap_slab_detach(&attr.cap_run, &attr.cap_free_head, &attr.cap_width);
            spawn_unwind(k, attr, tk, stack, stack_size, i, lock);
            return -KOS_EOVERFLOW; // an object refcount is at its ceiling
        }
        thread_create(&k.threads.slots[i], p->entry, p->arg, stack, stack_size, attr);
        // Nothing below here may fail: the validation above guarantees each install succeeds,
        // and the reference loop already holds a reference for every cap seated here.
        Thread* const child = &k.threads.slots[i];
        cap_install_defaults(child, lock);
        cap_seat_authority(child, p->authority);
        for (int ci = 0; ci < ncaps; ci++)
        {
            cap_install_at(child, static_cast<int>(deleg_dest[ci]), deleg_obj[ci],
                           static_cast<CapType>(kcap_grant_type(deleg_kind[ci])),
                           kcap_grant_rights(deleg_kind[ci]), deleg_badge[ci]);
        }
        // Stays inside the lock: a spawner slain between an allocated child and sched::add never
        // returns to its continuation (switch_book redirects a CANCEL_SLAY thread to the exit
        // stub), leaving a fully built INACTIVE orphan that nothing frees.
        sched::add(child, lock);
        presync_commit();
        *out_thread = k.threads.handle_for(i);
        return 0;
    }

    int thread_create_call(kos_thread_params const* p, kos_thread_t* out_thread)
    {
        return spawn_masked(p, out_thread);
    }

    // Cooperative: the target dies at its next syscall boundary, in its own exit_current. A
    // thread that never enters the kernel again survives, and no caller may assume it will not.
    int thread_kill(kos_thread_t thread)
    {
        IrqLock lock;
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
        Thread* const c = sched::current();
        if (t == c)
        {
            return -KOS_EINVAL; // ending yourself is kos_exit; this path must return to its caller
        }
        if (not caller_spawned(t, c))
        {
            return -KOS_EPERM;
        }
        thread_cancel(t, lock);
        return 0;
    }

    // The target's resume is claimed (switch_to rebuilds its context into
    // kickos_thread_slay_exit before arch_switch), so it never returns to userspace and never
    // gets the window in which a driver would have quieted its device. The victim still runs
    // its own cap_teardown, in its own context.
    //
    // No timer is needed to reach a spinning victim: on one core a target distinct from the
    // caller is READY, BLOCKED or refused below, and both live states are claimed at the resume.
    int thread_slay(kos_thread_t thread, uint32_t timeout_us)
    {
        Thread* const c = sched::current();
        uint32_t epoch = 0;
        {
            IrqLock lock;
            // Ahead of the park and of the cancel below: an exit taken between them would leave
            // the victim un-slain with the caller gone.
            ParkToken const ask = park_cancel_pending(c);
            if (ask.cancelled())
            {
                sched::exit_current(KOS_EXIT_CANCELLED, sched::EXIT_RETURN, &lock);
            }
            Thread* const t = kernel().threads.resolve(thread);
            if (t == nullptr)
            {
                return -KOS_EBADF;
            }
            if (t->state == ThreadState::EXITED or t->state == ThreadState::INACTIVE)
            {
                return -KOS_EBADF;
            }
            if (t == c)
            {
                return -KOS_EINVAL; // ending yourself is kos_exit; this path must return
            }
            // Slaying idle ends the scheduler's fallback. A privileged thread may be inside
            // kernel work holding kernel invariants, which discarding its frames breaks.
            if (sched::is_idle(t) or t->privileged)
            {
                return -KOS_EINVAL;
            }
            if (not caller_spawned(t, c))
            {
                return -KOS_EPERM;
            }
            // The caller parks first: thread_cancel_kind switches to a victim that outranks the
            // caller, and on a backend that swaps inline that victim can reach EXITED first, its
            // exit sweep then finding nobody parked on it. park_queueless also detaches
            // `current`, which the cancel may have republished. Its epoch is the one sampled
            // there: the victim may run, die and wake this thread inside the cancel, and an
            // epoch read afterwards would already carry the resume, so wq_confirm_resume would
            // spin to KICKOS_POLL_SPIN_MAX and panic.
            epoch = park_queueless(ask)(c, WAIT_JOIN, t, lock);
            // 0 is already behind the min-delta floor, so the timer releases this park at the
            // first opportunity unless the victim got there first.
            ktime_deadline_arm(c, timeout_us, lock);
            thread_cancel_kind(t, CANCEL_SLAY, lock);
            sched::reschedule(nullptr, lock);
        }
        wq_confirm_resume(c, epoch); // the lock is released across this: see sync.h
        // 0 (the target is gone and swept), -KOS_ETIMEDOUT (condemned but not yet gone), or
        // -KOS_ECANCELED (the caller was itself cancelled, e.g. by the victim's group cancel).
        return static_cast<int>(c->wait_result);
    }

    uint64_t thread_self()
    {
        IrqLock lock;
        Kernel& k = kernel();
        int const i = k.threads.index_of(sched::current());
        if (i < 0)
        {
            return static_cast<uint64_t>(-KOS_EINVAL);
        }
        return k.threads.handle_for(i);
    }

    int thread_set_affinity(kos_thread_t thread, uint32_t core_mask)
    {
#if KICKOS_KERNEL_CORES > 1
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
        if (not task_same_group(c, t) and not c->privileged)
        {
            return -KOS_EPERM;
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
        return 0;
#else
        (void)thread;
        (void)core_mask;
        return -KOS_ENOSYS;
#endif
    }

#if KICKOS_KERNEL_CORES > 1
    static_assert(KICKOS_KERNEL_CORES <= 31, "a core mask must never read as an error");

    int thread_affinity(kos_thread_t thread)
    {
        IrqLock lock;
        Thread const* const c = sched::current();
        Thread const* const t = kernel().threads.resolve(thread);
        if (t == nullptr or t->state == ThreadState::EXITED
            or t->state == ThreadState::INACTIVE)
        {
            return -KOS_EBADF;
        }
        if (not task_same_group(c, t) and not c->privileged)
        {
            return -KOS_EPERM;
        }
        return static_cast<int>(t->affinity);
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

    int task_sched_grant(kos_task_t task, uint8_t prio_ceiling, uint32_t core_mask)
    {
        IrqLock lock;
        Thread* const c = sched::current();
        Task* const t = task_resolve(task);
        if (t == nullptr)
        {
            return -KOS_EBADF;
        }
        if (not task_created_by(t, kernel().threads.kill_tag_of(c)))
        {
            return -KOS_EPERM;
        }
        // Thread::affinity is a subset of this set and nothing re-derives it, so a grant that
        // narrowed under a live member would strand that member where it already runs.
        if (task_member_count(t) != 0)
        {
            return -KOS_EBUSY;
        }
        if (prio_ceiling > KICKOS_PRIO_MAX)
        {
            return -KOS_EINVAL;
        }
        // Both halves against the caller's grant: a creator cannot give what it does not hold.
        // task_sched_narrow re-checks against the task's, so a second narrowing of the same
        // task only narrows.
        uint32_t asked = core_mask;
#if KICKOS_KERNEL_CORES > 1
        if (core_mask != 0)
        {
            int const crc = sched_admit_mask(core_mask, task_core_set(c->task),
                                             MaskBound::SUBSET, &asked);
            if (crc != 0)
            {
                return crc;
            }
        }
#else
        if (core_mask != 0 and (core_mask & KICKOS_CORE_SET_ALL) == 0)
        {
            return -KOS_EINVAL;
        }
#endif
        if (prio_ceiling != 0 and prio_ceiling > task_prio_ceiling(c->task))
        {
            return -KOS_EPERM;
        }
        return task_sched_narrow(t, prio_ceiling, asked);
    }

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

    // An empty group that exists before any of its threads, holding a domain built from this
    // grant. Only the creator may seat members into it or end it.
    int task_create_call(void* mem_base, size_t mem_size, uint32_t mem_attr,
                         kos_task_t* out_task)
    {
        IrqLock lock;
        *out_task = KOS_TASK_NONE; // seated before every early return
        Thread* const c = sched::current();
        int const arc = task_create_admit(c, mem_base, mem_size, mem_attr);
        if (arc != 0)
        {
            return arc;
        }
        int derr = 0;
        Task* const t = task_create(kernel().threads.kill_tag_of(c), mem_base, mem_size, mem_attr,
                                    thread_domain(c), &derr);
        if (t == nullptr)
        {
            return -derr;
        }
        task_sched_inherit(t, c->task);
        presync_commit();
        *out_task = task_handle(t);
        return 0;
    }

    // Every member is slain, a member running no system call included, and the slot goes back
    // once the last member's sweep is done. Returns without waiting for that, as task_slay waits.
    int task_kill(kos_task_t task)
    {
        IrqLock lock;
        Task* const t = task_resolve(task);
        if (t == nullptr)
        {
            return -KOS_EBADF;
        }
        if (not task_created_by(t, kernel().threads.kill_tag_of(sched::current())))
        {
            return -KOS_EPERM;
        }
        // The group cancel runs before the hold is dropped: dropping it first can free the
        // slot outright when the group is already empty, and `t` would then be a dangling name.
        task_stop(t, lock);
        task_drop_hold(t, lock);
        return 0;
    }

    // The creator's hold is dropped only on the way out of a successful wait: dropping it first
    // frees the slot the moment the group empties, leaving `t` a dangling name in a wait edge.
    // On a timeout the hold survives with the group.
    int task_slay(kos_task_t task, uint32_t timeout_us)
    {
        Thread* const c = sched::current();
        uint32_t epoch = 0;
        Task* t = nullptr;
        {
            IrqLock lock;
            // Ahead of the resolve: the hold this caller would owe a task_drop_hold is one
            // task_orphan_created_by drops for it out of sched::exit_current.
            ParkToken const ask = park_cancel_pending(c);
            if (ask.cancelled())
            {
                sched::exit_current(KOS_EXIT_CANCELLED, sched::EXIT_RETURN, &lock);
            }
            t = task_resolve(task);
            if (t == nullptr)
            {
                return -KOS_EBADF;
            }
            if (not task_created_by(t, kernel().threads.kill_tag_of(c)))
            {
                return -KOS_EPERM;
            }
            if (c->task == t)
            {
                // A member would be waiting for its own death, and the group cancel would claim
                // its resume too.
                return -KOS_EINVAL;
            }
            // Already empty and swept: nothing could wake a park here. A member still sweeping
            // wakes it at the end of its sweep.
            if (task_member_count(t) == 0 and task_sweeping(t) == 0)
            {
                task_drop_hold(t, lock);
                return 0;
            }
            // Parked before the group cancel, and the epoch sampled before it, as in
            // thread_slay: a member that outranks this thread can reach its own exit from
            // inside that call on a backend that swaps inline.
            epoch = park_queueless(ask)(c, WAIT_TASK_EMPTY, t, lock);
            ktime_deadline_arm(c, timeout_us, lock);
            task_stop(t, lock);
            sched::reschedule(nullptr, lock);
        }
        wq_confirm_resume(c, epoch);
        int const rc = static_cast<int>(c->wait_result);
        if (rc == 0)
        {
            IrqLock lock;
            task_drop_hold(t, lock);
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
            if (not caller_spawned(t, c))
            {
                return -KOS_EPERM;
            }
            // Its exit sweep has already run, so a park here would never be woken.
            if (t->state == ThreadState::EXITED)
            {
                return 0;
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
