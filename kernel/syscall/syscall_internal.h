// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Cross-TU seam for the syscall implementation. Declares ONLY what crosses a TU boundary:
// the shared user-memory access funnel and the per-domain handlers the dispatch invokes.
// TU-private helpers belong in their own TU's anonymous namespace and must not appear here.
// NOT a public kernel header; the userspace-facing contract is <kickos/arch/arch.h> and the
// object naming layer is <kickos/cap.h>.

#ifndef KICKOS_SYSCALL_INTERNAL_H
#define KICKOS_SYSCALL_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include <kickos/cap.h>     // KCAP_INVALID (the minting out-parameter's failure value)
#include <kickos/sys/abi.h> // kos_thread_params (thread_spawn parameter)

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

    // A user READ buffer (name / console text): granted-region OR the app's
    // readable code/data extent (arch_user_text_readable).
    bool user_readable_ok(uintptr_t ptr, size_t len);

    // A user WRITE buffer / out-pointer: granted WRITE region OR the app's writable
    // static-data extent (arch_user_data_writable), which is what the backends modelling
    // no static-data region rely on.
    bool user_writable_ok(uintptr_t ptr, size_t len);

    // --- MMIO possession (syscall_mem.cc) --------------------------------------
    // The current thread holds a live DEV region whose base is exactly `base`. The
    // whole authorisation for arch_periph_enable; no authority bit gates it. Exact
    // base, not containment, so a sub-block window cannot reach a whole-block table
    // entry.
    bool caller_holds_mmio_block(uintptr_t base);

    // The write seam's stronger twin: the region matched by the exact base must also
    // CONTAIN [base + offset, +4). Possession of a window is possession of that window
    // only, so an offset the window does not cover is refused before the chip allowlist
    // is consulted. `offset` must already be 4-aligned and non-wrapping (the dispatch
    // arm answers -KOS_EINVAL for those).
    bool caller_holds_mmio_reg(uintptr_t base, uintptr_t offset);

    // The kernel<->user byte-access seam (identity today; one physical space).
    void kaccess_from_user(void* kdst, uintptr_t usrc, size_t n);
    void kaccess_to_user(uintptr_t udst, void const* ksrc, size_t n);

    // The user<->user endpoint-payload peer of the kaccess seam (both ends user
    // memory); bounded (<= KOS_EP_MSG_MAX), done under IrqLock at the copy site.
    void ep_copy(uintptr_t dst, uintptr_t src, size_t n);

    // Deliver a receiver's kos_recv_info (badge + reply_cap) into its parked
    // out-ptr, or nothing when out == 0. KCAP_INVALID marks a plain send.
    void write_recv_info(uintptr_t out, uint32_t badge, uint32_t reply_cap);

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
    // The three byte-count returns are int32_t so this side of the trap boundary matches
    // <kickos/sys.h> exactly; endpoint_create and endpoint_reply answer a plain status and
    // need no pinned width.
    //
    // ONE implementation per operation serves both the timed and the untimed syscall
    // number; the untimed dispatch arm passes KOS_TIMEOUT_NONE (recv: timed == false).
    int endpoint_create(uint32_t* out_cap);
    int32_t endpoint_send(uint32_t cap, uintptr_t buf, size_t len, uint32_t timeout_us);
    // `timed` selects KOS_SYS_RECV_TIMED, and then `badge_out` names a kos_recv_timed_opts
    // rather than a bare kos_recv_info: there is no argument slot left for the deadline, so
    // it is read out of that struct before this ever parks, and `badge_out` is rewritten to
    // the kos_recv_info NESTED in it. Nothing past that point sees the difference.
    int32_t endpoint_recv(uint32_t cap, uintptr_t buf, size_t cap_len, uintptr_t badge_out,
                          bool timed);
    // The deadline bounds BOTH call phases: the send-side park and the reply-side park.
    int32_t endpoint_call(uint32_t cap, uintptr_t buf, size_t send_len, size_t recv_cap,
                          uint32_t timeout_us);
    int endpoint_reply(uint32_t reply_cap, uintptr_t buf, size_t len);

    // --- Thread lifecycle (syscall_thread.cc) ----------------------------------
    // Same minting shape as the cap creators above, for the same reason: a thread handle
    // spends all 32 bits. *out_thread is written on EVERY path (KOS_THREAD_NONE on failure).
    int thread_spawn(kos_thread_params const* p, kos_thread_t* out_thread);
    // Cancels a thread the caller spawned: marks it, and wakes it out of an irq_wait with
    // -KOS_ECANCELED so the target runs its OWN exit. Returns 0, -KOS_EBADF, -KOS_EPERM or
    // -KOS_EINVAL. Takes its own IrqLock.
    int thread_kill(kos_thread_t thread);
    // Both BLOCK, so like the endpoint calls they must be reached with no caller-held
    // IrqLock: each takes its own for the gate and the park, then releases it before the
    // resume barrier and the wait_result read.
    //
    // Waits for a thread the caller spawned to be gone, up to `timeout_us` relative
    // microseconds (KOS_TIMEOUT_NONE: no bound). Returns 0 (including for a target that had
    // already exited), -KOS_ETIMEDOUT, -KOS_EBADF, -KOS_EPERM or -KOS_EDEADLK.
    int thread_join(kos_thread_t thread, uint32_t timeout_us);
    // Waits until the caller is the last live thread. ROOT ONLY: returns 0, or -KOS_EPERM
    // to any other caller.
    int thread_wait_last();
}

#endif
