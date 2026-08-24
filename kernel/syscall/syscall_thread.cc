// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Thread lifecycle syscalls: thread_spawn, thread_kill, thread_join and the
// wait-until-last aggregate. The noreturn exit path is sched::exit_current, which the
// dispatch calls directly.

#include <kickos/arch/arch.h>
#include <kickos/cap.h>
#include <kickos/config.h>
#include <kickos/grant.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/reent.h>
#include <kickos/sched.h>
#include <kickos/sync.h> // wq_confirm_resume
#include <kickos/task.h>
#include <kickos/thread.h>
#include <kickos/time.h> // ktime_deadline_arm

#include <kickos/sys/abi.h>
#include <kickos/sys/errno.h>
#include <kickos/tls.h>

#include "syscall_internal.h"

namespace kickos
{
    namespace
    {
        // Resolve a thread handle against the pool, or nullptr for a slot never allocated
        // or reclaimed under this handle. An EXITED slot RESOLVES here: the generation bumps
        // at reclaim and not at exit, so only the caller can say whether that state is a
        // refusal (thread_kill) or the answer (thread_join). Caller holds IrqLock.
        //
        // NO SIGN TEST. A slot aged past 32768 reclaims mints a handle with bit 31 set, so
        // rejecting "negative" handles would refuse live threads. KOS_THREAD_NONE and every
        // other malformed word is caught by the index range check.
        Thread* thread_resolve(kos_thread_t thread)
        {
            Kernel& k = kernel();
            int const index =
                static_cast<int>(thread & ((1u << ThreadPool::INDEX_BITS) - 1u));
            uint16_t const gen = static_cast<uint16_t>(thread >> ThreadPool::INDEX_BITS);
            if (index >= k.threads.next)
            {
                return nullptr;
            }
            if (k.threads.gen[index] != gen)
            {
                return nullptr;
            }
            return &k.threads.slots[index];
        }

        // Spawn parenthood: the whole gate on both thread_kill and thread_join, and
        // NON-TRANSFERABLE, there being no table entry for a cap_grant to copy. kill_tag_of
        // never answers KILL_TAG_NONE, so an orphan (a child whose spawner's slot changed
        // hands) matches nobody at all. Caller holds IrqLock.
        bool caller_spawned(Thread const* t, Thread const* c)
        {
            if (c == nullptr or t->spawner_tag == ThreadPool::KILL_TAG_NONE)
            {
                return false;
            }
            return t->spawner_tag == kernel().threads.kill_tag_of(c);
        }
    }

