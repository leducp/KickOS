// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The arch-independent syscall table + dispatch. The arch entry (sim
// trampoline / ARM SVC handler) reads the number + args and calls
// syscall_dispatch(); the result is delivered back to the caller frame.
// Kernel objects addressable from userspace (semaphores, threads) live in
// static pools referenced by small integer handles -- no pointers cross the
// boundary.

#include <kickos/arch/arch.h>
#include <kickos/cap.h>
#include <kickos/config.h>
#include <kickos/domain.h>
#include <kickos/grant.h>
#include <kickos/instance.h>
#include <kickos/sched.h>
#include <kickos/sync.h>
#include <kickos/time.h>
#include <kickos/kernel.h>
#include <kickos/irq.h>
#include <kickos/irqlock.h>
#include <kickos/ktrace.h>
#include <kickos/console_tx.h>

#include <kickos/sys/abi.h>

namespace kickos
{
    namespace
    {
        // B1 backstop: max yield passes kos_console_publish waits for the in-flight
        // chip-writer count to reach 0 before declaring a stuck writer (a real bug).
        constexpr uint32_t CONSOLE_PUBLISH_DRAIN_MAX = KICKOS_POLL_SPIN_MAX;

        // Endpoint badge delivered to a receiver. Stage i has no per-endpoint badge
        // scheme yet, so every message carries this. Distinct from the "no out-ptr"
        // markers (ipc.badge_out == 0), which mean the receiver asked for no badge.
        constexpr uint32_t KOS_BADGE_NONE = 0;

        // --- Semaphore capabilities (per-task cap table over the global pool) -------
        // A sem lives in the global generational pool (slotpool.h); a task names it by
        // a per-task CAP_SEM capability (cap.h). cap_resolve is the single validate-and-
        // resolve chokepoint (per-task cap-gen guard, then the pool's object-gen guard).
        // sem_wait needs CAP_WAIT, sem_post needs CAP_SIGNAL.

        int sem_create(int initial)
        {
            IrqLock lock;
            Thread* c = sched::current();
            if (c == nullptr)
            {
                return -KOS_EPERM; // no caller context (defensive; unreachable from a real syscall)
            }
            int const i = kernel().sems.alloc();
            if (i < 0)
            {
                return -KOS_ENOMEM; // sem pool exhausted
            }
            sem_init(kernel().sems.at(i), initial);
            kernel().sem_refs[i] = 1; // this creator's cap is the first reference
            int const obj = kernel().sems.handle_for(i);
            // Install the owning cap with full rights (WAIT|SIGNAL|TRANSFER) in the
            // creator's table; the returned CAP handle is what userspace sees. A full
            // table is a clean failure: release the just-claimed sem (refs -> 0).
            int const cap = cap_install(c, obj, CapType::CAP_SEM,
                                        CAP_WAIT | CAP_SIGNAL | CAP_TRANSFER);
            if (cap < 0)
            {
                kernel().sem_refs[i] = 0;
                kernel().sems.free(obj);
                return -KOS_ENOMEM; // cap table full
            }
            return cap;
        }

        // --- PI-mutex capability (mirrors sem_create) ------------------------------
        // A mutex lives in the global pool (slotpool.h); a task names it by a per-task
        // CAP_MUTEX capability. Possession IS the lock/unlock authority (no WAIT/SIGNAL
        // split), so the creator cap carries CAP_TRANSFER only and lock/unlock resolve
        // with need == 0. Rollback on a full table mirrors sem_create.
        int mutex_create()
        {
            IrqLock lock;
            Thread* c = sched::current();
            if (c == nullptr)
            {
                return -KOS_EPERM; // no caller context (defensive)
            }
            int const i = kernel().mutexes.alloc();
            if (i < 0)
            {
                return -KOS_ENOMEM; // mutex pool exhausted
            }
            mutex_init(kernel().mutexes.at(i));
            kernel().mutex_refs[i] = 1;
            int const obj = kernel().mutexes.handle_for(i);
            int const cap = cap_install(c, obj, CapType::CAP_MUTEX, CAP_TRANSFER);
            if (cap < 0)
            {
                kernel().mutex_refs[i] = 0;
                kernel().mutexes.free(obj);
                return -KOS_ENOMEM; // cap table full
            }
            return cap;
        }

