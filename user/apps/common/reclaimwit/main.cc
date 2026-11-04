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

namespace
{
    // -KOS_ETIMEDOUT means condemned but not yet swept, so a timeout costs the sweep's
    // timing, not the death.
    constexpr uint32_t SLAY_TIMEOUT_US = 5000000u;

    __attribute__((noreturn)) void park_forever(void)
    {
        static kos_sem_t idle = KOS_SEM_INIT(0);
        while (true)
        {
            (void)kos_sem_wait(&idle, KOS_TIMEOUT_NONE);
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

    // The driver is a task of its own: the console's death is its task's end, and main's task
    // never ends before the verdict. Its declaration publishes the console through the endpoint
    // main passes, so a refused create publishes nothing and reports on a kernel-owned console.
    kos_program_t prog = KOS_CAP_NONE;
    int const find_rc = kos_program_find(KICKOS_APP_IMAGE, &prog);
    if (find_rc != 0)
    {
        print_rc("FAIL program_find", find_rc);
        park_forever();
    }
    kos_cap_grant const caps[1] = {{ep, KOS_CAP_WAIT | KOS_CAP_TRANSFER}};
    kos_task_params params = {};
    params.instance = "sink";
    params.caps = caps;
    params.cap_count = 1;
    kos_task_t drv_task = KOS_TASK_NONE;
    int const task_rc = kos_task_create(prog, &params, &drv_task);
    (void)kos_handle_close(prog);
    if (task_rc != 0)
    {
        print_rc("FAIL task_create", task_rc);
        park_forever();
    }

    // Main's own WAIT and HANDOUT must go, or the endpoint still receives, or may again, once
    // the driver is dead. The kernel's stdout ref keeps the endpoint alive.
    int const close_rc = kos_handle_close(ep);

    char const mute[] = "[reclaimwit] MUTE kernel console while the driver holds it\n";
    (void)KICKOS_KCONSOLE_MEASURED(DROP, mute, sizeof(mute) - 1u);

    // Returns only once the driver has TAKEN the bytes, so the published route is served
    // and not merely created.
    char const served[] = "[reclaimwit] routed through the driver, which discards it\n";
    int32_t const serve_rc =
        kos_send(KOS_CAP_STDOUT, served, sizeof(served) - 1u, KOS_TIMEOUT_NONE);

    // Forcible, not cooperative: the driver never gets the window in which it would have
    // quieted its device. 0 means the task is empty and swept, so the reclaim has already been
    // attempted.
    int const slay_rc = kos_task_slay(drv_task, SLAY_TIMEOUT_US);

    // The driver's task is dead and nothing holds WAIT or HANDOUT, so this refuses at once.
    char const dead = 'x';
    int32_t const refused_rc = kos_send(KOS_CAP_STDOUT, &dead, 1, KOS_TIMEOUT_NONE);

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