    static int spawn_masked(kos_thread_params const* p, kos_thread_t* out_thread,
                            Thread** out_child)
    {
        IrqLock lock;
        *out_thread = KOS_THREAD_NONE; // every early return below leaves the sentinel seated
        if (p == nullptr)
        {
            return -KOS_EINVAL;
        }
        // Every field below is read from the kernel-owned copy, never from *p. The name
        // pointer inside the copy is STILL user memory and is walked under a per-byte check
        // further down.
        // user_readable_ok, not user_range_ok: the struct may be an app global, which on a
        // backend modelling no static-data region lies in no granted region, and
        // arch_user_text_readable is the arm that recognises it.
        // The misalignment reject must precede the typed copy: the kernel does that load
        // privileged, and on a strict-align arch a misaligned word load traps IN THE KERNEL.
        uintptr_t const pu = reinterpret_cast<uintptr_t>(p);
        if ((pu & (alignof(kos_thread_params) - 1)) != 0)
        {
            return -KOS_EINVAL;
        }
        if (not user_readable_ok(pu, sizeof(*p)))
        {
            return -KOS_EFAULT;
        }
        kos_thread_params params;
        kaccess_from_user(&params, pu, sizeof(params));
        p = &params;
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
        // Validated BEFORE a slot is allocated, so a bad stack is a clean spawn error and
        // not a leaked slot. stack_base == 0 asks for the kernel default. Base AND size must
        // be KICKOS_STACK_ALIGN-aligned, or the initial stack top is not.
        if (p->stack_base != nullptr)
        {
            uintptr_t const base = reinterpret_cast<uintptr_t>(p->stack_base);
            if (p->stack_size < KICKOS_MIN_STACK_SIZE
                or (base & (KICKOS_STACK_ALIGN - 1)) != 0
                or (p->stack_size & (KICKOS_STACK_ALIGN - 1)) != 0
                or base + p->stack_size < base) // base+size must not wrap the address space
            {
                return -KOS_EINVAL;
            }
            // An unprivileged thread's stack is committed as ONE R|W MPU region, so the
            // block must be nameable by one descriptor on this arch; otherwise PMSA/NAPOT
            // snap the base and the enforced window covers the wrong span.
#if KICKOS_HAVE_MPU
            // Keys on the CHILD's privilege: a privileged child gets the whole arena plus
            // the background region and needs no stack descriptor.
            if (p->privileged == 0)
            {
                size_t const rsz = arch_ram_region_size(p->stack_size);
                if (not arch_ram_region_admissible(base, rsz))
                {
                    return -KOS_EINVAL;
                }
                // Rule 7 admission, arena-confined for EVERY caller with no privileged
                // waiver: without it an out-of-arena stack_base grants an R|W window over
                // peripheral or kernel SRAM. The RAM arm ignores the authorization flag.
                if (not grant_region_admissible(base, rsz, ARCH_MPU_R | ARCH_MPU_W,
                                                cap_check_authority(sched::current(),
                                                                    AUTH_MEMORY)))
                {
                    return -KOS_EPERM; // stack outside the arena / hits a reserved block
                }
            }
#endif
        }
        // mem_base's arena-confinement and Rule 7 admission belong to domain_for, which
        // reports -KOS_EPERM directly. Only the wrap test is duplicated, UNGATED, so a
        // wrapping mem_base is a clean -KOS_EINVAL even on a no-MPU part where domain_for's
        // predicate is a stub.
        if (p->mem_base != nullptr and p->mem_size != 0)
        {
            uintptr_t const dbase = reinterpret_cast<uintptr_t>(p->mem_base);
            if (dbase + p->mem_size < dbase)
            {
                return -KOS_EINVAL;
            }
        }
        // THE admission boundary for a DEV window, which is the asking THREAD's own region
        // and is carried by no task or domain. This and the commit, thread_create composing
        // the region, both run inside this function's IrqLock, so the pair is atomic.
        if (p->mmio_base != nullptr)
        {
            if (not cap_check_authority(sched::current(), AUTH_MEMORY))
            {
                return -KOS_EPERM; // MMIO needs AUTH_MEMORY: never self-grantable
            }
            uintptr_t const mbase = reinterpret_cast<uintptr_t>(p->mmio_base);
            if (p->mmio_size == 0 or mbase + p->mmio_size < mbase)
            {
                return -KOS_EINVAL;
            }
            if (not arch_mpu_region_encodable(mbase, p->mmio_size))
            {
                return -KOS_EINVAL; // window one MPU descriptor cannot cover exactly
            }
            // A privileged child carries the whole-arena region and the background map, so
            // it is granted no window descriptor and there is nothing here to admit.
            if (p->privileged == 0)
            {
                if (not grant_region_admissible(mbase, p->mmio_size,
                                                ARCH_MPU_R | ARCH_MPU_W | ARCH_MPU_DEV,
                                                cap_check_authority(sched::current(),
                                                                    AUTH_MEMORY)))
                {
                    return -KOS_EPERM; // reserved block / bit-band alias / unauthorized DEV
                }
                if (not dev_window_free(mbase, p->mmio_size))
                {
                    return -KOS_EBUSY; // already held; no stealing
                }
            }
        }
        if (p->authority != 0)
        {
            if ((p->authority & ~CAP_AUTH_ALL) != 0)
            {
                // A bit no gate reads is refused, never silently masked off. The authority
                // word has its own numbering, so this catches only bits above the defined
                // authorities; an object right is not distinguishable here.
                return -KOS_EINVAL;
            }
            // Narrow-only, like a cap_grant mask: the caller must already hold every bit it
            // hands on.
            if (not cap_check_authority(sched::current(), p->authority))
            {
                return -KOS_EPERM;
            }
        }
        // The WHOLE grant list is validated BEFORE anything is claimed: every source cap
        // must resolve in the CALLER's table, carry CAP_TRANSFER, and narrow only. Installs
        // and ref bumps happen only after every check passes.
        // Sized by the GRANT bound, NOT by the table ceiling: these, plus gbuf and dbuf
        // below, live on the CALLER's stack, which can be 1 KiB.
        int deleg_obj[KICKOS_MAX_SPAWN_GRANTS];
        uint8_t deleg_type[KICKOS_MAX_SPAWN_GRANTS];
        uint8_t deleg_rights[KICKOS_MAX_SPAWN_GRANTS];
        // uint16_t: a destination is a capability-table index, and a table is up to
        // KICKOS_MAX_HANDLES == 65535 slots wide, which a byte cannot name.
        uint16_t deleg_dest[KICKOS_MAX_SPAWN_GRANTS];
        int const ncaps = static_cast<int>(p->cap_count);

        Thread* const spawner = sched::current();
        if (ncaps > 0)
        {
            // Delegated cap i lands at child index i+1 by DEFAULT, index 0 being the
            // kernel's stdout slot, and each grant may name its own index instead. The
            // default indices fit because KICKOS_MAX_SPAWN_GRANTS < KICKOS_CAP_CHILD_WIDTH;
            // a caller-NAMED destination is not covered by that and is refused at the bound
            // below.
            if (ncaps > KICKOS_MAX_SPAWN_GRANTS)
            {
                return -KOS_EINVAL;
            }
            uintptr_t const cu = reinterpret_cast<uintptr_t>(p->caps);
            if (p->caps == nullptr or (cu & (alignof(kos_cap_grant) - 1)) != 0)
            {
                return -KOS_EINVAL;
            }
            // user_readable_ok for the same reason as the params struct above: the array
            // may be a global.
            if (not user_readable_ok(cu, sizeof(kos_cap_grant) * static_cast<size_t>(ncaps)))
            {
                return -KOS_EFAULT;
            }
            // Snapshotted in one pass and validated from the copy: no double fetch of
            // p->caps[ci] between the check and the read.
            kos_cap_grant gbuf[KICKOS_MAX_SPAWN_GRANTS];
            for (int ci = 0; ci < ncaps; ci++)
            {
                kaccess_from_user(&gbuf[ci],
                                  cu + static_cast<size_t>(ci) * sizeof(kos_cap_grant),
                                  sizeof(kos_cap_grant));
            }
            // The optional destination array, snapshotted the same way and for the same
            // double-fetch reason. Absent => every entry defaults.
            uint16_t dbuf[KICKOS_MAX_SPAWN_GRANTS] = {};
            if (p->cap_dest != nullptr)
            {
                uintptr_t const du = reinterpret_cast<uintptr_t>(p->cap_dest);
                // The misalignment reject precedes the copy for the params struct's reason:
                // kaccess_from_user loads privileged, and a misaligned halfword load traps
                // in the kernel on a strict-align arch.
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
                    kaccess_from_user(&dbuf[ci],
                                      du + static_cast<size_t>(ci) * sizeof(uint16_t),
                                      sizeof(uint16_t));
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
                    return -KOS_EPERM; // no TRANSFER right
                }
                uint8_t const mask = g.rights_mask;
                if ((mask & se->rights) != mask) // mask must be a subset: no widening
                {
                    return -KOS_EINVAL;
                }
                deleg_obj[ci] = se->obj;
                deleg_type[ci] = se->type;
                deleg_rights[ci] = static_cast<uint8_t>(se->rights & mask);
                // An absent array, or a 0 entry, means default placement. 0 costs nothing
                // as a sentinel: index 0 is the kernel's stdout slot, which cap_install_at
                // refuses anyway.
                unsigned dest = dbuf[ci];
                if (dest == 0u)
                {
                    dest = static_cast<unsigned>(KOS_SPAWN_DELEGATED_CAP0) +
                           static_cast<unsigned>(ci);
                }
                deleg_dest[ci] = static_cast<uint16_t>(dest);
            }
            // No two grants may land on the same slot, defaulted ones included: the second
            // install would silently overwrite the first and leak its reference.
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
        // Must precede the slot claim, so a task- or domain-pool exhaustion is a clean spawn
        // failure and not a leaked thread slot. Neither arm takes a reference: a task
        // task_for creates but nobody references stays at refcount 0 with no creator, which
        // is a free slot, and the domain under it likewise.
        Task* tk = nullptr;
        if (p->task != KOS_TASK_NONE)
        {
            // JOIN a group the caller created. The memory the group shares is the TASK's,
            // so a member bringing its own data grant is refused, not silently ignored:
            // there would be no domain for it to land in.
            if (p->privileged != 0)
            {
                return -KOS_EINVAL; // a privileged thread holds the kernel domain: no group
            }
            if (p->mem_base != nullptr and p->mem_size != 0)
            {
                return -KOS_EINVAL; // the task's grant is the group's memory
            }
            tk = task_resolve(p->task);
            if (tk == nullptr)
            {
                return -KOS_EBADF; // never created, or the slot was freed under this handle
            }
            if (not task_created_by(tk, k.threads.kill_tag_of(spawner)))
            {
                return -KOS_EPERM; // only the creator seats members
            }
        }
        else
        {
            int derr = 0;
            // AUTH_MEMORY, not raw privilege, is the bit covering a spawn-time grant.
            // Resolved here because domain_for must not read sched::current().
            tk = task_for(p->privileged != 0, p->mem_base, p->mem_size,
                          cap_check_authority(sched::current(), AUTH_MEMORY), &derr);
            if (tk == nullptr)
            {
                // EPERM inadmissible grant, ENOMEM domain or task pool full.
                return -derr;
            }
        }
        // Reclaiming an EXITED slot is safe on SINGLE CORE only: such a thread parked in
        // exit_current until its switch-away committed, so it is off-CPU and off every
        // ready/wait/timer list by the time another thread gets here.
        int const i = k.threads.alloc();
        if (i < 0)
        {
            return -KOS_ENOMEM;
        }

        ThreadAttr attr;
        attr.name = "user";
        // EACH source byte is checked caller-readable before the privileged copy
        // dereferences it: the kernel must neither fault on a bad name pointer nor leak
        // another domain's page through it. That also BOUNDS the walk, so a string with no
        // NUL stops at the first unreachable byte, and an unreachable first byte leaves the
        // "user" default.
        char namebuf[KICKOS_THREAD_NAME_MAX]; // thread_create re-clamps regardless
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
                kaccess_from_user(&namebuf[ni], np + ni, 1);
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
        attr.mem_base = p->mem_base;
        attr.mem_size = p->mem_size;
        attr.mmio_base = p->mmio_base;
        attr.mmio_size = p->mmio_size;
        attr.task = tk;
        attr.spawner_tag = k.threads.kill_tag_of(spawner);

        // BOTH sources failing must release the slot just claimed, or the spawn leaks a TCB
        // and burns the prior occupant's join handle.
        void* stack = p->stack_base;
        size_t stack_size = p->stack_size;
        if (p->stack_base == nullptr)
        {
            stack = k.threads.stack_pop();
            if (stack == nullptr)
            {
                stack = arch_ram_alloc(KICKOS_USER_STACK_SIZE);
            }
            if (stack == nullptr)
            {
                k.threads.release(i);
                return -KOS_ENOMEM;
            }
            stack_size = KICKOS_USER_STACK_SIZE;
            attr.kstack_owned = true;
        }
        // A CALLER-SUPPLIED STACK MUST SATISFY THE TLS STRIDE. The thread pointer is SP
        // masked down to KICKOS_TLS_STRIDE, so a block that is not strided, or that spans
        // more than one stride, would hand this thread a pointer into a NEIGHBOUR's
        // thread_local storage. The pool's own blocks satisfy it by construction.
        if (not tls_stack_admissible(reinterpret_cast<uintptr_t>(stack), stack_size))
        {
            // Unreachable from the pool branch today, every arena block being one stride,
            // but a popped block dropped here is a slot the pool never gets back.
            if (attr.kstack_owned)
            {
                k.threads.stack_push(stack);
            }
            k.threads.release(i);
            return -KOS_EINVAL;
        }
        // Taken BEFORE the reference loop so one unwind path serves both failures.
        if (not cap_slab_attach(&attr.cap_run, KICKOS_CAP_CHILD_WIDTH, &attr.cap_free_head,
                                &attr.cap_width))
        {
            if (attr.kstack_owned)
            {
                k.threads.stack_push(stack);
            }
            k.threads.release(i);
            return -KOS_ENOMEM;
        }
        // Bounded by the run the child ACTUALLY gets. Checked before any reference is
        // taken, so the only unwind owed here is the run.
        for (int ci = 0; ci < ncaps; ci++)
        {
            if (deleg_dest[ci] >= attr.cap_width)
            {
                cap_slab_detach(&attr.cap_run, &attr.cap_free_head, &attr.cap_width);
                if (attr.kstack_owned)
                {
                    k.threads.stack_push(stack);
                }
                k.threads.release(i);
                return -KOS_EINVAL;
            }
        }

        // EVERY delegated object reference is taken before the child exists. obj_ref_inc is
        // the last fallible step in the spawn (a uint8_t refcount at its ceiling is refused,
        // not wrapped), and the only state to give back here is the slot and the
        // demand-allocated stack. After thread_create the unwind would additionally owe the
        // task reference and the child's already-seated caps.
        for (int ci = 0; ci < ncaps; ci++)
        {
            if (obj_ref_inc(static_cast<CapType>(deleg_type[ci]), deleg_obj[ci],
                            deleg_rights[ci]))
            {
                continue;
            }
            for (int cj = 0; cj < ci; cj++)
            {
                obj_ref_undo(static_cast<CapType>(deleg_type[cj]), deleg_obj[cj],
                             deleg_rights[cj]);
            }
            cap_slab_detach(&attr.cap_run, &attr.cap_free_head, &attr.cap_width);
            if (attr.kstack_owned)
            {
                k.threads.stack_push(stack);
            }
            k.threads.release(i);
            return -KOS_EOVERFLOW; // an object refcount is at its ceiling
        }
        thread_create(&k.threads.slots[i], p->entry, p->arg, stack, stack_size, attr);
        // Nothing below here may fail: the validation above guarantees each install
        // succeeds, and the reference loop above already holds a reference for every cap
        // seated here.
        Thread* const child = &k.threads.slots[i];
        cap_install_defaults(child);
        cap_seat_authority(child, p->authority);
        for (int ci = 0; ci < ncaps; ci++)
        {
            cap_install_at(child, static_cast<int>(deleg_dest[ci]), deleg_obj[ci],
                           static_cast<CapType>(deleg_type[ci]), deleg_rights[ci]);
        }
        // ONLY A REUSED SLOT PAYS. kmain seeds every slot's reent at boot, so a first
        // occupant needs nothing here; a slot whose previous occupant exited is marked
        // reent_stale and re-seeded now.
        //
        // WHAT IS LEFT IS STILL INSIDE THE LOCK, and deliberately. Between an allocated child
        // and sched::add the SPAWNER is preemptible, and a spawner slain in that gap never
        // returns to its continuation at all, because switch_book redirects a CANCEL_SLAY
        // thread to the exit stub. The child would then be a fully built INACTIVE orphan
        // holding a slot, a stack, a task reference and its delegated caps, with nothing left
        // running that knows to finish or free it. A spawn is a transaction and the lock is
        // what makes it one.
#if !KICKOS_ARCH_SIM
        if (k.threads.reent_stale[i])
        {
            kickos_reent_init(child->reent);
            k.threads.reent_stale[i] = false;
        }
#endif
        sched::add(child);
        *out_thread = k.threads.handle_for(i);
        *out_child = child;
        return 0;
    }

