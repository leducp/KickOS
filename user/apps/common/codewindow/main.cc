// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The user code window of the RV32 PMP chips, a TOR pair that ends where the last image's code
// ends: its first and last words read, then a fetch from the word below it (the kernel's text)
// or from the first word past it faults. Two binaries share this source, each one fetch, since
// the fault ends the task.

#include <kickos/kos.h>
#include <kickos/libc/fmt.h>

#include <stdint.h>

#if !defined(CODEWINDOW_BELOW) and !defined(CODEWINDOW_PAST)
#error "codewindow takes CODEWINDOW_BELOW or CODEWINDOW_PAST from its CMakeLists"
#endif

extern "C"
{
    extern char __kickos_pmp_code_start[];
    extern char __kickos_pmp_code_end[];
}

int main(int, char**)
{
    uintptr_t const lo = reinterpret_cast<uintptr_t>(__kickos_pmp_code_start);
    uintptr_t const hi = reinterpret_cast<uintptr_t>(__kickos_pmp_code_end);
    char msg[96];
    ksnprintf(msg, sizeof(msg), "[codewindow] window 0x%lx..0x%lx\n",
              static_cast<unsigned long>(lo), static_cast<unsigned long>(hi));
    kos::print(msg);

    volatile uint32_t const* const first = reinterpret_cast<volatile uint32_t const*>(lo);
    volatile uint32_t const* const last = reinterpret_cast<volatile uint32_t const*>(hi - 4u);
    uint32_t const sink = *first + *last;
    (void)sink;
    kos::print("[codewindow] first and last words read\n");

#if defined(CODEWINDOW_BELOW)
    uintptr_t const at = lo - 4u;
#else
    uintptr_t const at = hi;
#endif
    ksnprintf(msg, sizeof(msg), "[codewindow] fetching 0x%lx\n", static_cast<unsigned long>(at));
    kos::print(msg);
    void (*const target)() = reinterpret_cast<void (*)()>(at);
    target();
    kos::print("[codewindow] ERROR: the fetch was permitted\n");
    return 1;
}
