// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Board-agnostic witness that arch_console_reclaim gives the console back to the kernel when a
// published console driver is slain, and (KICKOS_RW_MODE 1) that kickos_terminate drains the
// console before the core stops. Past the publish no user line reaches the wire, so the verdict
// is main's own task panic, whose record only the console the kernel took back carries.

#include <kickos/board_config.h>
#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/cap_index.h>
#include <kickos/sys/errno.h>
#include <kickos/libc/fmt.h>

#include <kickos/apps/peer_run.h>

#include <stdint.h>
#include <string.h>

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
    constexpr std::chrono::seconds SLAY_BOUND{5};

    // A refusal before the publish, on the console the kernel still owns.
    void print_rc(char const* what, int rc)
    {
        char line[96];
        ksnprintf(line, sizeof(line), "[reclaimwit]   %s rc=%d\n", what, rc);
        kos::print(line);
    }

    void kconsole_raw(char const* s)
    {
        (void)KICKOS_KCONSOLE_MEASURED(ANSWER, s, strlen(s));
    }

    void print_reading_key(void)
    {
        kos::print("[reclaimwit] console reclaim + terminate drain witness\n");
        kos::print("[reclaimwit] HOW TO READ THIS CAPTURE:\n");
        kos::print("[reclaimwit]  1. this block is on the wire, so the kernel owns the console\n");
        kos::print("[reclaimwit]  2. the app now publishes the console to a driver it spawns\n");
        kos::print("[reclaimwit]  3. that driver never writes to a console, so the wire must go\n");
        kos::print("[reclaimwit]     silent: a MUTE line below must occur ZERO times\n");
        kos::print("[reclaimwit]  4. the app then SLAYS the driver and ends its task with a\n");
        kos::print("[reclaimwit]     panic, whose LIVE record only the kernel writes\n");
        kos::print("[reclaimwit]  5. LIVE present + MUTE absent == arch_console_reclaim fired.\n");
        kos::print("[reclaimwit]     MUTE present == the publish never took, verdict void.\n");
        kos::print("[reclaimwit]     both absent == the reclaim did not fire, console still "
                   "dark.\n");
#if KICKOS_RW_RTT
        kos::print("[reclaimwit] NOTE: this image also carries RTT. Read the CHIP UART capture;\n");
        kos::print("[reclaimwit] NOTE: the RTT stream carries kernel writes in every state.\n");
#endif
#if KICKOS_RW_MODE == 1
        kos::print("[reclaimwit] drain arm: kickos_terminate follows the LIVE record, written\n");
        kos::print("[reclaimwit] polled on the RECLAIMED console, so arch_console_flush_sync\n");
        kos::print("[reclaimwit] alone holds the core until the shift register empties. The\n");
        kos::print("[reclaimwit] record's last line must be INTACT, sentinel included.\n");
#else
        kos::print("[reclaimwit] park arm: the LIVE record is the last line of the app\n");
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
        return 1;
    }

    // The driver is a task of its own: the console's death is its task's end, and main's task
    // never ends before the verdict. Its declaration publishes the console through the endpoint
    // main passes, so a refused create publishes nothing and reports on a kernel-owned console.
    kos_program_t prog = KOS_CAP_NONE;
    int const find_rc = kos_program_find(KICKOS_APP_IMAGE, &prog);
    if (find_rc != 0)
    {
        print_rc("FAIL program_find", find_rc);
        return 1;
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
        return 1;
    }

    // Main's own WAIT and HANDOUT must go, or the endpoint still receives, or may again, once
    // the driver is dead. The kernel's stdout ref keeps the endpoint alive.
    int const close_rc = kos_handle_close(ep);

    kconsole_raw("[reclaimwit] MUTE kernel console while the driver holds it\n");

    // Returns only once the driver has TAKEN the bytes, so the published route is served
    // and not merely created.
    char const served[] = "[reclaimwit] routed through the driver, which discards it\n";
    int32_t const serve_rc =
        kos_send(KOS_CAP_STDOUT, served, sizeof(served) - 1u, KOS_NEVER);

    // Forcible, not cooperative: the driver never gets the window in which it would have
    // quieted its device. 0 means the task is dead, every member swept, so the reclaim has
    // already been attempted.
    int const slay_rc = kickos::apps::slay_until_dead(drv_task, kos::after(SLAY_BOUND));

    // The driver's task is dead and nothing holds WAIT or HANDOUT, so this refuses at once.
    char const dead = 'x';
    int32_t const refused_rc = kos_send(KOS_CAP_STDOUT, &dead, 1, KOS_NEVER);

    kconsole_raw("[reclaimwit] MUTE kernel console after the driver died\n");

    bool const ok = (close_rc == 0 and serve_rc == static_cast<int32_t>(sizeof(served) - 1u)
                     and slay_rc == 0 and refused_rc == -KOS_ECONNREFUSED);
    if (not ok)
    {
        char verdict[64];
        ksnprintf(verdict, sizeof(verdict), "[reclaimwit] LIVE FAIL %d %d %d %d", close_rc,
                  static_cast<int>(serve_rc), slay_rc, static_cast<int>(refused_rc));
        kos_panic(verdict);
    }
#if KICKOS_RW_MODE == 1
    kos_panic("[reclaimwit] LIVE PASS 0123456789abcdef <<<DRAIN-END>>>");
#else
    kos_panic("[reclaimwit] LIVE PASS reclaim fired");
#endif
}