    int thread_spawn(kos_thread_params const* p, kos_thread_t* out_thread)
    {
        Thread* child = nullptr;
        return spawn_masked(p, out_thread, &child);
    }

    // NOT a destroy. This MARKS the target and breaks whatever park it is in; the target
    // then reaches its own death point (the syscall boundary) and runs its own exit_current.
    // The only survivor is a thread that never enters the kernel again, which no caller may
    // assume it will not be.
    //
    // The gate is PARENTHOOD (caller_spawned), not an authority bit and not a capability.
    int thread_kill(kos_thread_t thread)
    {
        IrqLock lock;
        Thread* const t = thread_resolve(thread);
        if (t == nullptr)
        {
            return -KOS_EBADF; // never allocated, or the slot was reclaimed under this handle
        }
        // An exited-but-unreclaimed slot still gen-matches, and there is nothing left to
        // cancel in it. thread_join is the one caller that must ACCEPT that state instead.
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
        thread_cancel(t);
        return 0;
    }

    // The FORCIBLE half, same gate and same reach as thread_kill. The target's resume is
    // CLAIMED (switch_to rebuilds its context into kickos_thread_slay_exit before
    // arch_switch), so unlike a kill it never returns to userspace at all and never gets the
    // window in which a driver would have quieted its device. The victim still runs its own
    // cap_teardown, in its own context, through exit_current.
    //
    // No timer is needed to reach a spinning victim: on one core a target distinct from the
    // caller is READY, BLOCKED or refused below, and both live states are claimed at the
    // resume rather than at the request.
    int thread_slay(kos_thread_t thread, uint32_t timeout_us)
    {
        Thread* const c = sched::current();
        uint32_t epoch = 0;
        {
            IrqLock lock;
            Thread* const t = thread_resolve(thread);
            if (t == nullptr)
            {
                return -KOS_EBADF; // never allocated, or the slot was reclaimed under this handle
            }
            if (t->state == ThreadState::EXITED or t->state == ThreadState::INACTIVE)
            {
                return -KOS_EBADF; // as thread_kill: nothing left in the slot to condemn
            }
            if (t == c)
            {
                return -KOS_EINVAL; // ending yourself is kos_exit; this path must return
            }
            // Slaying idle ends the scheduler's fallback. A privileged thread may be inside
            // kernel work holding kernel invariants, and discarding its frames discards them
            // mid-flight. Same rule kickos_fault_kill_thread states for itself.
            if (t == sched::idle() or t->privileged)
            {
                return -KOS_EINVAL;
            }
            if (not caller_spawned(t, c))
            {
                return -KOS_EPERM;
            }
            // THE CALLER PARKS FIRST, and the order is load-bearing: thread_cancel_kind
            // switches to a victim that outranks the caller, and on a backend that swaps
            // inline that victim can reach EXITED before this line would otherwise have run,
            // its exit sweep then finding nobody parked on it. park_queueless also detaches
            // `current`, which the cancel may already have republished.
            park_queueless(c, WAIT_JOIN, t);
            if (timeout_us != KOS_TIMEOUT_NONE)
            {
                // 0 is the arm-and-return form: the deadline is already behind the
                // min-delta floor, so the timer releases this park at the first opportunity
                // and the answer is -KOS_ETIMEDOUT unless the victim got there first.
                ktime_deadline_arm(c, timeout_us);
            }
            // SAMPLED BEFORE THE CANCEL, unlike thread_join's: the cancel can switch, and on
            // a backend that swaps INLINE the victim may run, die and wake this thread back
            // up inside that call. An epoch read afterwards would already carry the resume it
            // is meant to wait for, and wq_confirm_resume would spin to
            // KICKOS_POLL_SPIN_MAX and panic.
            epoch = c->switch_count;
            thread_cancel_kind(t, CANCEL_SLAY);
            sched::reschedule();
        }
        wq_confirm_resume(c, epoch); // the lock is RELEASED across this: see sync.h
        // 0 (the target is gone and swept), -KOS_ETIMEDOUT (the timer arm: condemned but not
        // yet gone), or -KOS_ECANCELED (the caller was itself cancelled, e.g. because the
        // victim's group cancel reached it).
        return static_cast<int>(c->wait_result);
    }

