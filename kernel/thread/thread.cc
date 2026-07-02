// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/libc/string.h>

namespace kickos {

void thread_create(Thread* t, void (*entry)(void*), void* arg,
                   void* stack_base, size_t stack_size, const ThreadAttr& attr) {
  memset(t, 0, sizeof(*t));
  t->name        = attr.name;
  t->prio        = attr.prio;
  t->base_prio   = attr.prio;
  t->policy      = attr.policy;
  t->quantum_ns  = attr.quantum_ns;
  t->privileged  = attr.privileged;
  t->state       = ThreadState::INACTIVE;
  t->stack_base  = stack_base;
  t->stack_size  = stack_size;
  t->region_count = 0;
  t->slice_deadline_ns = UINT64_MAX;

  // MPU posture. Privileged threads are granted the protected guard region
  // (sim: the mprotect guard page); unprivileged threads get no region and so
  // fault on any access to it. Per-task regions are reloaded on every switch.
  if (attr.privileged) {
    uintptr_t g = arch_mpu_probe_addr();
    if (g != 0) {
      t->regions[0].base = g;
      t->regions[0].size = 4096;   // sim guard is a single page
      t->regions[0].attr = ARCH_MPU_R | ARCH_MPU_W;
      t->region_count = 1;
    }
  }

  arch_context_init(&t->ctx, entry, arg, stack_base, stack_size, attr.privileged);
  sched::add(t);
}

} // namespace kickos

// The arch trampoline routes here when a thread's entry function returns.
extern "C" void kickos_thread_return(void) {
  ::kickos::sched::exit_current();
  for (;;) {}   // unreachable
}