        // --- Endpoint capability (IPC rendezvous; mirrors sem_create) ---------------
        // An endpoint lives in the global pool (slotpool.h); a task names it by a
        // per-task CAP_ENDPOINT capability. The creator cap carries full rights
        // (WAIT|SIGNAL|TRANSFER); send needs CAP_SIGNAL, recv needs CAP_WAIT. Two
        // counters init visibly paired: endpoint_refs (all caps) and recv_holders
        // (WAIT-bearing caps). Rollback on a full table unwinds BOTH.
        int endpoint_create()
        {
            IrqLock lock;
            Thread* c = sched::current();
            if (c == nullptr)
            {
                return -KOS_EPERM; // no caller context (defensive)
            }
            int const i = kernel().endpoints.alloc();
            if (i < 0)
            {
                return -KOS_ENOMEM; // endpoint pool exhausted
            }
            Endpoint* ep = kernel().endpoints.at(i);
            ep->send_waiters = List{};
            ep->recv_waiters = List{};
            ep->recv_holders = 1;         // creator holds a WAIT-bearing cap
            kernel().endpoint_refs[i] = 1; // this creator's cap is the first reference
            int const obj = kernel().endpoints.handle_for(i);
            int const cap = cap_install(c, obj, CapType::CAP_ENDPOINT,
                                        CAP_WAIT | CAP_SIGNAL | CAP_TRANSFER);
            if (cap < 0)
            {
                kernel().endpoint_refs[i] = 0;
                kernel().endpoints.free(obj);
                return -KOS_ENOMEM; // cap table full
            }
            return cap;
        }

        // Privileged in-kernel IRQ handler bound by KOS_SYS_irq_attach: posts a
        // semaphore from ISR context, driving the interrupt-exit switch (trigger #4).
        // arg is the GLOBAL sem handle irq_attach resolved+stored (NOT a cap): an ISR
        // must never resolve a cap (current() is a random interrupted thread's table).
        void irq_sem_post(void* arg)
        {
            int handle = static_cast<int>(reinterpret_cast<intptr_t>(arg));
            Semaphore* s = kernel().sems.resolve(handle);
            if (s != nullptr)
            {
                sem_post(s);
            }
        }

        // --- Thread pool (slot allocation lives in ThreadPool, thread.h) -----------
        // Reuse is safe because thread_create re-inits the TCB + re-fabricates the arch
        // context from scratch, so a reclaimed slot's privilege posture is a clean reset
        // (the sim's mid-syscall `raised`, the ARM CONTROL.nPRIV, the RX PSW). No syscall
        // resolves a thread by handle yet; when join/kill-by-id adds one it must also
        // reject state==EXITED (the generation bumps at reclaim, not exit -- see ThreadPool).

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

        // Synchronous send: rendezvous with a parked receiver (deliver now) or park.
        // FULLY LOCKLESS from dispatch (see design section 3): a caller IrqLock spanning
        // this would keep BASEPRI raised across wq_confirm_resume and livelock ARM.
        // Returns bytes transferred (>= 0), or -KOS_E* (EINVAL oversize, EFAULT bad buffer,
        // EBADF/EPERM bad cap or missing SIGNAL, EPIPE dead endpoint / last receiver left).
        int endpoint_send(int cap, uintptr_t buf, size_t len)
        {
            if (len > KOS_EP_MSG_MAX)
            {
                return -KOS_EINVAL; // F4: reject oversize, never silently clamp
            }
            if (not user_readable_ok(buf, len))
            {
                return -KOS_EFAULT; // sender's own buffer, checked once in caller context
            }
            Thread* c = sched::current();
            if (c == nullptr)
            {
                return -KOS_EPERM; // no caller context (defensive)
            }
            uint64_t epoch = 0;
            {
                IrqLock lock;
                int err = 0;
                Endpoint* e = static_cast<Endpoint*>(
                    cap_resolve_e(c, cap, CapType::CAP_ENDPOINT, CAP_SIGNAL, &err));
                if (e == nullptr)
                {
                    return -err; // EBADF (bad cap) or EPERM (no SIGNAL right)
                }
                if (e->recv_holders == 0)
                {
                    return -KOS_EPIPE; // F1: dead endpoint -- no receiver can ever exist
                }
                Thread* w = wq_pop_highest(e->recv_waiters);
                if (w != nullptr)
                {
                    size_t n = len;
                    if (w->ipc.len < n)
                    {
                        n = w->ipc.len; // receiver-side datagram truncation (not an error)
                    }
                    ep_copy(w->ipc.buf, buf, n); // sender-ctx copy into the parked receiver's buffer
                    if (w->ipc.badge_out != 0)
                    {
                        uint32_t const badge = KOS_BADGE_NONE;
                        kaccess_to_user(w->ipc.badge_out, &badge, sizeof(badge));
                    }
                    w->wait_result = static_cast<intptr_t>(n);
                    sched::wake(w);
                    return static_cast<int>(n); // did not block: no resume barrier
                }
                c->ipc.buf = buf;
                c->ipc.len = len;
                c->ipc.badge_out = 0;
                epoch = c->switch_count;
                wq_block(e->send_waiters);
            }
            wq_confirm_resume(c, epoch);
            return static_cast<int>(c->wait_result); // n (>= 0), or -KOS_EPIPE (last receiver left)
        }