    // Create a task: an empty group that exists before any of its threads, holding a domain
    // built from THIS grant. Only the creator may seat members into it or end it, on the
    // same non-transferable parenthood gate as thread_kill.
    int task_create_call(void* mem_base, size_t mem_size, uint32_t mem_attr,
                         kos_task_t* out_task)
    {
        IrqLock lock;
        *out_task = KOS_TASK_NONE; // every early return below leaves the sentinel seated
        Thread* const c = sched::current();
        if (mem_base != nullptr and mem_size != 0)
        {
            uintptr_t const base = reinterpret_cast<uintptr_t>(mem_base);
            if (base + mem_size < base)
            {
                return -KOS_EINVAL; // the shared window wraps the address space
            }
        }
        int derr = 0;
        Task* const t = task_create(kernel().threads.kill_tag_of(c), mem_base, mem_size,
                                    mem_attr, cap_check_authority(c, AUTH_MEMORY), &derr);
        if (t == nullptr)
        {
            return -derr; // EPERM inadmissible grant, ENOMEM domain or task pool full
        }
        *out_task = task_handle(t);
        return 0;
    }

    // End a group: every live member is cancelled, and the creator's hold goes with it so
    // the handle stops naming anything. Cooperative exactly as thread_kill is, so a member
    // that never enters the kernel again is never reached; and not a destroy, the members
    // running their own exits, so the slot goes back when the last one is gone.
    int task_kill(kos_task_t task)
    {
        IrqLock lock;
        Task* const t = task_resolve(task);
        if (t == nullptr)
        {
            return -KOS_EBADF; // never created, or the slot was freed under this handle
        }
        if (not task_created_by(t, kernel().threads.kill_tag_of(sched::current())))
        {
            return -KOS_EPERM;
        }
        // The group cancel runs BEFORE the hold is dropped: dropping it first can free the
        // slot outright when the group is already empty, and `t` would then be a dangling name.
        task_cancel_group(t, CANCEL_KILL);
        task_drop_hold(t);
        return 0;
    }

