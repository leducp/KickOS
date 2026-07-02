// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The KickOS syscall ABI shared by userspace stubs and the kernel dispatch
// table. Numbers are stable contract; argument packing is uintptr_t-wide.

#ifndef KICKOS_SYS_ABI_H
#define KICKOS_SYS_ABI_H

#include <stddef.h>
#include <stdint.h>

enum kos_syscall_nr {
  KOS_SYS_write        = 1,   // (fd, buf, len)        -> bytes written
  KOS_SYS_yield        = 2,   // ()                    -> 0
  KOS_SYS_sleep_ns     = 3,   // (ns)                  -> 0
  KOS_SYS_sem_create   = 4,   // (initial)             -> sem id, or -1
  KOS_SYS_sem_wait     = 5,   // (id)                  -> 0
  KOS_SYS_sem_post     = 6,   // (id)                  -> 0
  KOS_SYS_thread_spawn = 7,   // (kos_thread_params*)  -> thread id, or -1
  KOS_SYS_exit         = 8,   // (code)                -> does not return
  KOS_SYS_irq_inject   = 9,   // (irq)                 -> 0
  KOS_SYS_guard_addr   = 10,  // ()                    -> protected probe addr
  KOS_SYS_irq_attach   = 11,  // (irq, sem_id)         -> 0  (post sem on IRQ)
  KOS_SYS_clock_now    = 12   // ()                    -> monotonic ns
};

enum kos_policy {
  KOS_POLICY_FIFO = 0,
  KOS_POLICY_RR   = 1
};

// Thread-creation parameters (kernel allocates TCB + stack from a static pool).
struct kos_thread_params {
  void      (*entry)(void* arg);
  void*       arg;
  const char* name;
  uint8_t     prio;
  uint8_t     policy;       // enum kos_policy
  uint8_t     privileged;   // 0 => unprivileged user thread
  uint32_t    quantum_ns;   // RR slice; 0 => none
};

#endif // KICKOS_SYS_ABI_H
