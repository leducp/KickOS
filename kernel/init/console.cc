// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Minimal in-kernel debug console: write-only, unbuffered, routed to the arch
// console bottom edge (sim: host stdout). The standard microkernel exception
// for panic/early-boot/fault reporting.

#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/arch/arch.h>
#include <kickos/libc/string.h>
#include <kickos/libc/fmt.h>

#include <stdarg.h>

namespace kickos {

void kputs(const char* s) {
  arch_console_write(s, strlen(s));
}

void kprintf(const char* fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  kvsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  arch_console_write(buf, strlen(buf));
}

void kpanic(const char* msg) {
  kputs("\nKERNEL PANIC: ");
  kputs(msg);
  kputs("\n");
  arch_shutdown(1);
}

} // namespace kickos

// Memory-protection violation caught by the arch backend (sim: SIGSEGV over
// the guard page). Report the offending task + address through the console.
// M0: the intended wild-write demo is the final act, so we shut down cleanly
// after reporting. M2 will turn this into per-task fault + resume.
extern "C" void kickos_isr_fault(uintptr_t addr, int is_write) {
  ::kickos::Thread* c = ::kickos::sched::current();
  const char* who = "?";
  if (c != nullptr) who = c->name;
  const char* dir = "read";
  if (is_write) dir = "write";
  ::kickos::kprintf("\nMPU FAULT: task '%s' attempted %s at %p -- reported\n",
                    who, dir, reinterpret_cast<void*>(addr));
  arch_shutdown(0);
}