        // Synchronous recv: take from a parked sender (copy now) or park. FULLY LOCKLESS
        // from dispatch, same reason as endpoint_send. Returns bytes received (>= 0), or
        // -KOS_E* (EFAULT bad buffer/out-ptr, EINVAL misaligned badge, EBADF/EPERM bad cap or
        // missing WAIT). n == 0 is a VALID zero-length signal, not an error.
        int endpoint_recv(int cap, uintptr_t buf, size_t cap_len, uintptr_t badge_out)
        {
            if (cap_len > KOS_EP_MSG_MAX)
            {
                cap_len = KOS_EP_MSG_MAX; // capacity clamp is harmless
            }
            if (not user_writable_ok(buf, cap_len))
            {
                return -KOS_EFAULT;
            }
            if (badge_out != 0
                and ((badge_out & (alignof(uint32_t) - 1)) != 0
                     or not user_writable_ok(badge_out, sizeof(uint32_t))))
            {
                // Misalignment is a malformed arg (EINVAL); an unowned out-ptr is EFAULT.
                if ((badge_out & (alignof(uint32_t) - 1)) != 0)
                {
                    return -KOS_EINVAL; // alignment load-bearing for the privileged u32 store below
                }
                return -KOS_EFAULT;
            }
            Thread* c = sched::current();
            if (c == nullptr)
            {
                return -KOS_EPERM; // no caller context (defensive)
            }
            uint64_t epoch = 0;
            {
                IrqLock lock;
                int err = 0;
                Endpoint* e = static_cast<Endpoint*>(
                    cap_resolve_e(c, cap, CapType::CAP_ENDPOINT, CAP_WAIT, &err));
                if (e == nullptr)
                {
                    return -err; // EBADF (bad cap) or EPERM (no WAIT right)
                }
                Thread* s = wq_pop_highest(e->send_waiters);
                if (s != nullptr)
                {
                    size_t n = s->ipc.len;
                    if (cap_len < n)
                    {
                        n = cap_len; // truncate into the receiver's capacity
                    }
                    ep_copy(buf, s->ipc.buf, n); // receiver-ctx copy from the parked sender's buffer
                    if (badge_out != 0)
                    {
                        uint32_t const badge = KOS_BADGE_NONE;
                        kaccess_to_user(badge_out, &badge, sizeof(badge));
                    }
                    s->wait_result = static_cast<intptr_t>(n);
                    sched::wake(s);
                    return static_cast<int>(n);
                }
                c->ipc.buf = buf;
                c->ipc.len = cap_len;
                c->ipc.badge_out = badge_out;
                epoch = c->switch_count;
                wq_block(e->recv_waiters);
            }
            wq_confirm_resume(c, epoch);
            return static_cast<int>(c->wait_result);
        }

