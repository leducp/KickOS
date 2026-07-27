// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Thread lifecycle syscall: thread_spawn. Copies+validates the caller's params
// through the mem funnel (syscall_mem.cc), resolves a memory domain, claims a
// pool slot, and installs any spawn-time delegated caps. Split out of
// syscall.cc; the noreturn exit path lives in sched::exit_current (the dispatch
// calls it directly). External linkage; declared in syscall_internal.h.

#include <kickos/arch/arch.h>
#include <kickos/cap.h>
#include <kickos/config.h>
#include <kickos/domain.h>
#include <kickos/grant.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/thread.h>

#include <kickos/sys/abi.h>
#include <kickos/sys/errno.h>

#include "syscall_internal.h"

namespace kickos
{
    // --- Thread pool (slot allocation lives in ThreadPool, thread.h) -----------
    // Reuse is safe because thread_create re-inits the TCB + re-fabricates the arch
    // context from scratch, so a reclaimed slot's privilege posture is a clean reset
    // (the sim's mid-syscall `raised`, the ARM CONTROL.nPRIV, the RX PSW). No syscall
    // resolves a thread by handle yet; when join/kill-by-id adds one it must also
    // reject state==EXITED (the generation bumps at reclaim, not exit -- see ThreadPool).
    int thread_spawn(kos_thread_params const* p)
    {
        IrqLock lock;
        if (p == nullptr)
        {
            return -KOS_EINVAL; // null params
        }
        // Copy the caller's params into kernel memory through a checked read: an
        // unprivileged caller must not hand the kernel a pointer it could not read
        // (a kernel address would otherwise be dereferenced privileged). Read the
        // fields from the kernel-owned copy hereafter. (The name pointer inside is
        // still user memory; it is walked under a per-byte readable check below
        // before the kernel copies it.)
        // user_readable_ok, not the raw user_range_ok: the struct may be a caller
        // stack local OR an app global, and on a backend that models no static-data
        // region -- every no-MPU chip, and the host sim, whose globals live in the
        // host image rather than the arena -- a global lies in no granted region.
        // The arch_user_text_readable arm is what recognises it there; an enforcing
        // backend returns false from that arm and is byte-identical.
        // Reject a misaligned struct pointer BEFORE the typed copy below: the kernel
        // does that load privileged, and a misaligned word load traps in the kernel on
        // a strict-align arch (rv32imac) -- a user-triggerable kernel fault. alignof is
        // the arch-correct requirement, same rationale as the clock_now out-pointer.
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
        // Validate the user-supplied priority: it indexes the ready lists and
        // drives a 1u<<prio bitmap shift, so an out-of-range value is an OOB write / UB.
        // Priority 0 is reserved for the idle thread.
        if (p->prio < KICKOS_PRIO_MIN or p->prio > KICKOS_PRIO_MAX)
        {
            return -KOS_EINVAL; // out-of-range priority
        }
        // No privilege escalation: only a privileged thread may spawn one (a
        // privileged thread is granted the whole arena). The granted domain
        // region's geometry is validated arch-side in arch_mpu_apply.
        if (p->privileged != 0 and not sched::current()->privileged)
        {
            return -KOS_EPERM; // unprivileged caller cannot spawn a privileged child
        }
        // Caller-provided stack (optional): validate BEFORE allocating a slot, so a bad
        // one is a clean spawn error, not a leaked slot / silent overflow. stack_base==0
        // means "use the kernel default". A provided stack must clear the floor and be
        // aligned (base AND size KICKOS_STACK_ALIGN-aligned, so the initial top is aligned).
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
            // An unprivileged thread's stack is granted as one MPU region, so its
            // base must be naturally aligned to the (pow2) region size, else the
            // descriptor is invalid (PMSA/NAPOT snap the base and the enforced
            // window covers the wrong span). kos_ram_alloc hands out exactly such
            // naturally-aligned blocks. (Privileged threads get the whole arena +
            // the background region, so their stack needs no separate descriptor.)
            // Without MPU there is no region descriptor to form, so this does not
            // apply -- matching KOS_STACK_DEFINE, which only natural-aligns a caller
            // stack under enforcement (16-byte ABI alignment otherwise).
#if KICKOS_HAVE_MPU
            // The stack is committed as one R|W MPU region (thread.cc), so [R10]
            // this keys on the CHILD's privilege: a privileged child gets the whole
            // arena + background and needs no stack descriptor.
            if (p->privileged == 0)
            {
                size_t const rsz = arch_ram_region_size(p->stack_size);
                if ((base & (rsz - 1)) != 0)
                {
                    return -KOS_EINVAL; // stack base not naturally aligned to its region size
                }
                // Rule 7: admit the stack region through the same predicate as any
                // grant -- arena-confined for EVERY caller (10C, no privileged waiver)
                // and reserved-block-clear. Refusal is -KOS_EPERM (the code the
                // out-of-arena selftest asserts). Without this an out-of-arena
                // stack_base grants an R|W window over peripheral / kernel SRAM.
                if (not grant_region_admissible(base, rsz, ARCH_MPU_R | ARCH_MPU_W,
                                                sched::current()->privileged))
                {
                    return -KOS_EPERM; // stack outside the arena / hits a reserved block
                }
            }
#endif
        }
        // Data-region grant: the arena-confinement + Rule 7 reserved-block admission
        // for mem_base lives in domain_for (evaluated for EVERY caller on the committed
        // R|W geometry -- 10C), which now reports -KOS_EPERM for it directly, so there is
        // no pre-check here recovering an errno the chokepoint could not express. [R11]
        // Keep only a trivial UNGATED wrap check so a wrapping mem_base is a clean
        // -KOS_EINVAL on every board, including no-MPU parts where domain_for's predicate
        // is a no-op stub and would not catch it. (No-MPU boundary change: the old ungated
        // arena bound on no-MPU parts is dropped -- there is no MPU region to escalate
        // through there.)
        if (p->mem_base != nullptr and p->mem_size != 0)
        {
            uintptr_t const dbase = reinterpret_cast<uintptr_t>(p->mem_base);
            if (dbase + p->mem_size < dbase)
            {
                return -KOS_EINVAL; // mem_base window wraps the address space
            }
        }
        // MMIO grant (optional): a device register window handed to an unprivileged
        // driver. This is the PRECISE-ERROR boundary -- privileged-only (EPERM) and
        // exact-shape (EINVAL: zero-size, wrap, non-encodable), the codes the selftest
        // asserts. The AUTHORITATIVE Rule 7 admission (reserved-block overlap, bit-band
        // alias) is domain_for's grant_region_admissible on the same window; keeping
        // this thin gate here preserves the specific errno a malformed request earns,
        // which domain_for's single nullptr sentinel cannot express.
        if (p->mmio_base != nullptr)
        {
            if (not cap_check_authority(sched::current(), AUTH_MEMORY))
            {
                return -KOS_EPERM; // MMIO needs AUTH_MEMORY -- never self-grantable
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
        // Authority cap (optional): the child's seat at KOS_CAP_AUTHORITY. Validated here,
        // with the other boundary checks, so a bad request is a clean spawn refusal rather
        // than a half-built child.
        if (p->authority != 0)
        {
            if ((p->authority & ~CAP_AUTH_ALL) != 0)
            {
                // Object rights (WAIT/SIGNAL/TRANSFER) mean nothing on this type. Refuse
                // rather than mask: a caller passing them has misunderstood something, and
                // silently dropping the bits would hide which.
                return -KOS_EINVAL; // non-authority bits in the authority mask
            }
            // Narrow-only, exactly like a cap_grant mask: the caller must already hold
            // every bit it hands on. cap_check_authority IS that question, and it answers
            // true wholesale for a privileged caller -- which is what lets today's
            // privileged root seat a narrowed authority on a child before any board flips.
            if (not cap_check_authority(sched::current(), p->authority))
            {
                return -KOS_EPERM; // cannot grant an authority the caller does not hold
            }
            // Delegated cap i lands at child index i+1, so cap_count >= 2 puts a delegated
            // cap on the authority slot. Refuse the pair instead of letting one silently
            // overwrite the other; per-grant destination indices are the deferred fix (see
            // <kickos/sys/cap_index.h>).
            if (static_cast<int>(p->cap_count) >= KOS_CAP_AUTHORITY)
            {
                return -KOS_EINVAL; // delegation packing would collide with the authority slot
            }
        }
        // Spawn-time capability delegation (M3). Validate the WHOLE list BEFORE
        // claiming anything (finding 9): every source cap must resolve in the
        // CALLER's table, carry CAP_TRANSFER, and the mask must be a subset (narrow
        // only, never widen); cap_count + 1 must fit the child table (index 0
        // reserved). Only after every check passes do we install + bump refs, so a
        // mid-install failure -- which validation makes impossible -- cannot leave a
        // half-populated child with dangling ref bumps.
        int deleg_obj[KICKOS_MAX_HANDLES];
        uint8_t deleg_type[KICKOS_MAX_HANDLES];
        uint8_t deleg_rights[KICKOS_MAX_HANDLES];
        int const ncaps = static_cast<int>(p->cap_count);
        if (ncaps > 0)
        {
            // Delegated cap i lands at child index i+1 (index 0 reserved for stdout), so
            // at most KICKOS_MAX_HANDLES-1 are delegable. This i+1 packing is UNCHANGED
            // by the frozen cap-index range; an explicit per-grant destination index is
            // deferred (see cap_index.h), so delegation still fills from index 1.
            if (ncaps >= KICKOS_MAX_HANDLES)
            {
                return -KOS_EINVAL; // cap_count too big for the child table
            }
            uintptr_t const cu = reinterpret_cast<uintptr_t>(p->caps);
            if (p->caps == nullptr or (cu & (alignof(kos_cap_grant) - 1)) != 0)
            {
                return -KOS_EINVAL; // null / misaligned grant array
            }
            // user_readable_ok for the same reason as the params struct above: a
            // caller may perfectly well keep its grant array in a global.
            if (not user_readable_ok(cu, sizeof(kos_cap_grant) * static_cast<size_t>(ncaps)))
            {
                return -KOS_EFAULT; // grant array not readable by the caller
            }
            // Snapshot the whole grant array into kernel memory in one pass right after
            // the range check, then validate from the copy -- so a future SMP kernel
            // cannot let a peer core rewrite p->caps[ci] between check and read
            // (the classic double-fetch seam; single-core IrqLock closes it today).
            kos_cap_grant gbuf[KICKOS_MAX_HANDLES];
            for (int ci = 0; ci < ncaps; ci++)
            {
                kaccess_from_user(&gbuf[ci],
                                  cu + static_cast<size_t>(ci) * sizeof(kos_cap_grant),
                                  sizeof(kos_cap_grant));
            }
            Thread* const caller = sched::current();
            for (int ci = 0; ci < ncaps; ci++)
            {
                kos_cap_grant const g = gbuf[ci];
                CapEntry* se = cap_lookup(caller, g.source_cap);
                if (se == nullptr)
                {
                    return -KOS_EBADF; // source cap names nothing valid
                }
                if ((se->rights & CAP_TRANSFER) != CAP_TRANSFER)
                {
                    return -KOS_EPERM; // source cap is not delegable (no TRANSFER right)
                }
                uint8_t const mask = g.rights_mask;
                if ((mask & se->rights) != mask) // mask must be a subset -- no widening
                {
                    return -KOS_EINVAL; // grant mask widens beyond the source rights
                }
                deleg_obj[ci] = se->obj;
                deleg_type[ci] = se->type;
                deleg_rights[ci] = static_cast<uint8_t>(se->rights & mask);
            }
        }
        Kernel& k = kernel();
        // Resolve the memory domain BEFORE claiming a slot, so a domain-pool
        // exhaustion is a clean spawn failure, not a leaked thread slot. domain_for
        // does not take a reference (thread_create does); a domain it creates but
        // we never reference stays refcount 0 == a free slot.
        int derr = 0;
        Domain* const dom = domain_for(p->privileged != 0, p->mem_base, p->mem_size,
                                       p->mmio_base, p->mmio_size,
                                       sched::current()->privileged, &derr);
        if (dom == nullptr)
        {
            // domain_for says WHICH refusal this is, so forward it verbatim: EPERM for
            // an inadmissible grant (fix the grant), ENOMEM for a full domain pool
            // (retry later). Collapsing both into ENOMEM used to make those two
            // indistinguishable to a caller that can only act on one of them.
            return -derr;
        }
        // Reclaim an EXITED slot or bump-allocate (ThreadPool::alloc). Single-core: an
        // EXITED thread is guaranteed off-CPU by the time any other thread reaches here
        // -- it parked in exit_current until its switch-away committed -- and is off
        // every ready/wait/timer list, so reinit is safe. current() is RUNNING, never
        // EXITED, so it is never picked.
        int const i = k.threads.alloc();
        if (i < 0)
        {
            return -KOS_ENOMEM; // thread pool exhausted
        }

        ThreadAttr attr;
        attr.name = "user";
        // Copy the caller's name into kernel memory, checking EACH source byte is
        // caller-readable before the privileged copy dereferences it -- the kernel
        // must not fault (DoS) on, or leak another domain's page through, a bad name
        // pointer. This BOUNDS the walk: a string not NUL-terminated within a
        // granted region stops at the first unreachable byte. If the very first byte
        // is unreachable the name is dropped (default "user" stands). A privileged
        // caller passes user_readable_ok wholesale, so its name copies as before.
        char namebuf[KICKOS_THREAD_NAME_MAX]; // matches Thread::name_buf; thread_create re-clamps anyway
        if (p->name != nullptr)
        {
            uintptr_t const np = reinterpret_cast<uintptr_t>(p->name);
            size_t ni = 0;
            bool name_ok = false;
            for (; ni + 1 < sizeof(namebuf); ni++)
            {
                if (not user_readable_ok(np + ni, 1))
                {
                    break; // unreachable byte -- bound the walk here
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

        // Caller's stack if given (a thread's stack is a userspace concern), else a
        // kernel-default stack, demand-allocated: reuse a reclaimed block from the
        // free list, else bump a fresh one from the arena (naturally aligned into a
        // valid MPU region). BOTH failing (arena exhausted) is a clean spawn failure
        // -- release the slot we just claimed so we neither leak a TCB nor burn the
        // prior occupant's join handle.
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
        thread_create(&k.threads.slots[i], p->entry, p->arg, stack, stack_size, attr);
        // The child table is fresh (thread_create zeroed it to CAP_EMPTY, cap-gen 0),
        // so the reserved default set is a no-op and each delegated cap i lands at
        // index i+1 with handle value i+1 (B1). Validation above guarantees this
        // install cannot fail; bump each named object's refcount as its new cap lands.
        Thread* const child = &k.threads.slots[i];
        cap_install_defaults(child);
        // Before the delegation loop: the guard above already refused any cap_count that
        // could reach index 2, so the two cannot fight over the slot in either order.
        cap_seat_authority(child, p->authority);
        for (int ci = 0; ci < ncaps; ci++)
        {
            cap_install_at(child, ci + 1, deleg_obj[ci],
                           static_cast<CapType>(deleg_type[ci]), deleg_rights[ci]);
            obj_ref_inc(static_cast<CapType>(deleg_type[ci]), deleg_obj[ci], deleg_rights[ci]);
        }
        return k.threads.handle_for(i);
    }
}
