// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Board-agnostic witness that arch_console_reclaim gives the console back when a published
// console driver is slain, and (KICKOS_RW_MODE 1) that kickos_terminate drains the console
// before the core stops.

#include <kickos/board_config.h>
#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/cap_index.h>
#include <kickos/sys/errno.h>
#include <kickos/libc/fmt.h>

#include <stdint.h>

#ifndef KICKOS_RW_MODE
#error "KICKOS_RW_MODE must be set by this app's CMakeLists"
#endif
#ifndef KICKOS_RW_RTT
#define KICKOS_RW_RTT 0
#endif

// The driver's stack, by this name in tests/static/app_stack_roots.txt, which bounds it.
#if KICKOS_USER_STACK_SIZE > 2048
#define RW_DRIVER_STACK KICKOS_USER_STACK_SIZE
#else
#define RW_DRIVER_STACK 2048
#endif
#if KICKOS_TLS and KICKOS_TLS_FROM_SP
static_assert(RW_DRIVER_STACK == KICKOS_TLS_STRIDE,
              "a masked thread pointer admits a caller stack of exactly one stride");
#endif

namespace
{
    // At or above every stdout client's priority: the console rendezvous carries no
    // priority inheritance.
    constexpr uint8_t DRIVER_PRIO = 12;

    // -KOS_ETIMEDOUT means condemned but not yet swept, so a timeout costs the sweep's
    // timing, not the death.
    constexpr uint32_t SLAY_TIMEOUT_US = 5000000u;

    // The published console driver. It must stay a pure sink: one console write here
    // destroys what separates a fired reclaim from a driver that never died.
    void console_sink(void*)
    {
        uint8_t buf[KOS_EP_MSG_MAX];
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, KOS_SPAWN_DELEGATED_CAP0, KOS_RECV_NO_INFO,
                                 KOS_TIMEOUT_NONE);
        while (true)
        {
            int32_t const n = kos_reply_recv(KOS_CAP_NONE, buf,
                                             kos_call_lens_pack(0, sizeof(buf)), &opts);
            if (n < 0)
            {
                break;
            }
        }
        kos_exit(0);
    }

    __attribute__((noreturn)) void park_forever(void)
    {
        // Sleep fallback for an unmintable semaphore, which would otherwise spin failing
        // syscalls.
        kos_cap_t idle = KOS_CAP_NONE;
        (void)kos_sem_create(0, &idle);
        while (true)
        {
            if (idle == KOS_CAP_NONE)
            {
                kos_sleep_ns(1000000000ull);
                continue;
            }
            (void)kos_sem_wait(idle);
        }
    }

    void print_rc(char const* what, int rc)
    {
        char line[96];
        ksnprintf(line, sizeof(line), "[reclaimwit]   %s rc=%d\n", what, rc);
        kos::print(line);
    }

    void print_reading_key(void)
    {
        kos::print("[reclaimwit] console reclaim + terminate drain witness\n");
        kos::print("[reclaimwit] HOW TO READ THIS CAPTURE:\n");
        kos::print("[reclaimwit]  1. this block is on the wire, so the kernel owns the console\n");
        kos::print("[reclaimwit]  2. the app now publishes the console to a driver it spawns\n");
        kos::print("[reclaimwit]  3. that driver never writes to a console, so the wire must go\n");
        kos::print("[reclaimwit]     silent: a MUTE line below must occur ZERO times\n");
        kos::print("[reclaimwit]  4. the app then SLAYS the driver and prints a LIVE line to\n");
        kos::print("[reclaimwit]     the kernel console the MUTE line was dropped from\n");
        kos::print("[reclaimwit]  5. LIVE present + MUTE absent == arch_console_reclaim fired.\n");
        kos::print("[reclaimwit]     MUTE present == the publish never took, verdict void.\n");
        kos::print("[reclaimwit]     both absent == the reclaim did not fire, console still "
                   "dark.\n");
#if KICKOS_RW_RTT
        // kconsole_write feeds RTT in every ownership state, so the MUTE line reaches an
        // RTT viewer even on a correct run.
        kos::print("[reclaimwit] NOTE: this image also carries RTT. Read the CHIP UART capture;\n");
        kos::print("[reclaimwit] NOTE: the RTT stream carries kernel writes in every state.\n");
#endif
    }
}

