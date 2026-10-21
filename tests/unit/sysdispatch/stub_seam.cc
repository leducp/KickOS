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

#include <kickos/sys/errno.h>
#include <kickos/sys/init.h>

#include "stub_seam.h"
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

namespace kickos
{
    namespace testfix
    {
        uint32_t g_reboot_outcome = REBOOT_NONE;
        uint32_t g_console_drains = 0;
        uintptr_t g_context_entry = 0;
        uintptr_t g_second_arg = 0;
    }
}

extern "C"
{
    void console_tx_flush_sync(void)
    {
        kickos::testfix::g_console_drains++;
    }

    void arch_console_flush_sync(void)
    {
    }

    int arch_reboot(uint32_t outcome)
    {
        kickos::testfix::g_reboot_outcome = outcome;
        printf("SEAM: arch_reboot(%u)\n", static_cast<unsigned>(outcome));
        return -KOS_ENOSYS;
    }

    void arch_shutdown(int status)
    {
        printf("SEAM: arch_shutdown(%d)\n", status);
        exit(0);
    }

    // Every user address the gate hands the kernel lies in a range the caller was granted.
    bool arch_user_text_readable(uintptr_t, size_t)
    {
        return false;
    }

    bool arch_user_data_writable(uintptr_t, size_t)
    {
        return false;
    }

    void arch_context_init(struct arch_context*, void (*entry)(void*), void*, void*, size_t, int)
    {
        kickos::testfix::g_context_entry = reinterpret_cast<uintptr_t>(entry);
        kickos::testfix::g_second_arg = 0;
    }

#if KICKOS_HAVE_ASPACE
    void arch_context_second_arg(struct arch_context*, uintptr_t value)
    {
        kickos::testfix::g_second_arg = value;
    }

    void kickos_task_start(void*, void (*)(void*)) {}
#endif

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
        return kickos::testfix::STUB_CPU_CLOCK_HZ;
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

    int console_handover_refused(void)
    {
        unreached("console_handover_refused");
    }

    void console_handover_begin(void)
    {
        unreached("console_handover_begin");
    }

    void console_owner_set_user(void)
    {
        unreached("console_owner_set_user");
    }
}
