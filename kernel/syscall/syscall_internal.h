// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Cross-TU seam for the syscall implementation, split per domain (mem funnel,
// IPC, threads, cap objects) out of syscall.cc. Declares ONLY what crosses a TU
// boundary: the shared user-memory access funnel (defined in syscall_mem.cc,
// used by every domain + the dispatch), and each domain handler the dispatch in
// syscall.cc invokes. Genuinely TU-private helpers stay in their own TU's
// anonymous namespace and never appear here. Not a public kernel header -- the
// userspace-facing contract is <kickos/arch/arch.h> (syscall_dispatch) and the
// object naming layer is <kickos/cap.h>.

#ifndef KICKOS_SYSCALL_INTERNAL_H
#define KICKOS_SYSCALL_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include <kickos/sys/abi.h> // kos_thread_params (thread_spawn parameter)

namespace kickos
{
    // --- User-memory access funnel (syscall_mem.cc) ----------------------------
    // The confused-deputy floor + the kernel<->user byte-access seam. Every
    // kernel-side dereference of a user pointer funnels through these, so the MMU
    // era reimplements exactly this set. Callers MUST validate (user_*_ok) before
    // accessing (kaccess_* / ep_copy) -- the access functions are the COPY, not the
    // check. See syscall_mem.cc for the full per-function invariants.

    // A range lies within one region the current thread is granted with `need`
    // access. Privileged callers and len==0 pass.
    bool user_range_ok(uintptr_t ptr, size_t len, uint32_t need);

    // A user READ buffer (name / console text): granted-region OR the app's
    // readable code/data extent (arch_user_text_readable).
    bool user_readable_ok(uintptr_t ptr, size_t len);

    // A user WRITE buffer / out-pointer: granted WRITE region OR the app's writable
    // static-data extent (arch_user_data_writable), for the backends that model no
    // static-data region -- no-MPU chips and the host sim.
    bool user_writable_ok(uintptr_t ptr, size_t len);

    // The kernel<->user byte-access seam (identity today; one physical space).
    void kaccess_from_user(void* kdst, uintptr_t usrc, size_t n);
    void kaccess_to_user(uintptr_t udst, void const* ksrc, size_t n);

    // The user<->user endpoint-payload peer of the kaccess seam (both ends user
    // memory); bounded (<= KOS_EP_MSG_MAX), done under IrqLock at the copy site.
    void ep_copy(uintptr_t dst, uintptr_t src, size_t n);

    // Deliver a receiver's kos_recv_info (badge + reply_cap) into its parked
    // out-ptr, or nothing when out == 0. reply_cap == -1 marks a plain send.
    void write_recv_info(uintptr_t out, uint32_t badge, int32_t reply_cap);

    // --- Cap-object creators (syscall_obj.cc) ----------------------------------
    int sem_create(int initial);
    int mutex_create();

    // --- IPC endpoints (syscall_ipc.cc) ----------------------------------------
    // endpoint_send/recv/call are FULLY LOCKLESS from dispatch (they take their own
    // IrqLock for the resolve/deliver/park, then release it before the resume
    // barrier): a spanning caller lock would livelock ARM (design section 3).
    int endpoint_create();
    int endpoint_send(int cap, uintptr_t buf, size_t len);
    int endpoint_recv(int cap, uintptr_t buf, size_t cap_len, uintptr_t badge_out);
    int endpoint_call(int cap, uintptr_t buf, size_t send_len, size_t recv_cap);
    int endpoint_reply(int reply_cap, uintptr_t buf, size_t len);

    // --- Thread lifecycle (syscall_thread.cc) ----------------------------------
    int thread_spawn(kos_thread_params const* p);
}

#endif
