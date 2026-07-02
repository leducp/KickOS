// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Kernel bring-up: create the idle + root threads and start the scheduler.
// The root thread calls the application entry (dependency inversion): the app
// owns kickos_app_main(); the kernel boot path calls it after init.

#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/time.h>
#include <kickos/arch/arch.h>

namespace kickos {
namespace {

alignas(16) unsigned char g_idle_stack[64 * 1024];
alignas(16) unsigned char g_root_stack[64 * 1024];
Thread g_idle_tcb;
Thread g_root_tcb;

void idle_entry(void*) {
  for (;;) arch_idle_wait();
}

void root_entry(void*) {
  kickos_app_main();
  // Returns -> the trampoline exits this thread; other threads keep running.
}

} // namespace

int kmain() {
  sched::init();
  ktime_init();

  ThreadAttr idle_attr;
  idle_attr.name       = "idle";
  idle_attr.prio       = KICKOS_PRIO_IDLE;
  idle_attr.policy     = Policy::FIFO;
  idle_attr.privileged = true;
  thread_create(&g_idle_tcb, idle_entry, nullptr,
                g_idle_stack, sizeof(g_idle_stack), idle_attr);

  // Root runs at a low priority: adding a thread does not itself reschedule,
  // so root still runs first (nothing higher is READY until it spawns them)
  // and does all setup; then, once it blocks, higher-priority workers run,
  // and their completion posts never preempt the low-priority orchestrator.
  ThreadAttr root_attr;
  root_attr.name       = "root";
  root_attr.prio       = KICKOS_PRIO_MIN + 1;
  root_attr.policy     = Policy::FIFO;
  root_attr.privileged = true;
  thread_create(&g_root_tcb, root_entry, nullptr,
                g_root_stack, sizeof(g_root_stack), root_attr);

  sched::start();   // returns only if the scheduler ever unwinds to boot
  return 0;
}

} // namespace kickos