        int thread_spawn(kos_thread_params const* p)
        {
            IrqLock lock;
            if (p == nullptr)
            {
                return -KOS_EINVAL; // null params
            }
            // Copy the caller's params into kernel memory through a checked read: an
            // unprivileged caller must not hand the kernel a pointer it could not read
            // (a kernel address would otherwise be dereferenced privileged). The struct
            // is a caller stack local (kos::thread::spawn), so it lies in the stack
            // region. Read the fields from the kernel-owned copy hereafter. (The name
            // pointer inside is still user memory; it is walked under a per-byte
            // readable check below before the kernel copies it.)
            // Reject a misaligned struct pointer BEFORE the typed copy below: the kernel
            // does that load privileged, and a misaligned word load traps in the kernel on
            // a strict-align arch (rv32imac) -- a user-triggerable kernel fault. alignof is
            // the arch-correct requirement, same rationale as the clock_now out-pointer.
            uintptr_t const pu = reinterpret_cast<uintptr_t>(p);
            if ((pu & (alignof(kos_thread_params) - 1)) != 0)
            {
                return -KOS_EINVAL; // misaligned params struct
            }
            if (not user_range_ok(pu, sizeof(*p), ARCH_MPU_R))
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
            // for mem_base now lives in domain_for (evaluated for EVERY caller on the
            // committed R|W geometry -- 10C). [R11] Keep only a trivial UNGATED wrap check
            // here so a wrapping mem_base is a clean -KOS_EINVAL on every board, including
            // no-MPU parts where domain_for's predicate is a no-op stub and would not
            // catch it. (No-MPU boundary change: the old ungated arena bound on no-MPU
            // parts is dropped -- there is no MPU region to escalate through there.)
            if (p->mem_base != nullptr and p->mem_size != 0)
            {
                uintptr_t const dbase = reinterpret_cast<uintptr_t>(p->mem_base);
                if (dbase + p->mem_size < dbase)
                {
                    return -KOS_EINVAL; // mem_base window wraps the address space
                }
#if KICKOS_HAVE_MPU
                // Errno coherence with the stack_base path above: a Rule-7 / out-of-arena
                // data grant is a POLICY refusal (-KOS_EPERM), not pool exhaustion. domain_for
                // stays the authoritative chokepoint (it re-checks the same predicate), but its
                // single nullptr sentinel collapses to -KOS_ENOMEM below and cannot distinguish
                // "retry later" from "never". Pre-check it here on the SAME committed geometry
                // (arch_ram_region_size) so an unprivileged child's inadmissible mem_base earns
                // -KOS_EPERM; a genuine domain-pool exhaustion still falls through to -KOS_ENOMEM.
                // A privileged child bypasses domain_for's grant check (it gets the whole arena),
                // so gate on the CHILD's privilege, exactly as domain_for does.
                if (p->privileged == 0)
                {
                    if (not grant_region_admissible(dbase, arch_ram_region_size(p->mem_size),
                                                    ARCH_MPU_R | ARCH_MPU_W,
                                                    sched::current()->privileged))
                    {
                        return -KOS_EPERM; // data grant outside the arena / hits a reserved block
                    }
                }
#endif
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
                if (not sched::current()->privileged)
                {
                    return -KOS_EPERM; // MMIO is privileged-only -- never self-grantable
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
                if (not user_range_ok(cu, sizeof(kos_cap_grant) * static_cast<size_t>(ncaps),
                                      ARCH_MPU_R))
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
            Domain* const dom = domain_for(p->privileged != 0, p->mem_base, p->mem_size,
                                           p->mmio_base, p->mmio_size,
                                           sched::current()->privileged);
            if (dom == nullptr)
            {
                // nullptr is domain_for's single refusal sentinel: EITHER the domain
                // pool is exhausted OR the grant is inadmissible (Rule 7 reserved-block
                // hit, out-of-arena data, or a misaligned/malformed region). The precise
                // MMIO shape/privilege errors were already returned at the boundary above;
                // everything else collapses to ENOMEM here (the errno is coarse, but the
                // spawn correctly fails either way).
                return -KOS_ENOMEM;
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
            for (int ci = 0; ci < ncaps; ci++)
            {
                cap_install_at(child, ci + 1, deleg_obj[ci],
                               static_cast<CapType>(deleg_type[ci]), deleg_rights[ci]);
                obj_ref_inc(static_cast<CapType>(deleg_type[ci]), deleg_obj[ci], deleg_rights[ci]);
            }
            return k.threads.handle_for(i);
        }

    }
}

using namespace kickos;

#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
namespace
{
    uint16_t syscall_tid()
    {
        Thread* c = sched::current();
        if (c == nullptr)
        {
            return static_cast<uint16_t>(trace::TRACE_NO_THREAD);
        }
        return c->id;
    }

    // RAII SYSCALL_ENTER/EXIT bracket. The EXIT fires in the destructor on EVERY
    // ordinary return path -- but KOS_SYS_exit switches away permanently inside the
    // dispatch (never returns to this frame), so its destructor never runs and it
    // is recorded as ENTER-only (the decoder handles the missing EXIT).
    struct SyscallTrace
    {
        uint16_t tid;
        uint16_t nr;
        SyscallTrace(uint16_t t, uint16_t n) : tid(t), nr(n)
        {
            ktrace_syscall_enter(tid, nr);
        }
        ~SyscallTrace()
        {
            ktrace_syscall_exit(tid, nr);
        }
    };
}
#define KTRACE_SYSCALL_SCOPE(nr) SyscallTrace _kt_syscall(syscall_tid(), static_cast<uint16_t>(nr))
#else
#define KTRACE_SYSCALL_SCOPE(nr) do { } while (0)
#endif

extern "C" uintptr_t syscall_dispatch(uintptr_t nr,
                                      uintptr_t a0, uintptr_t a1,
                                      uintptr_t a2, uintptr_t a3)
{
    KTRACE_SYSCALL_SCOPE(nr);
    switch (nr)
    {
        case KOS_SYS_kconsole_write:
        {
            // Explicit (buf, len): the kernel must never strlen a user pointer.
            // Clamp len (a garbage/huge value must not walk off RAM or hog the UART),
            // then bound buf against the caller's memory so an unprivileged thread
            // cannot launder another domain's arena page out through the console
            // (the kernel reads buf privileged). Reject => wrote nothing.
            constexpr size_t kMaxConsoleWrite = 4096;
            // MMU-era NOTE: this hands a user pointer straight to kconsole_write, which
            // streams it privileged -- the one kernel-side user read NOT funnelled
            // through kaccess_from_user. Funnelling it needs a bounce buffer + chunk
            // loop (a real design choice: buffer size vs kernel-stack budget), not an
            // identity refactor, so it is deferred to the copy_from_user work.
            char const* buf = reinterpret_cast<char const*>(a0);
            size_t len = static_cast<size_t>(a1);
            if (len > kMaxConsoleWrite)
            {
                len = kMaxConsoleWrite;
            }
            if (not user_readable_ok(a0, len))
            {
                // Cross-domain / bad buffer -- write nothing. Negative code, NOT 0: a
                // len-0 write legitimately returns 0, so 0 must not double as reject.
                return static_cast<uintptr_t>(-KOS_EFAULT);
            }
            kconsole_write(buf, len); // fan-out (chip + RTT), not the raw transport
            return len;
        }
        case KOS_SYS_yield:
        {
            sched::yield();
            return 0;
        }
        case KOS_SYS_sleep_ns:
        {
            ktime_sleep_ns(kos_u64_join(static_cast<uint32_t>(a0),
                                        static_cast<uint32_t>(a1)));
            return 0;
        }
        case KOS_SYS_sem_create:
        {
            return static_cast<uintptr_t>(sem_create(static_cast<int>(a0)));
        }
        case KOS_SYS_handle_close:
        {
            // Type-agnostic close: drop THIS task's cap (a cap knows its own type).
            // Refcounted -- the object is freed only at the last close.
            IrqLock lock;
            return static_cast<uintptr_t>(handle_close(sched::current(), static_cast<int>(a0)));
        }
        case KOS_SYS_sem_wait:
        {
            // Resolve and use under one lock (sem_wait/sem_post nest their own):
            // otherwise a concurrent close could free the slot between resolve and use.
            IrqLock lock;
            int err = 0;
            Semaphore* s = static_cast<Semaphore*>(
                cap_resolve_e(sched::current(), static_cast<int>(a0), CapType::CAP_SEM, CAP_WAIT, &err));
            if (s == nullptr)
            {
                return static_cast<uintptr_t>(-err); // EBADF (bad/closed cap) or EPERM (no WAIT right)
            }
            sem_wait(s);
            return 0;
        }
        case KOS_SYS_sem_post:
        {
            IrqLock lock;
            int err = 0;
            Semaphore* s = static_cast<Semaphore*>(
                cap_resolve_e(sched::current(), static_cast<int>(a0), CapType::CAP_SEM, CAP_SIGNAL, &err));
            if (s == nullptr)
            {
                return static_cast<uintptr_t>(-err); // EBADF (bad/closed cap) or EPERM (no SIGNAL right)
            }
            sem_post(s);
            return 0;
        }
        case KOS_SYS_mutex_create:
        {
            return static_cast<uintptr_t>(mutex_create());
        }
        case KOS_SYS_mutex_lock:
        {
            // Resolve under a short lock; mutex_lock then takes its OWN lock for the
            // acquire/park and (critically) releases it before the resume barrier +
            // wait_result read, so the ARM deferred-PendSV block completes first (a
            // continuous lock across mutex_lock would reintroduce the stale read).
            // The resolve->call window needs no lock: the caller's own cap pins the
            // mutex (mutex_refs >= 1), and there is no cross-thread close/kill path
            // that could free it, so the resolved pointer stays valid. need == 0:
            // possession is the authority.
            Mutex* m;
            int err = 0;
            {
                IrqLock lock;
                m = static_cast<Mutex*>(
                    cap_resolve_e(sched::current(), static_cast<int>(a0), CapType::CAP_MUTEX, 0, &err));
            }
            if (m == nullptr)
            {
                return static_cast<uintptr_t>(-err); // -KOS_EBADF (need == 0, so never EPERM here)
            }
            // 0 / -KOS_EOWNERDEAD (HELD, owner died) / -KOS_EDEADLK (NOT held). EOWNERDEAD is
            // negative but still an ACQUIRE -- the wrapper decl documents the held-vs-not caveat.
            return static_cast<uintptr_t>(mutex_lock(m));
        }
        case KOS_SYS_mutex_unlock:
        {
            IrqLock lock;
            int err = 0;
            Mutex* m = static_cast<Mutex*>(
                cap_resolve_e(sched::current(), static_cast<int>(a0), CapType::CAP_MUTEX, 0, &err));
            if (m == nullptr)
            {
                return static_cast<uintptr_t>(-err); // -KOS_EBADF (bad cap)
            }
            return static_cast<uintptr_t>(mutex_unlock(m)); // 0, or -KOS_EPERM if not owner (no panic)
        }
        case KOS_SYS_endpoint_create:
        {
            return static_cast<uintptr_t>(endpoint_create());
        }
        case KOS_SYS_send:
        {
            // FULLY LOCKLESS (no dispatch IrqLock): endpoint_send takes its own lock for
            // the resolve/deliver/park, then releases it before the resume barrier -- a
            // spanning caller lock would livelock ARM (design section 3).
            return static_cast<uintptr_t>(
                endpoint_send(static_cast<int>(a0), a1, static_cast<size_t>(a2)));
        }
        case KOS_SYS_recv:
        {
            return static_cast<uintptr_t>(
                endpoint_recv(static_cast<int>(a0), a1, static_cast<size_t>(a2), a3));
        }
        case KOS_SYS_console_publish:
        {
            // Hand the console UART to a userspace driver named by an endpoint cap.
            // Privileged-only (like ram_alloc / MMIO grant): it disables a live IRQ line
            // and mutates global console routing. See the handover design (D3).
            Thread* c = sched::current();
            if (c == nullptr or not c->privileged)
            {
                return static_cast<uintptr_t>(-KOS_EPERM); // privileged-only
            }
            int handle = -1;
            {
                IrqLock lock;
                // Resolve the endpoint cap to its GLOBAL gen-encoded handle -- NOT the pool
                // index (S3). cap_lookup validates the cap-gen; re-check type + object
                // liveness (mirrors irq_attach's resolve-once pattern). Any rights: the
                // publish is identity-only.
                CapEntry* e = cap_lookup(c, static_cast<int>(a0));
                if (e == nullptr or e->type != static_cast<uint8_t>(CapType::CAP_ENDPOINT)
                    or kernel().endpoints.resolve(e->obj) == nullptr)
                {
                    return static_cast<uintptr_t>(-KOS_EBADF); // bad / non-endpoint / stale cap
                }
                handle = e->obj;
                if (console_owner_is_kernel() != 0)
                {
                    console_tx_deinit(); // D2 relinquish (idempotent; skipped on re-publish)
                }
                cap_console_publish(handle); // take the kernel stdout ref, drop any prior target
                console_owner_set_user();    // flip to USER_OWNED -- LAST
            }
            // B1: drain any stale chip writer that raced past the pre-flip state read, with
            // the lock RELEASED, before returning. After the flip no path increments the
            // count, so it strictly drains. Root spawns the driver only after this returns,
            // so the preempted writer is off the device before the driver touches it.
            //
            // The scheduler is STRICT PRIORITY, so a bare busy-spin here would LIVELOCK: an
            // in-flight writer preempted mid arch_console_write_sync (a polled loop run
            // WITHOUT IrqLock) can only finish once rescheduled, and it may be LOWER priority
            // than this publisher (root, typically high). Drop to the minimum real priority
            // and yield each pass so that lower-prio writer runs to completion. Safe to
            // drain-to-zero because the state is already USER_OWNED: no path increments the
            // count anymore, and an in-flight writer never blocks between enter and leave (a
            // polled write), so a non-zero count always means a RUNNABLE writer exists.
            Thread* pub = sched::current();
            uint8_t const saved_prio = pub->prio;
            sched::set_prio(pub, KICKOS_PRIO_MIN);
            // Generous bounded backstop: each pass is a full scheduler round and a poke is a
            // handful of polled bytes, so a count that never drains is a real bug. Fail LOUD
            // rather than hang silently or (worse) proceed while a writer still pokes the UART.
            uint32_t guard = 0;
            while (console_chip_writers() != 0)
            {
                sched::yield();
                guard = guard + 1;
                if (guard >= CONSOLE_PUBLISH_DRAIN_MAX)
                {
                    kpanic("console_publish: chip-writer drain did not converge");
                }
            }
            sched::set_prio(pub, saved_prio);
            return 0;
        }
        case KOS_SYS_cpu_clock_set:
        {
            // Privileged-only (like console_publish / ram_alloc): it mutates
            // SystemCoreClock, retimes every thread's SysTick basis, and moves the
            // shared console baud -- an unprivileged retune could DoS every task's
            // timing. Return 0 (== the cannot-change sentinel) on the unprivileged
            // path so the caller needs only ONE error test. The coherence sequence
            // (mask / disarm / flush / retune / re-arm) lives in cpu_clock_set.
            // NOTE: this syscall stays OUT of the -KOS_E* scheme -- it returns a u32 Hz
            // whose 0 sentinel already means cannot/unsupported/not-privileged, and the
            // console-owned refusal (an EBUSY-shaped condition) surfaces as "unchanged Hz".
            Thread* c = sched::current();
            if (c == nullptr or not c->privileged)
            {
                return 0;
            }
            return static_cast<uintptr_t>(
                cpu_clock_set(static_cast<kos_pstate_t>(a0)));
        }
        case KOS_SYS_thread_spawn:
        {
            return static_cast<uintptr_t>(
                thread_spawn(reinterpret_cast<kos_thread_params const*>(a0)));
        }
        case KOS_SYS_exit:
        {
            sched::exit_current(static_cast<int>(a0)); // noreturn
            return 0;
        }
#if defined(KICKOS_ENABLE_SELFTEST)
        case KOS_SYS_irq_inject:
        {
            // Test scaffolding only (real IRQs come from devices), so compiled out
            // of the production ABI -- like guard_addr below. The line is
            // unprivileged-user-reachable: validate it at the boundary and reject a
            // bad value with -KOS_EINVAL rather than passing it to the controller. (Never
            // KICKOS_UNREACHABLE a user-supplied number -- that would let a user
            // halt the kernel.)
            // Deliberately NOT privilege-gated (unlike irq_unmask/irq_attach): this
            // simulates a DEVICE firing, not an arm of the controller, and the tier-1
            // model has unprivileged drivers receive IRQs (selftest injects from an
            // unprivileged thread). Test-only + one-owner attach already bound the line.
            int irq = static_cast<int>(a0);
            if (irq < 0 or irq >= KICKOS_MAX_IRQ)
            {
                return static_cast<uintptr_t>(-KOS_EINVAL); // bad irq line
            }
            arch_irq_inject(irq);
            return 0;
        }
        case KOS_SYS_guard_addr:
        {
            return arch_mpu_probe_addr();
        }
        case KOS_SYS_irq_spurious:
        {
            return static_cast<uintptr_t>(irq_spurious_count());
        }
#if KICKOS_HAVE_MPU
        case KOS_SYS_grant_probe:
        {
            // Test scaffolding: exercise the Rule 7 grant predicates directly, so the
            // overlap arithmetic (equal / contained / partial / one-byte-edge / alias)
            // and the RAM/DEV admission rules are unit-testable without forging a real
            // MPU descriptor. Pure reads, no state change, so not privilege-gated (like
            // guard_addr). op selects the predicate + posture; the kernel supplies the
            // attr so userspace needs no ARCH_MPU_* enum. Compiled only under enforcement
            // (grant_hits_reserved / arch_reserved_blocks exist only then).
            uintptr_t const op = a0;
            uintptr_t const base = a1;
            size_t const size = static_cast<size_t>(a2);
            uint32_t const rw = ARCH_MPU_R | ARCH_MPU_W;
            uint32_t const dev = ARCH_MPU_R | ARCH_MPU_W | ARCH_MPU_DEV;
            bool result = false;
            switch (op)
            {
                case 0:
                    result = grant_hits_reserved(base, size);
                    break;
                case 1:
                    result = grant_region_admissible(base, size, rw, true);
                    break;
                case 2:
                    result = grant_region_admissible(base, size, rw, false);
                    break;
                case 3:
                    result = grant_region_admissible(base, size, dev, true);
                    break;
                case 4:
                    result = grant_region_admissible(base, size, dev, false);
                    break;
                case 5:
                {
                    struct arch_reserved_block blk[KICKOS_MAX_RESERVED];
                    return arch_reserved_blocks(blk, KICKOS_MAX_RESERVED);
                }
                case 6:
                {
                    struct arch_reserved_block blk[KICKOS_MAX_RESERVED];
                    size_t const n = arch_reserved_blocks(blk, KICKOS_MAX_RESERVED);
                    if (base >= n)
                    {
                        return 0;
                    }
                    return blk[base].base;
                }
                case 7:
                {
                    struct arch_reserved_block blk[KICKOS_MAX_RESERVED];
                    size_t const n = arch_reserved_blocks(blk, KICKOS_MAX_RESERVED);
                    if (base >= n)
                    {
                        return 0;
                    }
                    return blk[base].size;
                }
                default:
                    return static_cast<uintptr_t>(-KOS_EINVAL);
            }
            if (result)
            {
                return 1u;
            }
            return 0u;
        }
#endif
        case KOS_SYS_irq_unmask:
        {
            // Test scaffolding: enable an UNBOUND line so an injected raise reaches
            // the default (spurious) handler on masked-by-default controllers (ARM
            // NVIC, RX), which else drop it. Privileged-only (it arms a controller
            // line), like irq_attach.
            if (not sched::current()->privileged)
            {
                return static_cast<uintptr_t>(-KOS_EPERM); // arms a controller line: privileged-only
            }
            int irq = static_cast<int>(a0);
            if (irq < 0 or irq >= KICKOS_MAX_IRQ)
            {
                return static_cast<uintptr_t>(-KOS_EINVAL); // bad irq line
            }
            arch_irq_unmask(irq);
            return 0;
        }
#endif
        case KOS_SYS_irq_attach:
        {
            // Tier-2 installs a privileged in-kernel handler: privileged-only, so
            // an unprivileged thread cannot bind (or steal) a line's dispatch.
            if (not sched::current()->privileged)
            {
                return static_cast<uintptr_t>(-KOS_EPERM);
            }
            // Resolve + attach + unmask under one lock (like sem_wait/post): otherwise a
            // concurrent close between the resolve check and the attach could bind the
            // line to an already-dead handle.
            IrqLock lock;
            int irq = static_cast<int>(a0);
            int cap_handle = static_cast<int>(a1);
            if (irq < 0 or irq >= KICKOS_MAX_IRQ)
            {
                return static_cast<uintptr_t>(-KOS_EINVAL); // bad irq line
            }
            // Resolve the CAP once, HERE (requires CAP_SIGNAL -- an ISR posts), and store
            // the GLOBAL sem handle in the binding: irq_sem_post re-resolves that global
            // via the pool per fire (an ISR must NEVER resolve a cap -- current() is a
            // random interrupted thread's table). The binding holds no ref, so a
            // last-close (now reachable via a thread exit) makes it a dead binding that
            // fails safe, not a wrong post.
            CapEntry* e = cap_lookup(sched::current(), cap_handle);
            if (e == nullptr or e->type != static_cast<uint8_t>(CapType::CAP_SEM)
                or kernel().sems.resolve(e->obj) == nullptr)
            {
                return static_cast<uintptr_t>(-KOS_EBADF); // bad / non-sem / stale cap
            }
            if ((e->rights & CAP_SIGNAL) != CAP_SIGNAL)
            {
                return static_cast<uintptr_t>(-KOS_EPERM); // cap lacks SIGNAL (an ISR posts)
            }
            int const sem_handle = e->obj;
            // irq_attach fails if the line is already owned -- no stealing (EBUSY).
            if (not irq_attach(irq, irq_sem_post,
                               reinterpret_cast<void*>(static_cast<intptr_t>(sem_handle))))
            {
                return static_cast<uintptr_t>(-KOS_EBUSY);
            }
            // Enable the line: a userspace tier-2 binding has no separate unmask
            // syscall (tier-1 unmasks via register/irq_ack), so attach must arm it.
            // Required on default-masked controllers (ARM NVIC, RX); sim/riscv were
            // unmasked-by-default and only worked by that leniency. In-kernel
            // irq_attach (console) still unmasks on its own schedule -- untouched.
            arch_irq_unmask(irq);
            return 0;
        }
        case KOS_SYS_clock_now:
        {
            // Out-pointer for a 64-bit store: reject null and misalignment, then bound it
            // against the caller's writable regions -- the kernel writes it privileged, so an
            // unprivileged caller must own it. The stub passes a stack local (in its stack
            // region); privileged callers bypass the ownership check. Closes the privileged-
            // kernel-writes-a-user-pointer hole. Alignment is alignof(uint64_t) -- arch-specific
            // (4 on RX, 8 on ARM/RISC-V) and exactly what makes the typed store below well-defined.
            if (a0 == 0 or (a0 & (alignof(uint64_t) - 1)) != 0)
            {
                return static_cast<uintptr_t>(-KOS_EINVAL); // null or misaligned out-ptr
            }
            if (not user_writable_ok(a0, sizeof(uint64_t)))
            {
                return static_cast<uintptr_t>(-KOS_EFAULT); // out-ptr not owned by the caller
            }
            uint64_t const now = arch_clock_now();
            kaccess_to_user(a0, &now, sizeof(now));
            return 0;
        }
        case KOS_SYS_cpu_clock_hz:
        {
            // Read-only, no user pointer: the u32 fits a register, so return it
            // directly rather than via an out-ptr. Stays OUT of the -KOS_E* scheme:
            // it is a u32 Hz whose 0 sentinel already means unknown/no-silicon-clock.
            return static_cast<uintptr_t>(arch_cpu_clock_hz());
        }
        case KOS_SYS_ram_alloc:
        {
            // Privileged-only: domains are carved by the privileged setup path,
            // not by arbitrary user threads (avoids a DoS on the shared pool and
            // matches static-allocation-first). IrqLock: arch_ram_alloc does an
            // unguarded read-modify-write of the bump pointer.
            // POINTER return -- OUT of the -KOS_E* scheme: a negative errno cast to
            // void* would be a non-NULL pointer. EVERY failure path returns 0 (NULL) so
            // the documented `if (p == NULL)` check is correct: the not-privileged reject
            // and the arena-exhausted reject both yield NULL (arch_ram_alloc already
            // returns 0 when exhausted).
            IrqLock lock;
            if (not sched::current()->privileged)
            {
                return 0; // NULL, not (uintptr_t)-1 -- the latent dual-sentinel bug fixed
            }
            return reinterpret_cast<uintptr_t>(
                arch_ram_alloc(static_cast<size_t>(a0)));
        }
        case KOS_SYS_irq_register:
        {
            return static_cast<uintptr_t>(irq_register(static_cast<int>(a0)));
        }
        case KOS_SYS_irq_wait:
        {
            return static_cast<uintptr_t>(irq_wait(static_cast<int>(a0)));
        }
        case KOS_SYS_irq_ack:
        {
            return static_cast<uintptr_t>(irq_ack(static_cast<int>(a0)));
        }
        case KOS_SYS_diag_led_set:
        {
            // Benign single LED (the kernel's diagnostic pin, borrowed): left
            // unprivileged like the console. A no-op on boards with no LED.
            kdiag_led_set(a0 != 0);
            return 0;
        }
        case KOS_SYS_diag_led_toggle:
        {
            kdiag_led_toggle();
            return 0;
        }
        default:
        {
            // Unknown syscall from userspace is a caller error, not a kernel
            // invariant violation: fault the caller (EINVAL), never panic the kernel.
            return static_cast<uintptr_t>(-KOS_EINVAL);
        }
    }
}