    // The group form. Every live member is SLAIN rather than cancelled, and the caller waits
    // for the group to be empty, which is a different condition from any member's death and
    // has its own park kind.
    //
    // The creator's hold is dropped only on the way out of a successful wait, never before
    // it: dropping it first frees the slot the moment the group empties, leaving `t` a
    // dangling name in a wait edge. On a timeout the hold survives with the group, so the
    // handle still names something and a second call is possible.
    int task_slay(kos_task_t task, uint32_t timeout_us)
    {
        Thread* const c = sched::current();
        uint32_t epoch = 0;
        Task* t = nullptr;
        {
            IrqLock lock;
            t = task_resolve(task);
            if (t == nullptr)
            {
                return -KOS_EBADF; // never created, or the slot was freed under this handle
            }
            if (not task_created_by(t, kernel().threads.kill_tag_of(c)))
            {
                return -KOS_EPERM;
            }
            if (c->task == t)
            {
                // A member slaying its own group would be waiting for its own death, and the
                // group cancel below would claim ITS resume too. kos_exit ends a member and
                // takes the group with it.
                return -KOS_EINVAL;
            }
            // Already empty, so there is nothing to wait for and nothing that could wake a
            // park here. The hold is what still names the slot; dropping it IS the whole job.
            if (task_member_count(t) == 0)
            {
                task_drop_hold(t);
                return 0;
            }
            // Parked before the group cancel, and the epoch sampled before it, for the
            // reasons thread_slay above states: a member that outranks this thread can reach
            // its own exit from inside that call on a backend that swaps inline.
            park_queueless(c, WAIT_TASK_EMPTY, t);
            if (timeout_us != KOS_TIMEOUT_NONE)
            {
                ktime_deadline_arm(c, timeout_us);
            }
            epoch = c->switch_count;
            task_cancel_group(t, CANCEL_SLAY);
            sched::reschedule();
        }
        wq_confirm_resume(c, epoch);
        int const rc = static_cast<int>(c->wait_result);
        if (rc == 0)
        {
            // The group is empty and the hold is the only thing left holding the slot.
            IrqLock lock;
            task_drop_hold(t);
        }
        return rc;
    }