int main(int, char**)
{
    print_reading_key();

    kos_cap_t ep = KOS_CAP_NONE;
    int const ep_rc = kos_endpoint_create(&ep);
    if (ep_rc != 0)
    {
        print_rc("FAIL endpoint_create", ep_rc);
        park_forever();
    }

    void* const drv_stack = kos_ram_alloc(RW_DRIVER_STACK);
    if (drv_stack == nullptr)
    {
        print_rc("FAIL ram_alloc driver stack", -KOS_ENOMEM);
        park_forever();
    }
    // Under translation the stack must also be mapped in the task the driver joins. Not
    // elsewhere: an ARMv8-M MPU faults on an address two regions cover.
    void* task_mem = nullptr;
    uint32_t task_mem_size = 0;
#if KICKOS_HAVE_ASPACE
    task_mem = drv_stack;
    task_mem_size = RW_DRIVER_STACK;
#endif

    // The driver is a task of its own: the console's death is its task's end, and main's task
    // never ends before the verdict.
    kos_task_t drv_task = KOS_TASK_NONE;
    int const task_rc = kos_task_create(task_mem, task_mem_size, 0, &drv_task);
    if (task_rc != 0)
    {
        print_rc("FAIL task_create", task_rc);
        park_forever();
    }

    // Spawned before the publish so a refused spawn still reports on a kernel-owned
    // console. Unprivileged: a task slay kills a privileged member rather than slaying it.
    kos_cap_grant const caps[1] = {{ep, KOS_CAP_WAIT}};
    auto const drv = kos::thread::create_caps(console_sink, nullptr, "rwdrv", DRIVER_PRIO,
                                              caps, /*cap_count=*/1, KOS_POLICY_FIFO, 0,
                                              /*privileged=*/false, nullptr, 0, 0, nullptr,
                                              drv_task, drv_stack, RW_DRIVER_STACK);
    if (not drv.valid())
    {
        print_rc("FAIL driver spawn", drv.error());
        park_forever();
    }

    int const pub_rc = kos_console_publish(ep, drv_task);
    if (pub_rc != 0)
    {
        print_rc("FAIL console_publish", pub_rc);
        park_forever();
    }

    // Main's own WAIT and HANDOUT must go, or the endpoint still receives, or may again, once
    // the driver is dead. The kernel's stdout ref keeps the endpoint alive.
    int const close_rc = kos_handle_close(ep);

    char const mute[] = "[reclaimwit] MUTE kernel console while the driver holds it\n";
    (void)kos_kconsole_write(mute, sizeof(mute) - 1u); // a dropped line is the measurement

    // Returns only once the driver has TAKEN the bytes, so the published route is served
    // and not merely created.
    char const served[] = "[reclaimwit] routed through the driver, which discards it\n";
    int32_t const serve_rc = kos_send(KOS_CAP_STDOUT, served, sizeof(served) - 1u);

    // Forcible, not cooperative: the driver never gets the window in which it would have
    // quieted its device. 0 means the task is empty and swept, so the reclaim has already been
    // attempted.
    int const slay_rc = kos_task_slay(drv_task, SLAY_TIMEOUT_US);

    // The driver's task is dead and nothing holds WAIT or HANDOUT, so this refuses at once.
    char const dead = 'x';
    int32_t const refused_rc = kos_send(KOS_CAP_STDOUT, &dead, 1);

    kos::print("[reclaimwit] LIVE kernel console after the driver died\n");
    print_rc("handle_close", close_rc);
    print_rc("driver serve bytes", serve_rc);
    print_rc("slay", slay_rc);
    print_rc("post-death send (want -KOS_ECONNREFUSED)", refused_rc);

    bool const ok = (close_rc == 0 and serve_rc == static_cast<int32_t>(sizeof(served) - 1u)
                     and slay_rc == 0 and refused_rc == -KOS_ECONNREFUSED);
    if (ok)
    {
        kos::print("[reclaimwit] PASS reclaim fired: driver slain, endpoint refused, wire back\n");
    }
    else
    {
        kos::print("[reclaimwit] FAIL see the rc lines above\n");
    }

#if KICKOS_RW_MODE == 1
    kos::print("[reclaimwit] drain arm: kickos_terminate follows. The ring is disarmed in\n");
    kos::print("[reclaimwit] RECLAIMED, so arch_console_flush_sync alone holds the core until\n");
    kos::print("[reclaimwit] the shift register empties. The capture must END with the next\n");
    kos::print("[reclaimwit] line INTACT, sentinel included; a short tail is a drain that\n");
    kos::print("[reclaimwit] did not complete before arch_shutdown stopped the core.\n");
    kos::print("[reclaimwit] DRAINTAIL 0123456789abcdef0123456789abcdef <<<DRAIN-END>>>\n");
    if (ok)
    {
        return 0;
    }
    return 1;
#else
    kos::print("[reclaimwit] park arm: the system stays up, nothing further is printed\n");
    park_forever();
#endif
}
