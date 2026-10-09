// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// What the system-call dispatch names beside the calls the gate makes, at both postures: the
// layers no case reaches answer FIXTURE FAIL, and the arch answers the cases read are fixed.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <kickos/arch/arch.h>
#include <kickos/kernel.h>
#include <kickos/task.h>

#include "syscall_internal.h"

namespace
{
    [[noreturn]] void unreached(char const* what)
    {
        printf("FIXTURE FAIL: %s reached\n", what);
        exit(1);
    }
}

namespace kickos
{
    int endpoint_create(uint32_t*)
    {
        unreached("endpoint_create");
    }

    int amp_endpoint_create(uint32_t, uint32_t, uint32_t*)
    {
        unreached("amp_endpoint_create");
    }

    int32_t endpoint_send(uint32_t, uintptr_t, size_t, uint32_t)
    {
        unreached("endpoint_send");
    }

    int32_t endpoint_call(uint32_t, uintptr_t, size_t, size_t, uint32_t)
    {
        unreached("endpoint_call");
    }

    int32_t endpoint_reply_recv(uint32_t, uintptr_t, uintptr_t, uintptr_t)
    {
        unreached("endpoint_reply_recv");
    }

    int32_t endpoint_reply(uint32_t, uintptr_t, size_t)
    {
        unreached("endpoint_reply");
    }

    uint64_t cpu_clock_set(kos_pstate_t)
    {
        unreached("cpu_clock_set");
    }

    void ktime_sleep_ns(uint64_t)
    {
        unreached("ktime_sleep_ns");
    }

    int kconsole_write_user(char const*, size_t, bool)
    {
        unreached("kconsole_write_user");
    }

    void kdiag_led_set(bool)
    {
        unreached("kdiag_led_set");
    }

    void kdiag_led_toggle()
    {
        unreached("kdiag_led_toggle");
    }

    bool console_window_withheld(uintptr_t, size_t, Task const*)
    {
        unreached("console_window_withheld");
    }
}

extern "C"
{
    // Every user address the gate hands the kernel lies in a range the caller was granted.
    bool arch_user_text_readable(uintptr_t, size_t)
    {
        return false;
    }

    bool arch_user_data_writable(uintptr_t, size_t)
    {
        return false;
    }

    void arch_context_init(struct arch_context*, void (*)(void*), void*, void*, size_t, int) {}

    uint64_t arch_clock_now(void)
    {
        return 0;
    }

    void* kmemcpy(void* dst, void const* src, size_t n)
    {
        return memcpy(dst, src, n);
    }

    void* kmemset(void* dst, int c, size_t n)
    {
        return memset(dst, c, n);
    }

    struct arch_reserved_span arch_reserved_blocks(void)
    {
        return {};
    }

    struct arch_reserved_span arch_window_apertures(void)
    {
        return {};
    }

    struct arch_reserved_span arch_bus_master_apertures(void)
    {
        return {};
    }

    int arch_bitband_present(void)
    {
        return 0;
    }

    uint64_t arch_cpu_clock_hz(void)
    {
        unreached("arch_cpu_clock_hz");
    }

    uint32_t arch_periph_clock_hz(uintptr_t)
    {
        unreached("arch_periph_clock_hz");
    }

    int arch_periph_enable(uintptr_t)
    {
        unreached("arch_periph_enable");
    }

    int arch_periph_reg_write(uintptr_t, uintptr_t, uint32_t)
    {
        unreached("arch_periph_reg_write");
    }

    int arch_pinmux_set(uint32_t, uint32_t, uint32_t)
    {
        unreached("arch_pinmux_set");
    }

    void console_handover_begin(void)
    {
        unreached("console_handover_begin");
    }

    uint32_t console_chip_writers(void)
    {
        unreached("console_chip_writers");
    }

    void console_owner_set_user(void)
    {
        unreached("console_owner_set_user");
    }
}