    // Park until the named thread is gone, bounded by `timeout_us` unless that is
    // KOS_TIMEOUT_NONE. The joiner parks queue-less tagged WAIT_JOIN; sched::exit_current
    // sweeps the pool for that tag, and that sweep is the only thing that wakes it.
    int thread_join(kos_thread_t thread, uint32_t timeout_us)
    {
        Thread* const c = sched::current();
        uint32_t epoch = 0;
        {
            IrqLock lock;
            Thread* const t = thread_resolve(thread);
            if (t == nullptr or t->state == ThreadState::INACTIVE)
            {
                return -KOS_EBADF; // never allocated, or reclaimed under this handle
            }
            if (t == c)
            {
                return -KOS_EDEADLK; // nothing would ever wake this park
            }
            if (not caller_spawned(t, c))
            {
                return -KOS_EPERM;
            }
            // THE state join exists to observe, and the one thread_kill refuses: the
            // generation bumps at RECLAIM and not at exit, so a handle to an
            // exited-but-unreclaimed slot still resolves and the target IS gone. Refusing it
            // here would hang a joiner on an already-dead thread.
            if (t->state == ThreadState::EXITED)
            {
                return 0;
            }
            park_queueless(c, WAIT_JOIN, t);
            if (timeout_us != KOS_TIMEOUT_NONE)
            {
                ktime_deadline_arm(c, timeout_us);
            }
            epoch = c->switch_count;
            sched::reschedule();
        }
        wq_confirm_resume(c, epoch); // the lock is RELEASED across this: see sync.h
        // 0 (target exited), -KOS_ETIMEDOUT (the timer arm), or -KOS_ECANCELED (the joiner
        // itself was cancelled, e.g. a task group kill; thread_abort_park handles WAIT_JOIN)
        return static_cast<int>(c->wait_result);
    }

    // Park until the CALLER is the last live thread. Takes NO deadline, and it is the only
    // shutdown primitive that covers a main's GRANDCHILDREN, which no handle it holds can
    // name.
    //
    // ROOT ONLY. It observes, and waits on, threads outside the caller's own spawn subtree,
    // and it is single-seat: whoever parks here first denies the primitive to everyone else
    // for as long as it waits. Root cannot be inside two syscalls at once, so there is never
    // a second waiter.
    int thread_wait_last()
    {
        Thread* const c = sched::current();
        uint32_t epoch = 0;
        {
            IrqLock lock;
            if (not kernel().threads.is_root(c))
            {
                return -KOS_EPERM;
            }
            if (sched::live_count() <= 1)
            {
                return 0; // already the last one: parking here would never be woken
            }
            park_queueless(c, WAIT_LIVE_LAST, nullptr);
            epoch = c->switch_count;
            sched::reschedule();
        }
        wq_confirm_resume(c, epoch);
        // The exit sweep is the only waker and it releases this park only when the caller IS
        // the last live thread, so there is no other outcome to report.
        return 0;
    }
}
