// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Cross-TU seam for the syscall implementation. Declares ONLY what crosses a TU boundary;
// TU-private helpers belong in their own TU's anonymous namespace and must not appear here.
// NOT a public kernel header; the userspace-facing contract is <kickos/arch/arch.h> and the
// object naming layer is <kickos/cap.h>.

#ifndef KICKOS_KERNEL_SYSCALL_SYSCALL_INTERNAL_H
#define KICKOS_KERNEL_SYSCALL_SYSCALL_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include <kickos/arch/arch.h> // ARCH_MPU_NOCACHE
#include <kickos/cap.h>       // KCAP_INVALID (the minting out-parameter's failure value)
#include <kickos/sys/abi.h>   // kos_thread_params (thread_create_call parameter), kos_mem_flags

namespace kickos
{
    // --- User-memory access funnel (syscall_mem.cc) ----------------------------
    // The confused-deputy floor. EVERY kernel-side dereference of a user pointer must
    // funnel through these, and callers MUST validate (user_*_ok) before accessing
    // (kaccess_* / ep_copy): the access functions are the COPY, never the check.
    // See syscall_mem.cc for the full per-function invariants.

    // A range lies within one region the current thread is granted with `need`
    // access. Privileged callers and len==0 pass.
    bool user_range_ok(uintptr_t ptr, size_t len, uint32_t need);

    // The same scan WITHOUT the privileged and len==0 short-circuits: does a region the
    // caller already carries describe this range with exactly `need`, MEMORY TYPE included?
    // Privileged reach comes from the background map, which carries the chip's DEFAULT
    // memory type.
    bool user_range_typed_ok(uintptr_t ptr, size_t len, uint32_t need);

    // A user READ buffer (name / console text): granted-region OR the app's
    // readable code/data extent (arch_user_text_readable).
    bool user_readable_ok(uintptr_t ptr, size_t len);

    // A user WRITE buffer / out-pointer: granted WRITE region OR the app's writable
    // static-data extent (arch_user_data_writable), which is what the backends modelling
    // no static-data region rely on.
    bool user_writable_ok(uintptr_t ptr, size_t len);

    // kos_mem_flags -> the ARCH_MPU_* memory-type bits, ORed into *attr. False on an
    // undefined bit: refused (-KOS_EINVAL), never masked off.
    inline bool mem_flags_to_attr(uintptr_t flags, uint32_t* attr)
    {
        if ((flags & ~static_cast<uintptr_t>(KOS_MEM_FLAGS_ALL)) != 0)
        {
            return false;
        }
        if ((flags & static_cast<uintptr_t>(KOS_MEM_NOCACHE)) != 0)
        {
            *attr |= ARCH_MPU_NOCACHE;
        }
        return true;
    }

    // --- MMIO possession (syscall_mem.cc) --------------------------------------
    // The current thread's own DEV window has base exactly `base`. The whole
    // authorisation for arch_periph_enable; no authority bit gates it. Exact base, not
    // containment, so a sub-block window cannot reach a whole-block table entry. The
    // answer comes from the thread's possession record and never from what it can reach.
    bool caller_holds_mmio_block(uintptr_t base);

    // The write seam's stronger twin: the region matched by the exact base must also
    // CONTAIN [base + offset, +4). Possession of a window is possession of that window
    // only, so an offset the window does not cover is refused before the chip allowlist
    // is consulted. `offset` must already be 4-aligned and non-wrapping (the dispatch
    // arm answers -KOS_EINVAL for those).
    bool caller_holds_mmio_reg(uintptr_t base, uintptr_t offset);

    // --- The owner of a user end -----------------------------------------------
    // Every access below names the address space each user end belongs to, and a NULL
    // space means the address is directly kernel-dereferenceable: kernel storage, and
    // every backend that translates nothing. An owner cannot be recovered from an
    // address once two processes hold different frames at one virtual address, so a
    // site passes what it already holds (docs/design-m6-mmu.md section 3.3).
    struct Thread;
#if KICKOS_HAVE_ASPACE
    // The space a thread's own user pointers lie in.
    struct arch_aspace* user_space_of(Thread const* t);
    // The space a PARKED thread's ipc.buf lies in: its own, EXCEPT under the fastpath's
    // park, whose buffer is the caller's saved trap frame and therefore kernel storage.
    struct arch_aspace* ipc_buf_space(Thread const* t);
#else
    inline struct arch_aspace* user_space_of(Thread const* t)
    {
        (void)t;
        return nullptr;
    }
    inline struct arch_aspace* ipc_buf_space(Thread const* t)
    {
        (void)t;
        return nullptr;
    }
#endif

    // The kernel<->user byte-access seam: split at granule boundaries, each page reached
    // through the acquire seam of the space that owns it.
    void kaccess_from_user(void* kdst, struct arch_aspace* sspace, uintptr_t usrc, size_t n);
    void kaccess_to_user(struct arch_aspace* dspace, uintptr_t udst, void const* ksrc,
                         size_t n);

    // The user<->user endpoint-payload peer of the kaccess seam (both ends user
    // memory, and one of them a PARKED peer's); bounded (<= KOS_EP_MSG_MAX), done under
    // IrqLock at the copy site.
    void ep_copy(struct arch_aspace* dspace, uintptr_t dst, struct arch_aspace* sspace,
                 uintptr_t src, size_t n);

