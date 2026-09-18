// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Cross-TU seam for the syscall implementation. Declares ONLY what crosses a TU boundary;
// TU-private helpers belong in their own TU's anonymous namespace and must not appear here.

#ifndef KICKOS_KERNEL_SYSCALL_SYSCALL_INTERNAL_H
#define KICKOS_KERNEL_SYSCALL_SYSCALL_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include <kickos/arch/arch.h> // ARCH_MPU_NOCACHE
#include <kickos/aspace.h>    // the kaccess byte-access seam
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

    // The same scan without the privileged and len==0 short-circuits: does a region the caller
    // already carries describe this range with exactly `need`, MEMORY TYPE included?
    bool user_range_typed_ok(uintptr_t ptr, size_t len, uint32_t need);

    // A user READ buffer (name / console text): granted-region OR the app's
    // readable code/data extent (arch_user_text_readable).
    bool user_readable_ok(uintptr_t ptr, size_t len);

    // A user WRITE buffer / out-pointer: granted WRITE region OR the app's writable
    // static-data extent (arch_user_data_writable), which is what the backends modelling
    // no static-data region rely on.
    bool user_writable_ok(uintptr_t ptr, size_t len);

    // The CONJUNCTION of the two above over one base: readable across `read_len` AND writable
    // across `write_len`, in one walk. For a buffer the kernel reads and then writes back.
    bool user_readable_and_writable_ok(uintptr_t ptr, size_t read_len, size_t write_len);

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
    // The current thread's own DEV window has base exactly `base`. The whole authorisation for
    // arch_periph_enable; no authority bit gates it. Exact base, so a sub-block window cannot
    // reach a whole-block table entry, and the answer comes from the possession record.
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
    // site passes what it already holds.
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

    // The kernel<->user byte-access seam and its user<->user peer are declared in
    // kickos/aspace.h. ep_copy's payloads here are bounded (<= KOS_EP_MSG_MAX) and copied
    // under IrqLock at the copy site.

    // Write badge and reply_cap to out in ospace; out == 0 skips metadata.
    // KCAP_INVALID marks a plain send. Return whether the write succeeded.
    // Do not assert on failure: fault reporting uses this path and a panic would
    // re-enter console output. It would also add console stack use to syscall paths.
    [[nodiscard]] bool write_recv_info(struct arch_aspace* ospace, uintptr_t out, uint32_t badge,
                                       uint32_t reply_cap);

    // How many calls the trap-handler IPC fastpath COMPLETED; a refusal does not count.
    // The two paths answer a caller identically, so this is the only thing that separates
    // them from userspace. Reads 0 on a backend with no fastpath.
    uint32_t ipc_fast_taken_count();

    // --- Cap-object creators (syscall_obj.cc) ----------------------------------
    // Every minting call has one shape: a status return plus a handle out-parameter,
    // which is written on EVERY path (KCAP_INVALID on failure). A handle spends all 32
    // bits, so it cannot share the return value with an errno.
    //
    // THREE EXHAUSTION ANSWERS, and each of the three creators here and irq_claim can give
    // any of them: -KOS_ENOMEM the pool, -KOS_EMFILE the caller's table, -KOS_EOVERFLOW the
    // calling TASK's ceiling for that pool while the pool still holds slots (task.h).
    int sem_create(int initial, uint32_t* out_cap);
    int mutex_create(uint32_t* out_cap);

    // IPC endpoints (syscall_ipc.cc).
    // Call send/call/reply_recv without IrqLock. They lock internally and must
    // release it before the resume barrier; an outer lock would livelock ARM.
    // Untimed calls use KOS_TIMEOUT_NONE.
    int endpoint_create(uint32_t* out_cap);
    // The receiver runs in another node's kernel. Privileged, and -KOS_ENOSYS where the
    // image runs one kernel over one core.
    int amp_endpoint_create(uint32_t node, uint32_t port, uint32_t* out_cap);
#if KICKOS_AMP_NODE
    // The same mint with the privilege gate left to the caller. Caller holds IrqLock.
    int amp_endpoint_mint(Thread* c, uint32_t node, uint32_t port, uint32_t* out_cap);
#endif
    int32_t endpoint_send(uint32_t cap, uintptr_t buf, size_t len, uint32_t timeout_us);
    // The deadline bounds BOTH call phases: the send-side park and the reply-side park.
    int32_t endpoint_call(uint32_t cap, uintptr_t buf, size_t send_len, size_t recv_cap,
                          uint32_t timeout_us);
    int endpoint_reply(uint32_t reply_cap, uintptr_t buf, size_t len);
    // KOS_SYS_REPLY_RECV. `lens` is kos_call_lens_pack(reply_len, recv_cap) and `opts` names a
    // kos_reply_recv_opts the kernel reads and writes back.
    int32_t endpoint_reply_recv(uint32_t reply_cap, uintptr_t buf, uintptr_t lens,
                                uintptr_t opts);

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
    // Placement and the scheduling grant. Both are total over every posture: at one kernel
    // core the placement call answers -KOS_ENOSYS and the grant carries its ceiling half only.
    int thread_set_affinity(kos_thread_t thread, uint32_t core_mask);
    int task_sched_grant(kos_task_t task, uint8_t prio_ceiling, uint32_t core_mask);
    int task_slay(kos_task_t task, uint32_t timeout_us);

#if KICKOS_HAVE_ASPACE && defined(KICKOS_ENABLE_SELFTEST)
    // Test scaffolding for the address-space seam (syscall_aspace.cc). One scenario per op,
    // run entirely kernel-side, so no mapping primitive is exposed to a caller.
    uint64_t aspace_probe(uintptr_t op, uintptr_t a1);
#endif

#if KICKOS_AMP_NODE && defined(KICKOS_ENABLE_SELFTEST)
    // Test scaffolding for the shared window (syscall_amp.cc).
    uint64_t amp_probe(uintptr_t op, uintptr_t a1);
#endif
}

#endif
