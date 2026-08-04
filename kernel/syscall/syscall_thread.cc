// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Thread lifecycle syscalls: thread_spawn and thread_kill. The noreturn exit path is
// sched::exit_current, which the dispatch calls directly.

#include <kickos/arch/arch.h>
#include <kickos/cap.h>
#include <kickos/config.h>
#include <kickos/domain.h>
#include <kickos/grant.h>
#include <kickos/instance.h>
#include <kickos/irq.h> // irq_thread_parked (the one park a cancel may end)
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/thread.h>

#include <kickos/sys/abi.h>
#include <kickos/sys/errno.h>

#include "syscall_internal.h"

namespace kickos
{
    // Slot reuse is safe because thread_create re-inits the TCB and re-fabricates the arch
    // context from scratch, so a reclaimed slot's privilege posture is a clean reset (the
    // sim's mid-syscall `raised`, the ARM CONTROL.nPRIV, the RX PSW). Any syscall that
    // resolves a thread BY HANDLE must additionally reject state == EXITED: the generation
    // bumps at reclaim, not at exit, so an exited slot still gen-matches.
    int thread_spawn(kos_thread_params const* p, kos_thread_t* out_thread)
    {
        IrqLock lock;
        *out_thread = KOS_THREAD_NONE; // every early return below leaves the sentinel seated
        if (p == nullptr)
        {
            return -KOS_EINVAL; // null params
        }
        // Every field below is read from the kernel-owned copy, never from *p: an
        // unprivileged caller must not hand the kernel a pointer it could not itself read,
        // or a kernel address gets dereferenced privileged. The name pointer inside the
        // copy is STILL user memory and is walked under a per-byte check further down.
        // user_readable_ok, not user_range_ok: the struct may be an app global, which on a
        // backend modelling no static-data region lies in no granted region, and
        // arch_user_text_readable is the arm that recognises it.
        // The misalignment reject must precede the typed copy: the kernel does that load
        // privileged, and on a strict-align arch a misaligned word load traps IN THE KERNEL.
        uintptr_t const pu = reinterpret_cast<uintptr_t>(p);
        if ((pu & (alignof(kos_thread_params) - 1)) != 0)
        {
            return -KOS_EINVAL; // misaligned params struct
        }
        if (not user_readable_ok(pu, sizeof(*p)))
        {
            return -KOS_EFAULT; // params not readable by the caller
        }
        kos_thread_params params;
        kaccess_from_user(&params, pu, sizeof(params));
        p = &params;
        // prio indexes the ready lists and drives a 1u<<prio bitmap shift, so an
        // out-of-range value is an OOB write and UB. Priority 0 is idle's alone.
        if (p->prio < KICKOS_PRIO_MIN or p->prio > KICKOS_PRIO_MAX)
        {
            return -KOS_EINVAL; // out-of-range priority
        }
        // No privilege escalation: only a privileged thread may spawn one, a privileged
        // thread being granted the whole arena.
        if (p->privileged != 0 and not sched::current()->privileged)
        {
            return -KOS_EPERM; // unprivileged caller cannot spawn a privileged child
        }
        // Validated BEFORE a slot is allocated, so a bad stack is a clean spawn error
        // rather than a leaked slot or a silent overflow. stack_base == 0 asks for the
        // kernel default. Base AND size must be KICKOS_STACK_ALIGN-aligned, or the initial
        // stack top is not.
        if (p->stack_base != nullptr)
        {
            uintptr_t const base = reinterpret_cast<uintptr_t>(p->stack_base);
            if (p->stack_size < KICKOS_MIN_STACK_SIZE
                or (base & (KICKOS_STACK_ALIGN - 1)) != 0
                or (p->stack_size & (KICKOS_STACK_ALIGN - 1)) != 0
                or base + p->stack_size < base) // base+size must not wrap the address space
            {
                return -KOS_EINVAL; // bad caller stack: size / alignment / wrap
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
                    return -KOS_EINVAL; // stack block not nameable by one descriptor
                }
                // The stack takes the same Rule 7 admission as any grant: arena-confined
                // for EVERY caller with no privileged waiver, and reserved-block-clear.
                // Without this an out-of-arena stack_base grants an R|W window over
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
        // reports -KOS_EPERM directly, so nothing here re-checks them to recover an errno.
        // Only the wrap test is duplicated, UNGATED, so a wrapping mem_base is a clean
        // -KOS_EINVAL even on a no-MPU part where domain_for's predicate is a stub.
        if (p->mem_base != nullptr and p->mem_size != 0)
        {
            uintptr_t const dbase = reinterpret_cast<uintptr_t>(p->mem_base);
            if (dbase + p->mem_size < dbase)
            {
                return -KOS_EINVAL; // mem_base window wraps the address space
            }
        }
        // The PRECISE-ERROR boundary for an MMIO grant: authority (EPERM) and exact shape
        // (EINVAL for zero-size, wrap or non-encodable). The AUTHORITATIVE Rule 7 admission
        // is domain_for's on the same window; this thin gate exists only because
        // domain_for's single nullptr sentinel cannot express which malformation it was.
        if (p->mmio_base != nullptr)
        {
            if (not cap_check_authority(sched::current(), AUTH_MEMORY))
            {
                return -KOS_EPERM; // MMIO needs AUTH_MEMORY: never self-grantable
            }
            uintptr_t const mbase = reinterpret_cast<uintptr_t>(p->mmio_base);
            if (p->mmio_size == 0 or mbase + p->mmio_size < mbase)
            {
                return -KOS_EINVAL; // zero-size or wrapping MMIO window
            }
            if (not arch_mpu_region_encodable(mbase, p->mmio_size))
            {
                return -KOS_EINVAL; // window one MPU descriptor cannot cover exactly
            }
        }
        // Validated here with the other boundary checks, so a bad request is a clean spawn
        // refusal rather than a half-built child.
        if (p->authority != 0)
        {
            if ((p->authority & ~CAP_AUTH_ALL) != 0)
            {
                // A bit no gate reads is refused, never silently masked off. The authority
                // word has its own numbering, so this catches only bits above the defined
                // authorities; an object right is not distinguishable here.
                return -KOS_EINVAL; // non-authority bits in the authority mask
            }
            // Narrow-only, like a cap_grant mask: the caller must already hold every bit it
            // hands on.
            if (not cap_check_authority(sched::current(), p->authority))
            {
                return -KOS_EPERM; // cannot grant an authority the caller does not hold
            }
        }
        // The WHOLE grant list is validated BEFORE anything is claimed: every source cap
        // must resolve in the CALLER's table, carry CAP_TRANSFER, and narrow only. Installs
        // and ref bumps happen only after every check passes, so a mid-install failure
        // cannot leave a half-populated child with dangling ref bumps.
        // Sized by the GRANT bound, NOT by the table ceiling: these, plus gbuf and dbuf
        // below, live on the CALLER's stack, which can be 1 KiB. At KICKOS_MAX_HANDLES they
        // would be 16 bytes per table slot, a stack overflow mid-syscall on any board with
        // a large table.
        int deleg_obj[KICKOS_MAX_SPAWN_GRANTS];
        uint8_t deleg_type[KICKOS_MAX_SPAWN_GRANTS];
        uint8_t deleg_rights[KICKOS_MAX_SPAWN_GRANTS];
        // uint16_t: a destination is a capability-table index and a table is up to
        // KICKOS_MAX_HANDLES == 65535 slots wide, which a byte cannot name.
        uint16_t deleg_dest[KICKOS_MAX_SPAWN_GRANTS];
        int const ncaps = static_cast<int>(p->cap_count);

        Thread* const spawner = sched::current();
        if (ncaps > 0)
        {
            // Delegated cap i lands at child index i+1 by DEFAULT, index 0 being the
            // kernel's stdout slot, and each grant may name its own index instead. That
            // default packing is independent of the reserved cap-index range; what makes
            // the default indices fit is KICKOS_MAX_SPAWN_GRANTS < KICKOS_MAX_HANDLES,
            // static_asserted in cap.h.
            if (ncaps > KICKOS_MAX_SPAWN_GRANTS)
            {
                return -KOS_EINVAL; // more grants than one spawn may carry
            }
            uintptr_t const cu = reinterpret_cast<uintptr_t>(p->caps);
            if (p->caps == nullptr or (cu & (alignof(kos_cap_grant) - 1)) != 0)
            {
                return -KOS_EINVAL; // null / misaligned grant array
            }
            // user_readable_ok for the same reason as the params struct above: the array
            // may be a global.
            if (not user_readable_ok(cu, sizeof(kos_cap_grant) * static_cast<size_t>(ncaps)))
            {
                return -KOS_EFAULT; // grant array not readable by the caller
            }
            // Snapshotted in one pass right after the range check and validated from the
            // copy, so no peer core can rewrite p->caps[ci] between check and read. The
            // single-core IrqLock closes that seam today; the copy is what keeps it closed
            // under SMP.
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
                // kaccess_from_user loads privileged, and a misaligned halfword load traps in
                // the kernel on a strict-align arch.
                if ((du & (alignof(uint16_t) - 1)) != 0)
                {
                    return -KOS_EINVAL; // misaligned destination array
                }
                if (not user_readable_ok(du, sizeof(uint16_t) * static_cast<size_t>(ncaps)))
                {
                    return -KOS_EFAULT; // destination array not readable by the caller
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
                    return -KOS_EBADF; // source cap names nothing valid
                }
                if ((se->rights & CAP_TRANSFER) != CAP_TRANSFER)
                {
                    return -KOS_EPERM; // source cap is not delegable (no TRANSFER right)
                }
                uint8_t const mask = g.rights_mask;
                if ((mask & se->rights) != mask) // mask must be a subset: no widening
                {
                    return -KOS_EINVAL; // grant mask widens beyond the source rights
                }
                deleg_obj[ci] = se->obj;
                deleg_type[ci] = se->type;
                deleg_rights[ci] = static_cast<uint8_t>(se->rights & mask);
                // An absent array, or a 0 entry, means default placement. 0 costs no
                // expressiveness as a sentinel: index 0 is the kernel's stdout slot and
                // cap_install_at refuses it anyway.
                unsigned dest = dbuf[ci];
                if (dest == 0u)
                {
                    dest = static_cast<unsigned>(KOS_SPAWN_DELEGATED_CAP0) +
                           static_cast<unsigned>(ci);
                }
                deleg_dest[ci] = static_cast<uint16_t>(dest);
            }
            // No two grants may land on the same slot, defaulted ones included: the second
            // install would silently overwrite the first and leak its reference. Runs
            // before the child exists, so a colliding list costs only a return.
            for (int ci = 1; ci < ncaps; ci++)
            {
                for (int cj = 0; cj < ci; cj++)
                {
                    if (deleg_dest[ci] == deleg_dest[cj])
                    {
                        return -KOS_EINVAL; // two grants named one destination
                    }
                }
            }
        }
        Kernel& k = kernel();
        // Must precede the slot claim, so a domain-pool exhaustion is a clean spawn failure
        // rather than a leaked thread slot. domain_for takes no reference; a domain it
        // creates but nobody references stays at refcount 0, which is a free slot.
        int derr = 0;
        // AUTH_MEMORY, not raw privilege, is the bit covering a spawn-time MMIO grant.
        // Resolved here because domain_for must not read sched::current().
        Domain* const dom = domain_for(p->privileged != 0, p->mem_base, p->mem_size,
                                       p->mmio_base, p->mmio_size,
                                       cap_check_authority(sched::current(), AUTH_MEMORY),
                                       &derr);
        if (dom == nullptr)
        {
            // domain_for already distinguished the refusal, so forward it rather than
            // flattening it: EPERM inadmissible grant, EBUSY DEV window already held,
            // ENOMEM domain pool full.
            return -derr;
        }
        // Reclaiming an EXITED slot is safe on single core: such a thread parked in
        // exit_current until its switch-away committed, so it is off-CPU and off every
        // ready/wait/timer list by the time another thread gets here. current() is RUNNING,
        // never EXITED, so it can never be picked.
        int const i = k.threads.alloc();
        if (i < 0)
        {
            return -KOS_ENOMEM; // thread pool exhausted
        }

        ThreadAttr attr;
        attr.name = "user";
        // EACH source byte is checked caller-readable before the privileged copy
        // dereferences it: the kernel must neither fault on a bad name pointer nor leak
        // another domain's page through it. That also BOUNDS the walk, so a string with no
        // NUL inside a granted region stops at the first unreachable byte. An unreachable
        // first byte drops the name and leaves the "user" default.
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
                    break; // unreachable byte: bound the walk here
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
        attr.domain = dom;
        attr.spawner_tag = k.threads.kill_tag_of(spawner);

        // With no caller stack, reuse a reclaimed block from the free list, else bump a
        // fresh one from the arena. BOTH failing must release the slot just claimed, or the
        // spawn leaks a TCB and burns the prior occupant's join handle.
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
                return -KOS_ENOMEM; // stack arena exhausted
            }
            stack_size = KICKOS_USER_STACK_SIZE;
            attr.kstack_owned = true;
        }
        // Taken BEFORE the reference loop so one unwind path serves both failures. This is
        // -KOS_ENOMEM and not the table-full -KOS_EMFILE: the child gets no table at all,
        // and the slab is a shared supply. Unreachable anyway: KCAP_RUN_COUNT is one run per
        // holder (cap.h), and k.threads.alloc() above already claimed this spawn's slot, whose
        // own run its reclaim arm gave back, so the pool always fills first.
        if (not cap_slab_attach(&attr.cap_run, &attr.cap_free_head))
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
            if (deleg_dest[ci] >= KICKOS_MAX_HANDLES)
            {
                cap_slab_detach(&attr.cap_run, &attr.cap_free_head);
                if (attr.kstack_owned)
                {
                    k.threads.stack_push(stack);
                }
                k.threads.release(i);
                return -KOS_EINVAL; // destination past the child's table
            }
        }

        // EVERY delegated object reference is taken before the child exists. obj_ref_inc is
        // the last fallible step in the spawn (a uint8_t refcount at its ceiling is refused,
        // not wrapped), and at this point the only state to give back is the slot and the
        // demand-allocated stack. Moving this after thread_create would make the unwind
        // additionally owe the domain reference and the child's already-seated caps.
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
            cap_slab_detach(&attr.cap_run, &attr.cap_free_head);
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
        // seated here. The child table is fresh, so a delegated cap i lands at index i+1
        // with handle value i+1.
        Thread* const child = &k.threads.slots[i];
        cap_install_defaults(child);
        cap_seat_authority(child, p->authority);
        for (int ci = 0; ci < ncaps; ci++)
        {
            cap_install_at(child, static_cast<int>(deleg_dest[ci]), deleg_obj[ci],
                           static_cast<CapType>(deleg_type[ci]), deleg_rights[ci]);
        }
        *out_thread = k.threads.handle_for(i);
        return 0;
    }

    // NOT a destroy. This only MARKS the target and wakes it out of the one wait it can be
    // woken from with an error; the target then runs its own exit_current and the existing
    // cap_teardown does the rest. So it is COOPERATIVE: a target that never reaches a
    // cancellation point keeps running, and no caller may assume otherwise.
    //
    // The gate is PARENTHOOD, not an authority bit and not a capability, which makes it
    // NON-TRANSFERABLE: there is no table entry for a cap_grant to copy, so a driver
    // cannot hand its children's lives to a client.
    int thread_kill(kos_thread_t thread)
    {
        IrqLock lock;
        Kernel& k = kernel();
        // NO SIGN TEST. A slot aged past 32768 reclaims mints a handle with bit 31 set, so
        // rejecting "negative" handles would refuse to cancel live threads. KOS_THREAD_NONE
        // and every other malformed word is caught by the index range check below, the pool
        // never seating the all-ones index.
        int const index = static_cast<int>(thread & ((1u << ThreadPool::INDEX_BITS) - 1u));
        uint16_t const gen = static_cast<uint16_t>(thread >> ThreadPool::INDEX_BITS);
        if (index >= k.threads.next)
        {
            return -KOS_EBADF; // names a slot that was never allocated
        }
        Thread* const t = &k.threads.slots[index];
        if (k.threads.gen[index] != gen)
        {
            return -KOS_EBADF; // the slot was reclaimed under this handle
        }
        // The generation bumps at RECLAIM, not at exit, so an exited-but-unreclaimed slot
        // still gen-matches and has to be rejected here.
        if (t->state == ThreadState::EXITED or t->state == ThreadState::INACTIVE)
        {
            return -KOS_EBADF;
        }
        Thread* const c = sched::current();
        if (t == c)
        {
            return -KOS_EINVAL; // ending yourself is kos_exit; this path must return to its caller
        }
        if (c == nullptr or t->spawner_tag == ThreadPool::KILL_TAG_NONE
            or t->spawner_tag != k.threads.kill_tag_of(c))
        {
            return -KOS_EPERM;
        }
        t->cancelled = true; // one-way: honoured at the target's next irq_wait
        if (irq_thread_parked(t))
        {
            // sched::wake neither unlinks nor writes wait_result: both are the waker's job,
            // under this lock, BEFORE the call (see sync.h).
            t->wait_queue->unlink(&t->link);
            t->wait_queue = nullptr;
            t->wait_result = -KOS_ECANCELED;
            sched::wake(t);
        }
        return 0;
    }
}