    // Deliver a receiver's kos_recv_info (badge + reply_cap) into its parked
    // out-ptr, or nothing when out == 0. KCAP_INVALID marks a plain send. `ospace` is the
    // owner of `out`, which is the RECEIVER at four of the six callers.
    void write_recv_info(struct arch_aspace* ospace, uintptr_t out, uint32_t badge,
                         uint32_t reply_cap);

    // How many calls the trap-handler IPC fastpath COMPLETED; a refusal does not count.
    // The two paths answer a caller identically, so this is the only thing that separates
    // them from userspace. Reads 0 on a backend with no fastpath.
    uint32_t ipc_fast_taken_count();

    // --- Cap-object creators (syscall_obj.cc) ----------------------------------
    // Every minting call has one shape: a status return plus a handle out-parameter,
    // which is written on EVERY path (KCAP_INVALID on failure). A handle spends all 32
    // bits, so it cannot share the return value with an errno.
    int sem_create(int initial, uint32_t* out_cap);
    int mutex_create(uint32_t* out_cap);

    // --- IPC endpoints (syscall_ipc.cc) ----------------------------------------
    // endpoint_send/recv/call MUST be called with no caller-held IrqLock: they take their
    // own for the resolve/deliver/park and release it before the resume barrier, and a
    // spanning caller lock livelocks ARM.
    //
    // ONE implementation per operation serves both the timed and the untimed syscall
    // number; the untimed dispatch arm passes KOS_TIMEOUT_NONE (recv: timed == false).
    int endpoint_create(uint32_t* out_cap);
    int32_t endpoint_send(uint32_t cap, uintptr_t buf, size_t len, uint32_t timeout_us);
    // `timed` selects KOS_SYS_RECV_TIMED, and then `badge_out` names a kos_recv_timed_opts
    // rather than a bare kos_recv_info: the deadline is read out of that struct before this
    // parks, and `badge_out` is rewritten to the kos_recv_info NESTED in it.
    int32_t endpoint_recv(uint32_t cap, uintptr_t buf, size_t cap_len, uintptr_t badge_out,
                          bool timed);
    // The deadline bounds BOTH call phases: the send-side park and the reply-side park.
    int32_t endpoint_call(uint32_t cap, uintptr_t buf, size_t send_len, size_t recv_cap,
                          uint32_t timeout_us);
    int endpoint_reply(uint32_t reply_cap, uintptr_t buf, size_t len);

    // --- Thread lifecycle (syscall_thread.cc) ----------------------------------
    // Same minting shape as the cap creators above: *out_thread is written on EVERY path
    // (KOS_THREAD_NONE on failure).
    int thread_create_call(kos_thread_params const* p, kos_thread_t* out_thread);
    // Cancels a thread the caller spawned: marks it, and breaks whatever park it is in with
    // -KOS_ECANCELED so it reaches its own death point. Returns 0, -KOS_EBADF, -KOS_EPERM or
    // -KOS_EINVAL. Takes its own IrqLock.
    int thread_kill(kos_thread_t thread);
    // Creates an empty task holding a shared data region, and cancels a task's whole group.
    // Both gate on the creator being the caller. *out_task is written on EVERY path
    // (KOS_TASK_NONE on failure). Each takes its own IrqLock.
    int task_create_call(void* mem_base, size_t mem_size, uint32_t mem_attr,
                         kos_task_t* out_task);
    int task_kill(kos_task_t task);
    // Both BLOCK, so like the endpoint calls they must be reached with no caller-held
    // IrqLock: each takes its own for the gate and the park, then releases it before the
    // resume barrier and the wait_result read.
    //
    // Waits for a thread the caller spawned to be gone, up to `timeout_us` relative
    // microseconds (KOS_TIMEOUT_NONE: no bound). Returns 0 (including for a target that had
    // already exited), -KOS_ETIMEDOUT, -KOS_ECANCELED, -KOS_EBADF, -KOS_EPERM or
    // -KOS_EDEADLK.
    int thread_join(kos_thread_t thread, uint32_t timeout_us);
    // Waits until the caller is the last live thread. ROOT ONLY: returns 0, or -KOS_EPERM
    // to any other caller.
    int thread_wait_last();

    // The FORCIBLE half of the pair above, and both BLOCK on the same terms as thread_join:
    // no caller-held IrqLock. A marked target executes no further unprivileged instruction.
    // 0 means GONE; -KOS_ETIMEDOUT means the redirect is armed and irrevocable with the
    // capability sweep unfinished.
    int thread_slay(kos_thread_t thread, uint32_t timeout_us);
    int task_slay(kos_task_t task, uint32_t timeout_us);

#if KICKOS_HAVE_ASPACE && defined(KICKOS_ENABLE_SELFTEST)
    // Test scaffolding for the address-space seam (syscall_aspace.cc). One scenario per op,
    // run entirely kernel-side, so no mapping primitive is exposed to a caller.
    uint64_t aspace_probe(uintptr_t op, uintptr_t a1);
#endif
}

#endif
